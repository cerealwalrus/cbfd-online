// Netplay milestone 2: the online session (lockstep over TCP).
//
// Every machine runs the whole game; only the controllers travel. One machine hosts and is player 1 (port 1);
// the others join and are players 2..4. Each game frame the host collects every player's input for that frame,
// puts them together in one FRAME message and sends it to everyone (itself included, so every machine feeds the
// game exactly the same bytes). A machine that hasn't got the FRAME message of the frame it's about to start
// waits for it, so nobody runs ahead of anybody.
//
// Input delay hides the network's latency: the input read at game frame f is the one used at frame f + delay
// (3 frames by default, 100 ms at the game's 30 frames a second), so a FRAME message has that long to arrive.
//
// The host also tells the others the game's RNG state every 30 frames (CHECK). A machine that sees a different
// one has drifted (netplay.cpp tells why that can happen); resync.cpp's snapshot, taken by the host, puts it
// right. This file only notices and asks (on_desync below).
//
// Environment (until there's a lobby screen):
//   CONKER_NET_HOST=<port>        host a game on that TCP port
//   CONKER_NET_PLAYERS=<2..4>     how many players the host waits for before the game starts (default 2)
//   CONKER_NET_JOIN=<host>:<port> join a game
//   CONKER_NET_DELAY=<frames>     input delay (default 3)
//   CONKER_NET_TEST_DESYNC=<frame>  (a client) upsets its own RNG at that frame, to see the resync work
//   CONKER_NET_INPUT=<keyboard|gamepad|gamepad2>  this machine's player uses only that (two windows on one PC);
//                                 gamepad2 is the second controller. Unset: the keyboard and the first controller
//   CONKER_NET_LOG=<file>         a log of the session
//
// The session ends if a machine can't be reached for a while; the others go on without it (its controller
// becomes unplugged), and a machine whose host is gone goes on offline with its own controller.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <SDL.h>

#include "ultramodern/ultramodern.hpp"

#include "conker.hpp"

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#endif

namespace {
#ifdef _WIN32
    using socklen_t = int;
    using socket_t = SOCKET;
    const socket_t invalid_socket = INVALID_SOCKET;
    void close_socket(socket_t s) { closesocket(s); }
    void shutdown_socket(socket_t s) { shutdown(s, SD_BOTH); }
#else
    using socket_t = int;
    const socket_t invalid_socket = -1;
    void close_socket(socket_t s) { close(s); }
    void shutdown_socket(socket_t s) { shutdown(s, SHUT_RDWR); }
#endif
    using clock = std::chrono::steady_clock;

    constexpr int max_players = 4;
    constexpr uint32_t magic = 0x544E4243;  // "CBNT"
    constexpr uint32_t protocol_version = 2;
    constexpr uint32_t check_every = 30;
    constexpr uint32_t history = 1200;       // frames of inputs kept (for resyncs)
    constexpr uint32_t max_message = 64u << 20;
    constexpr auto host_waits_for_player = std::chrono::seconds(5);
    constexpr auto client_waits_for_host = std::chrono::seconds(10);

    enum Msg : uint8_t { Hello = 1, Welcome = 2, Start = 3, Input = 4, Frame = 5, Check = 6, ResyncRequest = 7, Snapshot = 8, FrameRngMsg = 9, DiagRequest = 10, DiagData = 11 };

    struct PlayerInput {
        uint8_t plugged = 0;
        uint16_t buttons = 0;
        float x = 0.0f;
        float y = 0.0f;
    };
    struct FrameInputs {
        std::array<PlayerInput, max_players> p{};
    };

    // --- Messages: u32 length (of what follows), u8 type, payload ---

    struct Writer {
        std::vector<uint8_t> bytes;
        explicit Writer(Msg type) { bytes.resize(4); bytes.push_back(type); }
        template <typename T> Writer& put(const T& v) {
            const uint8_t* b = reinterpret_cast<const uint8_t*>(&v);
            bytes.insert(bytes.end(), b, b + sizeof(T));
            return *this;
        }
        Writer& put_bytes(const void* data, size_t size) {
            const uint8_t* b = static_cast<const uint8_t*>(data);
            bytes.insert(bytes.end(), b, b + size);
            return *this;
        }
        Writer& put_input(const PlayerInput& in) { return put(in.plugged).put(in.buttons).put(in.x).put(in.y); }
        const std::vector<uint8_t>& finish() {
            const uint32_t length = (uint32_t)(bytes.size() - 4);
            std::memcpy(bytes.data(), &length, 4);
            return bytes;
        }
    };

    struct Reader {
        const uint8_t* data;
        size_t size;
        size_t at = 0;
        bool bad = false;
        template <typename T> T get() {
            T v{};
            if (at + sizeof(T) > size) { bad = true; return v; }
            std::memcpy(&v, data + at, sizeof(T));
            at += sizeof(T);
            return v;
        }
        PlayerInput get_input() {
            PlayerInput in;
            in.plugged = get<uint8_t>();
            in.buttons = get<uint16_t>();
            in.x = get<float>();
            in.y = get<float>();
            return in;
        }
    };

    bool send_all(socket_t s, const uint8_t* data, size_t size) {
        while (size > 0) {
            const int n = (int)send(s, reinterpret_cast<const char*>(data), (int)std::min<size_t>(size, 1u << 20), 0);
            if (n <= 0) {
                return false;
            }
            data += n;
            size -= (size_t)n;
        }
        return true;
    }

    bool recv_all(socket_t s, uint8_t* data, size_t size) {
        while (size > 0) {
            const int n = (int)recv(s, reinterpret_cast<char*>(data), (int)std::min<size_t>(size, 1u << 20), 0);
            if (n <= 0) {
                return false;
            }
            data += n;
            size -= (size_t)n;
        }
        return true;
    }

    // Reads one message. False when the connection is gone or what came isn't a message.
    bool read_message(socket_t s, uint8_t& type, std::vector<uint8_t>& payload) {
        uint32_t length = 0;
        if (!recv_all(s, reinterpret_cast<uint8_t*>(&length), 4) || length < 1 || length > max_message) {
            return false;
        }
        std::vector<uint8_t> body(length);
        if (!recv_all(s, body.data(), length)) {
            return false;
        }
        type = body[0];
        payload.assign(body.begin() + 1, body.end());
        return true;
    }

    struct Peer {
        socket_t sock = invalid_socket;
        int slot = -1;
        std::mutex send_mutex;
        std::atomic<bool> alive{ false };
        std::thread reader;

        bool send_message(const std::vector<uint8_t>& bytes) {
            std::lock_guard lock{ send_mutex };
            if (!alive || !send_all(sock, bytes.data(), bytes.size())) {
                alive = false;
                return false;
            }
            return true;
        }
        void drop() {
            alive = false;
            if (sock != invalid_socket) {
                shutdown_socket(sock);
            }
        }
    };

    // --- The session's state ---

    std::mutex mtx;
    std::condition_variable cv;

    bool enabled = false;          // CONKER_NET_HOST or _JOIN was given and the session hasn't ended
    bool is_host = false;
    bool started = false;          // every player has joined and the game may begin
    bool failed = false;           // the session is over (this machine goes on offline)
    int my_slot = 0;
    int players_expected = 2;
    uint32_t delay = 3;
    uint8_t* ram = nullptr;
    bool (*sampler)(uint16_t*, float*, float*) = nullptr;

    // Frame numbering: frames_started is the number of the next frame to start.
    uint32_t frames_started = 0;
    uint32_t current_frame = 0;
    bool in_frame = false;
    int last_port = 99;
    FrameInputs current;
    std::map<uint32_t, FrameInputs> ready;       // frames whose inputs everyone agrees on
    // What the game's memory looked like at a check frame: the RNG and a hash of each 16 KB block.
    struct CheckInfo {
        uint64_t rng = 0;
        std::vector<uint32_t> blocks;
        std::vector<std::pair<uint32_t, uint64_t>> frame_rng;   // (the host's) the RNG at the start of each recent frame
        std::vector<uint32_t> objects;   // a hash of each of the first game objects (the players')
    };

    // Where the game's RNG was at the start of each recent frame, and who asked it for numbers during the frame
    // (the address each call came from; the top bit is set for calls made while the frame was being drawn):
    // two machines that drift apart show in which frame, and by which code, they used it differently.
    struct FrameRng {
        uint32_t frame = 0xFFFFFFFFu;
        uint64_t start_rng = 0;
        uint32_t calls = 0;
        uint32_t callers[64] = {};
        uint32_t states[64] = {};   // a digest of the RNG's state at each call
    };
    constexpr uint32_t rng_ring_size = 256;
    FrameRng rng_ring[rng_ring_size];
    std::mutex rng_mutex;
    FrameRng* rng_current = nullptr;
    std::thread::id game_thread;      // the thread that reads the controllers: the game's logic
    int diag_episodes = 0;            // drifts written up in the log so far
    uint32_t last_diverged_frame = 0; // the frame before the first one the RNGs differed at, for the resync request
    std::map<uint32_t, CheckInfo> pending_checks;  // the host's, for frames this machine hasn't started yet
    std::map<uint32_t, PlayerInput> sent_inputs;    // (client) what this machine sent for each recent frame, to send again after a resync
    std::map<uint32_t, CheckInfo> mine_history;    // this machine's at the start of its recent check frames
    std::vector<uint8_t> noisy_blocks;             // blocks that differed while the RNGs agreed (audio, buffers, ...)

