// A test (0.9.39): which of the game's memory a cheat code changes. The game keeps entered cheats in memory only (they are gone
// when it is switched off), so to unlock the multiplayer characters online for everybody the flags have to be found. With a file
// named cheat_watch.on next to the game (in its working folder), the game's static data (0x80030000-0x80100000) is compared every
// half second, and a word that changes after staying the same for 10 seconds is written to cheat_watch_log.txt, with the frame
// and the time. Enter the codes one at a time with a pause between them, and the flags show up as lone changes at those times.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>

#include "recomp.h"

namespace {
    constexpr uint32_t watch_start = 0x80030000;
    constexpr uint32_t watch_end = 0x80100000;
    constexpr uint32_t words = (watch_end - watch_start) / 4;

    bool checked = false;
    bool on = false;
    FILE* out = nullptr;
    std::vector<uint32_t> previous;
    std::vector<uint32_t> since;   // the tick each word last changed
    uint32_t tick = 0;
    uint32_t frames = 0;
    std::chrono::steady_clock::time_point started;
}

extern "C" void conker_cheat_watch(uint8_t* rdram) {
    if (!checked) {
        checked = true;
        std::error_code error;
        on = std::filesystem::exists("cheat_watch.on", error);
        if (on) {
            out = std::fopen("cheat_watch_log.txt", "w");
            on = out != nullptr;
            started = std::chrono::steady_clock::now();
            if (on) {
                std::fprintf(out, "cheat watch: 0x%08X-0x%08X every 15 frames; a word that changes after 20 checks (10 s) the same is listed\n",
                             (unsigned)watch_start, (unsigned)watch_end);
                std::fflush(out);
            }
        }
    }
    if (!on) {
        return;
    }
    frames++;
    if (frames % 15 != 0) {
        return;
    }
    tick++;
    if (previous.empty()) {
        previous.resize(words);
        since.assign(words, 0);
        for (uint32_t i = 0; i < words; i++) {
            previous[i] = (uint32_t)MEM_W(0, (gpr)(int32_t)(watch_start + i * 4));
        }
        return;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    int listed = 0;
    for (uint32_t i = 0; i < words; i++) {
        const uint32_t value = (uint32_t)MEM_W(0, (gpr)(int32_t)(watch_start + i * 4));
        if (value == previous[i]) {
            continue;
        }
        if (tick - since[i] >= 20 && listed < 200) {
            listed++;
            std::fprintf(out, "%8.1f s (frame %u): %08X %08X -> %08X (the same for %u s)\n", seconds, (unsigned)frames, (unsigned)(watch_start + i * 4),
                         (unsigned)previous[i], (unsigned)value, (unsigned)((tick - since[i]) / 2));
        }
        previous[i] = value;
        since[i] = tick;
    }
    if (listed != 0) {
        std::fflush(out);
    }
}
