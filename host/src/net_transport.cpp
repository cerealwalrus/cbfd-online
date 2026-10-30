// See net_transport.hpp. One ENet host per server (all its players) or per client (its one connection), each run by a thread of its own
// that sends what the session queued and files what comes in; the session's threads only touch queues.
#include "net_transport.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#include <enet/enet.h>

namespace conker::transport {
    namespace {
        using clock = std::chrono::steady_clock;
        constexpr enet_uint32 timeout_limit = 32, timeout_min_ms = 5000, timeout_max_ms = 15000;
        constexpr size_t bulk_chunk = 16 * 1024;
        constexpr size_t bulk_queue_limit = 24;   // ENet commands allowed to be waiting before the next chunk is handed over
        constexpr auto close_grace = std::chrono::seconds(3);   // a connection that doesn't finish closing by itself is cut after this

        // (possess158) ENet drops a share of the unreliable packets it sends while it sees the round trip grow (its packet throttle,
        // down by its deceleration each time): just when the session's second copies of the inputs and frames (sent unreliably on
        // channel 3) are what gets the game past a lost packet. No deceleration: the throttle stays full and none are dropped. (ENet
        // tells the other end too.)
        void keep_unreliable_flowing(ENetPeer* peer) {
            enet_peer_throttle_configure(peer, ENET_PEER_PACKET_THROTTLE_INTERVAL, ENET_PEER_PACKET_THROTTLE_ACCELERATION, 0);
        }
    }

    struct Conn::State {
        ENetPeer* peer = nullptr;            // the network thread's only
        bool gone = false;                   // the network thread's only: the peer is no more
        clock::time_point close_began{};     // the network thread's only
        std::shared_ptr<Loop> loop;
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<std::vector<uint8_t>> incoming;
        std::atomic<bool> is_closed{ false };   // nothing more will come (what is in `incoming` can still be read)
        std::atomic<bool> closing{ false };     // close() was called
        std::atomic<double> rtt{ 0.0 };
        std::string remote;
        std::mutex stats_mutex;
        std::string stats_text;
        // Channel 1 (bulk) messages wait here and are handed to ENet a chunk at a time (the network thread's only), so a snapshot isn't a
        // thousand datagrams queued ahead of the next input; the receiving end puts the chunks back together.
        std::deque<std::vector<uint8_t>> bulk;
        std::deque<std::vector<uint8_t>> bulk_urgent;   // (sent 2: these go before what waits in `bulk`, in their own order)
        bool sending_urgent = false;                     // the message being chunked (bulk_offset > 0) is bulk_urgent's front
        size_t bulk_offset = 0;
        bool close_requested = false;
        std::vector<uint8_t> assembling;    // (the receiving side, network thread's only)
        // How much bulk may be on its way unacknowledged (network thread's only). A snapshot sent as fast as ENet's window allows filled
        // the link's queue: the round trip went from 110 to 200 ms while it went (0.9.41 test), and every input waited in that queue too.
        // So the amount follows the round trip: smaller while it is well above the best seen, larger again while it is near it.
        size_t bulk_cap = 64 * 1024;
        enet_uint32 best_rtt = 0xFFFFFFFFu;
        clock::time_point cap_changed{};
    };

    class Loop : public std::enable_shared_from_this<Loop> {
    public:
        struct Job {
            std::shared_ptr<Conn::State> state;
            std::vector<uint8_t> data;
            int channel = 0;
            bool close = false;
        };

        ENetHost* host = nullptr;
        bool server = false;
        std::mutex mutex;                                   // jobs, accepted, finished
        std::condition_variable accept_cv, done_cv;
        std::deque<Job> jobs;
        std::deque<std::shared_ptr<Conn>> accepted;
        std::atomic<bool> stopping{ false };
        bool finished = false;
        std::map<ENetPeer*, std::shared_ptr<Conn::State>> by_peer;   // the network thread's only

        void start() {
            auto self = shared_from_this();
            std::thread([self]() { self->run(); }).detach();
        }

        void post(Job job) {
            std::lock_guard lock{ mutex };
            jobs.push_back(std::move(job));
        }

        static void mark_closed(Conn::State& st) {
            {
                std::lock_guard lock{ st.mutex };
                st.is_closed = true;
            }
            st.cv.notify_all();
            st.gone = true;
        }