    // Diagnostic: raw copies of the game's memory at the start of each recent frame (both machines), the host's
    // RNG for every frame, and, when the client's RNG first differs, a word-by-word comparison of the two.
    constexpr uint32_t raw_ring_size = 20;
    struct RawCopy { uint32_t frame = 0xFFFFFFFFu; std::vector<uint8_t> data; };
    RawCopy raw_ring[raw_ring_size];
    std::map<uint32_t, uint32_t> host_frame_obj;    // client: the host's pointer-free object hash at the start of frames
    uint32_t obj_ring[raw_ring_size][3];            // {frame, objects hash, camera records hash}
    std::map<uint32_t, uint32_t> host_frame_cam;
    uint32_t cam_bad_run = 0;
    uint32_t last_change[12] = {};
    uint32_t last_change_buttons = 0;
    int change_lines = 0;
    uint32_t obj_bad_run = 0;                       // client: frames in a row where the objects disagreed
    uint32_t last_obj_frame_checked = 0xFFFFFFFFu;
    uint32_t last_obj_resync = 0;            // this machine's: {frame, hash}
    std::map<uint32_t, uint64_t> host_frame_rng;    // client: what the host's RNG was at the start of frames
    int diag_dumps = 0;
    int object_mismatches = 0, object_diags = 0;
    uint32_t last_object_diag = 0;
    uint32_t last_diag_frame = 0;
    struct DiagPending { uint32_t frame = 0; std::vector<RawCopy> mine; bool active = false; };
    DiagPending diag_pending;

    // The host's.
    std::vector<std::unique_ptr<Peer>> peers;     // host: its clients; client: the host (one)
    std::array<bool, max_players> connected{};
    std::map<uint32_t, std::array<std::pair<bool, PlayerInput>, max_players>> inbox;
    uint32_t next_broadcast = 0;
    socket_t listener = invalid_socket;

    // Counters.
    uint64_t desyncs = 0;
    uint64_t stalls = 0;
    double waited_ms = 0.0;
    uint64_t waited_frames = 0;
    std::atomic<uint32_t> desync_requested_frame{ 0xFFFFFFFFu };

    int filter = 0;
    uint32_t test_desync_frame = 0xFFFFFFFFu;   // CONKER_NET_TEST_DESYNC: a client upsets its own RNG at that frame

    FILE* log_file = nullptr;
    void log_line(const char* format, ...) {
        if (log_file == nullptr) {
            return;
        }
        static std::mutex log_mutex;
        std::lock_guard lock{ log_mutex };
        va_list args;
        va_start(args, format);
        std::vfprintf(log_file, format, args);
        va_end(args);
        std::fputc('\n', log_file);
        std::fflush(log_file);
    }

    // SDL_getenv, as netplay.cpp does, copied at once (it may hand back one shared buffer).
    std::string get_env(const char* name) {
        const char* value = SDL_getenv(name);
        return std::string(value != nullptr ? value : "");
    }

    uint32_t hash_block(const uint8_t* p, size_t n) {
        uint64_t h = 0x9E3779B97F4A7C15ull;
        for (size_t i = 0; i + 8 <= n; i += 8) {
            uint64_t v;
            std::memcpy(&v, p + i, 8);
            h = (h ^ v) * 0x100000001B3ull;
            h ^= h >> 29;
        }
        return (uint32_t)(h ^ (h >> 32));
    }

    // Like hash_block, but words that look like pointers into game memory are skipped: heap addresses
    // legitimately differ between machines and say nothing about the game state.
    uint32_t hash_block_nopointers(const uint8_t* p, size_t n) {
        uint64_t h = 0x9E3779B97F4A7C15ull;
        for (size_t i = 0; i + 4 <= n; i += 4) {
            uint32_t v;
            std::memcpy(&v, p + i, 4);
            if (v >= 0x80000000u && v < 0x80800000u) {
                v = 0;
            }
            h = (h ^ v) * 0x100000001B3ull;
            h ^= h >> 29;
        }
        return (uint32_t)(h ^ (h >> 32));
    }

    // The first `count` game objects (0x32C bytes each from 0x800CC2D0), without heap pointers and without the words
    // seen to differ between the machines for no reason that matters (flag bits, a counter): offsets 0x0C, 0x6C, 0x134.
    uint32_t objects_hash(int first, int count) {
        uint64_t h = 0x9E3779B97F4A7C15ull;
        for (int i = first; i < first + count; i++) {
            const uint8_t* p = ram + ((0x800CC2D0u + (uint32_t)i * 0x32Cu) & 0x7FFFFFu);
            for (uint32_t off = 0; off < 0x328; off += 4) {
                uint32_t v;
                std::memcpy(&v, p + off, 4);
                if ((v >= 0x80000000u && v < 0x80800000u) || off == 0x0C || off == 0x6C || off == 0x134) {
                    v = 0;
                }
                h = (h ^ v) * 0x100000001B3ull;
                h ^= h >> 29;
            }
        }
        return (uint32_t)(h ^ (h >> 32));
    }

    CheckInfo take_check_info();

    uint64_t read_rng() {
        if (ram == nullptr) {
            return 0;
        }
        // The game's memory is words stored host-endian; the RNG state is two of them (netplay.cpp).
        const uint32_t base = 0x800885B0u & 0x7FFFFFu;
        uint32_t hi, lo;
        std::memcpy(&hi, ram + base, 4);
        std::memcpy(&lo, ram + base + 4, 4);
        return ((uint64_t)hi << 32) | lo;
    }


    // --- A small LZ4-style codec for snapshots (8 MB of game memory, mostly zeros and repeats) ---

    void lz_emit(std::vector<uint8_t>& out, const uint8_t* lit, size_t lit_len, size_t offset, size_t mlen) {
        const size_t ml = mlen != 0 ? mlen - 4 : 0;
        out.push_back((uint8_t)((std::min<size_t>(lit_len, 15) << 4) | (mlen != 0 ? std::min<size_t>(ml, 15) : 0)));
        if (lit_len >= 15) {
            size_t extra = lit_len - 15;
            while (extra >= 255) { out.push_back(255); extra -= 255; }
            out.push_back((uint8_t)extra);
        }
        out.insert(out.end(), lit, lit + lit_len);
        if (mlen != 0) {
            out.push_back((uint8_t)(offset & 0xFF));
            out.push_back((uint8_t)(offset >> 8));
            if (ml >= 15) {
                size_t extra = ml - 15;
                while (extra >= 255) { out.push_back(255); extra -= 255; }
                out.push_back((uint8_t)extra);
            }
        }
    }

    std::vector<uint8_t> lz_compress(const uint8_t* src, size_t n) {
        std::vector<uint8_t> out;
        out.reserve(n / 3 + 64);
        std::vector<int64_t> table(1u << 16, -1);
        size_t ip = 0, anchor = 0;
        if (n >= 13) {
            const size_t limit = n - 12;
            while (ip < limit) {
                uint32_t seq;
                std::memcpy(&seq, src + ip, 4);
                const uint32_t h = (seq * 2654435761u) >> 16;
                const int64_t ref = table[h];
                table[h] = (int64_t)ip;
                if (ref >= 0 && ip - (size_t)ref <= 65535 && std::memcmp(src + ref, src + ip, 4) == 0) {
                    size_t mlen = 4;
                    const size_t maxlen = n - 5 - ip;
                    while (mlen < maxlen && src[(size_t)ref + mlen] == src[ip + mlen]) {
                        mlen++;
                    }
                    lz_emit(out, src + anchor, ip - anchor, ip - (size_t)ref, mlen);
                    ip += mlen;
                    anchor = ip;
                }
                else {
                    ip++;
                }
            }
        }
        lz_emit(out, src + anchor, n - anchor, 0, 0);
        return out;
    }

    bool lz_decompress(const uint8_t* in, size_t in_size, uint8_t* out, size_t out_size) {
        size_t ip = 0, op = 0;
        while (ip < in_size) {
            const uint8_t token = in[ip++];
            size_t lit = token >> 4;
            if (lit == 15) {
                uint8_t b;
                do {
                    if (ip >= in_size) return false;
                    b = in[ip++];
                    lit += b;
                } while (b == 255);
            }
            if (ip + lit > in_size || op + lit > out_size) return false;
            std::memcpy(out + op, in + ip, lit);
            ip += lit;
            op += lit;
            if (ip >= in_size) break;
            if (ip + 2 > in_size) return false;
            const size_t offset = in[ip] | ((size_t)in[ip + 1] << 8);
            ip += 2;
            size_t ml = token & 15;
            if (ml == 15) {
                uint8_t b;
                do {
                    if (ip >= in_size) return false;
                    b = in[ip++];
                    ml += b;
                } while (b == 255);
            }
            ml += 4;
            if (offset == 0 || offset > op || op + ml > out_size) return false;
            for (size_t i = 0; i < ml; i++) {
                out[op + i] = out[op + i - offset];
            }
            op += ml;
        }
        return op == out_size;
    }

    // --- Resync state ---

    uint32_t snapshot_wanted_mask = 0;       // host: who asked
    bool capture_in_flight = false;          // host
    clock::time_point last_snapshot_sent{};  // host
    bool resync_in_flight = false;           // client
    clock::time_point resync_asked_at{};     // client
    constexpr size_t snapshot_max_size = 48u << 20;   // a state is the game's 8 MB and the runtime's registers and so on
    constexpr uint32_t block_size = 16384;
    constexpr uint32_t ram_bytes = 8u << 20;

    void on_resync_requested(int slot, uint32_t frame, uint32_t diverged);
    void on_snapshot_message(const std::vector<uint8_t>& payload);
    void on_check(uint32_t f, CheckInfo&& theirs);
    void compare_check(uint32_t f, const CheckInfo& mine, const CheckInfo& theirs);
    void request_resync(uint32_t f);
    void diag_check_frame(uint32_t f);
    void begin_diag(uint32_t f, const char* why);
    void on_diag_request(uint32_t f);
    void on_diag_data(const std::vector<uint8_t>& payload);

