// A test (0.9.39-0.9.40): which of the game's memory a cheat code changes. The game keeps entered cheats in memory only (they are
// gone when it is switched off), so to unlock the multiplayer characters online for everybody their flags have to be found.
// With a file named cheat_watch.on next to the game (in its working folder), F9 takes a picture of the game's 4 MB and compares
// it with the one before. The pictures alternate: a quiet stretch (nothing done: whatever changes then changes by itself, and is
// left out from then on), then a stretch with one code entered, whose changes are written to cheat_watch_log.txt.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>

#include <SDL.h>

#include "recomp.h"

namespace {
    constexpr uint32_t watch_start = 0x80000000;
    constexpr uint32_t watch_end = 0x80400000;
    constexpr uint32_t words = (watch_end - watch_start) / 4;

    bool checked = false;
    bool on = false;
    FILE* out = nullptr;
    std::vector<uint32_t> previous;
    std::vector<uint8_t> noisy;   // changed in a quiet stretch
    int pictures = 0;
    bool key_was_down = false;
    uint32_t frames = 0;

    void picture(uint8_t* rdram) {
        std::vector<uint32_t> now(words);
        for (uint32_t i = 0; i < words; i++) {
            now[i] = (uint32_t)MEM_W(0, (gpr)(int32_t)(watch_start + i * 4));
        }
        pictures++;
        if (pictures == 1) {
            std::fprintf(out, "picture 1 (frame %u): the start. Now wait 10 seconds without touching anything and press F9.\n", (unsigned)frames);
        } else if (pictures % 2 == 0) {
            int count = 0;
            for (uint32_t i = 0; i < words; i++) {
                if (now[i] != previous[i]) {
                    noisy[i] = 1;
                    count++;
                }
            }
            std::fprintf(out, "picture %d (frame %u): a quiet stretch, %d words changed by themselves (left out from now on). Now enter one code and press F9.\n",
                         pictures, (unsigned)frames, count);
        } else {
            std::fprintf(out, "picture %d (frame %u): code number %d entered; what changed (and never changed in a quiet stretch):\n", pictures, (unsigned)frames,
                         (pictures - 1) / 2);
            int listed = 0;
            for (uint32_t i = 0; i < words; i++) {
                if (now[i] != previous[i] && noisy[i] == 0) {
                    if (listed < 400) {
                        std::fprintf(out, "  %08X: %08X -> %08X\n", (unsigned)(watch_start + i * 4), (unsigned)previous[i], (unsigned)now[i]);
                    }
                    listed++;
                }
            }
            std::fprintf(out, "  (%d words)\nNow wait 10 seconds without touching anything and press F9.\n", listed);
        }
        std::fflush(out);
        previous.swap(now);
    }
}

extern "C" void conker_cheat_watch(uint8_t* rdram) {
    if (!checked) {
        checked = true;
        std::error_code error;
        on = std::filesystem::exists("cheat_watch.on", error);
        if (on) {
            out = std::fopen("cheat_watch_log.txt", "w");
            on = out != nullptr;
            if (on) {
                noisy.assign(words, 0);
                std::fprintf(out, "cheat watch: press F9 in the cheats menu to begin.\n");
                std::fflush(out);
            }
        }
    }
    if (!on) {
        return;
    }
    frames++;
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    const bool down = keys != nullptr && keys[SDL_SCANCODE_F9] != 0;
    if (down && !key_was_down) {
        picture(rdram);
    }
    key_was_down = down;
}
