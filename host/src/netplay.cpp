// Netplay, milestone 1: the determinism test harness.
//
// Lockstep netplay only works if the game, given the same controller inputs, ends up in exactly
// the same state every time. This file tests that, before any networking exists.
//
//   Record: play normally. Every game frame, the four ports' inputs and a hash of the game's
//           memory (RDRAM) are written to a file.
//   Replay: the recorded inputs are fed back, and after every frame the memory's hash is compared
//           with the recording's. The first frame that differs is reported, with the blocks of memory
//           that differ, which point at what is nondeterministic (a clock, the RNG, a thread's
//           timing...).
//
// Switched on by environment variables (nothing changes without them):
//   CONKER_NETPLAY_RECORD=<file>   record to <file> (written when the game exits, and every 900 frames)
//   CONKER_NETPLAY_REPLAY=<file>   replay <file>; a report is written to <file>.report.txt
//   CONKER_NETPLAY_FRAMES=<n>      stop after n frames (the game is closed)
//   CONKER_NETPLAY_BLOCK=<bytes>   block size of the memory hashes when recording (default 4096)
//   CONKER_NETPLAY_IGNORE=<a-b,..> hex address ranges (0x8xxxxxxx) left out of the hashes, once the
//                                  report names memory that is known to be harmless (audio buffers...)
//   CONKER_NETPLAY_DUMP=<n,n,...>  also write all of memory at those frames (to <file>.f<n>.<tag>.ram)
//   CONKER_NETPLAY_OUT=<file>      a replay also writes the states it saw, as a recording of its own, so two replays
//                                  can be compared (or replayed against each other)
//   CONKER_NETPLAY_AUTODUMP=1      a replay writes all of memory at the frame the RNG first differs, and the frame before
//   CONKER_NETPLAY_TAG=<name>      the <tag> in those names (default a), so two runs can be compared
//
// A "frame" is one pass of the game's controller read (func_1501BBB8, once per game frame, which
// asks the host for port 0, 1, 2, 3 in turn): the hash is taken as that pass starts, so it covers
// everything the previous frame did. Port reads come from the game thread, at which point the
// game's other threads are parked, so memory is stable.

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <SDL.h>

#include "recomp.h"
#include "librecomp/game.hpp"
#include "ultramodern/ultramodern.hpp"

#include "conker.hpp"

namespace {
    constexpr int ports = 4;
    constexpr uint32_t ram_size = 0x800000;           // the N64's 8 MB
    constexpr uint32_t rng_address = 0x800885B0;      // the game's RNG state (64 bits), see random_u32
    constexpr uint32_t gobjects_address = 0x800CC2D0; // gObjects, 0x32C bytes each
    constexpr uint32_t gobject_size = 0x32C;
    constexpr char file_magic[8] = { 'C', 'B', 'N', 'P', 'R', 'E', 'C', '1' };
    constexpr uint32_t file_version = 2;   // 2: each frame also holds who used the RNG (version 1 files still replay)
    constexpr int flush_every = 900;
    constexpr int report_frames = 3;   // frames of differences listed in full
    constexpr int report_blocks = 48;  // blocks listed per frame

    enum class Mode { Off, Record, Replay, Online };  // Online: net_session.cpp's game; only the clock and the frame length are fixed

    struct Frame {
        uint8_t plugged[ports] = {};
        uint16_t buttons[ports] = {};
        float x[ports] = {};
        float y[ports] = {};
        uint64_t rng = 0;
        uint64_t full = 0;
        std::vector<uint32_t> blocks;
        std::vector<uint32_t> callers;   // return addresses of each call to the game's RNG during the frame
    };

    std::mutex state_mutex;
    Mode mode = Mode::Off;
    uint8_t* ram = nullptr;
    std::string file_path;
    uint32_t block_size = 4096;
    uint32_t max_frames = 0;
    std::vector<std::pair<uint32_t, uint32_t>> ignored; // guest address ranges [a, b)

    std::vector<Frame> frames;       // recording: the finished frames; replay: the whole file
    Frame current;                   // the frame in progress
    bool in_frame = false;
    uint32_t frame_index = 0;        // number of the frame in progress
    int last_port = 1 << 20;
    bool device_present[ports] = { true, false, false, false };
    bool written_at_exit = false;
    bool finished = false;