    // Mutex held or not: who called the RNG in a frame (a 'd' before the address: while the frame was drawn).
    void log_callers(const char* who, uint32_t frame) {
        std::lock_guard rng_lock{ rng_mutex };
        const FrameRng& fr = rng_ring[frame % rng_ring_size];
        if (fr.frame != frame) {
            log_line("[net]   %s: no record of the RNG calls in frame %u", who, frame);
            return;
        }
        std::string list;
        char text[24];
        for (uint32_t i = 0; i < fr.calls && i < 64; i++) {
            std::snprintf(text, sizeof(text), " %s%08X:%04X", (fr.callers[i] & 0x80000000u) ? "d" : ((fr.callers[i] & 0x40000000u) ? "T" : ""), (unsigned)(fr.callers[i] & 0x3FFFFFFFu),
                (unsigned)(fr.states[i] & 0xFFFFu));
            list += text;
        }
        log_line("[net]   %s: RNG calls in frame %u (%u):%s", who, frame, (unsigned)fr.calls, list.c_str());
    }

    CheckInfo take_check_info() {
        CheckInfo info;
        info.rng = read_rng();
        if (ram != nullptr) {
            for (int i = 0; i < 6; i++) {
                info.objects.push_back(objects_hash(i, 1));
            }
        }
        if (ram != nullptr) {
            info.blocks.resize(ram_bytes / block_size);
            for (size_t i = 0; i < info.blocks.size(); i++) {
                info.blocks[i] = hash_block(ram + i * block_size, block_size);
            }
        }
        return info;
    }

    // --- Host: put the frames together ---

    // Mutex held. Sends every frame that all connected players' inputs are in for.
    void try_broadcast() {
        while (true) {
            const uint32_t f = next_broadcast;
            auto it = inbox.find(f);
            FrameInputs fi;
            bool complete = true;
            for (int s = 0; s < max_players && complete; s++) {
                if (!connected[s]) {
                    continue;
                }
                if (it == inbox.end() || !it->second[s].first) {
                    complete = false;
                    break;
                }
                fi.p[s] = it->second[s].second;
                fi.p[s].plugged = 1;
            }
            if (!complete) {
                return;
            }
            ready[f] = fi;
            Writer w(Frame);
            w.put(f);
            for (int s = 0; s < max_players; s++) {
                w.put_input(fi.p[s]);
            }
            const auto& bytes = w.finish();
            for (auto& peer : peers) {
                if (peer->alive) {
                    peer->send_message(bytes);
                }
            }
            inbox.erase(f);
            next_broadcast++;
            if (f >= history) {
                ready.erase(ready.begin(), ready.lower_bound(f - history));
            }
            cv.notify_all();
        }
    }

    // Mutex held.
    void host_store_input(int slot, uint32_t frame, const PlayerInput& in) {
        if (slot < 0 || slot >= max_players || !connected[slot] || frame < next_broadcast) {
            return;
        }
        auto& entry = inbox[frame][slot];
        if (entry.first) {
            return;   // the first one counts (a resynced machine sends some twice)
        }
        entry = { true, in };
        try_broadcast();
    }

    void host_reader(Peer* peer) {
        uint8_t type;
        std::vector<uint8_t> payload;
        while (peer->alive && read_message(peer->sock, type, payload)) {
            Reader r{ payload.data(), payload.size() };
            std::lock_guard lock{ mtx };
            if (type == Input) {
                const uint32_t frame = r.get<uint32_t>();
                PlayerInput in = r.get_input();
                if (!r.bad) {
                    host_store_input(peer->slot, frame, in);
                }
            }
            else if (type == DiagRequest) {
                const uint32_t frame = r.get<uint32_t>();
                if (!r.bad) {
                    on_diag_request(frame);
                }
            }
            else if (type == ResyncRequest) {
                const uint32_t frame = r.get<uint32_t>();
                const uint32_t diverged = r.get<uint32_t>();
                if (!r.bad) {
                    on_resync_requested(peer->slot, frame, diverged);
                }
            }
        }
        std::lock_guard lock{ mtx };
        if (peer->alive || connected[peer->slot]) {
            log_line("[net] player %d left", peer->slot + 1);
        }
        peer->alive = false;
        connected[peer->slot] = false;
        try_broadcast();
        cv.notify_all();
    }

    void host_accept_loop() {
        int joined = 0;
        while (joined < players_expected - 1) {
            sockaddr_in addr{};
            socklen_t len = sizeof(addr);
            socket_t s = accept(listener, reinterpret_cast<sockaddr*>(&addr), &len);
            if (s == invalid_socket) {
                std::lock_guard lock{ mtx };
                failed = true;
                cv.notify_all();
                return;
            }
            int one = 1;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
            uint8_t type;
            std::vector<uint8_t> payload;
            if (!read_message(s, type, payload) || type != Hello) {
                close_socket(s);
                continue;
            }
            Reader r{ payload.data(), payload.size() };
            const uint32_t m = r.get<uint32_t>();
            const uint32_t v = r.get<uint32_t>();
            if (r.bad || m != magic || v != protocol_version) {
                log_line("[net] a connection with the wrong version was refused");
                close_socket(s);
                continue;
            }
            auto peer = std::make_unique<Peer>();
            peer->sock = s;
            peer->slot = ++joined;
            peer->alive = true;
            Writer w(Welcome);
            w.put((uint8_t)peer->slot).put((uint8_t)players_expected).put(delay);
            peer->send_message(w.finish());
            log_line("[net] player %d joined", peer->slot + 1);
            Peer* raw = peer.get();
            {
                std::lock_guard lock{ mtx };
                connected[peer->slot] = true;
                peers.push_back(std::move(peer));
            }
            raw->reader = std::thread(host_reader, raw);
            raw->reader.detach();
        }
        // Everyone is here: the game begins. The first `delay` frames have nobody's input yet, so they are
        // neutral for all.
        std::lock_guard lock{ mtx };
        Writer s(Start);
        s.put(delay);
        for (auto& peer : peers) {
            peer->send_message(s.finish());
        }
        started = true;
        for (uint32_t f = 0; f < delay; f++) {
            for (int slot = 0; slot < max_players; slot++) {
                if (connected[slot]) {
                    inbox[f][slot] = { true, PlayerInput{} };
                }
            }
        }
        try_broadcast();
        log_line("[net] all %d players are here, starting", players_expected);
        cv.notify_all();
    }

    // --- Client ---

    void client_reader(Peer* peer) {
        uint8_t type;
        std::vector<uint8_t> payload;
        while (peer->alive && read_message(peer->sock, type, payload)) {
            Reader r{ payload.data(), payload.size() };
            std::lock_guard lock{ mtx };
            switch (type) {
                case Start:
                    delay = r.get<uint32_t>();
                    started = true;
                    log_line("[net] the game starts");
                    break;
                case Frame: {
                    const uint32_t f = r.get<uint32_t>();
                    FrameInputs fi;
                    for (int s = 0; s < max_players; s++) {
                        fi.p[s] = r.get_input();
                    }
                    if (!r.bad) {
                        ready[f] = fi;
                        if (f >= history) {
                            ready.erase(ready.begin(), ready.lower_bound(f - history));
                        }
                    }
                    break;
                }
                case Check: {
                    const uint32_t f = r.get<uint32_t>();
                    CheckInfo theirs;
                    theirs.rng = r.get<uint64_t>();
                    const uint16_t count = r.get<uint16_t>();
                    for (uint16_t i = 0; i < count && !r.bad; i++) {
                        theirs.blocks.push_back(r.get<uint32_t>());
                    }
                    const uint16_t frames_count = r.get<uint16_t>();
                    for (uint16_t i = 0; i < frames_count && !r.bad; i++) {
                        const uint32_t g = r.get<uint32_t>();
                        const uint64_t v = r.get<uint64_t>();
                        theirs.frame_rng.emplace_back(g, v);
                    }
                    const uint16_t object_count = r.get<uint16_t>();
                    for (uint16_t i = 0; i < object_count && !r.bad; i++) {
                        theirs.objects.push_back(r.get<uint32_t>());
                    }
                    if (!r.bad) {
                        on_check(f, std::move(theirs));
                    }
                    break;
                }
                case Snapshot:
                    on_snapshot_message(payload);
                    break;
                case FrameRngMsg: {
                    const uint32_t f = r.get<uint32_t>();
                    const uint64_t v = r.get<uint64_t>();
                    const uint32_t oh = r.get<uint32_t>();
                    const uint32_t ch = r.get<uint32_t>();
                    if (!r.bad) {
                        host_frame_cam[f] = ch;
                        if (host_frame_cam.size() > 128) {
                            host_frame_cam.erase(host_frame_cam.begin());
                        }
                        host_frame_rng[f] = v;
                        host_frame_obj[f] = oh;
                        if (host_frame_obj.size() > 128) {
                            host_frame_obj.erase(host_frame_obj.begin());
                        }
                        if (host_frame_rng.size() > 128) {
                            host_frame_rng.erase(host_frame_rng.begin());
                        }
                        diag_check_frame(f);
                    }
                    break;
                }
                case DiagData:
                    on_diag_data(payload);
                    break;
                default:
                    break;
            }
            cv.notify_all();
        }
        std::lock_guard lock{ mtx };
        if (!failed && peer->alive) {
            log_line("[net] the host is gone");
            conker::notices::show("The host left, playing on offline");
        }
        peer->alive = false;
        failed = true;
        cv.notify_all();
    }

