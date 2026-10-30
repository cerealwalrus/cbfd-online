// The network layer under the online play: messages over UDP (ENet, host/third_party/enet), reliable and in order like the TCP
// connection they replace, but with ENet's own retransmit timing (so a lost packet doesn't hold everything up for a TCP-sized
// back-off) and a connection that is a UDP flow, not a byte stream the system buffers.
//
// The API is blocking and per connection, so the session's reader threads work as they did with sockets:
//   - Server::accept() gives the next player's connection;
//   - Conn::receive() waits for the next message (false when the connection is gone, or the wait timed out);
//   - Conn::send() queues a message (the network thread sends it within about a millisecond);
//   - Conn::close() lets what was queued go out first, then ends the connection; a reader waiting in receive() wakes at once.
// A message is any bytes up to max_packet; the session frames them itself.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace conker::transport {
    constexpr size_t max_packet = 64u << 20;
    constexpr int unreliable_channel = 3;
    constexpr size_t unreliable_most = 1200;   // (within one datagram of ENet's 1392-byte MTU: an unreliable packet past it would be fragmented)

    // Starts the library; call before anything else. False if it can't.
    bool startup();

    class Loop;

    class Conn {
    public:
        ~Conn();
        // Queues one message. False when the connection is gone. Channel 0 is the game's (inputs, frames, pings); channel 1 is for big things
        // (snapshots): each channel is in order on its own, so a snapshot on its way doesn't hold the game's messages up behind it.
        // Channel 2 is channel 1 too, but its messages go ahead of channel 1's that haven't started going out (in their own order).
        // (possess158) Channel 3 (unreliable_channel) is sent once, unreliably and out of order (ENet's unsequenced packets): it may
        // never arrive, and nothing waits for it; up to unreliable_most bytes, one datagram.
        bool send(const uint8_t* data, size_t size, int channel = 0);
        // Waits for the next message; timeout_ms < 0 waits for ever. False: the connection is gone (closed() is true) or the wait ran out.
        bool receive(std::vector<uint8_t>& message, int timeout_ms = -1);
        bool closed() const;
        // Ends the connection after what was queued has gone out; a reader blocked in receive() returns.
        void close();
        // The round trip to the other end as the library measures it, in milliseconds (0 until measured).
        double rtt_ms() const;
        // What the library counts about this connection (packets sent and lost, its round-trip estimate, how long its network thread went
        // without running), as a line for the log; updated about once a second.
        std::string stats() const;
        // The other end's address ("a.b.c.d").
        std::string address() const;

        struct State;
        std::shared_ptr<State> state;

        Conn();   // (made by Server::accept and connect only)
    };

    class Server {
    public:
        ~Server();
        // Opens the UDP port. Null (and `error` set) when it can't.
        static std::shared_ptr<Server> listen(uint16_t port, std::string* error);
        // Waits for the next connection; null once the server has been stopped.
        std::shared_ptr<Conn> accept();
        // Closes the port (it is free again when this returns) and wakes accept().
        void stop();

    private:
        Server() = default;
        std::shared_ptr<Loop> loop;
    };

    // failure: 0 connected, 1 the name wasn't found, 2 nobody answered in time, 3 cancelled.
    std::shared_ptr<Conn> connect(const std::string& host, uint16_t port, int timeout_ms, const std::function<bool()>& cancelled, int* failure);
}
