// Netplay milestone 2: the online session (lockstep over UDP).
//
// Every machine runs the whole game; only the controllers travel. One machine hosts and is player 1 (port 1);
// the others join and are players 2..4. Each game frame the host collects every player's input for that frame,
// puts them together in one FRAME message and sends it to everyone (itself included, so every machine feeds the
// game exactly the same bytes). A machine that hasn't got the FRAME message of the frame it's about to start
// waits for it, so nobody runs ahead of anybody.
//
// Input delay hides the network's latency: the input read at game frame f is the one used at frame f + delay
// (100 ms for 3 frames at the game's 30 frames a second), so a FRAME message has that long to arrive. On Auto (the lobby's
// default) the host works it out from the round trips and changes it during the game when they change.
//
// The host also tells the others the game's RNG state every 30 frames (CHECK). A machine that sees a different
// one has drifted (netplay.cpp tells why that can happen); resync.cpp's snapshot, taken by the host, puts it
// right. This file only notices and asks (on_desync below).
//
// Environment (until there's a lobby screen):
//   CONKER_NET_HOST=<port>        host a game on that UDP port
//   CONKER_NET_PLAYERS=<2..4>     how many players the host waits for before the game starts (default 2)
//   CONKER_NET_JOIN=<host>:<port> join a game
//   CONKER_NET_DELAY=<frames>     input delay (default 3; 0 or auto: from the round trips)
//   CONKER_NET_DIAG=1             when the games differ, the client is sent the host's memory to compare (megabytes: a test only)
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
#include <filesystem>
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
#include <dbghelp.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <SDL.h>

#include "librecomp/game.hpp"
#include "ultramodern/ultramodern.hpp"
#include "recomp.h"

#include "conker.hpp"
#include "net_transport.hpp"
#include "version.hpp"

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#endif

namespace {
    using ConnPtr = std::shared_ptr<conker::transport::Conn>;
    using clock = std::chrono::steady_clock;

    constexpr int max_players = 4;
    constexpr uint32_t magic = 0x544E4243;  // "CBNT"
    constexpr uint32_t protocol_version = 12;
    constexpr uint32_t check_every = 30;
    constexpr uint32_t history = 1200;       // frames of inputs kept (for resyncs)
    constexpr uint32_t max_message = 64u << 20;
    constexpr auto host_waits_for_player = std::chrono::seconds(5);
    constexpr auto client_waits_for_host = std::chrono::seconds(10);

    enum Msg : uint8_t { Hello = 1, Welcome = 2, Start = 3, Input = 4, Frame = 5, Check = 6, ResyncRequest = 7, Snapshot = 8, FrameRngMsg = 9, DiagRequest = 10, DiagData = 11, LobbyInfo = 12, Refused = 13, Chat = 14, Ping = 15, Pong = 16,
        SnapHashes = 17, SnapDiff = 18, DelayChange = 19 };

    struct PlayerInput {
        uint8_t plugged = 0;
        uint16_t buttons = 0;
        float x = 0.0f;
        float y = 0.0f;
        // The right stick turns this player's camera (Right Stick: Free Camera): how far this frame, yaw and pitch in
        // 1/10000 of a radian, with the player's own settings already applied (mouse_camera.cpp).
        uint8_t cam_on = 0;
        // The player's sticks are swapped (Aiming: Swap Sticks): they aim with the right stick and move with the left.
        uint8_t swap = 0;
        // The player's aiming settings that change what the game does with its sticks (look_aim.cpp's look_flag bits: the stick's
        // inverting, its response, the lock-on): every machine plays that player's aiming with that player's own settings.
        uint8_t look = 0;
        int16_t cam_yaw = 0;
        int16_t cam_pitch = 0;
        // Only the host's input has any: for each of the game's 16 sound slots, the handle of a sound that has finished
        // playing on the host's audio thread (0: none). Every machine acts on it at the same frame, see the sound notes below.
        std::array<uint16_t, 16> ended{};
        // A player chose QUIT in the pause menu: the game's own variables as its quit left them (the code of func_151DDC20 at
        // 0x151DE498), which every machine sets at the same frame so that all of them go back to the multiplayer menu together.
        struct Leave {
            uint8_t on = 0;
            uint8_t b94 = 0;      // 0x800E0B94
            uint32_t a88 = 0;     // 0x800E0A88 (the function the game runs on its way out)
            uint8_t fd74 = 0;     // 0x8008FD74
            uint8_t gs3f = 0;     // the byte at 0x3F of what 0x8008FDD4 points to
            uint8_t fd9c = 0;     // 0x8008FD9C
            uint8_t fe2c = 0;     // 0x8008FE2C
            uint8_t v7270 = 0;    // 0x80087270
        } leave;
    };
    struct FrameInputs {
        std::array<PlayerInput, max_players> p{};
        // The host's word, in the frame itself so every machine acts on it at the same frame: the players (a bit each) whose game is
        // to be put right by a resync. Every machine takes a copy of its game after this frame; the ones named send the host a hash
        // of each piece of theirs, and the host sends back only the pieces that differ from its own (see the resync part below).
        uint8_t snap = 0;
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
        Writer& put_string(const std::string& text) {
            const uint8_t size = (uint8_t)std::min<size_t>(text.size(), 255);
            put(size);
            return put_bytes(text.data(), size);
        }
        Writer& put_bytes(const void* data, size_t size) {
            const uint8_t* b = static_cast<const uint8_t*>(data);
            bytes.insert(bytes.end(), b, b + size);
            return *this;
        }
        Writer& put_input(const PlayerInput& in) {
            put(in.plugged).put(in.buttons).put(in.x).put(in.y);
            put((uint8_t)((in.cam_on ? 1 : 0) | (in.swap ? 2 : 0) | ((in.look & 0x3Fu) << 2)));
            if (in.cam_on) {
                put(in.cam_yaw).put(in.cam_pitch);
            }
            uint8_t count = 0;
            for (uint16_t h : in.ended) {
                count += h != 0 ? 1 : 0;
            }
            put(count);
            for (size_t k = 0; k < in.ended.size(); k++) {
                if (in.ended[k] != 0) {
                    put((uint8_t)k).put(in.ended[k]);
                }
            }
            put(in.leave.on);
            if (in.leave.on) {
                put(in.leave.b94).put(in.leave.a88).put(in.leave.fd74).put(in.leave.gs3f).put(in.leave.fd9c).put(in.leave.fe2c).put(in.leave.v7270);
            }
            return *this;
        }
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
        std::string get_string() {
            const uint8_t size = get<uint8_t>();
            if (bad || at + size > this->size) {
                bad = true;
                return {};
            }
            std::string text(reinterpret_cast<const char*>(data + at), size);
            at += size;
            return text;
        }
        PlayerInput get_input() {
            PlayerInput in;
            in.plugged = get<uint8_t>();
            in.buttons = get<uint16_t>();
            in.x = get<float>();
            in.y = get<float>();
            const uint8_t flags = get<uint8_t>();
            in.cam_on = flags & 1;
            in.swap = (flags & 2) ? 1 : 0;
            in.look = (uint8_t)(flags >> 2);
            if (in.cam_on) {
                in.cam_yaw = get<int16_t>();
                in.cam_pitch = get<int16_t>();
            }
            const uint8_t count = get<uint8_t>();
            for (uint8_t i = 0; i < count && !bad; i++) {
                const uint8_t k = get<uint8_t>();
                const uint16_t h = get<uint16_t>();
                if (k < in.ended.size()) {
                    in.ended[k] = h;
                }
            }
            in.leave.on = get<uint8_t>();
            if (in.leave.on) {
                in.leave.b94 = get<uint8_t>();
                in.leave.a88 = get<uint32_t>();
                in.leave.fd74 = get<uint8_t>();
                in.leave.gs3f = get<uint8_t>();
                in.leave.fd9c = get<uint8_t>();
                in.leave.fe2c = get<uint8_t>();
                in.leave.v7270 = get<uint8_t>();
            }
            return in;
        }
    };

    // (A message is one UDP-transport packet: [length][type][payload], length counting the type and the payload.)
    bool send_all(const ConnPtr& c, const uint8_t* data, size_t size) {
        return c != nullptr && c->send(data, size);
    }

    // Reads one message. False when the connection is gone, the wait (timeout_ms, -1 for ever) ran out, or what came isn't a message.
    bool read_message(const ConnPtr& c, uint8_t& type, std::vector<uint8_t>& payload, int timeout_ms = -1) {
        std::vector<uint8_t> body;
        if (c == nullptr || !c->receive(body, timeout_ms) || body.size() < 5) {
            return false;
        }
        uint32_t length = 0;
        std::memcpy(&length, body.data(), 4);
        if (length < 1 || length > max_message || length != body.size() - 4) {
            return false;
        }
        type = body[4];
        payload.assign(body.begin() + 5, body.end());
        return true;
    }

    struct Peer {
        ConnPtr conn;
        int slot = -1;
        std::mutex send_mutex;
        std::atomic<bool> alive{ false };
        std::atomic<bool> retired{ false };   // the lobby let go of it: its reader leaves the session's state alone
        std::thread reader;

        // round trips measured with Ping/Pong (milliseconds), see ping_tick
        std::mutex rtt_mutex;
        uint32_t ping_seq = 0;
        uint32_t pings_sent = 0, pongs_got = 0;
        double rtt_last = 0, rtt_sum = 0, rtt_min = 1e9, rtt_max = 0, rtt_sq = 0;   // since the last report
        double rtt_all_max = 0;
        std::array<double, 10> rtt_recent{};   // the last ten answers, for the launcher
        uint32_t rtt_recent_n = 0;
        double rtt_total_sum = 0, rtt_total_min = 1e9;   // since the connection began
        uint64_t rtt_total_n = 0;
        uint32_t rtt_count = 0;

        // channel 1 is for the big messages (snapshots, memory copies for the diagnostics): they go in order among themselves but don't
        // hold the game's messages up
        bool send_message(const std::vector<uint8_t>& bytes, int channel = 0) {
            std::lock_guard lock{ send_mutex };
            if (!alive || conn == nullptr || !conn->send(bytes.data(), bytes.size(), channel)) {
                alive = false;
                if (conn != nullptr) {
                    conn->close();   // wakes the reader, which sees the player is gone
                }
                return false;
            }
            return true;
        }
        void drop() {
            alive = false;
            if (conn != nullptr) {
                conn->close();
            }
        }
    };

    // --- The session's state ---

    std::mutex mtx;
    std::condition_variable cv;

    bool enabled = false;          // CONKER_NET_HOST or _JOIN was given and the session hasn't ended
    bool is_host = false;
    bool dedicated = false;        // the host is a server without a player: every slot is a remote player (slot 0 is the menu's "leader")
    std::map<uint32_t, std::array<uint16_t, 16>> host_notes;   // dedicated: the server's notes of finished sounds, by the frame they apply at
    bool started = false;          // every player has joined and the game may begin
    bool failed = false;           // the session is over (this machine goes on offline)
    int my_slot = 0;
    int players_expected = 2;
    uint32_t delay = 3;
    uint8_t* ram = nullptr;
    bool (*sampler)(uint16_t*, float*, float*) = nullptr;

    // Frame numbering: frames_started is the number of the next frame to start.
    uint32_t frames_started = 0;
    // 0.10.13: the frame at which every machine starts the hooks' own state over (the frame after a resync's order), and how many times
    // it has been. The resync gives a machine the host's game memory, but what the hooks keep between frames outside it (the free
    // camera's orbits, R-Look's walking and hits, the grenade's hold...) is each machine's own: the host started its orbits over as it
    // took its copy and the machine loading it did too, but the others didn't, and in a 4 player game they drifted from the host the
    // frame after (0.10.11's logs: player 4 resynced at frame 18155, players 2 and 3 had other cameras at 18156 and other objects at
    // 18157, and so on until the game froze). Now everyone starts it over at the same frame.
    uint32_t hook_reset_frame = 0xFFFFFFFFu;
    std::atomic<uint32_t> hook_generation_count{ 0 };
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
    uint32_t obj_ring[raw_ring_size][4];            // {frame, objects hash, camera records hash, sound slots hash}
    std::map<uint32_t, uint32_t> host_frame_cam;
    std::map<uint32_t, uint32_t> host_frame_snd;   // the host's sound slots hash, by frame (only compared and logged)
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
    // Dedicated server: a player who joins a game that is already running is "joining" until the snapshot of the game
    // has been sent to it; from the frame join_from on, the game waits for its input like for anyone's.
    std::array<bool, max_players> joining{};
    std::array<uint32_t, max_players> join_from = []{ std::array<uint32_t, max_players> a{}; a.fill(0xFFFFFFFFu); return a; }();
    std::array<clock::time_point, max_players> joining_since{};
    std::array<bool, max_players> input_since_join{};   // a late joiner has sent an input (its snapshot is loaded)
    bool late_join = false;       // client: joined a game that was already running, to be brought up to date by a snapshot
    bool late_synced = false;     // client: that snapshot is loaded
    std::map<uint32_t, std::array<std::pair<bool, PlayerInput>, max_players>> inbox;
    uint32_t next_broadcast = 0;
    std::shared_ptr<conker::transport::Server> listener;

    // Counters.
    uint64_t desyncs = 0;
    uint32_t last_quit_frame = 0;
    uint64_t stalls = 0;
    double waited_ms = 0.0;
    uint64_t waited_frames = 0;
    std::atomic<uint32_t> desync_requested_frame{ 0xFFFFFFFFu };

    int filter = 0;

    // The lobby (the launcher's Online screen): a session is set up before the game starts.
    //   phase: 0 nothing, 1 a client is connecting, 2 in the lobby (the host listening, or a client joined), 3 it didn't work
    int lobby_phase = 0;
    std::string lobby_message;                   // what went wrong, or what the lobby is doing
    std::atomic<uint32_t> lobby_gen{ 0 };        // changes when the lobby is left: threads of an older one stop
    bool lobby_locked = false;                   // host: the game has begun, nobody else joins
    bool game_launch = false;                    // client: the host started the game, the launcher should too
    uint8_t lobby_mask = 0;                      // bit per player slot that is in the lobby
    int lobby_input = 0;                         // the Controller choice (0 any, 1 keyboard, 2 gamepad)
    // Nicknames: only for showing (the lobby, the chat, the notices), not part of the game. Empty: "Player N".
    std::string my_name;
    std::array<std::string, 4> names;
    std::vector<std::unique_ptr<Peer>> graveyard;

    // Text chat: it doesn't touch the lockstep. A line goes to the host, which sends it on to everybody (the sender included,
    // so all see the lines in the same order); each machine's UI takes what has come in.
    std::deque<std::pair<int, std::string>> chat_inbox;       // (player slot, text)
    clock::time_point chat_last[max_players][5];                // when each player's last five lines were
    int chat_next[max_players] = {};   // peers let go of: their reader threads still point at them
    uint32_t test_desync_frame = 0xFFFFFFFFu;   // CONKER_NET_TEST_DESYNC: a client upsets its own RNG at that frame

    FILE* log_file = nullptr;
    std::string log_path;   // where it is (for the lobby to say)
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