    // Mutex held: the host says how its game looked at the start of frame f. This machine may not have got to
    // frame f yet (the host is often a little ahead), then the word is kept until it does.
    void on_check(uint32_t f, CheckInfo&& theirs) {
        const auto found = mine_history.find(f);
        if (found != mine_history.end()) {
            compare_check(f, found->second, theirs);
            return;
        }
        if (f >= frames_started) {
            pending_checks[f] = std::move(theirs);
            if (pending_checks.size() > 64) {
                pending_checks.erase(pending_checks.begin());
            }
        }
    }

    // Mutex held. With the RNGs the same, the blocks that differ are only noise (the sound's buffers, where the
    // heap put things): they're remembered as such. When the RNGs differ, the blocks that differ and aren't noise
    // are what drifted, and are written to the log: where to look for the cause.
    void compare_check(uint32_t f, const CheckInfo& mine, const CheckInfo& theirs) {
        const size_t n = std::min(mine.blocks.size(), theirs.blocks.size());
        if (noisy_blocks.size() < n) {
            noisy_blocks.resize(n, 0);
        }
        if (mine.rng == theirs.rng) {
            for (size_t i = 0; i < n; i++) {
                if (mine.blocks[i] != theirs.blocks[i]) {
                    noisy_blocks[i] = 1;
                }
            }
            // The RNGs agree, but do the players' objects? (They are what the player sees move.)
            if (!mine.objects.empty() && mine.objects.size() == theirs.objects.size()) {
                std::string which;
                for (size_t i = 0; i < mine.objects.size(); i++) {
                    if (mine.objects[i] != theirs.objects[i]) {
                        which += " " + std::to_string(i);
                    }
                }
                if (!which.empty()) {
                    object_mismatches++;
                    if (object_mismatches <= 40) {
                        log_line("[net] frame %u: the RNGs agree but these objects differ:%s", f, which.c_str());
                    }
                    if (object_diags < 4 && f > last_object_diag + 150) {
                        object_diags++;
                        last_object_diag = f;
                        begin_diag(f, "objects differ, RNG the same");
                    }
                }
            }
            return;
        }
        desyncs++;
        log_line("[net] frame %u: this machine's RNG is %016llx, the host's %016llx", f,
            (unsigned long long)mine.rng, (unsigned long long)theirs.rng);
        if (diag_episodes < 14) {
            // The first frame in the host's list where this machine's RNG at the start of the frame is different.
            bool found = false;
            uint32_t agreed = 0;
            bool any_agreed = false;
            std::lock_guard rng_lock{ rng_mutex };
            for (const auto& entry : theirs.frame_rng) {
                const FrameRng& fr = rng_ring[entry.first % rng_ring_size];
                if (fr.frame != entry.first) {
                    continue;
                }
                if (fr.start_rng == entry.second) {
                    agreed = entry.first;
                    any_agreed = true;
                    continue;
                }
                diag_episodes++;
                found = true;
                log_line("[net]   the RNGs first differ at the start of frame %u (they agreed at frame %u%s): this machine's %016llx, the host's %016llx",
                    entry.first, agreed, any_agreed ? "" : " - or none seen", (unsigned long long)fr.start_rng, (unsigned long long)entry.second);
                const uint32_t before = entry.first > 0 ? entry.first - 1 : 0;
                last_diverged_frame = before;
                break;
            }
            if (!found) {
                log_line("[net]   no frame in the host's list differs (the drift is older than %d frames or not in the list)", 31);
            }
        }
        if (last_diverged_frame != 0 && diag_episodes <= 14) {
            log_callers("client", last_diverged_frame);
        }
        if (desyncs <= 20) {
            std::string list;
            int shown = 0, noise = 0;
            for (size_t i = 0; i < n; i++) {
                if (mine.blocks[i] == theirs.blocks[i]) {
                    continue;
                }
                if (noisy_blocks[i]) {
                    noise++;
                }
                else if (shown++ < 60) {
                    char text[32];
                    std::snprintf(text, sizeof(text), " %08X", (unsigned)(0x80000000u + i * block_size));
                    list += text;
                }
            }
            log_line("[net]   blocks that differ and aren't usually noisy (%d):%s  (and %d usually noisy)", shown, list.c_str(), noise);
        }
        request_resync(f);
    }

    // Mutex held.
    void request_resync(uint32_t f) {
        static const bool suppressed = !get_env("CONKER_NET_NORESYNC").empty();
        if (suppressed) {
            static int shown = 0;
            if (shown++ < 20) {
                log_line("[net] frame %u: a resync would be asked for here (CONKER_NET_NORESYNC is set, so it is not)", f);
            }
            return;
        }
        if (resync_in_flight && clock::now() - resync_asked_at < std::chrono::seconds(3)) {
            return;
        }
        if (!peers.empty() && peers[0]->alive) {
            resync_in_flight = true;
            resync_asked_at = clock::now();
            Writer w(ResyncRequest);
            w.put(f).put(last_diverged_frame);
            last_diverged_frame = 0;
            peers[0]->send_message(w.finish());
        }
    }

    // --- Resync (the snapshot itself is conker::netsession's resync part, below) ---

    bool connect_to_host(const std::string& host, const std::string& port) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* result = nullptr;
        if (getaddrinfo(host.c_str(), port.c_str(), &hints, &result) != 0) {
            log_line("[net] couldn't look up %s", host.c_str());
            return false;
        }
        socket_t s = invalid_socket;
        const auto deadline = clock::now() + std::chrono::seconds(60);
        while (clock::now() < deadline && s == invalid_socket) {
            for (addrinfo* a = result; a != nullptr; a = a->ai_next) {
                socket_t c = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
                if (c == invalid_socket) {
                    continue;
                }
                if (connect(c, a->ai_addr, (int)a->ai_addrlen) == 0) {
                    s = c;
                    break;
                }
                close_socket(c);
            }
            if (s == invalid_socket) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
        freeaddrinfo(result);
        if (s == invalid_socket) {
            log_line("[net] couldn't reach %s:%s", host.c_str(), port.c_str());
            return false;
        }
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
        Writer hello(Hello);
        hello.put(magic).put(protocol_version);
        if (!send_all(s, hello.finish().data(), hello.bytes.size())) {
            close_socket(s);
            return false;
        }
        uint8_t type;
        std::vector<uint8_t> payload;
        if (!read_message(s, type, payload) || type != Welcome) {
            close_socket(s);
            log_line("[net] the host didn't answer");
            return false;
        }
        Reader r{ payload.data(), payload.size() };
        my_slot = r.get<uint8_t>();
        players_expected = r.get<uint8_t>();
        delay = r.get<uint32_t>();
        if (r.bad || my_slot < 1 || my_slot >= max_players) {
            close_socket(s);
            return false;
        }
        auto peer = std::make_unique<Peer>();
        peer->sock = s;
        peer->slot = 0;
        peer->alive = true;
        Peer* raw = peer.get();
        peers.push_back(std::move(peer));
        raw->reader = std::thread(client_reader, raw);
        raw->reader.detach();
        log_line("[net] joined as player %d of %d, delay %u", my_slot + 1, players_expected, delay);
        return true;
    }

    // --- Bringing the session up ---

    bool net_startup() {
#ifdef _WIN32
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
#else
        return true;
#endif
    }

    bool start_host(uint16_t port) {
        listener = socket(AF_INET, SOCK_STREAM, 0);
        if (listener == invalid_socket) {
            return false;
        }
        int one = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);
        if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listener, 8) != 0) {
            log_line("[net] couldn't listen on port %u", (unsigned)port);
            close_socket(listener);
            listener = invalid_socket;
            return false;
        }
        connected[0] = true;
        std::thread(host_accept_loop).detach();
        log_line("[net] hosting on port %u, waiting for %d players", (unsigned)port, players_expected);
        return true;
    }
}

// --- The game's side ---

bool conker::netsession::init(uint8_t* rdram, bool (*local_sampler)(uint16_t*, float*, float*)) {
    const std::string host_port = get_env("CONKER_NET_HOST");
    const std::string join = get_env("CONKER_NET_JOIN");
    if (host_port.empty() && join.empty()) {
        return false;
    }
    const std::string log_path = get_env("CONKER_NET_LOG");
    if (!log_path.empty()) {
        log_file = std::fopen(log_path.c_str(), "w");
    }
    ram = rdram;
    sampler = local_sampler;
    const std::string delay_text = get_env("CONKER_NET_DELAY");
    if (!delay_text.empty()) {
        delay = std::clamp<uint32_t>((uint32_t)std::strtoul(delay_text.c_str(), nullptr, 10), 1, 30);
    }
    const std::string input_text = get_env("CONKER_NET_INPUT");
    if (input_text == "keyboard") {
        filter = 1;
    }
    else if (input_text == "gamepad") {
        filter = 2;
    }
    else if (input_text == "keyboard+gamepad2") {
        filter = 4;   // the keyboard and the second controller together
    }
    else if (input_text == "gamepad2") {
        filter = 3;   // the second controller (the second to press a button), no keyboard
    }
    const std::string test_text = get_env("CONKER_NET_TEST_DESYNC");
    if (!test_text.empty()) {
        test_desync_frame = (uint32_t)std::strtoul(test_text.c_str(), nullptr, 10);
    }
    const std::string players_text = get_env("CONKER_NET_PLAYERS");
    if (!players_text.empty()) {
        players_expected = std::clamp(std::atoi(players_text.c_str()), 2, max_players);
    }
    if (!net_startup()) {
        return false;
    }
    bool ok;
    if (!host_port.empty()) {
        is_host = true;
        my_slot = 0;
        ok = start_host((uint16_t)std::atoi(host_port.c_str()));
    }
    else {
        const size_t colon = join.rfind(':');
        if (colon == std::string::npos) {
            std::fprintf(stderr, "[net] CONKER_NET_JOIN must be <host>:<port>\n");
            return false;
        }
        ok = connect_to_host(join.substr(0, colon), join.substr(colon + 1));
    }
    if (!ok) {
        std::fprintf(stderr, "[net] the online session could not be set up (see CONKER_NET_LOG)\n");
        return false;
    }
    enabled = true;
    std::fprintf(stderr, "[net] online session: %s, slot %d\n", is_host ? "hosting" : "joined", my_slot + 1);
    return true;
}