    // replay results
    uint32_t divergent_frames = 0;
    int64_t first_divergence = -1;
    int frames_reported = 0;
    std::string report;
    std::vector<uint32_t> block_diff_frames; // per block: in how many frames it differed
    std::vector<int64_t> block_first;        // per block: the first frame it differed in
    int64_t first_rng_divergence = -1;       // the first frame the RNG state differed in
    // The game's cycle counter (osGetCount), which the game uses for cosmetic randomness and the like. While a
    // test or a netplay game runs it is made from the game frame instead of the real clock, so every run
    // (and every machine) reads the same values. One game frame is 2 retraces: 93.75 MHz / 2 / 30 = 1.5625 M.
    std::atomic<uint32_t> clock_frame{ 0 };
    std::atomic<uint32_t> clock_calls{ 0 };
    uint32_t virtual_count() {
        return 0x01000000u + clock_frame.load() * 1562500u + clock_calls.fetch_add(1) * 16u;
    }
    void start_clock_frame(uint32_t frame) {
        clock_frame.store(frame);
        clock_calls.store(0);
    }

    std::vector<uint32_t> pending_callers;   // RNG calls since the frame started (they belong to the frame in progress)
    bool file_has_callers = false;           // the recording being replayed says who used the RNG
    bool callers_reported = false;
    std::string out_path;                    // CONKER_NETPLAY_OUT: a replay also writes what it saw, as a recording
    std::vector<Frame> out_frames;
    bool autodump = false;                   // CONKER_NETPLAY_AUTODUMP: memory is written when the RNG first differs
    bool autodumped = false;
    std::vector<uint8_t> previous_ram;       // memory at the previous frame, for the autodump