        void handle(const ENetEvent& ev) {
            switch (ev.type) {
                case ENET_EVENT_TYPE_CONNECT: {
                    if (!server) {
                        break;
                    }
                    auto st = std::make_shared<Conn::State>();
                    st->peer = ev.peer;
                    st->loop = shared_from_this();
                    char text[64] = "";
                    enet_address_get_host_ip(&ev.peer->address, text, sizeof(text));
                    st->remote = text;
                    enet_peer_timeout(ev.peer, timeout_limit, timeout_min_ms, timeout_max_ms);
                    keep_unreliable_flowing(ev.peer);
                    by_peer[ev.peer] = st;
                    std::shared_ptr<Conn> conn(new Conn());
                    conn->state = st;
                    {
                        std::lock_guard lock{ mutex };
                        accepted.push_back(std::move(conn));
                    }
                    accept_cv.notify_all();
                    break;
                }
                case ENET_EVENT_TYPE_RECEIVE: {
                    const auto it = by_peer.find(ev.peer);
                    if (it != by_peer.end() && !it->second->closing.load()) {
                        Conn::State& st = *it->second;
                        const uint8_t* data = ev.packet->data;
                        size_t size = ev.packet->dataLength;
                        bool complete = true;
                        if (ev.channelID == 1) {
                            // [0 = more follows / 1 = last chunk][bytes]
                            complete = false;
                            if (size >= 1) {
                                st.assembling.insert(st.assembling.end(), data + 1, data + size);
                                complete = data[0] == 1;
                                if (complete) {
                                    std::lock_guard lock{ st.mutex };
                                    st.incoming.push_back(std::move(st.assembling));
                                    st.assembling.clear();
                                }
                            }
                        }
                        else {
                            std::lock_guard lock{ st.mutex };
                            st.incoming.emplace_back(data, data + size);
                        }
                        if (complete) {
                            st.cv.notify_all();
                        }
                    }
                    enet_packet_destroy(ev.packet);
                    break;
                }
                case ENET_EVENT_TYPE_DISCONNECT: {
                    const auto it = by_peer.find(ev.peer);
                    if (it != by_peer.end()) {
                        mark_closed(*it->second);
                        by_peer.erase(it);
                    }
                    break;
                }
                default:
                    break;
            }
        }