bool conker::netsession::active() {
    std::lock_guard lock{ mtx };
    return enabled && !failed;
}

uint32_t conker::netsession::frame() {
    std::lock_guard lock{ mtx };
    return current_frame;
}

int conker::netsession::input_filter() {
    return filter;
}

int conker::netsession::local_slot() {
    return my_slot;
}

void conker::netsession::log(const char* line) {
    log_line("%s", line);
}

// Every port read. The first of a pass starts a game frame: it sends this machine's input for the frame
// `delay` ahead, then waits for the agreed inputs of the frame it's starting.
void conker::netsession::on_port_read(int port) {
    conker::netview::port_read();
    {
        std::lock_guard lock{ mtx };
        if (!enabled || failed) {
            return;
        }
        const bool new_pass = port <= last_port;
        last_port = port;
        if (!new_pass) {
            return;
        }
    }

    // The game's frame f should see the world as it is once the previous frame's graphics task is done, whatever the time
    // this machine's renderer took (it draws this machine's view, which isn't the other machines'). Without this, the
    // renderer's speed changes when the game's scheduler gets its messages, and the machines drift apart.
    // Off unless CONKER_NET_GFXSYNC=1 (in a test it gave up waiting nearly every frame, so it did nothing).
    {
        static const bool gfx_sync = get_env("CONKER_NET_GFXSYNC") == "1";
        static uint64_t waits = 0, timeouts = 0;
        if (gfx_sync && started) {
            const auto began = clock::now();
            bool quiet = conker::save_states::runtime_quiet();
            while (!quiet && clock::now() - began < std::chrono::milliseconds(30)) {
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                quiet = conker::save_states::runtime_quiet();
            }
            waits++;
            if (!quiet) {
                timeouts++;
            }
            if (waits % 600 == 0) {
                log_line("[net] graphics sync: %llu frames, %llu of them gave up waiting (30 ms)", (unsigned long long)waits, (unsigned long long)timeouts);
            }
        }
    }

    uint16_t buttons = 0;
    float x = 0.0f, y = 0.0f;
    bool plugged = true;
    if (sampler != nullptr) {
        plugged = sampler(&buttons, &x, &y);
    }

    std::unique_lock lock{ mtx };
    // The game doesn't start until everyone is here.
    cv.wait(lock, [] { return started || failed; });
    if (failed) {
        return;
    }
    // The online pause menu (net_menu.cpp). Start opens the game's own pause screen on this machine only, and while it's
    // open this player's own controller drives it and the game gets a still one: the game goes on for everyone. Only
    // what this machine sends changes, so nothing differs between the machines.
    {
        static bool was_start = false, was_a = false, was_b = false;
        static bool open = false, hold_off = false;
        static uint32_t at_open = 0;
        static bool warned = false;
        static const bool allow_pause = std::getenv("CONKER_NET_ALLOWPAUSE") != nullptr;
        const auto rd8 = [&](uint32_t a) { return (int)(int8_t)ram[(a ^ 3u) & 0x7FFFFFu]; };
        const auto wr8 = [&](uint32_t a, int v) { ram[(a ^ 3u) & 0x7FFFFFu] = (uint8_t)v; };
        const bool start = (buttons & 0x1000) != 0, a = (buttons & 0x8000) != 0, b = (buttons & 0x4000) != 0;
        const bool in_match = ram != nullptr && ram[(0x800BE616u ^ 3u) & 0x7FFFFFu] != 0 && ram[(0x800BEAC0u ^ 3u) & 0x7FFFFFu] == 0;
        // the controller, as the pause menu wants it: the stick from the stick or the D-pad, the buttons only on the
        // frame they go down
        const auto stick = [](float v, bool minus, bool plus) { return plus ? 80 : minus ? -80 : (int)std::lround(std::max(-1.0f, std::min(1.0f, v)) * 80.0f); };
        const int stick_x = stick(x, (buttons & 0x0200) != 0, (buttons & 0x0100) != 0);
        const int stick_y = stick(y, (buttons & 0x0400) != 0, (buttons & 0x0800) != 0);
        if (!open && !allow_pause && plugged && in_match && start && !was_start) {
            // a player slot nobody plays in: the menu reads its input from there
            int slot = -1;
            for (int s = 3; s >= 0; s--) {
                if (s >= players_expected) {
                    slot = s;
                    break;
                }
            }
            if (slot < 0) {
                if (!warned) {
                    warned = true;
                    log_line("[menu] no free player slot for the pause menu (all four players are in)");
                }
            }
            else {
                open = true;
                at_open = (uint32_t)rd8(0x800E0B94u) & 0xFF;
                // as the game's own pause starts (game_20AE20.c, the Start press): the selection on CONT, a short wait before it
                // listens, the stick as it is
                wr8(0x8008FE2C, 0);
                wr8(0x8008FD98, 0);
                wr8(0x8008FEEC, 10);
                wr8(0x8008FEF0, 0);
                wr8(0x8008FEF4, stick_x);
                wr8(0x800E0A85, stick_y);
                conker::netmenu::set_input(slot, 0, stick_x, stick_y);
                conker::netmenu::set_open(true);
                log_line("[menu] opened (player slot %d)", slot);
            }
        }
        else if (open) {
            bool close = false;
            if (!in_match) {
                close = true;
            }
            else if (rd8(0x8008FEF0) != 0) {
                // CONT, B or Start: the game began to unpause (a few frames' wait), which isn't needed
                wr8(0x8008FEF0, 0);
                wr8(0x8008FEEC, 0);
                close = true;
            }
            else if (rd8(0x800E0B94u) == 7 && at_open != 7) {
                // QUIT confirmed: the game is on its way out of the match on this machine, so this machine leaves
                // the session (the others see it gone) and goes on offline
                log_line("[net] this player left the match from the menu");
                for (auto& peer : peers) {
                    peer->drop();
                }
                failed = true;
                cv.notify_all();
                conker::netmenu::set_open(false);
                open = false;
                // for now QUIT closes the game rather than going on offline
                SDL_Event quit;
                SDL_zero(quit);
                quit.type = SDL_QUIT;
                SDL_PushEvent(&quit);
                return;
            }
            else {
                // RESTART would restart this machine's game alone, so it's not offered: the picked button skips it (from CONT the
                // stick goes to QUIT, and from QUIT it stays on QUIT), and A on it, should it get picked, does nothing
                if (rd8(0x8008FE2C) == 1) {
                    wr8(0x8008FE2C, 2);
                }
                // RESTART would restart this machine's game alone: its button does nothing
                int bits = ((start && !was_start) ? 0x1000 : 0) | ((b && !was_b) ? 0x4000 : 0) | ((a && !was_a) ? 0x8000 : 0);
                if (rd8(0x8008FE2C) == 1) {
                    bits &= ~0x8000;
                }
                conker::netmenu::set_input(rd8(0x800E0A98) >= 0 ? rd8(0x800E0A98) : 3, (uint16_t)bits, stick_x, stick_y);
            }
            if (close) {
                conker::netmenu::set_open(false);
                wr8(0x8008FE2C, 0);
                open = false;
                hold_off = true;
            }
        }
        // A button that was pressed for the menu isn't passed on to the game until it is let go.
        if (open || (hold_off && (start || a || b))) {
            buttons = 0;
            x = 0.0f;
            y = 0.0f;
        }
        else {
            hold_off = false;
        }
        was_start = start;
        was_a = a;
        was_b = b;
    }
    const uint32_t f = frames_started;
    {
        std::lock_guard rng_lock{ rng_mutex };
        FrameRng& fr = rng_ring[f % rng_ring_size];
        fr.frame = f;
        fr.start_rng = read_rng();
        fr.calls = 0;
        rng_current = &fr;
        game_thread = std::this_thread::get_id();
    }
    conker::netsession::watch_poll("start", f);
    if (ram != nullptr) {
        RawCopy& rc = raw_ring[f % raw_ring_size];
        rc.frame = f;
        rc.data.resize(ram_bytes);
        std::memcpy(rc.data.data(), ram, ram_bytes);
        obj_ring[f % raw_ring_size][0] = f;
        obj_ring[f % raw_ring_size][1] = objects_hash(0, 6);
        {
            uint32_t cam_base = 0;
            std::memcpy(&cam_base, ram + (0x800BE628u & 0x7FFFFFu), 4);
            // a pointer-free hash of what the game's logic reads and computes around the players' cameras: the camera records,
            // the cameras' view matrices (D_800D9C10) and the top of the game thread's stack
            uint64_t h = 0x9E3779B97F4A7C15ull;
            const auto add = [&](uint32_t address, uint32_t bytes) {
                for (uint32_t off = 0; off < bytes; off += 4) {
                    uint32_t v;
                    std::memcpy(&v, ram + ((address + off) & 0x7FFFFFu), 4);
                    if (v >= 0x80000000u && v < 0x80800000u) {
                        v = 0;
                    }
                    h = (h ^ v) * 0x100000001B3ull;
                    h ^= h >> 29;
                }
            };
            if ((cam_base >> 24) == 0x80 && (cam_base & 0x7FFFFFu) + 0x180u * 4 <= 0x800000u) {
                add(cam_base, 0x180u * 4);
            }
            add(0x800D9C00u, 0x400u);
            obj_ring[f % raw_ring_size][2] = (uint32_t)(h ^ (h >> 32));
        }
    }
    if (is_host) {
        Writer w(FrameRngMsg);
        w.put(f).put(read_rng()).put(obj_ring[f % raw_ring_size][0] == f ? obj_ring[f % raw_ring_size][1] : 0u).put(obj_ring[f % raw_ring_size][0] == f ? obj_ring[f % raw_ring_size][2] : 0u);
        const auto& bytes = w.finish();
        for (auto& peer : peers) {
            if (peer->alive) {
                peer->send_message(bytes);
            }
        }
    }
    else {
        diag_check_frame(f);
    }
    PlayerInput mine;
    mine.plugged = plugged ? 1 : 0;
    mine.buttons = buttons;
    mine.x = x;
    mine.y = y;
    if (is_host) {
        host_store_input(0, f + delay, mine);
    }
    else if (!peers.empty()) {
        Writer w(Input);
        w.put(f + delay).put_input(mine);
        peers[0]->send_message(w.finish());
        sent_inputs[f + delay] = mine;
        if (sent_inputs.size() > 120) {
            sent_inputs.erase(sent_inputs.begin());
        }
    }

    // A test of the resync: this machine's RNG is upset on purpose (it is the first thing a real drift shows in).
    if (!is_host && f == test_desync_frame && ram != nullptr) {
        uint32_t lo;
        std::memcpy(&lo, ram + (0x800885B4u & 0x7FFFFFu), 4);
        lo ^= 0x5A5A5A5Au;
        std::memcpy(ram + (0x800885B4u & 0x7FFFFFu), &lo, 4);
        log_line("[net] frame %u: the RNG was upset on purpose (CONKER_NET_TEST_DESYNC)", f);
    }

    if (f % check_every == 0) {
        CheckInfo info = take_check_info();
        if (is_host) {
            Writer w(Check);
            w.put(f).put(info.rng).put((uint16_t)info.blocks.size());
            for (uint32_t h : info.blocks) {
                w.put(h);
            }
            {
                std::lock_guard rng_lock{ rng_mutex };
                std::vector<const FrameRng*> recent;
                for (uint32_t g = f >= 30 ? f - 30 : 0; g <= f; g++) {
                    const FrameRng& fr = rng_ring[g % rng_ring_size];
                    if (fr.frame == g) {
                        recent.push_back(&fr);
                    }
                }
                w.put((uint16_t)recent.size());
                for (const FrameRng* fr : recent) {
                    w.put(fr->frame).put(fr->start_rng);
                }
                w.put((uint16_t)info.objects.size());
                for (uint32_t h : info.objects) {
                    w.put(h);
                }
            }
            const auto& bytes = w.finish();
            for (auto& peer : peers) {
                if (peer->alive) {
                    peer->send_message(bytes);
                }
            }
        }
        else {
            const auto waiting = pending_checks.find(f);
            if (waiting != pending_checks.end()) {
                CheckInfo theirs = std::move(waiting->second);
                pending_checks.erase(waiting);
                compare_check(f, info, theirs);
            }
            mine_history[f] = std::move(info);
            if (mine_history.size() > 64) {
                mine_history.erase(mine_history.begin());
            }
        }
    }

    const auto began = clock::now();
    while (ready.find(f) == ready.end() && !failed) {
        const auto limit = is_host ? host_waits_for_player : client_waits_for_host;
        if (cv.wait_for(lock, std::chrono::milliseconds(250)) == std::cv_status::timeout) {
            if (clock::now() - began > limit) {
                if (is_host) {
                    // Whoever hasn't sent its input for this frame is dropped; the game goes on without it.
                    const auto it = inbox.find(next_broadcast);
                    for (int s = 1; s < max_players; s++) {
                        if (connected[s] && (it == inbox.end() || !it->second[s].first)) {
                            log_line("[net] player %d stopped answering, dropped", s + 1);
                            connected[s] = false;
                            for (auto& peer : peers) {
                                if (peer->slot == s) {
                                    peer->drop();
                                }
                            }
                        }
                    }
                    try_broadcast();
                }
                else {
                    log_line("[net] no word from the host for %lld s, going on offline",
                        (long long)std::chrono::duration_cast<std::chrono::seconds>(clock::now() - began).count());
                    conker::notices::show("No word from the host, playing on offline");
                    failed = true;
                }
            }
        }
    }
    if (failed) {
        return;
    }
    const double ms = std::chrono::duration<double, std::milli>(clock::now() - began).count();
    waited_ms += ms;
    waited_frames++;
    if (ms > 50.0) {
        stalls++;
    }
    if (f % 600 == 0) {
        log_line("[net] frame %u: waited %.1f ms on average (%llu longer than 50 ms), %llu desyncs", f,
            waited_ms / (double)std::max<uint64_t>(waited_frames, 1), (unsigned long long)stalls, (unsigned long long)desyncs);
    }
    current = ready[f];
    if (f % 30 == 0) {
        log_line("[inputs] f=%u p1=%d:%04X,%g,%g p2=%d:%04X,%g,%g", f, (int)current.p[0].plugged, (unsigned)current.p[0].buttons, (double)current.p[0].x, (double)current.p[0].y,
            (int)current.p[1].plugged, (unsigned)current.p[1].buttons, (double)current.p[1].x, (double)current.p[1].y);
    }
    if (f % 10 == 0 && ram != nullptr) {
        // Where the two players are and a pointer-free hash of each object, to compare the logs of both machines.
        uint32_t pos[6];
        std::memcpy(&pos[0], ram + (0x800CC2E4u & 0x7FFFFFu), 12);
        std::memcpy(&pos[3], ram + (0x800CC610u & 0x7FFFFFu), 12);
        log_line("[pos] f=%u o0=%08X,%08X,%08X o1=%08X,%08X,%08X h=%08X,%08X,%08X,%08X", f, pos[0], pos[1], pos[2], pos[3], pos[4], pos[5],
            objects_hash(0, 1), objects_hash(1, 1), objects_hash(2, 1), objects_hash(3, 1));
    }
    if (ram != nullptr && change_lines < 30000) {
        // Every frame where the players' inputs or either player's position or facing changed: the two machines' logs
        // can be compared line by line to find the first frame they disagree at.
        uint32_t t[12];
        std::memcpy(&t[0], ram + (0x800CC2E4u & 0x7FFFFFu), 12);
        std::memcpy(&t[3], ram + (0x800CC610u & 0x7FFFFFu), 12);
        std::memcpy(&t[6], ram + (0x800CC324u & 0x7FFFFFu), 4);
        std::memcpy(&t[7], ram + (0x800CC650u & 0x7FFFFFu), 4);
        std::memcpy(&t[8], &current.p[0].x, 4);
        std::memcpy(&t[9], &current.p[0].y, 4);
        std::memcpy(&t[10], &current.p[1].x, 4);
        std::memcpy(&t[11], &current.p[1].y, 4);
        uint32_t b = ((uint32_t)current.p[0].buttons << 16) | current.p[1].buttons;
        if (std::memcmp(t, last_change, sizeof(t)) != 0 || b != last_change_buttons) {
            std::memcpy(last_change, t, sizeof(t));
            last_change_buttons = b;
            change_lines++;
            log_line("[chg] f=%u o0=%08X,%08X,%08X a0=%08X o1=%08X,%08X,%08X a1=%08X in0=%08X,%08X in1=%08X,%08X btn=%08X", f, t[0], t[1], t[2], t[6], t[3], t[4], t[5], t[7], t[8], t[9], t[10], t[11], b);
        }
    }
    // A player whose controller was plugged in last frame and isn't now has left (the host drops whoever stops
    // answering, and says so in the frame's inputs): every machine says so, as the same frame comes round for all.
    {
        static bool was_plugged[max_players] = {};
        for (int s = 1; s < max_players; s++) {
            const bool now = current.p[s].plugged != 0;
            if (was_plugged[s] && !now) {
                conker::notices::show("Player " + std::to_string(s + 1) + " left the match");
            }
            was_plugged[s] = now;
        }
    }
    current_frame = f;
    frames_started = f + 1;
    in_frame = true;
}