    void write_ram(const std::string& path, const uint8_t* data) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (out) {
            out.write((const char*)data, ram_size);
            std::fprintf(stderr, "[netplay] wrote memory to %s\n", path.c_str());
        }
    }

    std::vector<uint32_t> dump_frames;       // frames whose whole memory is written out (CONKER_NETPLAY_DUMP)
    std::string dump_tag = "a";              // CONKER_NETPLAY_TAG, so two runs can be told apart

    // Writes all of the game's memory at this frame, so two runs can be compared byte for byte.
    void maybe_dump(uint32_t frame) {
        if (ram == nullptr || std::find(dump_frames.begin(), dump_frames.end(), frame) == dump_frames.end()) {
            return;
        }
        const std::string path = file_path + ".f" + std::to_string(frame) + "." + dump_tag + ".ram";
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (out) {
            out.write((const char*)ram, ram_size);
            std::fprintf(stderr, "[netplay] wrote memory at frame %u to %s\n", frame, path.c_str());
        }
        else {
            std::fprintf(stderr, "[netplay] can't write %s\n", path.c_str());
        }
    }

    uint64_t mix(uint64_t h, uint64_t w) {
        h ^= w;
        h *= 0x9E3779B97F4A7C15ULL;
        h ^= h >> 29;
        return h;
    }

    uint64_t hash_bytes(const uint8_t* p, size_t n) {
        uint64_t h = 0x243F6A8885A308D3ULL;
        size_t i = 0;
        for (; i + 8 <= n; i += 8) {
            uint64_t w;
            std::memcpy(&w, p + i, 8);
            h = mix(h, w);
        }
        for (; i < n; i++) {
            h = mix(h, p[i]);
        }
        return h;
    }

    bool block_is_ignored(uint32_t block) {
        const uint32_t begin = 0x80000000u + block * block_size;
        const uint32_t end = begin + block_size;
        for (const auto& [a, b] : ignored) {
            if (begin < b && a < end) {
                return true;
            }
        }
        return false;
    }

    // The state at this moment: the RNG, and a hash per block of memory.
    void sample_state(Frame& f) {
        uint8_t* rdram = ram;
        f.rng = ((uint64_t)(uint32_t)MEM_W(0, (gpr)(int32_t)rng_address) << 32) |
            (uint64_t)(uint32_t)MEM_W(4, (gpr)(int32_t)rng_address);
        const uint32_t count = ram_size / block_size;
        f.blocks.assign(count, 0);
        uint64_t full = 0x13198A2E03707344ULL;
        for (uint32_t i = 0; i < count; i++) {
            const uint32_t h = block_is_ignored(i) ? 0 : (uint32_t)hash_bytes(ram + (size_t)i * block_size, block_size);
            f.blocks[i] = h;
            full = mix(full, h);
        }
        f.full = full;
    }

    // --- The file ---

    template <typename T> void put(std::ofstream& out, const T& v) { out.write((const char*)&v, sizeof(T)); }
    template <typename T> bool get(std::ifstream& in, T& v) { in.read((char*)&v, sizeof(T)); return (bool)in; }

    void write_trace(const std::string& path, const std::vector<Frame>& list) {
        if (path.empty()) {
            return;
        }
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::fprintf(stderr, "[netplay] can't write %s\n", path.c_str());
            return;
        }
        out.write(file_magic, sizeof(file_magic));
        put(out, file_version);
        put(out, ram_size);
        put(out, block_size);
        for (int p = 0; p < ports; p++) {
            const uint8_t present = device_present[p] ? 1 : 0;
            put(out, present);
        }
        const uint32_t count = (uint32_t)list.size();
        put(out, count);
        for (uint32_t i = 0; i < count; i++) {
            const Frame& f = list[i];
            put(out, i);
            for (int p = 0; p < ports; p++) {
                put(out, f.plugged[p]);
                put(out, f.buttons[p]);
                put(out, f.x[p]);
                put(out, f.y[p]);
            }
            put(out, f.rng);
            put(out, f.full);
            out.write((const char*)f.blocks.data(), f.blocks.size() * sizeof(uint32_t));
            const uint32_t n = (uint32_t)f.callers.size();
            put(out, n);
            out.write((const char*)f.callers.data(), f.callers.size() * sizeof(uint32_t));
        }
        std::fprintf(stderr, "[netplay] wrote %u frames to %s\n", count, path.c_str());
    }

    void write_file() {
        if (mode == Mode::Record) {
            write_trace(file_path, frames);
        }
    }

    bool read_file() {
        std::ifstream in(file_path, std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "[netplay] can't read %s\n", file_path.c_str());
            return false;
        }
        char magic[8];
        in.read(magic, sizeof(magic));
        uint32_t version = 0, size = 0, block = 0, count = 0;
        if (!in || std::memcmp(magic, file_magic, sizeof(magic)) != 0 || !get(in, version) || (version != file_version && version != 1) ||
            !get(in, size) || size != ram_size || !get(in, block) || block == 0 || ram_size % block != 0) {
            std::fprintf(stderr, "[netplay] %s isn't a recording this version can replay\n", file_path.c_str());
            return false;
        }
        block_size = block;
        file_has_callers = version >= 2;
        for (int p = 0; p < ports; p++) {
            uint8_t present = 0;
            get(in, present);
            device_present[p] = present != 0;
        }
        if (!get(in, count)) {
            return false;
        }
        frames.clear();
        frames.reserve(count);
        for (uint32_t i = 0; i < count; i++) {
            Frame f;
            uint32_t index = 0;
            if (!get(in, index)) {
                break;
            }
            for (int p = 0; p < ports; p++) {
                get(in, f.plugged[p]);
                get(in, f.buttons[p]);
                get(in, f.x[p]);
                get(in, f.y[p]);
            }
            get(in, f.rng);
            get(in, f.full);
            f.blocks.resize(ram_size / block_size);
            in.read((char*)f.blocks.data(), f.blocks.size() * sizeof(uint32_t));
            if (file_has_callers) {
                uint32_t n = 0;
                if (get(in, n) && n < (1u << 20)) {
                    f.callers.resize(n);
                    in.read((char*)f.callers.data(), n * sizeof(uint32_t));
                }
            }
            if (!in) {
                break; // a recording cut short: replay what there is
            }
            frames.push_back(std::move(f));
        }
        std::fprintf(stderr, "[netplay] loaded %zu frames from %s (block size %u)\n", frames.size(), file_path.c_str(), block_size);
        return !frames.empty();
    }

    // --- The report ---

    void add(const char* format, ...) {
        char line[512];
        va_list args;
        va_start(args, format);
        std::vsnprintf(line, sizeof(line), format, args);
        va_end(args);
        report += line;
        std::fputs(line, stderr);
    }

    void write_report() {
        const std::string path = file_path + ".report.txt";
        std::ofstream out(path, std::ios::trunc);
        out << report;
    }

    std::string describe_address(uint32_t address) {
        char text[128];
        // 25 objects, as the decompilation declares gObjects (blocks past it are left unlabelled).
        if (address >= gobjects_address && address < gobjects_address + 25 * gobject_size) {
            const uint32_t offset = address - gobjects_address;
            std::snprintf(text, sizeof(text), "gObjects[%u]+0x%X", offset / gobject_size, offset % gobject_size);
            return text;
        }
        if (address <= rng_address && rng_address < address + 8) {
            return "the RNG state";
        }
        return "";
    }

    void report_divergence(uint32_t index, const Frame& now, const Frame& want) {
        add("\nFrame %u differs from the recording (hash %016llX, recorded %016llX)\n", index,
            (unsigned long long)now.full, (unsigned long long)want.full);
        if (now.rng != want.rng) {
            add("  RNG state: %016llX now, %016llX recorded\n", (unsigned long long)now.rng, (unsigned long long)want.rng);
        }
        int listed = 0, total = 0;
        for (size_t i = 0; i < now.blocks.size(); i++) {
            if (now.blocks[i] == want.blocks[i]) {
                continue;
            }
            total++;
            if (listed < report_blocks) {
                const uint32_t begin = 0x80000000u + (uint32_t)i * block_size;
                const std::string where = describe_address(begin);
                add("  memory 0x%08X-0x%08X differs %s\n", begin, begin + block_size - 1, where.c_str());
                listed++;
            }
        }
        add("  %d blocks differ%s\n", total, total > listed ? " (the first ones are listed)" : "");
    }

    // The calls to the RNG made during `frame`, against the recording's: the first difference names the code that
    // used the RNG differently (the return addresses are in game code, see the symbols for the function).
    void check_callers(uint32_t frame) {
        // (the recording's last frame is cut short, its calls are not all there)
        if (!file_has_callers || callers_reported || frame + 1 >= frames.size()) {
            return;
        }
        const std::vector<uint32_t>& want = frames[frame].callers;
        const std::vector<uint32_t>& now = pending_callers;
        if (now == want) {
            return;
        }
        callers_reported = true;
        size_t i = 0;
        while (i < now.size() && i < want.size() && now[i] == want[i]) {
            i++;
        }
        add("\nThe RNG was used differently in frame %u: %zu calls now, %zu recorded. The first difference is call #%zu.\n",
            frame, now.size(), want.size(), i);
        const size_t from = i >= 6 ? i - 6 : 0;
        for (size_t k = from; k < i + 6 && (k < now.size() || k < want.size()); k++) {
            char a[16] = "-", b[16] = "-";
            if (k < now.size()) std::snprintf(a, sizeof(a), "%08X", now[k]);
            if (k < want.size()) std::snprintf(b, sizeof(b), "%08X", want[k]);
            add("  call #%zu: caller %s now, %s recorded%s\n", k, a, b, k == i ? "   <-- first difference" : "");
        }
    }

    void finish_replay() {
        if (finished) {
            return;
        }
        finished = true;
        if (first_divergence < 0) {
            add("\nREPLAY OK: %zu frames, the game's memory matched the recording after every frame.\n", frames.size());
            conker::notices::show("Netplay test: replay matched the recording");
        }
        else {
            add("\n--- Summary over the whole replay ---\n");
            if (first_rng_divergence >= 0) {
                add("The RNG state first differed at frame %lld.\n", (long long)first_rng_divergence);
            }
            else {
                add("The RNG state never differed.\n");
            }
            // Blocks ordered by the frame they first differed in, then by address.
            std::vector<uint32_t> order;
            for (uint32_t i = 0; i < block_first.size(); i++) {
                if (block_first[i] >= 0) {
                    order.push_back(i);
                }
            }
            std::sort(order.begin(), order.end(), [](uint32_t a, uint32_t b) {
                return block_first[a] != block_first[b] ? block_first[a] < block_first[b] : a < b;
            });
            add("%zu blocks differed at some point. First frame / frames differing / block:\n", order.size());
            size_t listed = 0;
            for (uint32_t i : order) {
                if (listed++ >= 600) {
                    add("  ... and %zu more\n", order.size() - 600);
                    break;
                }
                const uint32_t begin = 0x80000000u + i * block_size;
                const std::string where = describe_address(begin);
                add("  %6lld  %6u  0x%08X-0x%08X %s\n", (long long)block_first[i], block_diff_frames[i], begin,
                    begin + block_size - 1, where.c_str());
            }
            add("\nREPLAY DIVERGED: first at frame %lld, and %u of %zu frames differed.\n", (long long)first_divergence,
                divergent_frames, frames.size());
            conker::notices::show("Netplay test: replay diverged at frame " + std::to_string(first_divergence) + " (see the report)");
        }
        write_report();
        write_trace(out_path, out_frames);
        SDL_Event quit;
        SDL_zero(quit);
        quit.type = SDL_QUIT;
        SDL_PushEvent(&quit);
    }

    void parse_ignored(const char* text) {
        std::string rest = text;
        size_t pos = 0;
        while (pos < rest.size()) {
            size_t comma = rest.find(',', pos);
            std::string item = rest.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            const size_t dash = item.find('-');
            if (dash != std::string::npos) {
                const uint32_t a = (uint32_t)std::strtoul(item.substr(0, dash).c_str(), nullptr, 16);
                const uint32_t b = (uint32_t)std::strtoul(item.substr(dash + 1).c_str(), nullptr, 16);
                if (b > a) {
                    ignored.emplace_back(a, b);
                }
            }
            if (comma == std::string::npos) {
                break;
            }
            pos = comma + 1;
        }
    }

    void at_exit() {
        std::lock_guard lock{ state_mutex };
        if (mode == Mode::Record && !written_at_exit) {
            written_at_exit = true;
            if (in_frame) {
                current.callers = pending_callers;
                frames.push_back(current);
                in_frame = false;
            }
            write_file();
        }
        if (mode == Mode::Replay && !finished) {
            if (first_divergence >= 0) {
                add("\nGame closed during the replay, at frame %u. First divergence: frame %lld.\n", frame_index, (long long)first_divergence);
            }
            else {
                add("\nGame closed during the replay, at frame %u of %zu. No divergence so far.\n", frame_index, frames.size());
            }
            write_report();
            write_trace(out_path, out_frames);
        }
    }
}