        void run() {
            std::vector<Job> local;
            auto last_pass = clock::now();
            auto last_stats = last_pass;
            long long max_gap_ms = 0;
            for (;;) {
                {
                    const auto pass = clock::now();
                    max_gap_ms = std::max<long long>(max_gap_ms, std::chrono::duration_cast<std::chrono::milliseconds>(pass - last_pass).count());
                    last_pass = pass;
                }
                {
                    std::lock_guard lock{ mutex };
                    while (!jobs.empty()) {
                        local.push_back(std::move(jobs.front()));
                        jobs.pop_front();
                    }
                }
                for (Job& job : local) {
                    Conn::State& st = *job.state;
                    if (st.peer == nullptr || st.gone) {
                        continue;
                    }
                    if (!job.close && job.channel == 1) {
                        st.bulk.push_back(std::move(job.data));
                    }
                    else if (!job.close && job.channel == 2) {
                        st.bulk_urgent.push_back(std::move(job.data));
                    }
                    else if (!job.close && job.channel == unreliable_channel) {
                        // (possess158) out at once and on its own: not resent, not held up by anything lost before it, and nothing
                        // waits for it (the session sends what it carries reliably too)
                        ENetPacket* packet = enet_packet_create(job.data.data(), job.data.size(), ENET_PACKET_FLAG_UNSEQUENCED);
                        if (packet != nullptr && enet_peer_send(st.peer, 0, packet) < 0) {
                            enet_packet_destroy(packet);
                        }
                    }
                    else if (!job.close) {
                        ENetPacket* packet = enet_packet_create(job.data.data(), job.data.size(), ENET_PACKET_FLAG_RELIABLE);
                        if (packet != nullptr && enet_peer_send(st.peer, (enet_uint8)job.channel, packet) < 0) {
                            enet_packet_destroy(packet);
                        }
                    }
                    else {
                        st.close_requested = true;
                    }
                }
                local.clear();
                for (auto& entry : by_peer) {
                    Conn::State& st = *entry.second;
                    ENetPeer* peer = entry.first;
                    if ((!st.bulk.empty() || !st.bulk_urgent.empty()) && !st.gone) {
                        if (peer->roundTripTime > 0 && peer->roundTripTime < st.best_rtt) {
                            st.best_rtt = peer->roundTripTime;
                        }
                        const auto now = clock::now();
                        if (now - st.cap_changed > std::chrono::milliseconds(100)) {
                            st.cap_changed = now;
                            const long long over = (long long)peer->roundTripTime - (long long)st.best_rtt;
                            if (over > 40) {
                                st.bulk_cap = std::max<size_t>(8 * 1024, st.bulk_cap * 3 / 4);
                            }
                            else if (over < 20) {
                                st.bulk_cap = std::min<size_t>(64 * 1024, st.bulk_cap + 4 * 1024);
                            }
                        }
                    }
                    while ((!st.bulk.empty() || !st.bulk_urgent.empty()) && !st.gone) {
                        size_t queued = 0, queued_bytes = 0;
                        for (ENetListIterator i = enet_list_begin(&peer->outgoingCommands); i != enet_list_end(&peer->outgoingCommands) && queued <= bulk_queue_limit; i = enet_list_next(i)) {
                            queued++;
                            queued_bytes += reinterpret_cast<ENetOutgoingCommand*>(i)->fragmentLength;
                        }
                        for (ENetListIterator i = enet_list_begin(&peer->outgoingSendReliableCommands); i != enet_list_end(&peer->outgoingSendReliableCommands) && queued <= bulk_queue_limit; i = enet_list_next(i)) {
                            queued++;
                            queued_bytes += reinterpret_cast<ENetOutgoingCommand*>(i)->fragmentLength;
                        }
                        if (queued > bulk_queue_limit || (size_t)peer->reliableDataInTransit + queued_bytes >= st.bulk_cap) {
                            break;
                        }
                        // a message once begun is finished first (the other end puts the chunks together in order); between messages
                        // the urgent ones go first
                        if (st.bulk_offset == 0) {
                            st.sending_urgent = !st.bulk_urgent.empty();
                        }
                        std::deque<std::vector<uint8_t>>& queue = st.sending_urgent ? st.bulk_urgent : st.bulk;
                        const std::vector<uint8_t>& message = queue.front();
                        const size_t n = std::min(std::min(bulk_chunk, std::max<size_t>(4096, st.bulk_cap / 2)), message.size() - st.bulk_offset);
                        const bool last = st.bulk_offset + n >= message.size();
                        std::vector<uint8_t> wire(n + 1);
                        wire[0] = last ? 1 : 0;
                        std::memcpy(wire.data() + 1, message.data() + st.bulk_offset, n);
                        ENetPacket* packet = enet_packet_create(wire.data(), wire.size(), ENET_PACKET_FLAG_RELIABLE);
                        if (packet != nullptr && enet_peer_send(peer, 1, packet) < 0) {
                            enet_packet_destroy(packet);
                        }
                        st.bulk_offset += n;
                        if (last) {
                            queue.pop_front();
                            st.bulk_offset = 0;
                        }
                    }
                    if (st.close_requested && st.bulk.empty() && st.bulk_urgent.empty() && st.close_began == clock::time_point{}) {
                        enet_peer_disconnect_later(peer, 0);   // (disconnect_later: what was queued goes out first; plain disconnect drops it)
                        st.close_began = clock::now();
                    }
                }
                ENetEvent ev;
                // ENet sends one datagram per peer each time it is serviced, and waits out the timeout for something to come in: with a
                // snapshot (a thousand datagrams) queued, that would be a datagram a millisecond. So while anything is queued the host is
                // serviced without waiting.
                bool pending = false;
                for (const auto& entry : by_peer) {
                    pending = pending || !entry.second->bulk.empty() || !entry.second->bulk_urgent.empty() || !enet_list_empty(&entry.first->outgoingCommands) || !enet_list_empty(&entry.first->outgoingSendReliableCommands);
                }
                int rc = enet_host_service(host, &ev, pending ? 0 : 1);
                if (pending) {
                    std::this_thread::yield();
                }
                while (rc > 0) {
                    handle(ev);
                    rc = enet_host_check_events(host, &ev);
                }
                const auto now = clock::now();
                const bool update_stats = now - last_stats >= std::chrono::seconds(1);
                if (update_stats) {
                    last_stats = now;
                }
                for (auto it = by_peer.begin(); it != by_peer.end();) {
                    Conn::State& st = *it->second;
                    st.rtt = (double)it->first->roundTripTime;
                    if (update_stats) {
                        const ENetPeer* p = it->first;
                        char text[320];
                        std::snprintf(text, sizeof(text), "udp: %u packets sent, %u lost (now %.1f%%), round trip %u ms (+-%u, best %u), %u bytes waiting for an ack, network thread's longest gap %lld ms, bulk allowance %u KB",
                            (unsigned)p->packetsSent, (unsigned)p->packetsLost, (double)p->packetLoss * 100.0 / (double)ENET_PEER_PACKET_LOSS_SCALE, (unsigned)p->roundTripTime,
                            (unsigned)p->roundTripTimeVariance, (unsigned)p->lowestRoundTripTime, (unsigned)p->reliableDataInTransit, max_gap_ms, (unsigned)(st.bulk_cap / 1024));
                        std::lock_guard lock{ st.stats_mutex };
                        st.stats_text = text;
                    }
                    if (st.close_began != clock::time_point{} && now - st.close_began > close_grace) {
                        enet_peer_reset(it->first);
                        mark_closed(st);
                        it = by_peer.erase(it);
                    }
                    else {
                        ++it;
                    }
                }
                if (update_stats) {
                    max_gap_ms = 0;
                }
                if (stopping.load() || (!server && by_peer.empty())) {
                    break;
                }
            }
            for (auto& entry : by_peer) {
                enet_peer_reset(entry.first);
                mark_closed(*entry.second);
            }
            by_peer.clear();   // (breaks the loop <-> connection references)
            enet_host_destroy(host);
            host = nullptr;
            {
                std::lock_guard lock{ mutex };
                finished = true;
                jobs.clear();
            }
            done_cv.notify_all();
            accept_cv.notify_all();
        }
    };