bool conker::netsession::input(int port, bool* plugged, uint16_t* buttons, float* x, float* y) {
    std::lock_guard lock{ mtx };
    if (!enabled || failed || port < 0 || port >= max_players) {
        return false;
    }
    const PlayerInput& in = current.p[port];
    *plugged = in.plugged != 0;
    *buttons = in.buttons;
    // The game stops everything while it's paused, and every machine has to go on together, so online the Start
    // button doesn't pause during a match (the multiplayer arenas, D_800BE616 set; D_800BEAC0 is the game's pause).
    // CONKER_NET_ALLOWPAUSE=1 gives it back.
    static const bool allow_pause = std::getenv("CONKER_NET_ALLOWPAUSE") != nullptr;
    if (!allow_pause && ram != nullptr && ram[(0x800BE616u ^ 3u) & 0x7FFFFFu] != 0 && ram[(0x800BEAC0u ^ 3u) & 0x7FFFFFu] == 0) {
        *buttons &= (uint16_t)~0x1000u;
    }
    *x = in.x;
    *y = in.y;
    return true;
}

bool conker::netsession::device_present(int controller, bool* present) {
    std::lock_guard lock{ mtx };
    if (!enabled || failed || controller < 0 || controller >= max_players) {
        return false;
    }
    if (!started) {
        *present = controller == 0;
        return true;
    }
    // Who is connected is part of every frame's inputs; before the first frame it's whoever joined.
    *present = in_frame ? current.p[controller].plugged != 0 : (controller < players_expected);
    return true;
}