void conker::netplay::init(uint8_t* rdram) {
    // SDL_getenv may hand back one shared buffer that the next call overwrites (it does on Windows),
    // so every value is copied into a string straight away.
    auto get_env = [](const char* name) {
        const char* value = SDL_getenv(name);
        return std::string(value != nullptr ? value : "");
    };
    const std::string record = get_env("CONKER_NETPLAY_RECORD");
    const std::string replay = get_env("CONKER_NETPLAY_REPLAY");
    if (record.empty() && replay.empty()) {
        return;
    }
    std::lock_guard lock{ state_mutex };
    ram = rdram;
    const std::string frames_text = get_env("CONKER_NETPLAY_FRAMES");
    if (!frames_text.empty()) {
        max_frames = (uint32_t)std::strtoul(frames_text.c_str(), nullptr, 10);
    }
    const std::string block_text = get_env("CONKER_NETPLAY_BLOCK");
    if (!block_text.empty()) {
        const uint32_t value = (uint32_t)std::strtoul(block_text.c_str(), nullptr, 10);
        if (value >= 256 && value <= 65536 && (value & (value - 1)) == 0) {
            block_size = value;
        }
    }
    const std::string tag_text = get_env("CONKER_NETPLAY_TAG");
    if (!tag_text.empty()) {
        dump_tag = tag_text;
    }
    const std::string dump_text = get_env("CONKER_NETPLAY_DUMP");
    for (size_t pos = 0; pos < dump_text.size();) {
        size_t comma = dump_text.find(',', pos);
        const std::string item = dump_text.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        if (!item.empty()) {
            dump_frames.push_back((uint32_t)std::strtoul(item.c_str(), nullptr, 10));
        }
        if (comma == std::string::npos) {
            break;
        }
        pos = comma + 1;
    }
    out_path = get_env("CONKER_NETPLAY_OUT");
    autodump = !get_env("CONKER_NETPLAY_AUTODUMP").empty();
    const std::string ignore_text = get_env("CONKER_NETPLAY_IGNORE");
    if (!ignore_text.empty()) {
        parse_ignored(ignore_text.c_str());
    }
    if (!replay.empty()) {
        file_path = replay;
        if (read_file()) {
            mode = Mode::Replay;
            std::fprintf(stderr, "[netplay] replaying %s\n", file_path.c_str());
        }
        else {
            std::fprintf(stderr, "[netplay] could not read %s, netplay test is off\n", file_path.c_str());
        }
    }
    else {
        file_path = record;
        mode = Mode::Record;
        std::fprintf(stderr, "[netplay] recording to %s (blocks of %u bytes)\n", file_path.c_str(), block_size);
    }
    if (mode != Mode::Off) {
        std::atexit(at_exit);
        if (SDL_getenv("CONKER_NETPLAY_REAL_CLOCK") == nullptr) {
            ultramodern::set_count_source(&virtual_count);
            std::fprintf(stderr, "[netplay] the game's cycle counter now follows the game frame\n");
        }
    }
}