    void open_log(const std::string& default_name) {
        if (log_file != nullptr) {
            return;
        }
        const char* env = SDL_getenv("CONKER_NET_LOG");
        const std::string path = (env != nullptr && env[0] != 0) ? std::string(env) : default_name;
        if (!path.empty()) {
            // Beside the game, or where it was started from; if that can't be written to (a protected folder), in the
            // folder of the settings.
            // (the last run's log is kept beside it, as <name>.previous.txt: a new run would overwrite it)
            {
                std::error_code rename_error;
                const std::filesystem::path current_log(path);
                if (std::filesystem::exists(current_log, rename_error)) {
                    std::filesystem::path older = current_log;
                    older.replace_extension(".previous.txt");
                    std::filesystem::remove(older, rename_error);
                    std::filesystem::rename(current_log, older, rename_error);
                }
            }
            log_file = std::fopen(path.c_str(), "w");
            std::error_code ec;
            if (log_file != nullptr) {
                log_path = std::filesystem::absolute(path, ec).string();
            }
            else {
                const std::filesystem::path other = recomp::get_config_path() / std::filesystem::path(path).filename();
                log_file = std::fopen(other.string().c_str(), "w");
                if (log_file != nullptr) {
                    log_path = other.string();
                }
            }
        }
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
    // seen to differ between the machines for no reason that matters (flag bits, a counter): offsets 0x0C, 0x6C, 0x134, 0x8C (the last one differed alone, RNG and positions still equal, in every object-hash
    // resync of the 0.9.4 test, often for tens of frames before anything else did).
    uint32_t objects_hash(int first, int count) {
        uint64_t h = 0x9E3779B97F4A7C15ull;
        for (int i = first; i < first + count; i++) {
            const uint8_t* p = ram + ((0x800CC2D0u + (uint32_t)i * 0x32Cu) & 0x7FFFFFu);
            for (uint32_t off = 0; off < 0x328; off += 4) {
                uint32_t v;
                std::memcpy(&v, p + off, 4);
                if ((v >= 0x80000000u && v < 0x80800000u) || off == 0x0C || off == 0x6C || off == 0x134 || off == 0x8C) {
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

    // The memory comparisons (DIAG): seven copies of the host's memory sent to a client whenever their games differ. Only with
    // CONKER_NET_DIAG=1 (a test for finding a drift's cause): in a normal game they took the connection for seconds at a time.
    bool diag_env_on() {
        static const bool on = [] {
            const std::string v = get_env("CONKER_NET_DIAG");
            return !v.empty() && v != "0";
        }();
        return on;
    }
    // (0.10.74, measuring the co-op pause's drift) in the co-op stage the memory comparison is on too: the copies every frame, and the
    // first four comparisons asked for
    // (0.10.90: back to CONKER_NET_DIAG only; the co-op pause's drift is found and fixed)
    bool diag_copies_on() {
        return diag_env_on();
    }
    bool diag_dumps_on() {
        return diag_env_on();
    }

    // --- Input delay ---
    // The host's choice, or (auto_delay) worked out by the host from the round trips and changed during the game when they change.
    // Each machine puts its own input in for frame f + delay at frame f; when the delay grows, the frames in between get the same
    // input (so none is missing), and when it shrinks, an input whose frame was sent for already waits for the next one with what it
    // carries that mustn't be lost. So the machines needn't change the delay at the same frame: each one's inputs always cover every
    // frame, and the frames' inputs are still the host's alone.
    bool auto_delay = false;
    bool have_target = false;        // this machine has sent its input for every frame up to last_target
    uint32_t last_target = 0;
    PlayerInput carry;               // finished sounds and a QUIT from inputs that had no frame of their own
    int delay_up_seconds = 0, delay_down_seconds = 0;   // host: seconds in a row the round trips have called for more, or fewer, frames
    constexpr int auto_delay_min = 2, auto_delay_max = 10;
    // (0.10.14: Auto's delay is worked out as the game starts and kept; true would have it follow the round trips during the game again)
    constexpr bool delay_follows_round_trips = false;

    // --- Resyncs by the pieces that differ ---
    // A client whose game has drifted asks; the host names it in the next frame it sends (FrameInputs::snap); at that frame every
    // machine named, and the host, takes a copy of its game. The client sends a hash of each 2 KB piece of its copy, the host sends the
    // pieces of its own that differ (with a hash of the whole, to check it), and the client puts them in its copy and loads that.
    // The two copies are of the same frame of the same game, so only what drifted and what each machine has of its own (its view,
    // its sound) differs: a few hundred KB where a whole snapshot was 8 MB (1.6 MB packed).
    constexpr uint32_t page_bytes = 2048;
    uint8_t snap_order_pending = 0;      // host: the players to name in the next frame sent
    struct SnapRound {
        bool active = false;
        uint32_t order_frame = 0;
        uint8_t waiting = 0;             // the players still to be sent their pieces
        bool captured = false;           // the host's copy is taken (state empty: it couldn't be)
        uint32_t at_frame = 0;
        std::shared_ptr<const std::vector<uint8_t>> state;
        std::array<std::vector<uint32_t>, max_players> hashes;
        std::array<bool, max_players> hashes_in{};
        std::array<uint32_t, max_players> base_frame{};
        std::array<uint32_t, max_players> base_size{};
        clock::time_point began{};
    };
    SnapRound snap_round;                // host
    uint8_t full_next = 0;               // host: players whose next resync is a whole snapshot (the pieces didn't work for them)
    std::array<clock::time_point, max_players> resync_sent_at{};   // host: when each player was last sent a snapshot (it goes back in time to load it)
    clock::time_point last_round_end{};  // host
    struct ClientBase {
        bool busy = false;               // being taken
        bool have = false;
        uint32_t at_frame = 0;
        std::vector<uint8_t> state;
    };
    ClientBase snap_base;                // client: its own copy, for the host's pieces to go in
    bool want_full = false;              // client: the next resync it asks for is a whole snapshot
    constexpr uint32_t stale_snapshot_frames = 300;   // client: a snapshot this far behind (10 s) isn't loaded, a new one is asked for

    uint32_t piece_hash(const uint8_t* p, size_t n) {
        uint64_t h = 0xCBF29CE484222325ull ^ (uint64_t)n;
        size_t i = 0;
        for (; i + 8 <= n; i += 8) {
            uint64_t v;
            std::memcpy(&v, p + i, 8);
            h = (h ^ v) * 0x100000001B3ull;
            h ^= h >> 31;
        }
        for (; i < n; i++) {
            h = (h ^ p[i]) * 0x100000001B3ull;
        }
        return (uint32_t)(h ^ (h >> 32));
    }

    uint64_t whole_hash(const std::vector<uint8_t>& s) {
        uint64_t a = 0x9E3779B97F4A7C15ull ^ (uint64_t)s.size(), b = 0xD6E8FEB86659FD93ull;
        size_t i = 0;
        for (; i + 8 <= s.size(); i += 8) {
            uint64_t v;
            std::memcpy(&v, s.data() + i, 8);
            a = (a ^ v) * 0x100000001B3ull;
            a ^= a >> 29;
            b = (b + v) * 0xFF51AFD7ED558CCDull;
            b ^= b >> 33;
        }
        for (; i < s.size(); i++) {
            a = (a ^ s[i]) * 0x100000001B3ull;
            b = (b + s[i]) * 0xFF51AFD7ED558CCDull;
        }
        return a ^ (b * 0xC4CEB9FE1A85EC53ull);
    }

    void on_resync_requested(int slot, uint32_t frame, uint32_t diverged, bool full);
    void on_snapshot_message(const std::vector<uint8_t>& payload);
    void on_snap_order(uint32_t f, uint8_t mask);
    void on_snap_hashes(int slot, Reader& r);
    void on_snap_diff(const std::vector<uint8_t>& payload);
    void serve_snap_round();
    void start_capture_locked();
    void set_delay_locked(uint32_t d, const char* why);
    void on_check(uint32_t f, CheckInfo&& theirs);
    void compare_check(uint32_t f, const CheckInfo& mine, const CheckInfo& theirs);
    void request_resync(uint32_t f);
    void diag_check_frame(uint32_t f);
    void begin_diag(uint32_t f, const char* why);
    void on_diag_request(uint32_t f, Peer* asker);
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

    // Mutex held. Whether the game must wait for this player's input for frame f.
    bool required_at(int s, uint32_t f) {
        if (!connected[s]) {
            return false;
        }
        if (!joining[s]) {
            return true;
        }
        if (join_from[s] != 0xFFFFFFFFu && f >= join_from[s]) {
            joining[s] = false;   // from here on the player is part of the game
            return true;
        }
        return false;
    }

    // Mutex held. Sends every frame that all connected players' inputs are in for.
    void try_broadcast() {
        while (true) {
            const uint32_t f = next_broadcast;
            auto it = inbox.find(f);
            FrameInputs fi;
            bool complete = true;
            if (dedicated && host_notes.find(f) == host_notes.end()) {
                return;   // the server's own part (its notes of finished sounds) for this frame isn't in yet
            }
            for (int s = 0; s < max_players && complete; s++) {
                if (!required_at(s, f)) {
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
            if (dedicated) {
                fi.p[0].ended = host_notes[f];
                host_notes.erase(f);
            }
            if (snap_order_pending != 0 && !snap_round.active && clock::now() - last_round_end > std::chrono::milliseconds(500)) {
                // the players who asked for a resync: named in this frame, every machine takes its copy after it
                uint8_t mask = 0;
                for (int s = 0; s < max_players; s++) {
                    if ((snap_order_pending & (1u << s)) && connected[s] && !joining[s]) {
                        mask |= (uint8_t)(1u << s);
                    }
                }
                snap_order_pending = 0;
                if (mask != 0) {
                    fi.snap = mask;
                    snap_round = SnapRound{};
                    snap_round.active = true;
                    snap_round.order_frame = f;
                    snap_round.waiting = mask;
                    snap_round.began = clock::now();
                    log_line("[net] resync of players (mask %u) at frame %u: every machine takes a copy of its game after it", (unsigned)mask, f);
                }
            }
            ready[f] = fi;
            Writer w(Frame);
            w.put(f);
            for (int s = 0; s < max_players; s++) {
                w.put_input(fi.p[s]);
            }
            w.put(fi.snap);
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
        input_since_join[slot] = true;
        try_broadcast();
    }

    // Mutex held. Who is in the lobby, told to everybody in it.
    void send_lobby_info() {
        uint8_t mask = 0;
        for (int s = 0; s < max_players; s++) {
            if (connected[s]) {
                mask |= (uint8_t)(1u << s);
            }
        }
        lobby_mask = mask;
        Writer w(LobbyInfo);
        w.put((uint8_t)players_expected).put(mask).put(delay | (auto_delay ? 0x80000000u : 0u));
        for (int s = 0; s < max_players; s++) {
            w.put_string(connected[s] ? names[s] : std::string());
        }
        w.put((uint8_t)(dedicated ? 1 : 0));
        const auto& bytes = w.finish();
        for (auto& peer : peers) {
            if (peer->alive) {
                peer->send_message(bytes);
            }
        }
    }

    // A nickname: printable characters, no leading or trailing spaces, up to 16 bytes (not in the middle of a UTF-8 character).
    std::string clean_name(const std::string& text) {
        std::string out;
        for (unsigned char c : text) {
            if (c >= 32 && c != 127) {
                out += (char)c;
            }
        }
        const size_t first = out.find_first_not_of(' ');
        out = first == std::string::npos ? std::string() : out.substr(first);
        if (out.size() > 16) {
            out.resize(16);
            while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) {
                out.pop_back();
            }
            // (a character cut in half leaves its first byte behind)
            if (!out.empty() && static_cast<unsigned char>(out.back()) >= 0xC0) {
                out.pop_back();
            }
        }
        while (!out.empty() && out.back() == ' ') {
            out.pop_back();
        }
        return out;
    }

    // A player's name for the screen. Mutex held.
    std::string name_of(int slot) {
        if (slot >= 0 && slot < max_players && !names[slot].empty()) {
            return names[slot];
        }
        return "Player " + std::to_string(slot + 1);
    }

    std::string clean_chat(std::string text) {
        std::string out;
        for (unsigned char c : text) {
            if (c >= 32 && c != 127) {
                out += (char)c;
            }
        }
        if (out.size() > 120) {
            out.resize(120);
            while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) {
                out.pop_back();   // not in the middle of a UTF-8 character
            }
        }
        return out;
    }

    // Mutex held (host). A line from a player: sent on to everybody and kept for this machine's chat.
    void host_chat(int slot, const std::string& raw) {
        const std::string text = clean_chat(raw);
        if (text.empty() || slot < 0 || slot >= max_players) {
            return;
        }
        // no more than five lines in four seconds from one player
        const auto now = clock::now();
        const auto oldest = chat_last[slot][chat_next[slot]];
        if (oldest.time_since_epoch().count() != 0 && now - oldest < std::chrono::seconds(4)) {
            return;
        }
        chat_last[slot][chat_next[slot]] = now;
        chat_next[slot] = (chat_next[slot] + 1) % 5;
        Writer w(Chat);
        w.put((uint8_t)slot).put((uint16_t)text.size()).put_bytes(text.data(), text.size());
        const auto& bytes = w.finish();
        for (auto& peer : peers) {
            if (peer->alive) {
                peer->send_message(bytes);
            }
        }
        chat_inbox.emplace_back(slot, text);
        while (chat_inbox.size() > 50) {
            chat_inbox.pop_front();
        }
        log_line("[chat] player %d: %s", slot + 1, text.c_str());
    }

    bool read_chat(Reader& r, int* slot, std::string* text) {
        *slot = r.get<uint8_t>();
        const uint16_t n = r.get<uint16_t>();
        if (r.bad || r.at + n > r.size) {
            return false;
        }
        text->assign(reinterpret_cast<const char*>(r.data + r.at), n);
        r.at += n;
        return true;
    }

    // The game the lobby starts (the save file is named for it), and this machine's save data as the game would read it: the
    // host sends it to every player with Start, and theirs plays with it (librecomp's session save), so every machine's game starts
    // from the same save, with the same setup options in every multiplayer mode (each machine used to keep its own: the radar was
    // on for one player and off for the other, and the setup menu turned it the other way on each).
    std::u8string session_game_id = u8"conker.n64.us.1.0";
    constexpr size_t eeprom_size = 0x800;
    std::vector<char> host_save_image() {
        std::vector<char> image = recomp::session_save_image(session_game_id);
        image.resize(eeprom_size, 0);
        return image;
    }

    // Mutex held (host). The input delay the round trips call for: enough frames (at 30 a second) to cover the slowest player's round
    // trip, its usual ups and downs and a little more; -1 while nobody's is known. (A player's input for frame f is read at its frame
    // f - delay, takes half a round trip to the host, and the frame takes the other half to come back: with too few frames for that,
    // every frame waits, and the game runs slower than 30 frames a second for everyone, as with 3 frames on a 111 ms link.)
    int wanted_delay_locked() {
        double worst = -1.0;
        for (auto& peer : peers) {
            if (!peer->alive || peer->retired || peer->slot < 0 || peer->slot >= max_players) {
                continue;
            }
            if (started && (!connected[peer->slot] || joining[peer->slot])) {
                continue;
            }
            double rtt;
            {
                std::lock_guard rtt_lock{ peer->rtt_mutex };
                const uint32_t size = (uint32_t)peer->rtt_recent.size();
                const uint32_t n = std::min<uint32_t>(peer->rtt_recent_n, size);
                if (n == 0) {
                    continue;
                }
                double sum = 0, sq = 0, latest = 0;
                for (uint32_t k = 0; k < n; k++) {
                    const double v = peer->rtt_recent[k];
                    sum += v;
                    sq += v * v;
                }
                for (uint32_t j = 1; j <= std::min<uint32_t>(3, n); j++) {
                    latest = std::max(latest, peer->rtt_recent[(peer->rtt_recent_n - j) % size]);
                }
                const double mean = sum / n;
                const double spread = std::sqrt(std::max(0.0, sq / n - mean * mean));
                rtt = std::max(mean + 2.0 * spread, latest);
            }
            if (peer->conn != nullptr) {
                rtt = std::max(rtt, peer->conn->rtt_ms());
            }
            worst = std::max(worst, rtt);
        }
        if (worst < 0.0) {
            return -1;
        }
        const double frame_ms = 1000.0 / 30.0;
        return std::clamp((int)std::ceil((worst + 10.0) / frame_ms), auto_delay_min, auto_delay_max);
    }

    // Mutex held (host): the delay changes now, here and for everyone (each machine at the frame the word reaches it, see auto_delay).
    void set_delay_locked(uint32_t d, const char* why) {
        if (d == delay) {
            return;
        }
        log_line("[net] f=%u input delay %u -> %u frames (%s)", frames_started, delay, d, why);
        delay = d;
        Writer w(DelayChange);
        w.put(delay);
        const auto& bytes = w.finish();
        for (auto& peer : peers) {
            if (peer->alive && !peer->retired) {
                peer->send_message(bytes);
            }
        }
    }

    // Mutex held. The first `delay` frames have nobody's input yet, so they are neutral for all.
    void host_begin_game() {
        lobby_locked = true;
        if (auto_delay) {
            const int want = wanted_delay_locked();
            delay = (uint32_t)(want > 0 ? want : 4);
            log_line("[net] input delay: %u frames (worked out from the round trips%s; kept for the game)", delay, want > 0 ? "" : ", none known yet");
        }
        have_target = true;
        last_target = delay - 1;
        carry = PlayerInput{};
        Writer s(Start);
        const std::vector<char> save = host_save_image();
        s.put(delay).put((uint8_t)(dedicated ? 1 : 0)).put((uint8_t)0).put((uint32_t)save.size()).put_bytes(save.data(), save.size());
        log_line("[net] this machine's save data (%zu bytes) goes to every player: the same setup options for all", save.size());
        for (auto& peer : peers) {
            peer->send_message(s.finish());
        }
        started = true;
        for (uint32_t f = 0; f < delay; f++) {
            if (dedicated) {
                host_notes[f] = {};
            }
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

    // Ping/Pong: every machine pings the ones it is connected to once a second; a Ping is answered at once with a Pong that
    // carries the same numbers back. It goes over the same connection as the game's data, so it shows what the inputs go through.
    int64_t ping_now_us() {
        return std::chrono::duration_cast<std::chrono::microseconds>(clock::now().time_since_epoch()).count();
    }

    // true when the message was a Ping or Pong (and has been dealt with)
    bool ping_message(Peer* peer, uint8_t type, const std::vector<uint8_t>& payload) {
        if (type != Ping && type != Pong) {
            return false;
        }
        Reader r{ payload.data(), payload.size() };
        const uint32_t seq = r.get<uint32_t>();
        const int64_t sent = r.get<int64_t>();
        if (r.bad) {
            return true;
        }
        if (type == Ping) {
            Writer w(Pong);
            w.put(seq).put(sent);
            peer->send_message(w.finish());
        }
        else {
            const double ms = (double)(ping_now_us() - sent) / 1000.0;
            std::lock_guard lock{ peer->rtt_mutex };
            peer->pongs_got++;
            peer->rtt_last = ms;
            peer->rtt_total_sum += ms;
            peer->rtt_total_n++;
            peer->rtt_total_min = std::min(peer->rtt_total_min, ms);
            peer->rtt_recent[peer->rtt_recent_n++ % peer->rtt_recent.size()] = ms;
            peer->rtt_sum += ms;
            peer->rtt_sq += ms * ms;
            peer->rtt_min = std::min(peer->rtt_min, ms);
            peer->rtt_max = std::max(peer->rtt_max, ms);
            peer->rtt_all_max = std::max(peer->rtt_all_max, ms);
            peer->rtt_count++;
        }
        return true;
    }

    // Called by the watchdog thread every second: sends the pings, and every 10 s logs what came back.
    void ping_tick(int second) {
        std::vector<Peer*> to;
        {
            std::lock_guard lock{ mtx };
            if (dedicated && is_host && started) {
                // a player who joined and hasn't taken its snapshot in a minute is let go (the server goes on without it)
                for (int k = 0; k < max_players; k++) {
                    if (connected[k] && joining[k] && join_from[k] == 0xFFFFFFFFu && clock::now() - joining_since[k] > std::chrono::seconds(90)) {
                        log_line("[net] player %d never took the snapshot of the running game, dropped", k + 1);
                        connected[k] = false;
                        joining[k] = false;
                        for (auto& peer : peers) {
                            if (peer->slot == k) {
                                peer->drop();
                            }
                        }
                    }
                }
            }
            if (!(enabled || lobby_phase == 2) || failed) {
                return;
            }
            if (is_host && auto_delay) {
                // the delay follows the round trips: up (to what they call for) once they have called for more for 2 s, down (a frame
                // at a time) once they have called for fewer for 8 s; before the game, what the lobby shows
                const int want = wanted_delay_locked();
                if (want > 0 && !started) {
                    delay = (uint32_t)want;
                }
                // 0.10.14: once the game has started, Auto keeps the delay it worked out: it no longer follows the round trips. A
                // moment's long round trip (a player's connection, a resync's copy going out) took it from 3 frames to 8 or 10 for
                // half a minute (0.10.13's 4 player game, f=1677 and f=17947), and the game felt slow all that time.
                else if (want > 0 && !delay_follows_round_trips) {
                    delay_up_seconds = delay_down_seconds = 0;
                }
                else if (want > 0) {
                    if (want > (int)delay) {
                        delay_down_seconds = 0;
                        if (++delay_up_seconds >= 2) {
                            delay_up_seconds = 0;
                            set_delay_locked((uint32_t)want, "the round trips grew");
                        }
                    }
                    else if (want < (int)delay) {
                        delay_up_seconds = 0;
                        if (++delay_down_seconds >= 8) {
                            delay_down_seconds = 0;
                            set_delay_locked(delay - 1, "the round trips shrank");
                        }
                    }
                    else {
                        delay_up_seconds = delay_down_seconds = 0;
                    }
                }
            }
            if (is_host && snap_round.active && clock::now() - snap_round.began > std::chrono::seconds(6)) {
                // a resync whose copies or hashes didn't come in time: whoever is still waiting gets the whole snapshot
                log_line("[net] the resync of frame %u is taking too long (waiting for players mask %u, the host's copy %s): whole snapshots instead",
                    snap_round.order_frame, (unsigned)snap_round.waiting, snap_round.captured ? (snap_round.state ? "taken" : "failed") : "not taken yet");
                if (!snap_round.captured) {
                    full_next |= snap_round.waiting;
                    snapshot_wanted_mask |= snap_round.waiting;
                    snap_round = SnapRound{};
                    last_round_end = clock::now();
                    if (!capture_in_flight) {
                        start_capture_locked();
                    }
                }
                else {
                    for (int k = 0; k < max_players; k++) {
                        if (snap_round.waiting & (1u << k)) {
                            snap_round.hashes_in[k] = true;
                            snap_round.hashes[k].clear();
                        }
                    }
                    serve_snap_round();
                }
            }
            for (auto& peer : peers) {
                if (peer->alive && !peer->retired) {
                    to.push_back(peer.get());
                }
            }
        }
        for (Peer* peer : to) {
            uint32_t seq;
            {
                std::lock_guard lock{ peer->rtt_mutex };
                seq = ++peer->ping_seq;
                peer->pings_sent++;
            }
            Writer w(Ping);
            w.put(seq).put((int64_t)ping_now_us());
            peer->send_message(w.finish());
            if (second % 10 != 0) {
                continue;
            }
            std::lock_guard lock{ peer->rtt_mutex };
            const uint32_t n = peer->rtt_count;
            const char* who = is_host ? "player" : "the host";
            const int number = is_host ? peer->slot + 1 : 0;
            if (n == 0) {
                log_line("[ping] %s %d: no answer yet (%u pings sent, %u answered)", who, number, peer->pings_sent, peer->pongs_got);
            }
            else {
                const double mean = peer->rtt_sum / n;
                const double var = std::max(0.0, peer->rtt_sq / n - mean * mean);
                log_line("[ping] %s %d: round trip last %.1f ms, avg %.1f, min %.1f, max %.1f, jitter %.1f (last 10 s, %u answers); %u pings sent, %u answered in all, worst ever %.1f ms",
                    who, number, peer->rtt_last, mean, peer->rtt_min, peer->rtt_max, std::sqrt(var), n, peer->pings_sent, peer->pongs_got, peer->rtt_all_max);
                if (peer->conn != nullptr) {
                    log_line("[ping]   %s %d: %s", who, number, peer->conn->stats().c_str());
                }
            }
            peer->rtt_sum = peer->rtt_sq = peer->rtt_max = 0;
            peer->rtt_min = 1e9;
            peer->rtt_count = 0;
        }
    }

    // The pings go from a thread of their own, from the first connection on (so the lobby can show them too).
    void ensure_ping_thread() {
        static std::atomic<bool> running{ false };
        if (running.exchange(true)) {
            return;
        }
        std::thread([]() {
            for (int second = 1;; second++) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                ping_tick(second);
            }
        }).detach();
    }

    // The average of the last ten round trips to a peer, in ms, or -1 when none has come back yet.
    double peer_ping_ms(Peer* peer) {
        std::lock_guard lock{ peer->rtt_mutex };
        const uint32_t n = std::min<uint32_t>(peer->rtt_recent_n, (uint32_t)peer->rtt_recent.size());
        if (n == 0) {
            return -1.0;
        }
        double sum = 0;
        for (uint32_t k = 0; k < n; k++) {
            sum += peer->rtt_recent[k];
        }
        return sum / n;
    }

    void host_reader(Peer* peer) {
        uint8_t type;
        std::vector<uint8_t> payload;
        while (peer->alive && read_message(peer->conn, type, payload)) {
            if (ping_message(peer, type, payload)) {
                continue;
            }
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
                    on_diag_request(frame, peer);
                }
            }
            else if (type == ResyncRequest) {
                const uint32_t frame = r.get<uint32_t>();
                const uint32_t diverged = r.get<uint32_t>();
                const bool full = !r.bad && r.at < r.size && r.get<uint8_t>() != 0;
                if (!r.bad) {
                    on_resync_requested(peer->slot, frame, diverged, full);
                }
            }
            else if (type == SnapHashes) {
                on_snap_hashes(peer->slot, r);
            }
            else if (type == Chat) {
                int from;
                std::string text;
                if (read_chat(r, &from, &text)) {
                    host_chat(peer->slot, text);   // (the slot in the message is ignored: it's whoever sent it)
                }
            }
        }
        std::lock_guard lock{ mtx };
        peer->alive = false;
        if (peer->retired) {
            return;
        }
        if (connected[peer->slot]) {
            log_line("[net] player %d left", peer->slot + 1);
        }
        connected[peer->slot] = false;
        joining[peer->slot] = false;
        join_from[peer->slot] = 0xFFFFFFFFu;
        if (!started) {
            names[peer->slot].clear();
            send_lobby_info();
        }
        try_broadcast();
        cv.notify_all();
    }

    void refuse(const ConnPtr& c, uint8_t reason, const std::string& text = std::string()) {
        Writer w(Refused);
        w.put(reason);
        if (!text.empty()) {
            w.put_string(text);
        }
        send_all(c, w.finish().data(), w.bytes.size());
        c->close();   // (what was queued, this message, goes out first)
    }

    // Takes players in. With auto_start (the CONKER_NET_HOST way) the game begins once everyone is here; from the lobby
    // it begins when the host presses Start, and a player may leave and another join in the meantime.
    void host_accept_loop(std::shared_ptr<conker::transport::Server> listening, uint32_t gen, bool auto_start) {
        while (true) {
            ConnPtr s = listening->accept();
            if (gen != lobby_gen.load()) {
                if (s != nullptr) {
                    s->close();
                }
                return;
            }
            if (s == nullptr) {
                std::lock_guard lock{ mtx };
                if (!auto_start) {
                    lobby_phase = 3;
                    lobby_message = "The lobby stopped listening for players.";
                }
                failed = true;
                cv.notify_all();
                return;
            }
            // a connection that says nothing must not hold the others up
            uint8_t type;
            std::vector<uint8_t> payload;
            if (!read_message(s, type, payload, 5000) || type != Hello) {
                s->close();
                continue;
            }
            Reader r{ payload.data(), payload.size() };
            const uint32_t m = r.get<uint32_t>();
            const uint32_t v = r.get<uint32_t>();
            if (r.bad || m != magic) {
                log_line("[net] a connection that isn't a player was closed");
                s->close();
                continue;
            }
            if (v != protocol_version) {
                log_line("[net] a player with a different version (%u, this is %u) was refused", (unsigned)v, (unsigned)protocol_version);
                refuse(s, 2);
                continue;
            }
            const std::string joiner_name = clean_name(r.get_string());
            // the joiner's build (after its name; an older build sends none)
            std::string joiner_build = "an older build";
            if (!r.bad && r.at < r.size) {
                const std::string heard = r.get_string();
                if (!r.bad && !heard.empty()) {
                    joiner_build = heard;
                }
            }
            if (joiner_build != CONKER_ONLINE_VERSION) {
                log_line("[net] a player on version %s was refused (this is %s)", joiner_build.c_str(), CONKER_ONLINE_VERSION);
                refuse(s, 4, CONKER_ONLINE_VERSION);
                continue;
            }
            std::unique_lock lock{ mtx };
            if (gen != lobby_gen.load()) {
                lock.unlock();
                s->close();
                return;
            }
            if (lobby_locked && !dedicated) {
                lock.unlock();
                refuse(s, 3);
                continue;
            }
            int slot = -1;
            for (int k = dedicated ? 0 : 1; k < players_expected; k++) {
                if (!connected[k]) {
                    slot = k;
                    break;
                }
            }
            if (slot < 0) {
                lock.unlock();
                log_line("[net] a player was refused: the game is full");
                refuse(s, 1);
                continue;
            }
            auto peer = std::make_unique<Peer>();
            peer->conn = s;
            peer->slot = slot;
            peer->alive = true;
            Writer w(Welcome);
            w.put((uint8_t)slot).put((uint8_t)players_expected).put(delay | (auto_delay ? 0x80000000u : 0u));
            peer->send_message(w.finish());
            log_line("[net] player %d joined", slot + 1);
            Peer* raw = peer.get();
            connected[slot] = true;
            if (started) {
                // dedicated server, game running: this player is brought up to date by a snapshot (its own request)
                joining[slot] = true;
                join_from[slot] = 0xFFFFFFFFu;
                joining_since[slot] = clock::now();
                input_since_join[slot] = false;
                for (auto& entry : inbox) {
                    entry.second[slot] = {};   // (nothing left by whoever had the slot before)
                }
            }
            {
                // two players with the same name: the later gets a number
                std::string name = joiner_name;
                const std::string base = name;
                for (int n = 2; !name.empty(); n++) {
                    bool taken = false;
                    for (int k = 0; k < max_players; k++) {
                        taken |= k != slot && connected[k] && names[k] == name;
                    }
                    if (!taken) {
                        break;
                    }
                    name = base.substr(0, 13) + " " + std::to_string(n);
                }
                names[slot] = name;
                log_line("[net] player %d is called \"%s\"", slot + 1, name.c_str());
            }
            peers.push_back(std::move(peer));
            ensure_ping_thread();
            raw->reader = std::thread(host_reader, raw);
            raw->reader.detach();
            send_lobby_info();
            if (started) {
                Writer late(Start);
                const std::vector<char> save = host_save_image();
                late.put(delay).put((uint8_t)1).put((uint8_t)1).put((uint32_t)save.size()).put_bytes(save.data(), save.size());
                raw->send_message(late.finish());
                log_line("[net] player %d joins the running game, a snapshot will bring it up to date", slot + 1);
            }
            cv.notify_all();
            if (auto_start && !started) {
                int here = 0;
                for (int k = 0; k < players_expected; k++) {
                    here += connected[k] ? 1 : 0;
                }
                if (here >= players_expected) {
                    host_begin_game();
                    return;
                }
            }
        }
    }

    // --- Client ---

    void client_reader(Peer* peer) {
        uint8_t type;
        std::vector<uint8_t> payload;
        while (peer->alive && read_message(peer->conn, type, payload)) {
            if (ping_message(peer, type, payload)) {
                continue;
            }
            Reader r{ payload.data(), payload.size() };
            std::lock_guard lock{ mtx };
            switch (type) {
                case Start:
                    delay = r.get<uint32_t>();
                    if (r.at < r.size) {
                        dedicated = r.get<uint8_t>() != 0;
                    }
                    if (r.at < r.size && r.get<uint8_t>() != 0) {
                        late_join = true;      // the game is running already: a snapshot of it comes first
                        late_synced = false;
                    }
                    if (r.at + 4 <= r.size) {
                        // the host's save data: this machine's game plays with it (its own save file is left as it is)
                        const uint32_t n = r.get<uint32_t>();
                        if (!r.bad && n > 0 && n <= 0x20000 && r.at + n <= r.size) {
                            std::vector<char> image(reinterpret_cast<const char*>(r.data + r.at), reinterpret_cast<const char*>(r.data + r.at + n));
                            r.at += n;
                            recomp::set_session_save(image);
                            log_line("[net] the host's save data came (%u bytes): this game plays with it, so the setup options are the host's", (unsigned)n);
                        }
                    }
                    // the first `delay` frames are the host's (nobody's input): this machine's go in from frame `delay` on; one
                    // joining a running game starts from its snapshot's frame instead
                    have_target = !late_join;
                    last_target = delay > 0 ? delay - 1 : 0;
                    carry = PlayerInput{};
                    started = true;
                    game_launch = true;
                    log_line("[net] the game starts");
                    break;
                case Chat: {
                    int from;
                    std::string text;
                    if (read_chat(r, &from, &text) && from >= 0 && from < max_players) {
                        chat_inbox.emplace_back(from, clean_chat(text));
                        while (chat_inbox.size() > 50) {
                            chat_inbox.pop_front();
                        }
                    }
                    break;
                }
                case LobbyInfo: {
                    const uint8_t expected = r.get<uint8_t>();
                    const uint8_t mask = r.get<uint8_t>();
                    const uint32_t d = r.get<uint32_t>();
                    std::array<std::string, 4> heard;
                    for (auto& name : heard) {
                        name = clean_name(r.get_string());
                    }
                    const bool dedicated_heard = !r.bad && r.at < r.size && r.get<uint8_t>() != 0;
                    if (!r.bad && expected >= 2 && expected <= max_players) {
                        players_expected = expected;
                        lobby_mask = mask;
                        delay = d & 0xFFu;
                        auto_delay = (d & 0x80000000u) != 0;   // (the host works it out from the round trips)
                        names = heard;
                        dedicated = dedicated_heard;
                    }
                    break;
                }
                case Frame: {
                    const uint32_t f = r.get<uint32_t>();
                    FrameInputs fi;
                    for (int s = 0; s < max_players; s++) {
                        fi.p[s] = r.get_input();
                    }
                    fi.snap = r.get<uint8_t>();
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
                case SnapDiff:
                    on_snap_diff(payload);
                    break;
                case DelayChange: {
                    const uint32_t d = r.get<uint32_t>();
                    if (!r.bad && d >= 1 && d <= 30 && d != delay) {
                        log_line("[net] f=%u the host changed the input delay: %u -> %u frames", frames_started, delay, d);
                        delay = d;
                    }
                    break;
                }
                case FrameRngMsg: {
                    const uint32_t f = r.get<uint32_t>();
                    const uint64_t v = r.get<uint64_t>();
                    const uint32_t oh = r.get<uint32_t>();
                    const uint32_t ch = r.get<uint32_t>();
                    const uint32_t sh = r.get<uint32_t>();
                    if (!r.bad) {
                        host_frame_snd[f] = sh;
                        if (host_frame_snd.size() > 128) {
                            host_frame_snd.erase(host_frame_snd.begin());
                        }
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
        if (peer->retired) {
            peer->alive = false;
            return;
        }
        if (!started) {
            // still in the lobby
            if (lobby_phase == 2) {
                log_line("[net] the host closed the lobby");
                lobby_phase = 3;
                lobby_message = "The host closed the lobby.";
            }
            peer->alive = false;
            cv.notify_all();
            return;
        }
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
        // (one at a time: the pieces come in well under a second, a whole snapshot on a slow link in a few)
        if (resync_in_flight && clock::now() - resync_asked_at < std::chrono::seconds(8)) {
            return;
        }
        if (!peers.empty() && peers[0]->alive) {
            resync_in_flight = true;
            resync_asked_at = clock::now();
            Writer w(ResyncRequest);
            w.put(f).put(last_diverged_frame).put((uint8_t)(want_full ? 1 : 0));
            want_full = false;
            last_diverged_frame = 0;
            peers[0]->send_message(w.finish());
        }
    }

    // --- Resync (the snapshot itself is conker::netsession's resync part, below) ---

    // Connects to the host and takes the slot it gives. gen is the lobby's generation (0: not from the lobby); on false,
    // error says why.
    bool connect_to_host(const std::string& host, const std::string& port, int timeout_seconds, uint32_t gen, std::string* error) {
        const auto fail = [&](const std::string& text) {
            if (error != nullptr) {
                *error = text;
            }
            log_line("[net] %s", text.c_str());
            return false;
        };
        const auto cancelled = [&]() { return gen != 0 && gen != lobby_gen.load(); };
        int failure = 0;
        ConnPtr s = conker::transport::connect(host, (uint16_t)std::atoi(port.c_str()), timeout_seconds * 1000, cancelled, &failure);
        if (s == nullptr) {
            if (failure == 3) {
                return false;
            }
            if (failure == 1) {
                return fail("Couldn't find \"" + host + "\". Check the address.");
            }
            return fail("Couldn't reach " + host + ":" + port + ". Is the host's lobby open, and is the UDP port " + port + " forwarded on its router (and allowed in its firewall)?");
        }
        if (cancelled()) {
            s->close();
            return false;
        }
        Writer hello(Hello);
        hello.put(magic).put(protocol_version).put_string(my_name).put_string(CONKER_ONLINE_VERSION);
        if (!send_all(s, hello.finish().data(), hello.bytes.size())) {
            s->close();
            return fail("The host closed the connection.");
        }
        uint8_t type;
        std::vector<uint8_t> payload;
        if (!read_message(s, type, payload, 15000)) {
            s->close();
            return fail("The host didn't answer. It may be running a different version of the game.");
        }
        if (type == Refused) {
            const uint8_t reason = payload.empty() ? 0 : payload[0];
            s->close();
            if (reason == 4) {
                Reader heard{ payload.data() + 1, payload.size() - 1 };
                const std::string host_build = heard.get_string();
                return fail("The host is on version " + (host_build.empty() ? std::string("?") : host_build) + " and you are on version " + CONKER_ONLINE_VERSION +
                    ". You both need the same version: get the newer one from the releases page.");
            }
            return fail(reason == 1 ? "The game is full." : reason == 2 ? "The host has a different version of the game. Both of you need the same build."
                : reason == 3 ? "The game has already started." : "The host refused the connection.");
        }
        if (type != Welcome) {
            s->close();
            return fail("The host didn't answer as expected. It may be running a different version of the game.");
        }
        Reader r{ payload.data(), payload.size() };
        const int slot = r.get<uint8_t>();
        const int expected = r.get<uint8_t>();
        const uint32_t d = r.get<uint32_t>();
        if (r.bad || slot < 0 || slot >= max_players) {   // (0 is allowed: on a dedicated server the first player takes it)
            s->close();
            return fail("The host sent something this version doesn't understand.");
        }
        auto peer = std::make_unique<Peer>();
        peer->conn = s;
        peer->slot = 0;
        peer->alive = true;
        Peer* raw = peer.get();
        std::lock_guard lock{ mtx };
        if (cancelled()) {
            s->close();
            return false;
        }
        my_slot = slot;
        players_expected = expected;
        delay = d & 0xFFu;
        auto_delay = (d & 0x80000000u) != 0;
        peers.push_back(std::move(peer));
        ensure_ping_thread();
        raw->reader = std::thread(client_reader, raw);
        raw->reader.detach();
        log_line("[net] joined as player %d of %d, delay %u", my_slot + 1, players_expected, delay);
        return true;
    }

    // --- Bringing the session up ---

    bool net_startup() {
        return conker::transport::startup();
    }

    // Listens on the (UDP) port. The accept loop is started by the caller (it needs the listener).
    bool start_host(uint16_t port, std::string* error) {
        std::string why;
        listener = conker::transport::Server::listen(port, &why);
        if (listener == nullptr) {
            log_line("[net] couldn't listen on UDP port %u: %s", (unsigned)port, why.c_str());
            if (error != nullptr) {
                *error = why;
            }
            return false;
        }
        connected[0] = !dedicated;   // (a dedicated server has no player: the first to join takes slot 0)
        log_line("[net] %s on port %u, waiting for %d players", dedicated ? "dedicated server" : "hosting", (unsigned)port, players_expected);
        return true;
    }
}

// --- The game's side ---

bool conker::netsession::init(uint8_t* rdram, bool (*local_sampler)(uint16_t*, float*, float*)) {
    {
        // set up in the lobby (the launcher's Online screen): the players are here and the host has started the game
        std::lock_guard lock{ mtx };
        if (lobby_phase == 2 && started && !enabled) {
            ram = rdram;
            sampler = local_sampler;
            filter = lobby_input;
            enabled = true;
            log_line("[net] the game begins: player %d of %d, delay %u, controller choice %d", my_slot + 1, players_expected, delay, filter);
            std::fprintf(stderr, "[net] online session from the lobby: %s, slot %d\n", is_host ? "hosting" : "joined", my_slot + 1);
            return true;
        }
    }
    const std::string host_port = get_env("CONKER_NET_HOST");
    const std::string join = get_env("CONKER_NET_JOIN");
    // (0.9.83: the dedicated server is gone, launcher and --dedicated both; what is left of it here never turns on)
    dedicated = false;
    if (host_port.empty() && join.empty()) {
        return false;
    }
    open_log("");
    ram = rdram;
    sampler = dedicated ? nullptr : local_sampler;   // (a dedicated server has no controller of its own to read)
    const std::string delay_text = get_env("CONKER_NET_DELAY");
    if (!delay_text.empty()) {
        // (0 or "auto": worked out from the round trips, see wanted_delay_locked)
        auto_delay = delay_text == "0" || delay_text == "auto";
        delay = auto_delay ? 4u : std::clamp<uint32_t>((uint32_t)std::strtoul(delay_text.c_str(), nullptr, 10), 1, 30);
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
        ok = start_host((uint16_t)std::atoi(host_port.c_str()), nullptr);
        if (ok && dedicated) {
            // a dedicated server's game runs from the start, with or without players; whoever joins is brought up to date
            std::lock_guard lock{ mtx };
            host_begin_game();
        }
        if (ok) {
            std::thread(host_accept_loop, listener, lobby_gen.load(), true).detach();
        }
    }
    else {
        const size_t colon = join.rfind(':');
        if (colon == std::string::npos) {
            std::fprintf(stderr, "[net] CONKER_NET_JOIN must be <host>:<port>\n");
            return false;
        }
        ok = connect_to_host(join.substr(0, colon), join.substr(colon + 1), 60, 0, nullptr);
    }
    if (!ok) {
        std::fprintf(stderr, "[net] the online session could not be set up (see CONKER_NET_LOG)\n");
        return false;
    }
    enabled = true;
    std::fprintf(stderr, "[net] online session: %s, slot %d\n", is_host ? "hosting" : "joined", my_slot + 1);
    return true;
}

// --- The lobby: the session is set up before the game starts ---

void conker::netsession::set_game_id(const std::u8string& game_id) {
    std::lock_guard lock{ mtx };
    session_game_id = game_id;
}

void conker::netsession::lobby_leave() {
    std::lock_guard lock{ mtx };
    if (enabled) {
        return;   // a game is running
    }
    recomp::clear_session_save();   // (a host's save data that came before the game started: this machine's own again)
    ++lobby_gen;
    for (auto& peer : peers) {
        peer->retired = true;
        peer->drop();
        graveyard.push_back(std::move(peer));
    }
    peers.clear();
    if (listener != nullptr) {
        listener->stop();   // (the port is free again when this returns)
        listener.reset();
    }
    connected = {};
    inbox.clear();
    ready.clear();
    next_broadcast = 0;
    started = false;
    failed = false;
    game_launch = false;
    late_join = late_synced = false;
    lobby_locked = false;
    lobby_mask = 0;
    lobby_phase = 0;
    lobby_message.clear();
    is_host = false;
    my_slot = 0;
    cv.notify_all();
}

void conker::netsession::lobby_open(const LobbySettings& cfg) {
    lobby_leave();
    static const bool network_ok = net_startup();
    open_log(cfg.host ? "netplay_host.txt" : "netplay_join.txt");
    std::lock_guard lock{ mtx };
    if (enabled) {
        lobby_phase = 3;
        lobby_message = "An online game has been played in this run already. Close the program and start it again to play another.";
        return;
    }
    if (!network_ok) {
        lobby_phase = 3;
        lobby_message = "The network couldn't be started.";
        return;
    }
    const uint32_t gen = ++lobby_gen;
    is_host = cfg.host;
    lobby_input = std::clamp(cfg.input, 0, 2);
    my_name = clean_name(cfg.nickname);
    names = {};
    if (cfg.host) {
        names[0] = my_name;
    }
    players_expected = std::clamp(cfg.players, 2, max_players);
    auto_delay = cfg.delay <= 0;   // (0: Auto, worked out from the round trips)
    delay = auto_delay ? 4u : (uint32_t)std::clamp(cfg.delay, 1, 30);
    log_line("[net] build %s, protocol %d, lobby: %s, %d players, delay %u", CONKER_ONLINE_VERSION, (int)protocol_version, cfg.host ? "hosting" : "joining", players_expected, delay);
    if (cfg.host) {
        my_slot = 0;
        std::string error;
        if (!start_host((uint16_t)std::clamp(cfg.port, 1, 65535), &error)) {
            lobby_phase = 3;
            lobby_message = error;
            return;
        }
        lobby_phase = 2;
        lobby_mask = 1;
        std::thread(host_accept_loop, listener, gen, false).detach();
        return;
    }
    std::string host = cfg.address, port = std::to_string(std::clamp(cfg.port, 1, 65535));
    const size_t colon = host.rfind(':');
    if (colon != std::string::npos && host.find(':') == colon) {
        port = host.substr(colon + 1);
        host = host.substr(0, colon);
    }
    while (!host.empty() && host.back() == ' ') {
        host.pop_back();
    }
    while (!host.empty() && host.front() == ' ') {
        host.erase(host.begin());
    }
    if (host.empty()) {
        lobby_phase = 3;
        lobby_message = "Type the host's address first.";
        return;
    }
    lobby_phase = 1;
    lobby_message = "Connecting to " + host + ":" + port + "...";
    std::thread([host, port, gen]() {
        std::string error;
        const bool ok = connect_to_host(host, port, 12, gen, &error);
        std::lock_guard lock{ mtx };
        if (gen != lobby_gen.load()) {
            return;
        }
        if (ok) {
            lobby_phase = (!peers.empty() && peers[0]->alive) ? 2 : 3;
            lobby_message = lobby_phase == 2 ? "" : "The host closed the lobby.";
        }
        else {
            lobby_phase = 3;
            lobby_message = error;
        }
    }).detach();
}

conker::netsession::LobbyStatus conker::netsession::lobby_status() {
    std::lock_guard lock{ mtx };
    static clock::time_point last_info;
    if (is_host && lobby_phase == 2 && !started && clock::now() - last_info > std::chrono::seconds(2)) {
        last_info = clock::now();
        send_lobby_info();   // also finds players whose connection died quietly
    }
    LobbyStatus st;
    st.phase = lobby_phase;
    st.host = is_host;
    st.slot = my_slot;
    st.expected = players_expected;
    st.delay = (int)delay;
    st.auto_delay = auto_delay;
    st.message = lobby_message;
    st.names = names;
    st.log_path = log_path;
    uint8_t mask = 0;
    if (is_host) {
        for (int k = 0; k < max_players; k++) {
            if (connected[k]) {
                mask |= (uint8_t)(1u << k);
            }
        }
    }
    else {
        mask = lobby_mask;
    }
    st.mask = mask;
    int here = 0;
    for (int k = 0; k < max_players; k++) {
        here += (mask >> k) & 1;
    }
    st.can_start = is_host && lobby_phase == 2 && !started && here >= players_expected;
    st.launch = !is_host && game_launch;
    st.dedicated = dedicated;
    st.ping_ms.fill(-1.0f);
    for (auto& peer : peers) {
        if (peer->alive && !peer->retired) {
            const int who = is_host ? peer->slot : 0;   // a client sees the host's
            if (who >= 0 && who < max_players) {
                st.ping_ms[who] = (float)peer_ping_ms(peer.get());
            }
        }
    }
    return st;
}

bool conker::netsession::lobby_start() {
    std::lock_guard lock{ mtx };
    if (!is_host || lobby_phase != 2 || started) {
        return false;
    }
    int here = 0;
    for (int k = 0; k < max_players; k++) {
        here += connected[k] ? 1 : 0;
    }
    if (here < players_expected) {
        return false;
    }
    host_begin_game();
    return true;
}

bool conker::netsession::chat_available() {
    std::lock_guard lock{ mtx };
    return enabled && started && !failed;
}

// The /network chat command: lines about the connection (to the host, or from the host to each player).
std::vector<std::string> conker::netsession::network_report() {
    std::lock_guard lock{ mtx };
    std::vector<std::string> out;
    if (!enabled || !started || failed) {
        out.push_back("Network: no online game is running.");
        return out;
    }
    for (auto& peer : peers) {
        if (!peer->alive || peer->retired) {
            continue;
        }
        std::lock_guard rtt{ peer->rtt_mutex };
        const std::string who = is_host ? name_of(peer->slot) : std::string("the host");
        if (peer->rtt_total_n == 0) {
            out.push_back("Network: no answer from " + who + " yet.");
            continue;
        }
        char text[200];
        const uint32_t recent_n = std::min<uint32_t>(peer->rtt_recent_n, (uint32_t)peer->rtt_recent.size());
        double recent = 0;
        for (uint32_t k = 0; k < recent_n; k++) {
            recent += peer->rtt_recent[k];
        }
        recent /= std::max<uint32_t>(recent_n, 1);
        std::snprintf(text, sizeof(text), "Network: ping to %s averages %.0f ms (now %.0f, best %.0f, worst %.0f), input delay %u frames%s",
            who.c_str(), peer->rtt_total_sum / (double)peer->rtt_total_n, recent, peer->rtt_total_min, peer->rtt_all_max, (unsigned)delay, auto_delay ? " (auto)" : "");
        out.push_back(text);
    }
    if (out.empty()) {
        out.push_back("Network: nobody else is connected.");
    }
    return out;
}

bool conker::netsession::chat_send(const std::string& text) {
    std::lock_guard lock{ mtx };
    if (!enabled || !started || failed) {
        return false;
    }
    if (is_host) {
        host_chat(0, text);
        return true;
    }
    const std::string clean = clean_chat(text);
    if (clean.empty() || peers.empty() || !peers[0]->alive) {
        return false;
    }
    Writer w(Chat);
    w.put((uint8_t)my_slot).put((uint16_t)clean.size()).put_bytes(clean.data(), clean.size());
    return peers[0]->send_message(w.finish());
}

std::vector<std::pair<int, std::string>> conker::netsession::chat_poll() {
    std::lock_guard lock{ mtx };
    std::vector<std::pair<int, std::string>> lines(chat_inbox.begin(), chat_inbox.end());
    chat_inbox.clear();
    return lines;
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

std::string conker::netsession::player_name(int slot) {
    std::lock_guard lock{ mtx };
    return name_of(slot);
}

uint8_t conker::netsession::look_flags(int player) {
    std::lock_guard lock{ mtx };
    return (enabled && !failed && player >= 0 && player < max_players) ? current.p[player].look : 0;
}

bool conker::netsession::swap_flag(int player) {
    std::lock_guard lock{ mtx };
    return enabled && !failed && player >= 0 && player < max_players && current.p[player].swap != 0;
}

bool conker::netsession::camera_turn(int player, float* yaw, float* pitch) {
    *yaw = 0.0f;
    *pitch = 0.0f;
    std::lock_guard lock{ mtx };
    if (!enabled || failed || player < 0 || player >= max_players) {
        return false;
    }
    const PlayerInput& in = current.p[player];
    if (!in.cam_on) {
        return false;
    }
    *yaw = (float)in.cam_yaw / 10000.0f;
    *pitch = (float)in.cam_pitch / 10000.0f;
    return true;
}

void conker::netsession::log(const char* line) {
    log_line("%s", line);
}

// --- The game's sounds, the same on every machine ---
//
// The game keeps 16 sound slots (D_800425E0, 12 bytes each: u16 handle, u16 next handle, u16 sound, u32 pointer to the
// audio library's voice at +8). A slot is free once its voice has finished, and the audio thread (which plays at each
// machine's own pace) used to say so by clearing the slot's pointer itself, and answered "is it still playing?" from the
// voice. The game's logic chose slots, timers and random numbers from those answers, so the machines drifted apart.
// Now, online:
//   - the audio thread no longer touches the slots (conker_net_sound_hook 0 cuts the voice's link to its slot as the
//     voice is freed), and "is it still playing?" (func_100173C4) answers from the slot alone: yes while it has a voice
//     (conker_net_sound_hook 1);
//   - the host looks at its real voices at the start of each frame, and sends the handle of each that has ended with its
//     input; every machine, the host too, clears that slot's pointer at the start of the frame the input is for.
namespace {
    std::atomic<bool> sound_sync{ false };
    constexpr uint32_t sound_slots = 0x800425E0u;

    uint32_t ram_w(const uint8_t* base, uint32_t a) {
        uint32_t v;
        std::memcpy(&v, base + (a & 0x7FFFFFu), 4);
        return v;
    }
    uint16_t ram_h(const uint8_t* base, uint32_t a) {
        uint16_t v;
        std::memcpy(&v, base + ((a ^ 2u) & 0x7FFFFFu), 2);
        return v;
    }
    uint8_t ram_b(const uint8_t* base, uint32_t a) {
        return base[(a ^ 3u) & 0x7FFFFFu];
    }
    bool ram_pointer(uint32_t a) {
        return a >= 0x80000000u && a < 0x80800000u;
    }
}

// The audio library's list of voices in use (D_8002BA20: the first voice; each voice's first word is the next one). A freeze found in a
// long session had the game thread going round this list for ever (func_10017604, which stops sounds), so the list is looked at before
// that function walks it: if it comes back on a voice it has had (a loop), or runs to nothing that is a voice, it is cut there.
namespace {
    const uint32_t voice_list_head = 0x8002BA20u;

    // Looks at the list through `base` (the game's memory); `repair` cuts it where it is broken. Returns how many voices it has, or -1 if
    // it is a loop, -2 if it runs into something that is not a voice.
    int check_voice_list(uint8_t* base, bool repair, uint32_t* bad_at) {
        std::vector<uint32_t> seen;
        seen.reserve(64);
        uint32_t previous = 0;
        uint32_t current = ram_w(base, voice_list_head);
        while (current != 0) {
            if (!ram_pointer(current) || (current & 3u) != 0) {
                if (repair) {
                    const uint32_t zero = 0;
                    if (previous != 0) {
                        std::memcpy(base + ((previous) & 0x7FFFFFu), &zero, 4);
                    }
                    else {
                        std::memcpy(base + (voice_list_head & 0x7FFFFFu), &zero, 4);
                    }
                }
                if (bad_at != nullptr) {
                    *bad_at = current;
                }
                return -2;
            }
            if (std::find(seen.begin(), seen.end(), current) != seen.end() || seen.size() >= 4096) {
                if (repair) {
                    const uint32_t zero = 0;
                    std::memcpy(base + ((previous) & 0x7FFFFFu), &zero, 4);
                }
                if (bad_at != nullptr) {
                    *bad_at = current;
                }
                return -1;
            }
            seen.push_back(current);
            previous = current;
            current = ram_w(base, current);
        }
        return (int)seen.size();
    }
}

extern "C" void conker_net_sound_hook(uint8_t* rdram, recomp_context* ctx, int which) {
    if (which == 2) {
        // func_10017604 is about to walk the list of voices
        static int logged = 0;
        uint32_t bad = 0;
        const int count = check_voice_list(rdram, true, &bad);
        if (count < 0 && logged < 20) {
            logged++;
            conker::netsession::log("[sound] the audio library's list of voices was broken (a loop, or a link to something that is not a voice) as func_10017604 was about to go through it: cut");
            char line[160];
            std::snprintf(line, sizeof(line), "[sound]   it came to %08X (%s)", (unsigned)bad, count == -1 ? "a voice it had already passed" : "not a voice");
            conker::netsession::log(line);
        }
        return;
    }
    if (!sound_sync.load(std::memory_order_relaxed)) {
        return;
    }
    if (which == 0) {
        // func_10017298 (a voice is being freed): its link to the game's slot is dropped, so the audio thread doesn't clear it
        const uint32_t voice = (uint32_t)ctx->r4;
        if (ram_pointer(voice)) {
            const uint32_t zero = 0;
            std::memcpy(rdram + ((voice + 0x38u) & 0x7FFFFFu), &zero, 4);
        }
    }
    else if (which == 3) {
        // func_1000F44C returns (its argument, the handle, saved at 0x18($sp)): started while the slot has this handle and a voice
        const uint16_t handle = ram_h(rdram, (uint32_t)ctx->r29 + 0x1Au);
        const uint32_t slot = sound_slots + (uint32_t)(handle & 0xFu) * 12u;
        ctx->r2 = (ram_w(rdram, slot + 8u) != 0 && ram_h(rdram, slot) == handle) ? 1 : 0;
    }
    else if (which == 1) {
        // func_100173C4 returns (its argument, saved at 0x20($sp), is the address of a slot's pointer): playing while the slot has a voice
        const uint32_t arg = ram_w(rdram, (uint32_t)ctx->r29 + 0x20u);
        if (arg >= sound_slots + 8u && arg < sound_slots + 8u + 16u * 12u && (arg - sound_slots - 8u) % 12u == 0) {
            ctx->r2 = ram_w(rdram, arg) != 0 ? 1 : 0;
        }
    }
}

// func_1001C224 (alEvtqPostEvent) is about to queue an event: online, one for the sound player about a voice that is on its list
// of free voices is dropped (see conker.toml). The free list: D_8002BA28, each voice's first word the next one.
extern "C" int conker_net_sound_gate(uint8_t* rdram, recomp_context* ctx) {
    if (!sound_sync.load(std::memory_order_relaxed)) {
        return 0;
    }
    const uint32_t player = ram_w(rdram, 0x8002BA2Cu);
    if (!ram_pointer(player) || (uint32_t)ctx->r4 != player + 0x14u) {
        return 0;
    }
    const uint32_t event = (uint32_t)ctx->r5;
    if (!ram_pointer(event)) {
        return 0;
    }
    const uint32_t voice = ram_w(rdram, event + 4u);
    if (!ram_pointer(voice)) {
        return 0;
    }
    uint32_t current = ram_w(rdram, 0x8002BA28u);
    for (int n = 0; n < 512 && current != 0 && ram_pointer(current); n++) {
        if (current == voice) {
            static int logged = 0;
            if (logged < 30) {
                logged++;
                char line[200];
                std::snprintf(line, sizeof(line), "[sound] an event (type %04X) for voice %08X, which is free, was dropped: acted on, it would have freed the voice a second time",
                    (unsigned)ram_h(rdram, event), (unsigned)voice);
                conker::netsession::log(line);
            }
            return 1;
        }
        current = ram_w(rdram, current);
    }
    return 0;
}

// --- A watch on the game thread (for a freeze found in a long session: both machines stopped, with nothing in the log) ---
// A thread of its own looks once a second at when the game thread last started a frame. If it has been still for a few seconds
// it logs where the game thread is (the addresses on its stack that are in this program, with their names if the symbols are
// at hand), and whether it is in the wait for the other machine's inputs.
namespace {
    std::atomic<uint32_t> watch_frame{ 0 };
    std::atomic<int64_t> watch_last_ms{ 0 };
    std::atomic<int> watch_waiting{ 0 };
    std::atomic<bool> watch_started{ false };
#ifdef _WIN32
    HANDLE watch_game_thread = nullptr;
#endif

    int64_t watch_now_ms() {
        return (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    typedef BOOL(WINAPI* SymInitializeFn)(HANDLE, PCSTR, BOOL);
    typedef BOOL(WINAPI* SymFromAddrFn)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);

    void watch_log_game_thread_stack() {
        if (watch_game_thread == nullptr) {
            return;
        }
        static SymInitializeFn sym_initialize = nullptr;
        static SymFromAddrFn sym_from_addr = nullptr;
        static bool symbols_tried = false;
        if (!symbols_tried) {
            symbols_tried = true;
            HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
            if (dbghelp != nullptr) {
                sym_initialize = (SymInitializeFn)(void*)GetProcAddress(dbghelp, "SymInitialize");
                sym_from_addr = (SymFromAddrFn)(void*)GetProcAddress(dbghelp, "SymFromAddr");
                if (sym_initialize != nullptr && !sym_initialize(GetCurrentProcess(), nullptr, TRUE)) {
                    sym_from_addr = nullptr;
                }
            }
        }
        CONTEXT context;
        std::memset(&context, 0, sizeof(context));
        context.ContextFlags = CONTEXT_CONTROL;
        if (SuspendThread(watch_game_thread) == (DWORD)-1) {
            return;
        }
        const bool got = GetThreadContext(watch_game_thread, &context) != 0;
        std::vector<uint64_t> stack;
        if (got) {
            stack.resize(4096);
            SIZE_T read = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)context.Rsp, stack.data(), stack.size() * sizeof(uint64_t), &read)) {
                stack.resize(read / sizeof(uint64_t));
            }
            else {
                stack.resize(read / sizeof(uint64_t));
            }
        }
        const uint64_t rip = got ? context.Rip : 0;
        ResumeThread(watch_game_thread);
        if (!got) {
            return;
        }
        const uint8_t* image = (const uint8_t*)GetModuleHandleA(nullptr);
        uint64_t image_size = 0;
        if (image != nullptr) {
            const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)image;
            const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(image + dos->e_lfanew);
            image_size = nt->OptionalHeader.SizeOfImage;
        }
        const auto describe = [&](uint64_t address) {
            std::string text;
            char buffer[sizeof(SYMBOL_INFO) + 256];
            std::memset(buffer, 0, sizeof(buffer));
            SYMBOL_INFO* symbol = (SYMBOL_INFO*)buffer;
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 255;
            DWORD64 displacement = 0;
            if (sym_from_addr != nullptr && sym_from_addr(GetCurrentProcess(), (DWORD64)address, &displacement, symbol)) {
                text = symbol->Name;
                text += "+0x";
                char hex[24];
                std::snprintf(hex, sizeof(hex), "%llX", (unsigned long long)displacement);
                text += hex;
            }
            return text;
        };
        {
            char line[400];
            std::snprintf(line, sizeof(line), "[net] watchdog: game thread at %llX (offset %llX) %s", (unsigned long long)rip,
                (unsigned long long)(rip - (uint64_t)(uintptr_t)image), describe(rip).c_str());
            log_line("%s", line);
        }
        int shown = 0;
        int other = 0;
        for (uint64_t value : stack) {
            if (!(value >= (uint64_t)(uintptr_t)image && value < (uint64_t)(uintptr_t)image + image_size) && value > 0x10000 && other < 24) {
                // a return address into another module (the system's wait functions, the audio library...)
                MEMORY_BASIC_INFORMATION info;
                if (VirtualQuery((LPCVOID)value, &info, sizeof(info)) != 0 && info.Type == MEM_IMAGE &&
                    (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0) {
                    char module[260] = "";
                    GetModuleFileNameA((HMODULE)info.AllocationBase, module, sizeof(module));
                    const char* name = std::strrchr(module, '\\');
                    other++;
                    char line[400];
                    std::snprintf(line, sizeof(line), "[net] watchdog:   other module %llX %s+%llX %s", (unsigned long long)value, name != nullptr ? name + 1 : module,
                        (unsigned long long)(value - (uint64_t)(uintptr_t)info.AllocationBase), describe(value).c_str());
                    log_line("%s", line);
                }
            }
            if (value >= (uint64_t)(uintptr_t)image && value < (uint64_t)(uintptr_t)image + image_size && shown < 60) {
                shown++;
                char line[400];
                std::snprintf(line, sizeof(line), "[net] watchdog:   on its stack %llX (offset %llX) %s", (unsigned long long)value,
                    (unsigned long long)(value - (uint64_t)(uintptr_t)image), describe(value).c_str());
                log_line("%s", line);
            }
        }
    }
#endif

    void watch_main() {
        uint32_t reported_frame = 0xFFFFFFFFu;
        int reports = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const uint32_t frame = watch_frame.load();
            const int64_t last = watch_last_ms.load();
            if (last == 0) {
                continue;
            }
            const int64_t idle = watch_now_ms() - last;
            if (frame != reported_frame) {
                reported_frame = frame;
                reports = 0;
            }
            {
                // (offline after the session ended, the game no longer goes through the session's frames: nothing to watch)
                std::lock_guard lock{ mtx };
                if (failed || !enabled) {
                    continue;
                }
            }
            if (idle >= 4000 && reports < 6 && idle >= 4000 + (int64_t)reports * 4000) {
                reports++;
                log_line("[net] watchdog: the game has not started a frame for %lld s (last frame %u), %s", (long long)(idle / 1000), frame,
                    watch_waiting.load() != 0 ? "waiting for the other machine's inputs" : "NOT in the wait for inputs (the game itself is stuck)");
                if (ram != nullptr && reports == 1) {
                    // the voice list as it is now, without changing it (a loop shows as the same address twice)
                    std::vector<uint32_t> seen;
                    uint32_t current = ram_w(ram, voice_list_head);
                    std::string text;
                    while (current != 0 && seen.size() < 80) {
                        char piece[48];
                        const bool again = std::find(seen.begin(), seen.end(), current) != seen.end();
                        std::snprintf(piece, sizeof(piece), " %08X%s", (unsigned)current, again ? "(again)" : "");
                        text += piece;
                        if (again || !ram_pointer(current) || (current & 3u) != 0) {
                            break;
                        }
                        seen.push_back(current);
                        current = ram_w(ram, current);
                    }
                    log_line("[net] watchdog: the audio library's voice list:%s", text.c_str());
                }
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
                watch_log_game_thread_stack();
#endif
            }
        }
    }

    // Called by the game thread at the start of each frame.
    void watch_frame_started(uint32_t f) {
        if (!watch_started.exchange(true)) {
#ifdef _WIN32
            HANDLE duplicate = nullptr;
            if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                watch_game_thread = duplicate;
            }
#endif
            std::thread(watch_main).detach();
        }
        watch_frame = f;
        watch_last_ms = watch_now_ms();
    }
}

// ultra_extras.cpp, when the game stops itself with its fatal-error trap: writes what is known to the log (the game's error code,
// where it was, the audio voice list, and the heap's block chain), as the window that follows ends the program.
void conker::netsession::note_halt(uint8_t* rdram, uint32_t vram, uint32_t return_address) {
    if (log_file == nullptr || rdram == nullptr) {
        return;
    }
    log_line("[halt] the game halted: syscall at %08X, return address %08X, net frame %u (last QUIT at frame %u)", vram, return_address,
        (unsigned)current_frame, (unsigned)last_quit_frame);
    log_line("[halt] the game's error code (D_8003C8E0): %08X", (unsigned)ram_w(rdram, 0x8003C8E0u));
    // the audio voice list
    {
        std::vector<uint32_t> seen;
        uint32_t cur = ram_w(rdram, voice_list_head);
        std::string text;
        while (cur != 0 && seen.size() < 80) {
            char piece[32];
            const bool again = std::find(seen.begin(), seen.end(), cur) != seen.end();
            std::snprintf(piece, sizeof(piece), " %08X%s", (unsigned)cur, again ? "(again)" : "");
            text += piece;
            if (again || !ram_pointer(cur) || (cur & 3u) != 0) {
                break;
            }
            seen.push_back(cur);
            cur = ram_w(rdram, cur);
        }
        log_line("[halt] the audio library's voice list:%s", text.c_str());
    }
    // the heap's blocks (the chain from D_800380B4: next pointer at +0, state in the top byte of the word at +8)
    {
        uint32_t counts[256] = {};
        uint32_t cur = ram_w(rdram, 0x800380B4u);
        uint32_t blocks = 0;
        bool broken = false;
        std::vector<uint32_t> last;
        while (cur != 0 && blocks < 20000) {
            if (!ram_pointer(cur) || (cur & 3u) != 0) {
                broken = true;
                break;
            }
            counts[(ram_w(rdram, cur + 8u) >> 24) & 0xFF]++;
            last.push_back(cur);
            if (last.size() > 6) {
                last.erase(last.begin());
            }
            cur = ram_w(rdram, cur);
            blocks++;
        }
        std::string text;
        for (int k = 0; k < 256; k++) {
            if (counts[k] != 0) {
                char piece[40];
                std::snprintf(piece, sizeof(piece), " state %02X: %u", k, (unsigned)counts[k]);
                text += piece;
            }
        }
        log_line("[halt] heap chain: %u blocks%s;%s", blocks, broken ? ", then a link to something that is not memory" : (cur != 0 ? ", too long (a loop?)" : ""), text.c_str());
        std::string tail;
        for (uint32_t b : last) {
            char piece[64];
            std::snprintf(piece, sizeof(piece), " %08X(%08X)", (unsigned)b, (unsigned)ram_w(rdram, b + 8u));
            tail += piece;
        }
        log_line("[halt] the last blocks of the chain:%s", tail.c_str());
    }
}

namespace {
    // Mutex held. This machine's part of frame g's inputs: a dedicated server's is its notes of finished sounds (slot 0's input comes
    // from the leader), the host's goes straight in, a client's goes to the host.
    void put_own_input(uint32_t g, const PlayerInput& in) {
        if (is_host && dedicated) {
            host_notes[g] = in.ended;
        }
        else if (is_host) {
            host_store_input(0, g, in);
        }
        else if (!peers.empty()) {
            Writer w(Input);
            w.put(g).put_input(in);
            peers[0]->send_message(w.finish());
            sent_inputs[g] = in;
            if (sent_inputs.size() > 120) {
                sent_inputs.erase(sent_inputs.begin());
            }
        }
    }

    // Mutex held. The input read at frame f is for frame f + delay (see auto_delay): every frame after the last one sent for gets it
    // (more than one when the delay has just grown: the later ones without the finished sounds, the QUIT and the camera's turn, which
    // happen once), or, when f + delay was sent for already (the delay has just shrunk, or a resync took the game back), it has no frame
    // of its own and what must not be lost goes with the next one.
    void schedule_own_input(uint32_t f, PlayerInput mine) {
        const uint32_t target = f + delay;
        if (have_target && target <= last_target) {
            for (size_t k = 0; k < mine.ended.size(); k++) {
                if (mine.ended[k] != 0) {
                    carry.ended[k] = mine.ended[k];
                }
            }
            if (mine.leave.on) {
                carry.leave = mine.leave;
            }
            return;
        }
        for (size_t k = 0; k < mine.ended.size(); k++) {
            if (mine.ended[k] == 0) {
                mine.ended[k] = carry.ended[k];
            }
        }
        if (!mine.leave.on && carry.leave.on) {
            mine.leave = carry.leave;
        }
        carry = PlayerInput{};
        const uint32_t first = have_target ? last_target + 1 : target;
        if (target - first > 0) {
            static int said = 0;
            if (said++ < 40) {
                log_line("[net] f=%u the delay grew: this machine's input goes in for frames %u to %u", f, first, target);
            }
        }
        for (uint32_t g = first; g <= target; g++) {
            PlayerInput in = mine;
            if (g != first) {
                in.ended = {};
                in.leave = PlayerInput::Leave{};
                in.cam_yaw = 0;
                in.cam_pitch = 0;
            }
            put_own_input(g, in);
        }
        last_target = target;
        have_target = true;
        if (is_host && dedicated) {
            try_broadcast();
        }
    }
}

// Every port read. The first of a pass starts a game frame: it sends this machine's input for the frame
// `delay` ahead, then waits for the agreed inputs of the frame it's starting.
void conker::netsession::on_port_read(int port) {
    conker::netview::port_read();
    {
        std::lock_guard lock{ mtx };
        if (!enabled || failed) {
            sound_sync = false;
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
    // The right stick turning this player's camera: asked right after the controller was read, as it goes with it.
    float cam_yaw = 0.0f, cam_pitch = 0.0f;
    bool cam_on = sampler != nullptr && conker::mouse_camera::online_turn(&cam_yaw, &cam_pitch);
    bool swapped = sampler != nullptr && conker::look_aim::sampled_swap();
    if (cam_on && conker::look_aim::entry_hold()) {
        cam_on = false;
        cam_yaw = cam_pitch = 0.0f;
    }
    if (conker::netchat::typing()) {
        // typing in the chat: this player's game gets a still controller (only what this machine sends changes)
        buttons = 0;
        x = 0.0f;
        y = 0.0f;
        cam_yaw = cam_pitch = 0.0f;
        swapped = false;
    }

    std::unique_lock lock{ mtx };
    // The game doesn't start until everyone is here.
    cv.wait(lock, [] { return started || failed; });
    if (failed) {
        return;
    }
    if (!is_host && late_join && !late_synced) {
        // Joined a running game: until its snapshot is loaded this machine's game idles on its own, with nobody plugged
        // in, and asks for the snapshot (again every 3 s until it has come).
        current = FrameInputs{};
        in_frame = true;
        if (!peers.empty() && peers[0]->alive && (!resync_in_flight || clock::now() - resync_asked_at > std::chrono::seconds(8))) {
            resync_in_flight = true;
            resync_asked_at = clock::now();
            Writer w(ResyncRequest);
            w.put((uint32_t)0).put((uint32_t)0);
            peers[0]->send_message(w.finish());
            log_line("[net] asking the server for the running game's snapshot");
        }
        return;
    }
    static PlayerInput::Leave pending_leave;
    // The online pause menu (net_menu.cpp). Start opens the game's own pause screen on this machine only, and while it's
    // open this player's own controller drives it and the game gets a still one: the game goes on for everyone. Only
    // what this machine sends changes, so nothing differs between the machines.
    {
        static bool was_start = false, was_a = false, was_b = false;
        static bool open = false, hold_off = false;
        static PlayerInput::Leave before_menu;
        static uint8_t before_fd98 = 0;
        static uint8_t before_owner = 0;   // D_800E0A98, the player the game's menus listen to: the menu's hook sets it on this machine only
        static uint32_t at_open = 0;
        static bool warned = false;
        static const bool allow_pause = std::getenv("CONKER_NET_ALLOWPAUSE") != nullptr;
        const auto rd8 = [&](uint32_t a) { return (int)(int8_t)ram[(a ^ 3u) & 0x7FFFFFu]; };
        const auto wr8 = [&](uint32_t a, int v) { ram[(a ^ 3u) & 0x7FFFFFu] = (uint8_t)v; };
        const bool start = (buttons & 0x1000) != 0, a = (buttons & 0x8000) != 0, b = (buttons & 0x4000) != 0;
        {
            // a log of what could put the pause screen on a machine that didn't ask for it: the game's own pause flags and the
            // menu's flags, whenever one changes
            static int last_pause = -1, last_req = -1, last_wait = -1;
            const int pause_flag = ram != nullptr ? rd8(0x800BEAC0u) : 0, pause_req = ram != nullptr ? rd8(0x800BEAC1u) : 0, wait = ram != nullptr ? rd8(0x8008FEF0u) : 0;
            // the game's menu variables too (which button is picked, whose controller drives it, the quit state): whenever one of them
            // changes, so a log shows what the end-of-match screen sets
            static uint32_t last_menu_vars = 0xFFFFFFFFu;
            const uint32_t menu_vars = ram != nullptr ? ((uint32_t)(rd8(0x800E0B94u) & 0xFF) | ((uint32_t)(rd8(0x8008FE2Cu) & 0xFF) << 8) | ((uint32_t)(rd8(0x800E0A98u) & 0xFF) << 16) |
                ((uint32_t)(rd8(0x8008FD74u) & 0xFF) << 24)) : 0u;
            if (ram != nullptr && menu_vars != last_menu_vars) {
                last_menu_vars = menu_vars;
                log_line("[menu] f=%u menu variables: quit-state(800E0B94)=%d picked(8008FE2C)=%d owner(800E0A98)=%d fd74=%d fd9c=%d arena=%d open=%d", frames_started, rd8(0x800E0B94u), rd8(0x8008FE2Cu), rd8(0x800E0A98u),
                    rd8(0x8008FD74u), rd8(0x8008FD9Cu), rd8(0x800BE616u), (int)open);
            }
            // (0.9.71: the boot's menus, to take an online game straight to the multiplayer menu: out of a match, whenever the scene, the
            // phase, the game state's menu bytes (+0x38 to +0x4F of gGameState, D_8008FDD4; +0x42 the menu item) or this machine's buttons
            // change; and the menu items' table (D_800AB68C, 10 bytes each) once)
            if (ram != nullptr && rd8(0x800BE616u) == 0) {
                const auto rd32 = [&](uint32_t a) { return (uint32_t)(((uint32_t)(uint8_t)rd8(a) << 24) | ((uint32_t)(uint8_t)rd8(a + 1) << 16) | ((uint32_t)(uint8_t)rd8(a + 2) << 8) | (uint32_t)(uint8_t)rd8(a + 3)); };
                static bool table_logged = false;
                if (!table_logged && rd8(0x800E0B94u) == 3) {
                    table_logged = true;
                    for (uint32_t row = 0; row < 120; row++) {
                        char hex[64];
                        int at = 0;
                        for (uint32_t k = 0; k < 10; k++) {
                            at += std::snprintf(hex + at, sizeof(hex) - at, "%02X ", (unsigned)(uint8_t)rd8(0x800AB68Cu + row * 10 + k));
                        }
                        log_line("[boot menu] item %u: %s", row, hex);
                    }
                }
                static std::string last_boot;
                const uint32_t gs = rd32(0x8008FDD4u);
                char state[256];
                int at = std::snprintf(state, sizeof(state), "scene %02X phase %d fd80 %d gs %08X:", rd32(0x800BE9F0u), rd8(0x800E0B94u), rd8(0x8008FD80u), gs);
                if ((gs >> 24) == 0x80 && (gs & 0xFFFFFFu) < 0x7FFF00u) {
                    for (uint32_t k = 0x38; k < 0x50; k++) {
                        at += std::snprintf(state + at, sizeof(state) - at, " %02X", (unsigned)(uint8_t)rd8(gs + k));
                    }
                }
                const std::string now = state;
                static uint16_t last_buttons = 0;
                if (now != last_boot || buttons != last_buttons) {
                    last_boot = now;
                    last_buttons = buttons;
                    log_line("[boot menu] f=%u buttons %04X %s", frames_started, (unsigned)buttons, state);
                }
            }
            if (ram != nullptr && (pause_flag != last_pause || pause_req != last_req || wait != last_wait)) {
                log_line("[menu] f=%u game pause=%d request=%d unpause-wait=%d menu-open=%d hold-off=%d start-held=%d", frames_started, pause_flag, pause_req, wait, (int)open, (int)hold_off, (int)start);
                last_pause = pause_flag;
                last_req = pause_req;
                last_wait = wait;
            }
        }
        // (0.10.31: and the co-op campaign stage, as a match: the pause menu over it is this player's alone, the game going on)
        const bool in_match = ram != nullptr && (ram[(0x800BE616u ^ 3u) & 0x7FFFFFu] != 0 || conker::netmenu::coop_running(ram)) &&
            ram[(0x800BEAC0u ^ 3u) & 0x7FFFFFu] == 0;
        // the controller, as the pause menu wants it: the stick from the stick or the D-pad, the buttons only on the
        // frame they go down
        const auto stick = [](float v, bool minus, bool plus) { return plus ? 80 : minus ? -80 : (int)std::lround(std::max(-1.0f, std::min(1.0f, v)) * 80.0f); };
        const int stick_x = stick(x, (buttons & 0x0200) != 0, (buttons & 0x0100) != 0);
        const int stick_y = stick(y, (buttons & 0x0400) != 0, (buttons & 0x0800) != 0);
        if (start && !was_start && !open) {
            log_line("[menu] f=%u Start pressed: plugged %d, in match %d (arena flag %d, game pause %d), pause allowed %d", frames_started, (int)plugged, (int)in_match,
                ram != nullptr ? rd8(0x800BE616u) : -1, ram != nullptr ? rd8(0x800BEAC0u) : -1, (int)allow_pause);
        }
        // (not once the match is over: 800E0B94 is 0 all through a match and 6/7/3 on the screens after it, which the game runs with
        // player 1's controller; the pause menu opened over them took that controller away in the 0.9.4 test)
        if (!open && !allow_pause && plugged && in_match && rd8(0x800E0B94u) == 0 && start && !was_start) {
            // a player slot nobody plays in: the menu reads its input from there
            int slot = -1;
            for (int s = 3; s >= 0; s--) {
                if (s >= players_expected) {
                    slot = s;
                    break;
                }
            }
            if (slot < 0) {
                // all four players are in: the menu reads this player's own slot, put back as it was right after (net_menu.cpp)
                slot = my_slot;
                conker::netmenu::set_borrowed(true);
                if (!warned) {
                    warned = true;
                    log_line("[menu] no free player slot for the pause menu (all four players are in): it uses this player's own");
                }
            }
            else {
                conker::netmenu::set_borrowed(false);
            }
            {
                open = true;
                at_open = (uint32_t)rd8(0x800E0B94u) & 0xFF;
                // the variables QUIT changes, as they are now (they are put back when it's chosen, see below)
                before_menu.b94 = (uint8_t)rd8(0x800E0B94u);
                before_menu.a88 = ram_w(ram, 0x800E0A88u);
                before_menu.fd74 = (uint8_t)rd8(0x8008FD74u);
                const uint32_t state_ptr = ram_w(ram, 0x8008FDD4u);
                before_menu.gs3f = ram_pointer(state_ptr) ? (uint8_t)rd8(state_ptr + 0x3Fu) : 0;
                before_menu.fd9c = (uint8_t)rd8(0x8008FD9Cu);
                before_menu.fe2c = (uint8_t)rd8(0x8008FE2Cu);
                before_menu.v7270 = (uint8_t)rd8(0x80087270u);
                before_fd98 = (uint8_t)rd8(0x8008FD98u);
                before_owner = (uint8_t)rd8(0x800E0A98u);
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
                log_line("[menu] f=%u opened (player slot %d)", frames_started, slot);
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
                // QUIT confirmed: the game has just set the variables that take it out of the match, on this machine only. Their
                // values go to everyone with this machine's input, and every machine (this one too) sets them at the same frame; for
                // now this machine puts them back as they were.
                pending_leave.on = 1;
                pending_leave.b94 = (uint8_t)rd8(0x800E0B94u);
                pending_leave.a88 = ram_w(ram, 0x800E0A88u);
                pending_leave.fd74 = (uint8_t)rd8(0x8008FD74u);
                const uint32_t state_ptr = ram_w(ram, 0x8008FDD4u);
                pending_leave.gs3f = ram_pointer(state_ptr) ? (uint8_t)rd8(state_ptr + 0x3Fu) : 0;
                pending_leave.fd9c = (uint8_t)rd8(0x8008FD9Cu);
                pending_leave.fe2c = 0;
                pending_leave.v7270 = (uint8_t)rd8(0x80087270u);
                log_line("[net] f=%u this player chose QUIT in the menu: everyone goes back to the multiplayer menu", frames_started);
                wr8(0x800E0B94u, before_menu.b94);
                std::memcpy(ram + (0x800E0A88u & 0x7FFFFFu), &before_menu.a88, 4);
                wr8(0x8008FD74u, before_menu.fd74);
                if (ram_pointer(state_ptr)) {
                    wr8(state_ptr + 0x3Fu, before_menu.gs3f);
                }
                wr8(0x8008FD9Cu, before_menu.fd9c);
                wr8(0x8008FD98u, before_fd98);
                wr8(0x80087270u, before_menu.v7270);
                close = true;
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
                log_line("[menu] f=%u closed (in match %d)", frames_started, (int)in_match);
                conker::netmenu::set_open(false);
                wr8(0x8008FE2C, 0);
                // the menu's hook set the player the game's menus listen to (to the slot it reads), on this machine alone: back as
                // it was, or this machine's game differs from the others' from here on (0.9.6 test: the host's stayed 3, the
                // joiner's 0, through the next match)
                wr8(0x800E0A98u, (int8_t)before_owner);
                open = false;
                hold_off = true;
            }
        }
        // A button that was pressed for the menu isn't passed on to the game until it is let go.
        if (open || (hold_off && (start || a || b))) {
            buttons = 0;
            x = 0.0f;
            y = 0.0f;
            cam_yaw = cam_pitch = 0.0f;
            swapped = false;
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
        if (diag_copies_on()) {
            // (only for the memory comparisons: an 8 MB copy every frame, 160 MB kept)
            RawCopy& rc = raw_ring[f % raw_ring_size];
            rc.frame = f;
            rc.data.resize(ram_bytes);
            std::memcpy(rc.data.data(), ram, ram_bytes);
        }
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
        {
            // the game's sound slots (handle, next handle, sound, whether there is a voice) and the objects' sound handles (+0x8C,
            // +0x8E): the game's logic acts on them, while the voices themselves are each machine's own
            uint64_t h = 0x9E3779B97F4A7C15ull;
            const auto mix = [&](uint32_t v) { h = (h ^ v) * 0x100000001B3ull; h ^= h >> 29; };
            for (uint32_t k = 0; k < 16; k++) {
                const uint32_t slot = 0x800425E0u + k * 12u;
                uint32_t w0, w1, w2;
                std::memcpy(&w0, ram + (slot & 0x7FFFFFu), 4);
                std::memcpy(&w1, ram + ((slot + 4u) & 0x7FFFFFu), 4);
                std::memcpy(&w2, ram + ((slot + 8u) & 0x7FFFFFu), 4);
                mix(w0);
                mix(w1);
                mix(w2 != 0 ? 1u : 0u);
            }
            for (uint32_t i = 0; i < 6; i++) {
                uint32_t v;
                std::memcpy(&v, ram + ((0x800CC2D0u + i * 0x32Cu + 0x8Cu) & 0x7FFFFFu), 4);
                mix(v);
            }
            obj_ring[f % raw_ring_size][3] = (uint32_t)(h ^ (h >> 32));
            if (f % 600 == 0) {
                std::string text;
                for (uint32_t k = 0; k < 16; k++) {
                    const uint32_t slot = 0x800425E0u + k * 12u;
                    uint32_t w0, w2;
                    std::memcpy(&w0, ram + (slot & 0x7FFFFFu), 4);
                    std::memcpy(&w2, ram + ((slot + 8u) & 0x7FFFFFu), 4);
                    char piece[24];
                    std::snprintf(piece, sizeof(piece), " %08X%s", (unsigned)w0, w2 != 0 ? "+" : "");
                    text += piece;
                }
                log_line("[sound] f=%u slots (handle, next; + has a voice):%s", f, text.c_str());
            }
        }
    }
    if (is_host) {
        Writer w(FrameRngMsg);
        w.put(f).put(read_rng()).put(obj_ring[f % raw_ring_size][0] == f ? obj_ring[f % raw_ring_size][1] : 0u).put(obj_ring[f % raw_ring_size][0] == f ? obj_ring[f % raw_ring_size][2] : 0u).put(obj_ring[f % raw_ring_size][0] == f ? obj_ring[f % raw_ring_size][3] : 0u);
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
    mine.cam_on = cam_on ? 1 : 0;
    mine.swap = swapped ? 1 : 0;
    mine.look = sampler != nullptr ? conker::look_aim::sampled_look_flags() : 0;
    mine.cam_yaw = (int16_t)std::lround(std::clamp(cam_yaw, -3.0f, 3.0f) * 10000.0f);
    mine.cam_pitch = (int16_t)std::lround(std::clamp(cam_pitch, -3.0f, 3.0f) * 10000.0f);
    sound_sync = true;
    if (pending_leave.on) {
        mine.leave = pending_leave;
        pending_leave = PlayerInput::Leave{};
    }
    if (is_host && ram != nullptr) {
        // which sounds have finished playing, as this machine's audio thread sees it: the slot has a handle and a voice, and
        // the voice is idle (its state byte at +0x54 is 0) or belongs to another slot now (its link at +0x38)
        static std::array<uint16_t, 16> noted{};
        static int logged = 0;
        for (uint32_t k = 0; k < 16; k++) {
            const uint32_t slot = sound_slots + k * 12u;
            const uint16_t handle = ram_h(ram, slot);
            const uint32_t voice = ram_w(ram, slot + 8u);
            if (handle == 0) {
                noted[k] = 0;
                continue;
            }
            if (noted[k] == handle || !ram_pointer(voice)) {
                continue;
            }
            if (ram_b(ram, voice + 0x54u) == 0 || ram_w(ram, voice + 0x38u) != slot + 8u) {
                mine.ended[k] = handle;
                noted[k] = handle;
                if (logged < 40) {
                    logged++;
                    log_line("[sound] f=%u slot %u: sound %04X has ended", f, k, (unsigned)handle);
                }
            }
        }
    }
    if (ram != nullptr && f % 300 == 0) {
        // (A voice in use that no sound slot of the game points at, for 15 seconds or more: a sound the game can no longer stop, such as a
        // looping gun that stays on. Voice fields: +0x38 the slot's pointer it was started for, +0x44 the sound, +0x53 flags, +0x54 its state.)
        static std::map<uint32_t, std::pair<uint16_t, uint32_t>> loose;   // voice -> (sound, frame first seen)
        static int loose_logged = 0;
        std::map<uint32_t, std::pair<uint16_t, uint32_t>> now;
        int in_use = 0, playing = 0;
        std::vector<uint32_t> seen;
        uint32_t current = ram_w(ram, voice_list_head);
        while (current != 0 && ram_pointer(current) && (current & 3u) == 0 && seen.size() < 256 && std::find(seen.begin(), seen.end(), current) == seen.end()) {
            seen.push_back(current);
            in_use++;
            if (ram_b(ram, current + 0x54u) != 0) {
                playing++;
                const uint32_t link = ram_w(ram, current + 0x38u);
                const bool held = ram_pointer(link) && ram_w(ram, link) == current;
                if (!held) {
                    const uint16_t sound = ram_h(ram, current + 0x44u);
                    auto it = loose.find(current);
                    const uint32_t first = (it != loose.end() && it->second.first == sound) ? it->second.second : f;
                    now[current] = { sound, first };
                    if (f - first >= 900 && (f - first) % 900 == 0 && loose_logged < 100) {
                        loose_logged++;
                        log_line("[sound] f=%u voice %08X (sound %04X, flags %02X, state %02X) has played for %u frames with no slot of the game's holding it", f, (unsigned)current,
                            (unsigned)sound, (unsigned)ram_b(ram, current + 0x53u), (unsigned)ram_b(ram, current + 0x54u), f - first);
                    }
                }
            }
            current = ram_w(ram, current);
        }
        loose.swap(now);
        static int summary_logged = 0;
        if (f % 3000 == 0 && summary_logged < 60) {
            summary_logged++;
            log_line("[sound] f=%u voices in use: %d, playing: %d, without a slot: %d", f, in_use, playing, (int)loose.size());
        }
    }
    if (ram != nullptr) {
        // (A sound that keeps its slot and voice for a long time, on this machine: a stuck one, such as a looping laser, shows here.)
        static std::array<uint16_t, 16> held_handle{};
        static std::array<uint32_t, 16> held_since{};
        static int stuck_logged = 0;
        for (uint32_t k = 0; k < 16; k++) {
            const uint32_t slot = sound_slots + k * 12u;
            const uint16_t handle = ram_h(ram, slot);
            const uint32_t voice = ram_w(ram, slot + 8u);
            if (handle != 0 && ram_pointer(voice)) {
                if (held_handle[k] != handle) {
                    held_handle[k] = handle;
                    held_since[k] = f;
                }
                else if (f - held_since[k] >= 900 && (f - held_since[k]) % 900 == 0 && stuck_logged < 80) {
                    stuck_logged++;
                    log_line("[sound] f=%u slot %u: sound %04X (handle %04X) has had its voice for %u frames; voice state %02X, link %08X", f, k,
                        (unsigned)ram_h(ram, slot + 4u), (unsigned)handle, f - held_since[k], (unsigned)ram_b(ram, voice + 0x54u), (unsigned)ram_w(ram, voice + 0x38u));
                }
            }
            else {
                held_handle[k] = 0;
            }
        }
    }
    schedule_own_input(f, mine);

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

    watch_frame_started(f);
    watch_waiting = 1;
    const auto began = clock::now();
    while (ready.find(f) == ready.end() && !failed) {
        // (the machines start the game a little apart: the first frames wait longer)
        auto limit = frames_started < 900 ? std::chrono::seconds(45) : (is_host ? host_waits_for_player : client_waits_for_host);
        if (is_host && dedicated) {
            // a player that joined a running game and is still loading its snapshot (several at once on a slow or busy computer take
            // a while) is waited for longer: dropping it only makes it ask again
            for (int s = 0; s < max_players; s++) {
                if (connected[s] && !input_since_join[s] && joining_since[s] != clock::time_point{} && clock::now() - joining_since[s] < std::chrono::seconds(60)) {
                    limit = std::chrono::seconds(45);
                }
            }
        }
        if (is_host) {
            // a player that was just sent a snapshot loads it and goes back to its frame, then plays the frames since then again:
            // until it has caught up its inputs come late, which is waited for longer (it isn't gone)
            for (int s = 0; s < max_players; s++) {
                if (connected[s] && resync_sent_at[s] != clock::time_point{} && clock::now() - resync_sent_at[s] < std::chrono::seconds(30)) {
                    limit = std::max(limit, std::chrono::seconds(20));
                }
            }
        }
        if (cv.wait_for(lock, std::chrono::milliseconds(250)) == std::cv_status::timeout) {
            if (clock::now() - began > limit) {
                if (is_host) {
                    // Whoever hasn't sent its input for this frame is dropped; the game goes on without it.
                    const auto it = inbox.find(next_broadcast);
                    for (int s = dedicated ? 0 : 1; s < max_players; s++) {
                        if (required_at(s, next_broadcast) && (it == inbox.end() || !it->second[s].first)) {
                            log_line("[net] f=%u player %d stopped answering for %lld s (waiting for its input for frame %u), dropped", frames_started, s + 1, (long long)std::chrono::duration_cast<std::chrono::seconds>(clock::now() - began).count(), next_broadcast);
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
    watch_waiting = 0;
    if (failed) {
        return;
    }
    const double ms = std::chrono::duration<double, std::milli>(clock::now() - began).count();
    waited_ms += ms;
    waited_frames++;
    if (ms > 50.0) {
        stalls++;
    }
    if (dedicated && is_host && f % 600 == 0) {
        static clock::time_point server_began = clock::now();
        log_line("[server] frame %u after %.1f s (%.1f frames a second on average)", f, std::chrono::duration<double>(clock::now() - server_began).count(),
            (double)f / std::max(0.001, std::chrono::duration<double>(clock::now() - server_began).count()));
    }
    if (f % 600 == 0) {
        log_line("[net] frame %u: waited %.1f ms on average (%llu longer than 50 ms), %llu desyncs", f,
            waited_ms / (double)std::max<uint64_t>(waited_frames, 1), (unsigned long long)stalls, (unsigned long long)desyncs);
    }
    current = ready[f];
    if (current.snap != 0) {
        hook_reset_frame = f + 1;
        on_snap_order(f, current.snap);
    }
    if (f == hook_reset_frame) {
        hook_generation_count++;
        conker::look_aim::reset_online_state();
        conker::mouse_camera::reset();
        log_line("[net] f=%u the hooks' own state starts over (the resync's frame), on every machine", f);
    }
    if (ram != nullptr) {
        // a player chose QUIT in the pause menu: every machine sets what the game's quit sets, at this same frame
        for (int slot = 0; slot < max_players; slot++) {
            const PlayerInput::Leave& lv = current.p[slot].leave;
            if (!lv.on) {
                continue;
            }
            const auto set8 = [&](uint32_t a, uint8_t v) { ram[(a ^ 3u) & 0x7FFFFFu] = v; };
            set8(0x800E0B94u, lv.b94);
            std::memcpy(ram + (0x800E0A88u & 0x7FFFFFu), &lv.a88, 4);
            set8(0x8008FD74u, lv.fd74);
            const uint32_t state_ptr = ram_w(ram, 0x8008FDD4u);
            if (ram_pointer(state_ptr)) {
                set8(state_ptr + 0x3Fu, lv.gs3f);
            }
            set8(0x8008FD9Cu, lv.fd9c);
            set8(0x8008FE2Cu, lv.fe2c);
            set8(0x80087270u, lv.v7270);
            last_quit_frame = f;
            log_line("[net] f=%u player %d chose QUIT: back to the multiplayer menu", f, slot + 1);
            conker::notices::show((name_of(slot) + " ended the match").c_str());
        }
    }
    if (ram != nullptr) {
        // the host's notes of finished sounds (see above), applied by every machine at the same frame
        for (uint32_t k = 0; k < 16; k++) {
            const uint16_t handle = current.p[0].ended[k];
            if (handle != 0) {
                const uint32_t slot = sound_slots + k * 12u;
                static uint64_t applied = 0;
                if (ram_h(ram, slot) == handle) {
                    const uint32_t zero = 0;
                    std::memcpy(ram + ((slot + 8u) & 0x7FFFFFu), &zero, 4);
                    applied++;
                }
                if (applied % 200 == 1 && applied < 2000) {
                    log_line("[sound] f=%u %llu sounds have been ended by the host's notes so far", f, (unsigned long long)applied);
                }
            }
        }
    }
    if (f % 30 == 0) {
        log_line("[inputs] f=%u p1=%d:%04X,%g,%g p2=%d:%04X,%g,%g", f, (int)current.p[0].plugged, (unsigned)current.p[0].buttons, (double)current.p[0].x, (double)current.p[0].y,
            (int)current.p[1].plugged, (unsigned)current.p[1].buttons, (double)current.p[1].x, (double)current.p[1].y);
    }
    if (f % 30 == 0) {
        // players 3 and 4 as well (the 4-player test: one camera swayed the whole session): their input and their camera turn
        log_line("[inputs34] f=%u p3=%d:%04X,%g,%g,cam%d:%d,%d p4=%d:%04X,%g,%g,cam%d:%d,%d", f,
            (int)current.p[2].plugged, (unsigned)current.p[2].buttons, (double)current.p[2].x, (double)current.p[2].y, (int)current.p[2].cam_on, (int)current.p[2].cam_yaw, (int)current.p[2].cam_pitch,
            (int)current.p[3].plugged, (unsigned)current.p[3].buttons, (double)current.p[3].x, (double)current.p[3].y, (int)current.p[3].cam_on, (int)current.p[3].cam_yaw, (int)current.p[3].cam_pitch);
    }
    if (f % 10 == 0 && ram != nullptr) {
        // Where the two players are and a pointer-free hash of each object, to compare the logs of both machines.
        uint32_t pos[6];
        std::memcpy(&pos[0], ram + (0x800CC2E4u & 0x7FFFFFu), 12);
        std::memcpy(&pos[3], ram + (0x800CC610u & 0x7FFFFFu), 12);
        log_line("[pos] f=%u o0=%08X,%08X,%08X o1=%08X,%08X,%08X h=%08X,%08X,%08X,%08X", f, pos[0], pos[1], pos[2], pos[3], pos[4], pos[5],
            objects_hash(0, 1), objects_hash(1, 1), objects_hash(2, 1), objects_hash(3, 1));
    }
    if (ram != nullptr && change_lines < 30000 && diag_env_on()) {   // (0.10.90: CONKER_NET_DIAG only)
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
        for (int s = dedicated ? 0 : 1; s < max_players; s++) {
            const bool now = current.p[s].plugged != 0;
            if (was_plugged[s] && !now) {
                conker::notices::show(name_of(s) + " left the match");
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
    *plugged = dedicated || in.plugged != 0;   // a dedicated server's game always has all its controllers; an empty slot's is still
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
    // 0.9.81: nothing from any controller while the boot's black screen is held, on the way to the multiplayer menu (net_menu.cpp).
    if (ram != nullptr && conker::netmenu::boot_screen_held(ram)) {
        *buttons = 0;
        *x = 0.0f;
        *y = 0.0f;
    }
    return true;
}

bool conker::netsession::diag_on() {
    return diag_env_on();
}

uint32_t conker::netsession::hook_generation() {
    return hook_generation_count.load();
}

bool conker::netsession::input_for_game(int port, bool* plugged, uint16_t* buttons, float* x, float* y) {
    if (!input(port, plugged, buttons, x, y)) {
        return false;
    }
    bool swapped = false;
    uint8_t* rdram = nullptr;
    {
        std::lock_guard lock{ mtx };
        swapped = port >= 0 && port < max_players && current.p[port].swap != 0;
        rdram = ram;
    }
    // Swap Sticks: the left stick is the C-buttons while aiming (R held). Letting go of R with the stick still held made C-Down arrive
    // with R up, and the game threw a grenade. After R is let go the C-buttons are held back until none is pressed any more.
    static bool prev_r[max_players] = {};
    static bool hold_back[max_players] = {};
    static uint32_t hold_generation = 0;
    if (hold_generation != hook_generation_count.load()) {
        hold_generation = hook_generation_count.load();
        for (int k = 0; k < max_players; k++) {
            prev_r[k] = false;
            hold_back[k] = false;
        }
    }
    if (port >= 0 && port < max_players) {
        if (!swapped) {
            prev_r[port] = false;
            hold_back[port] = false;
        } else {
            const bool r = (*buttons & 0x0010) != 0;
            if (r) {
                hold_back[port] = false;
            } else if (prev_r[port]) {
                hold_back[port] = true;
            }
            prev_r[port] = r;
            if (hold_back[port]) {
                if ((*buttons & 0x000F) == 0) {
                    hold_back[port] = false;
                } else {
                    *buttons &= (uint16_t)~0x000F;
                }
            }
        }
    }
    // 0.10.0: sticks swapped and R held, with the player's look off (before it starts, or knocked out of it by a hit while R is still
    // held): neither stick does anything until the look is back. Knocked out of it while walking, the left stick's C-buttons (its
    // strafe in the look) and the right stick (the aim; out of the look the N64 stick) went to the game outside the look and turned
    // Conker and the camera about; standing still the game just brought the look back. 0.9.99 did this where the input is sampled,
    // a few frames ahead of the game: the frames of the input delay right after the hit still had the sticks. Here it goes by the
    // game's own look (its stage, +0x73C of each of the player's look records) at the frame the input is played, on every machine.
    if (swapped && rdram != nullptr && port >= 0 && port < 4 && (*buttons & 0x0010) != 0) {
        const uint32_t looks = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800DBFF0);
        if ((looks >> 24) == 0x80 && (looks & 0x7FFFFFu) + 4u * 0x9A0u <= 0x800000u) {
            // (0.10.9: the second aiming mode, the sniper rifle's, the revolver's and the knives', has no look record stage: it counts as the
            // look, or the sticks were held still all through it and the right stick couldn't aim, the 0.10.8 logs: R held, the right stick
            // sent, nothing turned)
            bool look_on = conker::look_aim::second_aim_recent(port);
            for (uint32_t k = 0; k < 4; k++) {
                const gpr record = (gpr)(int32_t)(looks + k * 0x9A0u);
                if ((int)MEM_BU(0x23D, record) == port && MEM_H(0x73C, record) != 0) {
                    look_on = true;
                }
            }
            static bool held[max_players] = {};
            if (!look_on) {
                if (!held[port] && (*x != 0.0f || *y != 0.0f || (*buttons & 0x000F) != 0) && port == local_slot()) {
                    log("[input] R held with the look off: the sticks held still until it is back");
                }
                *x = 0.0f;
                *y = 0.0f;
                *buttons &= (uint16_t)~0x000Fu;
            }
            held[port] = !look_on;
        }
    }
    if (swapped && rdram != nullptr) {
        conker::look_aim::no_weapon_walk(rdram, port, buttons);
        conker::look_aim::gun_side_walk(rdram, port, buttons);
    }
    return true;
}

bool conker::netsession::device_present(int controller, bool* present) {
    std::lock_guard lock{ mtx };
    if (!enabled || failed || controller < 0 || controller >= max_players) {
        return false;
    }
    if (dedicated) {
        // all the controllers are always plugged in (a console has no hot-plugging); whoever isn't playing holds a still one
        *present = true;
        return true;
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
            std::vector<std::vector<uint8_t>> catch_up;   // the frames from the snapshot's on, for whoever is joining
            uint32_t joiners = 0;
            if (ok) {
                conker::mouse_camera::reset();   // (at the safe point; the machine that loads it does the same)
            }
            {
                std::lock_guard lock{ mtx };
                at_frame = frames_started;
                mask = snapshot_wanted_mask;
                snapshot_wanted_mask = 0;
                capture_in_flight = false;
                last_snapshot_sent = clock::now();
                if (ok) {
                    for (int s = 0; s < max_players; s++) {
                        if ((mask & (1u << s)) && connected[s] && joining[s]) {
                            joiners |= 1u << s;
                            join_from[s] = next_broadcast;   // the game waits for this player's input from this frame on
                        }
                    }
                    if (joiners != 0) {
                        for (auto it = ready.lower_bound(at_frame > 0 ? at_frame - 1 : 0); it != ready.end(); ++it) {
                            Writer fw(Frame);
                            fw.put(it->first);
                            for (int s = 0; s < max_players; s++) {
                                fw.put_input(it->second.p[s]);
                            }
                            catch_up.push_back(fw.finish());
                        }
                        log_line("[net] snapshot at frame %u for joining players (mask %u): they take part from frame %u", at_frame, joiners, next_broadcast);
                    }
                }
            }
            if (!ok || mask == 0 || state.empty() || state.size() > snapshot_max_size) {
                log_line("[net] the snapshot could not be taken (%s, %zu bytes)", ok ? "taken" : "the game wasn't at a safe point in time", state.size());
                return;
            }
            // Compressing and sending take a while, so not on this thread.
            std::thread([at_frame, mask, joiners, catch_up = std::move(catch_up), state = std::move(state)]() {
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
                            resync_sent_at[peer->slot] = clock::now();
                        }
                    }
                }
                for (Peer* peer : targets) {
                    if (joiners & (1u << peer->slot)) {
                        for (const auto& message : catch_up) {
                            peer->send_message(message, 2);
                        }
                    }
                    peer->send_message(bytes, 2);   // (2: the bulk channel, ahead of a diagnostic dump waiting there)
                }
                log_line("[net] snapshot of frame %u sent: %zu bytes, packed to %zu", at_frame, state.size(), packed.size());
            }).detach();
        });
    }

    // Mutex held (host). full: the client asks for a whole snapshot (its pieces didn't check out).
    void on_resync_requested(int slot, uint32_t frame, uint32_t diverged, bool full) {
        log_line("[net] player %d asks for a resync at frame %u%s", slot + 1, frame, full ? " (a whole snapshot)" : "");
        if (diverged != 0) {
            log_callers("host", diverged);
        }
        const uint8_t bit = (uint8_t)(1u << slot);
        if (full || (full_next & bit) || joining[slot] || frame == 0) {
            // a whole snapshot: for a player joining a running game, or one whose pieces didn't work
            full_next &= (uint8_t)~bit;
            snapshot_wanted_mask |= bit;
            if (!capture_in_flight && clock::now() - last_snapshot_sent > std::chrono::seconds(2)) {
                start_capture_locked();
            }
            return;
        }
        if (snap_round.active && (snap_round.waiting & bit)) {
            return;   // (being done)
        }
        snap_order_pending |= bit;   // named in the next frame sent (try_broadcast)
    }

    // Mutex held (host): sends whoever's pieces are ready (both copies taken, the client's hashes in) what they need.
    void serve_snap_round() {
        if (!snap_round.active || !snap_round.captured) {
            return;
        }
        bool whole = false;
        for (int s = 0; s < max_players; s++) {
            const uint8_t bit = (uint8_t)(1u << s);
            if (!(snap_round.waiting & bit)) {
                continue;
            }
            if (!snap_round.state) {
                // the host's copy couldn't be taken: a whole snapshot, the old way
                snap_round.waiting &= (uint8_t)~bit;
                snapshot_wanted_mask |= bit;
                whole = true;
                continue;
            }
            if (!snap_round.hashes_in[s]) {
                continue;
            }
            snap_round.waiting &= (uint8_t)~bit;
            Peer* target = nullptr;
            for (auto& peer : peers) {
                if (peer->alive && peer->slot == s) {
                    target = peer.get();
                }
            }
            if (target == nullptr) {
                continue;
            }
            resync_sent_at[s] = clock::now();
            last_snapshot_sent = clock::now();
            // the pieces are found and packed on a thread of their own (the game's frames go on meanwhile)
            std::thread([s, target, at = snap_round.at_frame, base_frame = snap_round.base_frame[s], base_size = snap_round.base_size[s],
                         state = snap_round.state, hashes = std::move(snap_round.hashes[s])]() {
                const std::vector<uint8_t>& mine = *state;
                const uint32_t pages = (uint32_t)((mine.size() + page_bytes - 1) / page_bytes);
                // (the copies needn't be the same size: what the client's lacks or has of a different length is sent, and it makes
                // its copy the host's size first)
                bool whole_one = hashes.empty();
                std::vector<uint32_t> differ;
                if (!whole_one) {
                    for (uint32_t i = 0; i < pages; i++) {
                        const size_t at_byte = (size_t)i * page_bytes;
                        const size_t n = std::min<size_t>(page_bytes, mine.size() - at_byte);
                        const size_t theirs_n = base_size > at_byte ? std::min<size_t>(page_bytes, base_size - at_byte) : 0;
                        if (i >= hashes.size() || n != theirs_n || piece_hash(mine.data() + at_byte, n) != hashes[i]) {
                            differ.push_back(i);
                        }
                    }
                    whole_one = differ.size() > pages / 2;
                }
                std::vector<uint8_t> bytes;
                size_t raw = 0, packed_size = 0;
                if (whole_one) {
                    const std::vector<uint8_t> packed = lz_compress(mine.data(), mine.size());
                    Writer w(Snapshot);
                    w.put(at).put((uint32_t)mine.size()).put((uint32_t)packed.size()).put_bytes(packed.data(), packed.size());
                    bytes = w.finish();
                    raw = mine.size();
                    packed_size = packed.size();
                }
                else {
                    std::vector<uint8_t> body;
                    body.reserve(differ.size() * (4 + page_bytes));
                    for (uint32_t i : differ) {
                        const uint8_t* b = reinterpret_cast<const uint8_t*>(&i);
                        body.insert(body.end(), b, b + 4);
                    }
                    for (uint32_t i : differ) {
                        const size_t at_byte = (size_t)i * page_bytes;
                        const size_t n = std::min<size_t>(page_bytes, mine.size() - at_byte);
                        body.insert(body.end(), mine.data() + at_byte, mine.data() + at_byte + n);
                    }
                    const std::vector<uint8_t> packed = lz_compress(body.data(), body.size());
                    Writer w(SnapDiff);
                    w.put(at).put(base_frame).put((uint32_t)mine.size()).put(page_bytes).put(whole_hash(mine)).put((uint32_t)differ.size())
                        .put((uint32_t)body.size()).put((uint32_t)packed.size()).put_bytes(packed.data(), packed.size());
                    bytes = w.finish();
                    raw = body.size();
                    packed_size = packed.size();
                }
                std::lock_guard lock{ mtx };
                bool alive = false;
                for (auto& peer : peers) {
                    alive |= peer.get() == target && peer->alive;
                }
                if (!alive) {
                    return;
                }
                target->send_message(bytes, 2);
                if (whole_one) {
                    log_line("[net] resync of player %d: frame %u, a whole snapshot (%zu bytes, packed to %zu)%s", s + 1, at, raw, packed_size,
                        hashes.empty() ? ", its copy wasn't there" : ", too much differed");
                }
                else {
                    log_line("[net] resync of player %d: frame %u, %zu of %u pieces differ from its copy of frame %u (%zu bytes, packed to %zu)", s + 1, at,
                        differ.size(), pages, base_frame, raw, packed_size);
                }
            }).detach();
        }
        if (whole && !capture_in_flight) {
            start_capture_locked();
        }
        if (snap_round.waiting == 0) {
            snap_round = SnapRound{};
            last_round_end = clock::now();
        }
    }

    // Mutex held (client): takes this machine's copy of its game, for the host's pieces, and sends the host the hashes of its pieces.
    void take_snap_base() {
        if (snap_base.busy) {
            return;
        }
        snap_base = ClientBase{};
        snap_base.busy = true;
        const auto send_hashes = [](bool good, uint32_t at, std::vector<uint8_t>&& state) {
            Writer w(SnapHashes);
            const uint32_t pages = good ? (uint32_t)((state.size() + page_bytes - 1) / page_bytes) : 0u;
            w.put(at).put((uint8_t)(good ? 1 : 0)).put((uint32_t)(good ? state.size() : 0)).put(page_bytes).put(pages);
            for (uint32_t i = 0; i < pages; i++) {
                const size_t at_byte = (size_t)i * page_bytes;
                w.put(piece_hash(state.data() + at_byte, std::min<size_t>(page_bytes, state.size() - at_byte)));
            }
            std::lock_guard lock{ mtx };
            snap_base.busy = false;
            snap_base.have = good;
            snap_base.at_frame = at;
            snap_base.state = good ? std::move(state) : std::vector<uint8_t>();
            if (!peers.empty() && peers[0]->alive) {
                peers[0]->send_message(w.finish(), 2);
            }
            log_line("[net] this machine's copy of frame %u %s: the hashes of its %u pieces went to the host", at, good ? "is taken" : "couldn't be taken", pages);
        };
        const bool asked = conker::save_states::net_capture([send_hashes](bool ok, std::vector<uint8_t>&& state) {
            // (on the game's idle thread: the hashing goes on a thread of its own)
            uint32_t at;
            {
                std::lock_guard lock{ mtx };
                at = frames_started;
            }
            const bool good = ok && !state.empty() && state.size() <= snapshot_max_size;
            std::thread([send_hashes, good, at, state = std::move(state)]() mutable {
                send_hashes(good, at, std::move(state));
            }).detach();
        });
        if (!asked) {
            // (another save-state job is waiting: the host sends a whole snapshot instead)
            snap_base.busy = false;
            Writer w(SnapHashes);
            w.put(frames_started).put((uint8_t)0).put((uint32_t)0).put(page_bytes).put((uint32_t)0);
            if (!peers.empty() && peers[0]->alive) {
                peers[0]->send_message(w.finish(), 2);
            }
        }
    }

    // Mutex held: frame f names the players to resync (mask). The host takes its copy; a client named takes its own.
    void on_snap_order(uint32_t f, uint8_t mask) {
        if (is_host) {
            const bool asked = conker::save_states::net_capture([f](bool ok, std::vector<uint8_t>&& state) {
                // (0.10.13: the hooks' state starts over on every machine at the next frame's start instead, hook_reset_frame)
                std::lock_guard lock{ mtx };
                if (!snap_round.active || snap_round.order_frame != f) {
                    return;
                }
                snap_round.captured = true;
                snap_round.at_frame = frames_started;
                if (ok && !state.empty() && state.size() <= snapshot_max_size) {
                    snap_round.state = std::make_shared<const std::vector<uint8_t>>(std::move(state));
                }
                log_line("[net] the host's copy for the resync of frame %u: %s, frame %u", f, snap_round.state ? "taken" : "couldn't be taken", snap_round.at_frame);
                serve_snap_round();
            });
            if (!asked && snap_round.active && snap_round.order_frame == f) {
                snap_round.captured = true;
                serve_snap_round();
            }
        }
        else if (mask & (1u << my_slot)) {
            log_line("[net] f=%u the host resyncs this machine: taking its copy of the game", f);
            take_snap_base();
        }
    }

    // Mutex held (host): a client's hashes of its pieces.
    void on_snap_hashes(int slot, Reader& r) {
        const uint32_t at = r.get<uint32_t>();
        const bool good = r.get<uint8_t>() != 0;
        const uint32_t size = r.get<uint32_t>();
        const uint32_t piece = r.get<uint32_t>();
        const uint32_t pages = r.get<uint32_t>();
        std::vector<uint32_t> hashes;
        if (!r.bad && good && piece == page_bytes && pages == (size + page_bytes - 1) / page_bytes && pages <= snapshot_max_size / page_bytes + 1) {
            hashes.resize(pages);
            for (uint32_t i = 0; i < pages && !r.bad; i++) {
                hashes[i] = r.get<uint32_t>();
            }
            if (r.bad) {
                hashes.clear();
            }
        }
        if (slot < 0 || slot >= max_players || !snap_round.active || !(snap_round.waiting & (1u << slot))) {
            return;
        }
        snap_round.hashes[slot] = std::move(hashes);
        snap_round.hashes_in[slot] = true;
        snap_round.base_frame[slot] = at;
        snap_round.base_size[slot] = size;
        serve_snap_round();
    }

    // Mutex held (client): loads the host's game of frame at_frame (a snapshot, or this machine's copy with the host's pieces in).
    void load_host_state(uint32_t at_frame, std::vector<uint8_t>&& state) {
        if (!late_join && frames_started > at_frame + stale_snapshot_frames) {
            // (it came too late: loading it would take this machine back too far for the host to wait; a new one is asked for)
            log_line("[net] the snapshot of frame %u came with this machine at frame %u: too old, not loaded", at_frame, frames_started);
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
            conker::mouse_camera::reset();
            conker::look_aim::reset_online_state();
            hook_generation_count++;
            if (late_join) {
                late_synced = true;
            }
            // The game goes on from the state: it was in the frame before at_frame.
            const uint32_t was = frames_started;
            frames_started = at_frame;
            current_frame = at_frame > 0 ? at_frame - 1 : 0;
            const auto it = ready.find(current_frame);
            if (it != ready.end()) {
                current = it->second;
            }
            last_port = 99;
            mine_history.clear();
            pending_checks.clear();
            // The host has (or will have) this machine's inputs up to last_target; going back in time, the frames up to there are
            // played again with them (what is read meanwhile has no frame of its own), and after that the inputs go in as before. One
            // that never got to at_frame (a late joiner) starts there.
            if (!have_target || last_target + 1 < at_frame) {
                last_target = at_frame > 0 ? at_frame - 1 : 0;
                have_target = at_frame > 0;
            }
            carry = PlayerInput{};
            obj_bad_run = 0;
            log_line("[net] resynced, going on from frame %u (it was at %u)", at_frame, was);
            cv.notify_all();
        })) {
            resync_in_flight = false;
        }
    }

    // Mutex held (client): the host's snapshot has come (a whole one: unpacked on a thread of its own, then loaded).
    void on_snapshot_message(const std::vector<uint8_t>& payload) {
        Reader r{ payload.data(), payload.size() };
        const uint32_t at_frame = r.get<uint32_t>();
        const uint32_t raw_size = r.get<uint32_t>();
        const uint32_t packed_size = r.get<uint32_t>();
        if (r.bad || raw_size == 0 || raw_size > snapshot_max_size || r.at + packed_size > payload.size()) {
            resync_in_flight = false;
            return;
        }
        const bool base_busy = snap_base.busy;
        snap_base = ClientBase{};
        snap_base.busy = base_busy;
        std::vector<uint8_t> packed(payload.begin() + r.at, payload.begin() + r.at + packed_size);
        std::thread([at_frame, raw_size, packed = std::move(packed)]() {
            std::vector<uint8_t> state(raw_size);
            const bool ok = lz_decompress(packed.data(), packed.size(), state.data(), raw_size);
            std::lock_guard lock{ mtx };
            if (!ok) {
                log_line("[net] the snapshot did not unpack");
                resync_in_flight = false;
                return;
            }
            load_host_state(at_frame, std::move(state));
        }).detach();
    }

    // Mutex held (client): the host's pieces that differ from this machine's copy.
    void on_snap_diff(const std::vector<uint8_t>& payload) {
        Reader r{ payload.data(), payload.size() };
        const uint32_t at_frame = r.get<uint32_t>();
        const uint32_t base_frame = r.get<uint32_t>();
        const uint32_t size = r.get<uint32_t>();
        const uint32_t piece = r.get<uint32_t>();
        const uint64_t hash = r.get<uint64_t>();
        const uint32_t count = r.get<uint32_t>();
        const uint32_t body_size = r.get<uint32_t>();
        const uint32_t packed_size = r.get<uint32_t>();
        if (r.bad || r.at + packed_size > payload.size() || piece != page_bytes || body_size > snapshot_max_size + count * 4ull ||
            size == 0 || size > snapshot_max_size || !snap_base.have || snap_base.at_frame != base_frame) {
            log_line("[net] the host's pieces for frame %u don't fit this machine's copy (of frame %u, %s): a whole snapshot is asked for", at_frame,
                snap_base.at_frame, snap_base.have ? "taken" : "not there");
            want_full = true;
            resync_in_flight = false;
            return;
        }
        std::vector<uint8_t> state = std::move(snap_base.state);
        state.resize(size);   // (the host's size, see serve_snap_round)
        snap_base = ClientBase{};
        std::vector<uint8_t> packed(payload.begin() + r.at, payload.begin() + r.at + packed_size);
        std::thread([at_frame, count, body_size, hash, state = std::move(state), packed = std::move(packed)]() mutable {
            std::vector<uint8_t> body(body_size);
            bool ok = lz_decompress(packed.data(), packed.size(), body.data(), body_size) && body_size >= (size_t)count * 4;
            size_t at = (size_t)count * 4;
            for (uint32_t k = 0; k < count && ok; k++) {
                uint32_t i;
                std::memcpy(&i, body.data() + (size_t)k * 4, 4);
                const size_t at_byte = (size_t)i * page_bytes;
                if (at_byte >= state.size()) {
                    ok = false;
                    break;
                }
                const size_t n = std::min<size_t>(page_bytes, state.size() - at_byte);
                if (at + n > body.size()) {
                    ok = false;
                    break;
                }
                std::memcpy(state.data() + at_byte, body.data() + at, n);
                at += n;
            }
            ok = ok && at == body.size() && whole_hash(state) == hash;
            std::lock_guard lock{ mtx };
            if (!ok) {
                log_line("[net] the host's %u pieces for frame %u didn't check out: a whole snapshot is asked for", count, at_frame);
                want_full = true;
                resync_in_flight = false;
                return;
            }
            log_line("[net] the host's %u pieces for frame %u are in this machine's copy (%u bytes came)", count, at_frame, (unsigned)packed.size());
            load_host_state(at_frame, std::move(state));
        }).detach();
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
            {
                // the sound slots: only logged (once at the start of each run of frames where they differ)
                static bool snd_bad = false;
                const auto their_snd = host_frame_snd.find(f);
                if (their_snd != host_frame_snd.end() && obj_ring[f % raw_ring_size][0] == f && their_snd->second != 0) {
                    const bool bad = obj_ring[f % raw_ring_size][3] != their_snd->second;
                    if (bad && !snd_bad) {
                        log_line("[net] frame %u: the sound slots differ from the host's (RNG the same); not acted on", f);
                    }
                    snd_bad = bad;
                }
            }
            if (their_cam != host_frame_cam.end() && obj_ring[f % raw_ring_size][0] == f && their_cam->second != 0 &&
                obj_ring[f % raw_ring_size][2] != their_cam->second) {
                // (only logged now: in the 0.9.5 test this fired 10 times with the machines otherwise in step, and each memory dump it
                // asked for is megabytes that held the game's own messages up; CONKER_NET_DIAG_CAMERA=1 asks for them again)
                static const bool camera_dumps = std::getenv("CONKER_NET_DIAG_CAMERA") != nullptr;
                if (cam_bad_run == 0 && !camera_dumps) {
                    log_line("[net] frame %u: the camera records differ from the host's (RNG and objects the same); not acted on", f);
                }
                if (cam_bad_run++ == 0 && camera_dumps && !diag_pending.active && diag_dumps < 10 && (last_diag_frame == 0 || f >= last_diag_frame + 200)) {
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
        if (!diag_dumps_on()) {
            // (each one is seven copies of the game's memory, about 10 MB: in the 0.9.41 test they filled the connection for seconds at a
            // time, the round trip went from 110 to 200 ms, and the resync's snapshot waited 13 s behind one; CONKER_NET_DIAG=1 asks for them)
            static int said = 0;
            if (said++ < 20) {
                log_line("[net] DIAG: %s (no memory comparison: CONKER_NET_DIAG isn't set)", why);
            }
            return;
        }
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
    void on_diag_request(uint32_t f, Peer* asker) {
        std::vector<RawCopy> copies;
        for (uint32_t g = f >= 6 ? f - 6 : 0; g <= f; g++) {
            const RawCopy& rc = raw_ring[g % raw_ring_size];
            if (rc.frame == g) {
                copies.push_back(rc);
            }
        }
        std::thread([f, asker, copies = std::move(copies)]() {
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
                    // (only the machine that asked: the others have no use for several megabytes of someone else's diagnostics)
                    if (peer->alive && peer.get() == asker) {
                        targets.push_back(peer.get());
                    }
                }
            }
            for (Peer* peer : targets) {
                peer->send_message(bytes, 1);
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
                        int first_object = -1;
                        for (uint32_t offset = 0; offset < range.length; offset += 4) {
                            const size_t w = ((range.start & 0x7FFFFFu) + offset) / 4;
                            if (a[w] == b[w]) {
                                continue;
                            }
                            if (range.start == 0x800CC2D0u && a[w] >= 0x80000000u && a[w] < 0x80800000u && b[w] >= 0x80000000u && b[w] < 0x80800000u) {
                                continue;   // two heap pointers: they differ between machines and mean nothing
                            }
                            count++;
                            if (range.start == 0x800CC2D0u && first_object < 0) {
                                first_object = (int)(offset / 0x32Cu);
                            }
                            if (printed < 60) {
                                printed++;
                                char item[64];
                                if (range.start == 0x800CC2D0u) {
                                    std::snprintf(item, sizeof(item), " %08X(obj%u+%X):%08X/%08X", (unsigned)(range.start + offset), (unsigned)(offset / 0x32Cu),
                                        (unsigned)(offset % 0x32Cu), (unsigned)a[w], (unsigned)b[w]);
                                }
                                else {
                                    std::snprintf(item, sizeof(item), " %08X:%08X/%08X", (unsigned)(range.start + offset), (unsigned)a[w], (unsigned)b[w]);
                                }
                                text_list += item;
                            }
                        }
                        if (count != 0) {
                            log_line("[net] DIAG:   frame %u, %s (%d words differ):%s", frame, range.name, count, text_list.c_str());
                        }
                        if (first_object >= 0) {
                            // the whole of the first object that differs, both machines' non-zero words (heap pointers too), to tell what kind of
                            // object it is and what the differing word is
                            static int object_dumps = 0;
                            if (object_dumps < 6) {
                                object_dumps++;
                                std::string mine_text, host_text;
                                for (uint32_t offset = 0; offset < 0x32Cu; offset += 4) {
                                    const size_t w = ((range.start & 0x7FFFFFu) + (uint32_t)first_object * 0x32Cu + offset) / 4;
                                    char item[32];
                                    if (a[w] != 0) {
                                        std::snprintf(item, sizeof(item), " %X:%X", (unsigned)offset, (unsigned)a[w]);
                                        mine_text += item;
                                    }
                                    if (b[w] != 0) {
                                        std::snprintf(item, sizeof(item), " %X:%X", (unsigned)offset, (unsigned)b[w]);
                                        host_text += item;
                                    }
                                }
                                log_line("[net] DIAG:   frame %u, object %d in full (offset:word), this machine:%s", frame, first_object, mine_text.c_str());
                                log_line("[net] DIAG:   frame %u, object %d in full (offset:word), the host:%s", frame, first_object, host_text.c_str());
                            }
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
    if (!diag_env_on()) {
        return;   // (0.10.90: CONKER_NET_DIAG only)
    }
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