// --- Resync ---
// The host takes a snapshot of the whole game (save_states.cpp) and sends it to whoever asked; that machine
// loads it and goes on from the frame it was taken at, with the inputs it already has for the frames after.
// The snapshot is taken between two game frames' processing, not at a port read, so it is told which frame
// the game was in (frames_started) and the loaded game goes on from there.

namespace {
    // Mutex held (host).
    void start_capture_locked() {
        capture_in_flight = conker::save_states::net_capture([](bool ok, std::vector<uint8_t>&& state) {
            // On the game's idle thread, at the safe point the state was taken at.
            uint32_t at_frame;
            uint32_t mask;
            {
                std::lock_guard lock{ mtx };
                at_frame = frames_started;
                mask = snapshot_wanted_mask;
                snapshot_wanted_mask = 0;
                capture_in_flight = false;
                last_snapshot_sent = clock::now();
            }
            if (!ok || mask == 0 || state.empty() || state.size() > snapshot_max_size) {
                log_line("[net] the snapshot could not be taken (%s, %zu bytes)", ok ? "taken" : "the game wasn't at a safe point in time", state.size());
                return;
            }
            // Compressing and sending take a while, so not on this thread.
            std::thread([at_frame, mask, state = std::move(state)]() {
                const std::vector<uint8_t> packed = lz_compress(state.data(), state.size());
                Writer w(Snapshot);
                w.put(at_frame).put((uint32_t)state.size()).put((uint32_t)packed.size()).put_bytes(packed.data(), packed.size());
                const auto& bytes = w.finish();
                std::vector<Peer*> targets;
                {
                    std::lock_guard lock{ mtx };
                    for (auto& peer : peers) {
                        if (peer->alive && (mask & (1u << peer->slot))) {
                            targets.push_back(peer.get());
                        }
                    }
                }
                for (Peer* peer : targets) {
                    peer->send_message(bytes);
                }
                log_line("[net] snapshot of frame %u sent: %zu bytes, packed to %zu", at_frame, state.size(), packed.size());
            }).detach();
        });
    }

    // Mutex held.
    void on_resync_requested(int slot, uint32_t frame, uint32_t diverged) {
        log_line("[net] player %d asks for a resync at frame %u", slot + 1, frame);
        if (diverged != 0) {
            log_callers("host", diverged);
        }
        snapshot_wanted_mask |= 1u << slot;
        if (!capture_in_flight && clock::now() - last_snapshot_sent > std::chrono::seconds(2)) {
            start_capture_locked();
        }
    }

    // Mutex held (client): the host's snapshot has come.
    void on_snapshot_message(const std::vector<uint8_t>& payload) {
        Reader r{ payload.data(), payload.size() };
        const uint32_t at_frame = r.get<uint32_t>();
        const uint32_t raw_size = r.get<uint32_t>();
        const uint32_t packed_size = r.get<uint32_t>();
        if (r.bad || raw_size == 0 || raw_size > snapshot_max_size || r.at + packed_size > payload.size()) {
            resync_in_flight = false;
            return;
        }
        std::vector<uint8_t> state(raw_size);
        if (!lz_decompress(payload.data() + r.at, packed_size, state.data(), raw_size)) {
            log_line("[net] the snapshot did not unpack");
            resync_in_flight = false;
            return;
        }
        log_line("[net] snapshot of frame %u came (this machine is at frame %u), loading it", at_frame, frames_started);
        if (!conker::save_states::net_restore(std::move(state), [at_frame](bool ok) {
            // On the game's idle thread.
            std::lock_guard lock{ mtx };
            resync_in_flight = false;
            if (!ok) {
                log_line("[net] the snapshot could not be loaded");
                return;
            }
            // The game goes on from the state: it was in the frame before at_frame.
            frames_started = at_frame;
            current_frame = at_frame > 0 ? at_frame - 1 : 0;
            const auto it = ready.find(current_frame);
            if (it != ready.end()) {
                current = it->second;
            }
            last_port = 99;
            mine_history.clear();
            pending_checks.clear();
            // The host needs this machine's input for every frame from at_frame on. The game goes back to at_frame
            // and then sends the one for at_frame + delay, so those in between are sent again (the host keeps the
            // first it got for a frame), or, if this machine never got to them, made up from the last it sent.
            if (!is_host && !peers.empty()) {
                PlayerInput last;
                last.plugged = 1;
                if (!sent_inputs.empty()) {
                    last = sent_inputs.rbegin()->second;
                }
                for (uint32_t g = at_frame; g < at_frame + delay; g++) {
                    const auto sent = sent_inputs.find(g);
                    const PlayerInput& in = sent != sent_inputs.end() ? sent->second : last;
                    Writer w(Input);
                    w.put(g).put_input(in);
                    peers[0]->send_message(w.finish());
                }
            }
            obj_bad_run = 0;
            log_line("[net] resynced, going on from frame %u", at_frame);
            cv.notify_all();
        })) {
            resync_in_flight = false;
        }
    }
}


namespace {
    // Mutex held (client): the host's RNG for frame f and this machine's are both known: if they differ, the host
    // is asked for its memory around that frame, to compare it with this machine's.
    void diag_check_frame(uint32_t f) {
        if (is_host || resync_in_flight) {
            return;
        }
        const auto theirs = host_frame_rng.find(f);
        if (theirs == host_frame_rng.end()) {
            return;
        }
        const FrameRng& fr = rng_ring[f % rng_ring_size];
        if (fr.frame == f && fr.start_rng == theirs->second) {
            // The RNGs agree: do the objects (without their pointers)?
            const auto their_obj = host_frame_obj.find(f);
            const auto their_cam = host_frame_cam.find(f);
            if (their_cam != host_frame_cam.end() && obj_ring[f % raw_ring_size][0] == f && their_cam->second != 0 &&
                obj_ring[f % raw_ring_size][2] != their_cam->second) {
                if (cam_bad_run++ == 0 && !diag_pending.active && diag_dumps < 10 && (last_diag_frame == 0 || f >= last_diag_frame + 200)) {
                    diag_dumps++;
                    char why3[160];
                    std::snprintf(why3, sizeof(why3), "the camera records at the start of frame %u differ (RNG the same)", f);
                    begin_diag(f, why3);
                }
            }
            else {
                cam_bad_run = 0;
            }
            if (their_obj != host_frame_obj.end() && obj_ring[f % raw_ring_size][0] == f && their_obj->second != 0 && f != last_obj_frame_checked) {
                last_obj_frame_checked = f;
                if (obj_ring[f % raw_ring_size][1] != their_obj->second) {
                    obj_bad_run++;
                    if (obj_bad_run == 1 && !diag_pending.active && diag_dumps < 10 && (last_diag_frame == 0 || f >= last_diag_frame + 200)) {
                        diag_dumps++;
                        char why2[160];
                        std::snprintf(why2, sizeof(why2), "the objects at the start of frame %u differ (RNG the same)", f);
                        begin_diag(f, why2);
                    }
                    // Two frames in a row: a real drift (not a flicker). The host's whole game is fetched now, while
                    // the two machines are still almost the same, so that taking it moves nothing visibly.
                    if (obj_bad_run >= 2 && (last_obj_resync == 0 || f >= last_obj_resync + 90)) {
                        last_obj_resync = f;
                        log_line("[net] frame %u: the objects have differed for %u frames, asking for a resync", f, obj_bad_run);
                        request_resync(f);
                    }
                }
                else {
                    obj_bad_run = 0;
                }
            }
            return;
        }
        if (fr.frame != f || diag_dumps >= 10 || diag_pending.active) {
            return;
        }
        if (last_diag_frame != 0 && f < last_diag_frame + 200) {
            return;
        }
        diag_dumps++;
        char why[160];
        std::snprintf(why, sizeof(why), "the RNG at the start of frame %u differs (mine %016llx, the host's %016llx)", f,
            (unsigned long long)fr.start_rng, (unsigned long long)theirs->second);
        begin_diag(f, why);
    }

    // Mutex held (client): ask the host for its memory around frame f, to compare it with this machine's.
    void begin_diag(uint32_t f, const char* why) {
        if (is_host || diag_pending.active || resync_in_flight || peers.empty() || !peers[0]->alive) {
            return;
        }
        last_diag_frame = f;
        diag_pending.frame = f;
        diag_pending.mine.clear();
        for (uint32_t g = f >= 6 ? f - 6 : 0; g <= f; g++) {
            const RawCopy& rc = raw_ring[g % raw_ring_size];
            if (rc.frame == g) {
                diag_pending.mine.push_back(rc);
            }
        }
        diag_pending.active = true;
        log_line("[net] DIAG: %s, asking for the host's memory", why);
        Writer w(DiagRequest);
        w.put(f);
        peers[0]->send_message(w.finish());
    }