// The online session (net_session.cpp) makes the game's clock and frame length follow the game frame, as a
// test does, so that every machine of a game sees the same ones.
void conker::netplay::enter_online(uint8_t* rdram) {
    std::lock_guard lock{ state_mutex };
    if (mode != Mode::Off) {
        return;
    }
    ram = rdram;
    mode = Mode::Online;
    ultramodern::set_count_source(&virtual_count);
    std::fprintf(stderr, "[netplay] online: the game's cycle counter and frame length now follow the game frame\n");
}

bool conker::netplay::active() {
    return mode != Mode::Off;
}

// Every port read: the first of a pass (port 0, or the lowest port the game asks for) starts a frame.
void conker::netplay::on_port_read(int port) {
    if (mode == Mode::Off) {
        return;
    }
    std::lock_guard lock{ state_mutex };
    const bool new_pass = port <= last_port;
    last_port = port;
    if (mode == Mode::Online) {
        if (new_pass) {
            start_clock_frame(conker::netsession::frame());
        }
        return;
    }
    if (!new_pass || ram == nullptr || finished) {
        return;
    }
    if (mode == Mode::Record) {
        if (in_frame) {
            current.callers = std::move(pending_callers);
            frames.push_back(std::move(current));
            if (frames.size() % flush_every == 0) {
                write_file();
            }
        }
        current = Frame{};
        pending_callers.clear();
        in_frame = true;
        frame_index = (uint32_t)frames.size();
        start_clock_frame(frame_index);
        sample_state(current);
        maybe_dump(frame_index);
        if (max_frames != 0 && frame_index >= max_frames) {
            frames.push_back(current);
            in_frame = false;
            write_file();
            written_at_exit = true;
            finished = true;
            SDL_Event quit;
            SDL_zero(quit);
            quit.type = SDL_QUIT;
            SDL_PushEvent(&quit);
        }
        return;
    }
    // Replay
    if (in_frame) {
        check_callers(frame_index);
        if (!out_frames.empty()) {
            out_frames.back().callers = pending_callers;
        }
        pending_callers.clear();
        frame_index++;
    }
    in_frame = true;
    start_clock_frame(frame_index);
    if (frame_index >= frames.size() || (max_frames != 0 && frame_index >= max_frames)) {
        finish_replay();
        return;
    }
    current = Frame{};
    sample_state(current);
    maybe_dump(frame_index);
    const Frame& want = frames[frame_index];
    if (!out_path.empty()) {
        Frame seen = current;
        for (int p = 0; p < ports; p++) {
            seen.plugged[p] = want.plugged[p];
            seen.buttons[p] = want.buttons[p];
            seen.x[p] = want.x[p];
            seen.y[p] = want.y[p];
        }
        out_frames.push_back(std::move(seen));
    }
    if (autodump && !autodumped && current.rng != want.rng && !previous_ram.empty()) {
        autodumped = true;
        write_ram(file_path + ".div" + std::to_string(frame_index) + ".prev.ram", previous_ram.data());
        write_ram(file_path + ".div" + std::to_string(frame_index) + ".cur.ram", ram);
    }
    if (autodump && !autodumped) {
        previous_ram.assign(ram, ram + ram_size);
    }
    if (current.full != want.full || current.rng != want.rng) {
        divergent_frames++;
        if (first_divergence < 0) {
            first_divergence = frame_index;
        }
        if (current.rng != want.rng && first_rng_divergence < 0) {
            first_rng_divergence = frame_index;
        }
        if (block_first.size() != current.blocks.size()) {
            block_first.assign(current.blocks.size(), -1);
            block_diff_frames.assign(current.blocks.size(), 0);
        }
        for (size_t i = 0; i < current.blocks.size(); i++) {
            if (current.blocks[i] != want.blocks[i]) {
                block_diff_frames[i]++;
                if (block_first[i] < 0) {
                    block_first[i] = frame_index;
                }
            }
        }
        if (frames_reported < report_frames) {
            frames_reported++;
            report_divergence(frame_index, current, want);
        }
    }
}