    bool startup() {
        static const bool ok = enet_initialize() == 0;
        return ok;
    }

    Conn::Conn() = default;

    Conn::~Conn() {
        if (state != nullptr) {
            close();
        }
    }

    bool Conn::send(const uint8_t* data, size_t size, int channel) {
        if (state == nullptr || state->is_closed.load() || state->closing.load() || size == 0 || size > max_packet || channel < 0 || channel > unreliable_channel ||
            (channel == unreliable_channel && size > unreliable_most)) {
            return false;
        }
        Loop::Job job;
        job.state = state;
        job.data.assign(data, data + size);
        job.channel = channel;
        state->loop->post(std::move(job));
        return true;
    }

    bool Conn::receive(std::vector<uint8_t>& message, int timeout_ms) {
        if (state == nullptr) {
            return false;
        }
        std::unique_lock lock{ state->mutex };
        const auto ready = [&]() { return !state->incoming.empty() || state->is_closed.load(); };
        if (timeout_ms < 0) {
            state->cv.wait(lock, ready);
        }
        else {
            state->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), ready);
        }
        if (state->incoming.empty()) {
            return false;
        }
        message = std::move(state->incoming.front());
        state->incoming.pop_front();
        return true;
    }

    bool Conn::closed() const {
        return state == nullptr || state->is_closed.load();
    }

    void Conn::close() {
        if (state == nullptr || state->closing.exchange(true)) {
            return;
        }
        Loop::Job job;
        job.state = state;
        job.close = true;
        state->loop->post(std::move(job));
        {
            std::lock_guard lock{ state->mutex };
            state->is_closed = true;   // (a reader waiting in receive() leaves)
        }
        state->cv.notify_all();
    }

    double Conn::rtt_ms() const {
        return state != nullptr ? state->rtt.load() : 0.0;
    }

    std::string Conn::stats() const {
        if (state == nullptr) {
            return std::string();
        }
        std::lock_guard lock{ state->stats_mutex };
        return state->stats_text;
    }

    std::string Conn::address() const {
        return state != nullptr ? state->remote : std::string();
    }

    std::shared_ptr<Server> Server::listen(uint16_t port, std::string* error) {
        if (!startup()) {
            if (error != nullptr) {
                *error = "Couldn't start the network.";
            }
            return nullptr;
        }
        ENetAddress address;
        address.host = ENET_HOST_ANY;
        address.port = port;
        ENetHost* host = enet_host_create(&address, 32, 2, 0, 0);
        if (host == nullptr) {
            if (error != nullptr) {
                *error = "Couldn't open UDP port " + std::to_string((unsigned)port) + ". Another copy of the game may be using it; try another port.";
            }
            return nullptr;
        }
        host->maximumPacketSize = max_packet;
        host->maximumWaitingData = max_packet;
        std::shared_ptr<Server> server(new Server());
        server->loop = std::make_shared<Loop>();
        server->loop->host = host;
        server->loop->server = true;
        server->loop->start();
        return server;
    }

    Server::~Server() {
        stop();
    }

    std::shared_ptr<Conn> Server::accept() {
        const auto loop_copy = loop;
        if (loop_copy == nullptr) {
            return nullptr;
        }
        std::unique_lock lock{ loop_copy->mutex };
        loop_copy->accept_cv.wait(lock, [&]() { return !loop_copy->accepted.empty() || loop_copy->stopping.load() || loop_copy->finished; });
        if (loop_copy->accepted.empty()) {
            return nullptr;
        }
        auto conn = std::move(loop_copy->accepted.front());
        loop_copy->accepted.pop_front();
        return conn;
    }

    void Server::stop() {
        if (loop == nullptr) {
            return;
        }
        loop->stopping = true;
        loop->accept_cv.notify_all();
        {
            std::unique_lock lock{ loop->mutex };
            loop->done_cv.wait_for(lock, std::chrono::seconds(3), [&]() { return loop->finished; });
        }
    }

    std::shared_ptr<Conn> connect(const std::string& host_name, uint16_t port, int timeout_ms, const std::function<bool()>& cancelled, int* failure) {
        const auto fail = [&](int code) -> std::shared_ptr<Conn> {
            if (failure != nullptr) {
                *failure = code;
            }
            return nullptr;
        };
        if (!startup()) {
            return fail(2);
        }
        ENetAddress address;
        if (enet_address_set_host(&address, host_name.c_str()) != 0) {
            return fail(1);
        }
        address.port = port;
        ENetHost* host = enet_host_create(nullptr, 1, 2, 0, 0);
        if (host == nullptr) {
            return fail(2);
        }
        host->maximumPacketSize = max_packet;
        host->maximumWaitingData = max_packet;
        const auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
        ENetPeer* connected = nullptr;
        while (connected == nullptr && clock::now() < deadline) {
            if (cancelled && cancelled()) {
                enet_host_destroy(host);
                return fail(3);
            }
            ENetPeer* peer = enet_host_connect(host, &address, 2, 0);
            if (peer == nullptr) {
                break;
            }
            // one try: ENet sends its connect request again and again for a few seconds
            const auto attempt_end = std::min(deadline, clock::now() + std::chrono::seconds(4));
            while (clock::now() < attempt_end && !(cancelled && cancelled())) {
                ENetEvent ev;
                if (enet_host_service(host, &ev, 50) > 0) {
                    if (ev.type == ENET_EVENT_TYPE_CONNECT) {
                        connected = ev.peer;
                        break;
                    }
                    if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
                        break;
                    }
                    if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                        enet_packet_destroy(ev.packet);
                    }
                }
            }
            if (connected == nullptr) {
                enet_peer_reset(peer);
            }
        }
        if (connected == nullptr) {
            const bool was_cancelled = cancelled && cancelled();
            enet_host_destroy(host);
            return fail(was_cancelled ? 3 : 2);
        }
        enet_peer_timeout(connected, timeout_limit, timeout_min_ms, timeout_max_ms);
        keep_unreliable_flowing(connected);
        auto loop = std::make_shared<Loop>();
        loop->host = host;
        loop->server = false;
        auto st = std::make_shared<Conn::State>();
        st->peer = connected;
        st->loop = loop;
        char text[64] = "";
        enet_address_get_host_ip(&connected->address, text, sizeof(text));
        st->remote = text;
        loop->by_peer[connected] = st;
        std::shared_ptr<Conn> conn(new Conn());
        conn->state = st;
        loop->start();
        if (failure != nullptr) {
            *failure = 0;
        }
        return conn;
    }
}