    // Mutex held (host).
    void on_diag_request(uint32_t f) {
        std::vector<RawCopy> copies;
        for (uint32_t g = f >= 6 ? f - 6 : 0; g <= f; g++) {
            const RawCopy& rc = raw_ring[g % raw_ring_size];
            if (rc.frame == g) {
                copies.push_back(rc);
            }
        }
        std::thread([f, copies = std::move(copies)]() {
            Writer w(DiagData);
            w.put(f).put((uint32_t)copies.size());
            for (const RawCopy& rc : copies) {
                const std::vector<uint8_t> packed = lz_compress(rc.data.data(), rc.data.size());
                w.put(rc.frame).put((uint32_t)rc.data.size()).put((uint32_t)packed.size()).put_bytes(packed.data(), packed.size());
            }
            const auto& bytes = w.finish();
            std::vector<Peer*> targets;
            {
                std::lock_guard lock{ mtx };
                for (auto& peer : peers) {
                    if (peer->alive) {
                        targets.push_back(peer.get());
                    }
                }
            }
            for (Peer* peer : targets) {
                peer->send_message(bytes);
            }
        }).detach();
    }

    // Mutex held (client).
    void on_diag_data(const std::vector<uint8_t>& payload) {
        if (!diag_pending.active) {
            return;
        }
        DiagPending job = std::move(diag_pending);
        diag_pending = DiagPending{};
        std::vector<uint8_t> noisy = noisy_blocks;
        std::vector<uint8_t> copy = payload;
        std::thread([job = std::move(job), noisy = std::move(noisy), copy = std::move(copy)]() {
            Reader r{ copy.data(), copy.size() };
            const uint32_t f = r.get<uint32_t>();
            const uint32_t count = r.get<uint32_t>();
            log_line("[net] DIAG: memory of the host for frames up to %u came (%u copies); words that differ from this machine's (address: mine / host's):", f, count);
            std::vector<uint8_t> previous;
            for (uint32_t i = 0; i < count && !r.bad; i++) {
                const uint32_t frame = r.get<uint32_t>();
                const uint32_t raw_size = r.get<uint32_t>();
                const uint32_t packed_size = r.get<uint32_t>();
                if (r.bad || r.at + packed_size > copy.size() || raw_size != ram_bytes) {
                    break;
                }
                std::vector<uint8_t> theirs(raw_size);
                const bool ok = lz_decompress(copy.data() + r.at, packed_size, theirs.data(), raw_size);
                r.at += packed_size;
                const RawCopy* mine = nullptr;
                for (const RawCopy& rc : job.mine) {
                    if (rc.frame == frame) {
                        mine = &rc;
                    }
                }
                if (!ok || mine == nullptr) {
                    log_line("[net] DIAG: frame %u: %s", frame, ok ? "this machine no longer has its copy" : "did not unpack");
                    continue;
                }
                // Every word that differs, noisy blocks too. What matters is what starts to differ from one frame to
                // the next: the words that differed in the frame before are the usual difference.
                const size_t words = raw_size / 4;
                const uint32_t* a = reinterpret_cast<const uint32_t*>(mine->data.data());
                const uint32_t* b = reinterpret_cast<const uint32_t*>(theirs.data());
                std::vector<uint8_t> now(words, 0);
                size_t total = 0, fresh = 0;
                std::string list;
                char text[64];
                int shown = 0;
                std::map<uint32_t, size_t> buckets;
                std::vector<uint32_t> fresh_words;
                for (size_t w = 0; w < words; w++) {
                    if (a[w] == b[w]) {
                        continue;
                    }
                    now[w] = 1;
                    total++;
                    if (!previous.empty() && !previous[w]) {
                        fresh++;
                        buckets[(uint32_t)((w * 4) >> 16)]++;
                        fresh_words.push_back((uint32_t)w);
                    }
                }
                // (the words are listed from the 64 KB blocks where few began to differ: where thousands did is a buffer)
                for (uint32_t w : fresh_words) {
                    if (buckets[(w * 4) >> 16] <= 300 && shown < 70) {
                        shown++;
                        std::snprintf(text, sizeof(text), " %08X:%08X/%08X", (unsigned)(0x80000000u + w * 4), (unsigned)a[w], (unsigned)b[w]);
                        list += text;
                    }
                }
                std::string where;
                for (const auto& item : buckets) {
                    std::snprintf(text, sizeof(text), " %08X:%zu", (unsigned)(0x80000000u + (item.first << 16)), item.second);
                    where += text;
                }
                if (previous.empty()) {
                    log_line("[net] DIAG: frame %u: %zu words differ (the usual difference)", frame, total);
                }
                else {
                    log_line("[net] DIAG: frame %u: %zu words differ, %zu of them new since the frame before; where (64 KB blocks:count):%s", frame, total, fresh, where.c_str());
                    log_line("[net] DIAG:   new (address:mine/host's):%s", list.c_str());
                }
                // The game's own state, whether or not it was different before: the objects (Conker and the other
                // players first), the cameras, their shake state and the frame's globals.
                {
                    uint32_t camera_base = 0;
                    std::memcpy(&camera_base, mine->data.data() + (0x800BE628u & 0x7FFFFFu), 4);
                    struct Range { const char* name; uint32_t start; uint32_t length; };
                    const Range ranges[] = {
                        { "objects", 0x800CC2D0u, 0x32Cu * 6 },
                        { "cameras", camera_base, 0x180u * 4 },
                        { "shake", 0x800DC2C0u, 0x68u * 4 },
                        { "globals", 0x800BE900u, 0x120u },
                        { "views", 0x800D9C00u, 0x400u },
                        { "stack", 0x80030C00u, 0xA00u },
                    };
                    for (const Range& range : ranges) {
                        if ((range.start >> 24) != 0x80 || (range.start & 0x7FFFFFu) + range.length > raw_size) {
                            continue;
                        }
                        std::string text_list;
                        int count = 0, printed = 0;
                        for (uint32_t offset = 0; offset < range.length; offset += 4) {
                            const size_t w = ((range.start & 0x7FFFFFu) + offset) / 4;
                            if (a[w] == b[w]) {
                                continue;
                            }
                            if (range.start == 0x800CC2D0u && a[w] >= 0x80000000u && a[w] < 0x80800000u && b[w] >= 0x80000000u && b[w] < 0x80800000u) {
                                continue;   // two heap pointers: they differ between machines and mean nothing
                            }
                            count++;
                            if (printed < 60) {
                                printed++;
                                char item[48];
                                std::snprintf(item, sizeof(item), " %08X:%08X/%08X", (unsigned)(range.start + offset), (unsigned)a[w], (unsigned)b[w]);
                                text_list += item;
                            }
                        }
                        if (count != 0) {
                            log_line("[net] DIAG:   frame %u, %s (%d words differ):%s", frame, range.name, count, text_list.c_str());
                        }
                    }
                }
                previous.swap(now);
            }
        }).detach();
    }
}

// Diagnostic: the words at 0x80088540..0x80088580 (the first memory that differed between two machines) are
// looked at now and then; each change is logged with the frame and where it was seen, to tell when it happens.
void conker::netsession::watch_poll(const char* tag, uint32_t info) {
    static uint32_t last[17];
    static bool first = true;
    static int lines = 0;
    static std::mutex watch_mutex;
    if (ram == nullptr || log_file == nullptr) {
        return;
    }
    std::lock_guard lock{ watch_mutex };
    {
        // The RNG should only change inside the hooked calls (each one steps it once): any other change is a step
        // taken by code that the hooks don't see, or a write to the state.
        static uint64_t expected = 0;
        static bool have_expected = false;
        static uint32_t last_caller = 0;
        static uint64_t previous = 0;
        static int rng_lines = 0;
        const bool is_call = tag[0] == 'r';
        const uint64_t now = read_rng();
        if (have_expected && now != expected && rng_lines < 80) {
            rng_lines++;
            log_line("[rngwatch] f=%u %s:%X: expected %016llx, got %016llx (the last hooked call, from %X, saw %016llx)%s", current_frame, tag, (unsigned)info,
                (unsigned long long)expected, (unsigned long long)now, (unsigned)last_caller, (unsigned long long)previous,
                std::this_thread::get_id() != game_thread ? " (other thread)" : "");
        }
        if (is_call) {
            // random_u32's step, from its code: a 33-bit rotate, xor with the low 20 bits shifted up, then the
            // twelve bits at 20 xored in.
            const uint64_t t = now;
            uint64_t x = ((t & 1) << 32) | ((t >> 1) & 0xFFFFFFFFull);
            x ^= (t << 44) >> 32;
            expected = ((x >> 20) & 0xFFF) ^ x;
            have_expected = true;
            last_caller = info;
            previous = now;
        }
        else if (have_expected) {
            expected = now;   // (reported once)
        }
    }
    for (int i = 1; i < 17; i++) {
        uint32_t v;
        std::memcpy(&v, ram + (0x80088540u & 0x7FFFFFu) + i * 4, 4);
        if (!first && v != last[i] && lines < 6000) {
            lines++;
            log_line("[watch] f=%u %s:%X %08X %08X -> %08X%s", current_frame, tag, (unsigned)info, (unsigned)(0x80088540u + i * 4),
                (unsigned)last[i], (unsigned)v, std::this_thread::get_id() != game_thread ? " (other thread)" : "");
        }
        last[i] = v;
    }
    first = false;
}

// The game asked for a random number (hook at random_u32 and random_float; caller is where it came from).
void conker::netsession::note_rng_call(uint32_t caller, uint32_t state) {
    watch_poll("rng", caller & 0x3FFFFFFFu);
    std::lock_guard rng_lock{ rng_mutex };
    if (rng_current == nullptr) {
        return;
    }
    if (rng_current->calls < 64) {
        rng_current->callers[rng_current->calls] = (caller & 0x3FFFFFFFu) | (conker::netview::busy() ? 0x80000000u : 0u) |
            (std::this_thread::get_id() != game_thread ? 0x40000000u : 0u);
        rng_current->states[rng_current->calls] = state;
    }
    rng_current->calls++;
}