bool conker::netplay::replay_input(int port, bool* plugged, uint16_t* buttons, float* x, float* y) {
    if (mode != Mode::Replay || port < 0 || port >= ports) {
        return false;
    }
    std::lock_guard lock{ state_mutex };
    if (finished || frame_index >= frames.size()) {
        *plugged = port == 0;
        *buttons = 0;
        *x = 0.0f;
        *y = 0.0f;
        return true;
    }
    const Frame& want = frames[frame_index];
    *plugged = want.plugged[port] != 0;
    *buttons = want.buttons[port];
    *x = want.x[port];
    *y = want.y[port];
    return true;
}

void conker::netplay::record_input(int port, bool plugged, uint16_t buttons, float x, float y) {
    if (mode != Mode::Record || port < 0 || port >= ports) {
        return;
    }
    std::lock_guard lock{ state_mutex };
    current.plugged[port] = plugged ? 1 : 0;
    current.buttons[port] = buttons;
    current.x[port] = x;
    current.y[port] = y;
}

bool conker::netplay::replay_device(int controller, bool* present) {
    if (mode != Mode::Replay || controller < 0 || controller >= ports) {
        return false;
    }
    *present = device_present[controller];
    return true;
}

void conker::netplay::record_device(int controller, bool present) {
    if (mode != Mode::Record || controller < 0 || controller >= ports) {
        return;
    }
    std::lock_guard lock{ state_mutex };
    device_present[controller] = present;
}


void conker::netplay::on_rng_call(uint32_t caller, uint32_t state) {
    if (mode == Mode::Online) {
        conker::netsession::note_rng_call(caller, state);
        return;
    }
    if (mode == Mode::Off) {
        return;
    }
    std::lock_guard lock{ state_mutex };
    if (in_frame && pending_callers.size() < (1u << 16)) {
        pending_callers.push_back(caller);
    }
}

// Called from conker.toml's hooks at the start of random_u32 and random_float: ra is where the call came from.
extern "C" void conker_netplay_rng(uint8_t* rdram, recomp_context* ctx) {
    // (the game's RNG state, its two words xored, to tell whether two machines were at the same state at each call)
    const uint32_t offset = 0x800885B0u & 0x7FFFFFu;
    uint32_t words[2];
    std::memcpy(words, rdram + offset, 8);
    conker::netplay::on_rng_call((uint32_t)ctx->r31, words[0] * 0x9E3779B1u ^ words[1]);
}

// Called from conker.toml's hook at the start of func_10004F00, the scheduler's start of a frame's graphics task.
// The game takes the length of a frame ("D_800BE9E4", which every timer in the game counts down by) from
// D_8003B238, the number of retraces seen since the last task, which follows the host's real timing: 2 when the
// frame took 2 retraces and more when the host hitched. That makes timers expire a frame earlier or later from
// one run to the next. For a netplay test (and for a netplay game) every frame is 2 retraces, as on a 30 fps N64.
extern "C" void conker_netplay_frame_delta(uint8_t* rdram, recomp_context* ctx) {
    if (!conker::netplay::active()) {
        return;
    }
    // The function only starts a task when D_8002AC5C is 0; otherwise it does nothing, and so does this.
    if (MEM_W(0, (gpr)(int32_t)0x8002AC5C) == 0) {
        MEM_B(0, (gpr)(int32_t)0x8003B238) = 2;
    }
}
