// Online: each machine hears the game from its own player's camera.
//
// The split screen hears a sound in the world from whichever camera is nearest to it (func_1000F6B8 goes through the
// listeners, D_80041F68, one per look camera, and takes the nearest), and a sound played on a player with a camera
// (func_10010FFC) at full volume in the middle, for everybody. On one television that is right; online it is every
// player's sounds at full volume on every machine.
//
// What the game decides from a sound's volume stays as it was (whether func_10010E78 starts a sound, func_10010BE8's
// "too quiet to play" below 100): those decide which sound slots are used, and the slots and their handles are game
// state, the same on every machine. Only the volume and the pan given to the sound player are this machine's:
//   - func_1000F91C and func_1000F9D4 (the volume and pan of a sound that is playing, from its point) are answered
//     from this machine's listener;
//   - func_10010E78 (play a sound at a point) decides from the nearest listener as before, and the sound is started
//     with the volume and pan from this machine's listener;
//   - func_10010FFC (play a sound on an object) with another player's character: started as the game starts it, with
//     the volume and pan of a sound at that character's place, as this machine's player hears it.
// The volume and pan go to the sound player's own state and events, which nothing in the game reads.
//
// CONKER_NET_LOCAL_AUDIO=0 turns it off (every machine hears what the split screen would).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <SDL.h>

#include "recomp.h"

#include "conker.hpp"

// The volume (0..0x7FFF) and pan of a sound at a point, for the nearest listener (init_EB00.c).
extern "C" void func_1000F6B8(uint8_t* rdram, recomp_context* ctx);

namespace {
    constexpr uint32_t listeners = 0x80041F68;       // D_80041F68: one per look camera, in their order, 0x1C bytes each
    constexpr uint32_t listener_words = 7;
    constexpr uint32_t listener_size = 0x1C;
    constexpr uint32_t last_listener = 0x80082FA0;   // D_80082FA0: the index of the last listener in use (0: one)
    constexpr uint32_t look_cameras = 0x800DBFF0;    // pointer to the look cameras, 0x9A0 apart
    constexpr uint32_t look_size = 0x9A0;
    constexpr uint32_t look_player = 0x23D;          // the player a look camera is for
    constexpr uint32_t look_kind = 0x23E;            // the look in use (a player can have more than one record: the one with a kind)
    constexpr uint32_t object_look = 0x318;          // an object's look camera (players' characters)
    // func_1000F6B8 is called with a stack of ours past the game's 8 MB (nothing else there: net_view's regions end at
    // 0x00F46000 and start again at 0x00F50000), so the game's own stack isn't written below its pointer.
    constexpr uint32_t scratch_sp = 0x80F4C000;
    constexpr uint32_t scratch_pan = 0x80F4C800;

    // func_10010BE8's return addresses from the two calls that leave a volume and pan for it (see Pending)
    constexpr uint32_t from_play_at_point = 0x10010F18;
    constexpr uint32_t from_play_on_object = 0x10011078;

    // The volume and pan func_10010BE8 is to give the sound player instead of the game's, for the call about to be made
    // from the frame at `sp` (func_10010BE8's own frame is 0x50 below it) and returning to `ra`.
    struct Pending {
        bool set = false;
        uint32_t sp = 0;
        uint32_t ra = 0;
        uint32_t volume = 0;
        uint32_t pan = 0;
    };
    Pending pending;

    int logged_listener = -2;
    int logged_kind[4] = {};
    unsigned counts[4] = {};

    uint32_t word(uint8_t* rdram, uint32_t address) {
        return (uint32_t)MEM_W(0, (gpr)(int32_t)address);
    }

    void put_word(uint8_t* rdram, uint32_t address, uint32_t value) {
        MEM_W(0, (gpr)(int32_t)address) = (int32_t)value;
    }

    bool switched_on() {
        static const bool on = []() {
            const char* text = SDL_getenv("CONKER_NET_LOCAL_AUDIO");
            return text == nullptr || std::atoi(text) != 0;
        }();
        return on;
    }

    // The listener of this machine's player, or -1 (offline, one listener, or no look camera of this player's).
    int local_listener(uint8_t* rdram) {
        if (!switched_on() || !conker::netsession::active()) {
            return -1;
        }
        const int32_t last = (int32_t)word(rdram, last_listener);
        const uint32_t base = word(rdram, look_cameras);
        int found = -1;
        const int local = conker::netsession::local_slot();
        if (last >= 1 && last <= 3 && (base >> 24) == 0x80 && (base & 0x7FFFFFu) + 4u * look_size <= 0x800000u) {
            for (int k = 0; k <= last; k++) {
                const gpr record = (gpr)(int32_t)(base + (uint32_t)k * look_size);
                if ((int)MEM_BU(look_player, record) == local) {
                    if (found < 0 || MEM_BU(look_kind, record) != 0) {
                        found = k;
                    }
                    if (MEM_BU(look_kind, record) != 0) {
                        break;
                    }
                }
            }
        }
        if (found != logged_listener) {
            logged_listener = found;
            char line[200];
            if (found >= 0) {
                std::snprintf(line, sizeof(line), "[audio] this machine hears from listener %d (look camera of player %d; listeners 0-%d)", found, local + 1, (int)last);
            } else {
                std::snprintf(line, sizeof(line), "[audio] the game's own listener choice (last listener %d, player %d)", (int)last, local + 1);
            }
            conker::netsession::log(line);
            if (found >= 0) {
                for (int k = 0; k <= last; k++) {
                    const gpr record = (gpr)(int32_t)(base + (uint32_t)k * look_size);
                    const uint32_t entry = listeners + (uint32_t)k * listener_size;
                    std::snprintf(line, sizeof(line), "[audio]   listener %d: player %d, kind %02X, at %d %d %d", k, (int)MEM_BU(look_player, record) + 1,
                                  (unsigned)MEM_BU(look_kind, record), (int)word(rdram, entry + 0xC), (int)word(rdram, entry + 0x10), (int)word(rdram, entry + 0x14));
                    conker::netsession::log(line);
                }
            }
        }
        return found;
    }

    // func_1000F6B8 for listener `k` alone: its volume and pan. The game's memory is as it was when this returns.
    void evaluate(uint8_t* rdram, recomp_context* ctx, int k, uint32_t listener_arg, uint32_t x, uint32_t y, uint32_t z,
                  uint32_t near_distance, uint32_t far_distance, uint32_t* volume, uint32_t* pan) {
        uint32_t saved_entry[listener_words];
        for (uint32_t i = 0; i < listener_words; i++) {
            saved_entry[i] = word(rdram, listeners + i * 4);
        }
        const uint32_t saved_last = word(rdram, last_listener);
        for (uint32_t i = 0; i < listener_words; i++) {
            put_word(rdram, listeners + i * 4, word(rdram, listeners + (uint32_t)k * listener_size + i * 4));
        }
        put_word(rdram, last_listener, 0);

        const recomp_context saved = *ctx;
        ctx->r29 = (gpr)(int32_t)scratch_sp;
        ctx->r4 = (gpr)(int32_t)listener_arg;
        ctx->r5 = (gpr)(int32_t)x;
        ctx->r6 = (gpr)(int32_t)y;
        ctx->r7 = (gpr)(int32_t)z;
        put_word(rdram, scratch_sp + 0x10, scratch_pan);
        put_word(rdram, scratch_sp + 0x14, near_distance);
        put_word(rdram, scratch_sp + 0x18, far_distance);
        put_word(rdram, scratch_pan, 0x40);
        func_1000F6B8(rdram, ctx);
        *volume = (uint32_t)ctx->r2;
        *pan = word(rdram, scratch_pan);
        *ctx = saved;

        put_word(rdram, last_listener, saved_last);
        for (uint32_t i = 0; i < listener_words; i++) {
            put_word(rdram, listeners + i * 4, saved_entry[i]);
        }
    }

    void log_change(int which, const char* what, uint32_t game_volume, uint32_t game_pan, uint32_t volume, uint32_t pan, int x, int y, int z) {
        counts[which]++;
        if (logged_kind[which] < 6 || counts[which] % 500 == 0) {
            logged_kind[which]++;
            char line[220];
            std::snprintf(line, sizeof(line), "[audio] %s at %d %d %d: the game's volume %u pan %u, this machine's %u pan %u (%u so far)", what, x, y, z,
                          (unsigned)game_volume, (unsigned)(game_pan & 0x7F), (unsigned)volume, (unsigned)(pan & 0x7F), counts[which]);
            conker::netsession::log(line);
        }
    }

    float float_at(uint8_t* rdram, uint32_t address) {
        const uint32_t bits = word(rdram, address);
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
}

// The hooks (conker.toml):
//   0: func_1000F91C, 1: func_1000F9D4, 2: func_10010E78, just after their call to func_1000F6B8 (its arguments are still in the
//      caller's frame: the listener argument and x, y, z at 0x0-0xC($sp), the pan's address at 0x10, near and far at 0x14, 0x18);
//   3: func_10010FFC, about to start the sound of an object with a look camera with func_10010BE8 ($s0 the object, $a2 the volume);
//   4: func_10010BE8, about to give the sound player the volume ($a2) and pan ($a3);
//   5: func_10010E78 returns.
extern "C" void conker_local_audio(uint8_t* rdram, recomp_context* ctx, int which) {
    if (which == 4) {
        if (pending.set && (uint32_t)ctx->r29 + 0x50u == pending.sp && word(rdram, (uint32_t)ctx->r29 + 0x2Cu) == pending.ra) {
            ctx->r6 = (gpr)(int32_t)pending.volume;
            ctx->r7 = (gpr)(int32_t)pending.pan;
        }
        pending.set = false;
        return;
    }
    if (which == 5) {
        pending.set = false;
        return;
    }
    const int k = local_listener(rdram);
    if (k < 0) {
        return;
    }
    const uint32_t sp = (uint32_t)ctx->r29;
    if (which <= 2) {
        const uint32_t listener_arg = word(rdram, sp), x = word(rdram, sp + 4), y = word(rdram, sp + 8), z = word(rdram, sp + 0xC);
        const uint32_t pan_address = word(rdram, sp + 0x10);
        if (pan_address != sp + 0x2Cu) {
            return;
        }
        const uint32_t game_volume = (uint32_t)ctx->r2, game_pan = word(rdram, pan_address);
        uint32_t volume = 0, pan = 0x40;
        evaluate(rdram, ctx, k, listener_arg, x, y, z, word(rdram, sp + 0x14), word(rdram, sp + 0x18), &volume, &pan);
        if (which == 2) {
            // the game decides from its own volume whether to start the sound (and func_10010BE8, below 100); started, it has this one
            const uint32_t base_volume = (uint32_t)MEM_HU(0x3A, (gpr)(int32_t)sp);
            pending.set = true;
            pending.sp = sp;
            pending.ra = from_play_at_point;
            pending.volume = ((base_volume * volume) >> 15) & 0xFFFFu;
            pending.pan = pan & 0x7Fu;
            log_change(2, "a sound played at a point", (base_volume * game_volume) >> 15, game_pan, pending.volume, pan, (int16_t)x, (int16_t)y, (int16_t)z);
        } else {
            ctx->r2 = (gpr)(int32_t)volume;
            put_word(rdram, pan_address, pan);
            log_change(which, which == 0 ? "a playing sound's volume" : "a playing sound's pan", game_volume, game_pan, volume, pan, (int16_t)x, (int16_t)y, (int16_t)z);
        }
        return;
    }
    if (which == 3) {
        const uint32_t object = (uint32_t)ctx->r16;
        const uint32_t record = word(rdram, object + object_look);
        const uint32_t base = word(rdram, look_cameras);
        if ((record >> 24) != 0x80 || record < base || (record - base) % look_size != 0 || (record - base) / look_size >= 4) {
            static int odd = 0;
            if (odd < 4) {
                odd++;
                char line[160];
                std::snprintf(line, sizeof(line), "[audio] a sound on object %08X, whose look camera %08X isn't one of the four at %08X: left as the game plays it",
                              (unsigned)object, (unsigned)record, (unsigned)base);
                conker::netsession::log(line);
            }
            return;
        }
        const int player = (int)MEM_BU(look_player, (gpr)(int32_t)record);
        if (player == conker::netsession::local_slot()) {
            return;   // this machine's own player: in the middle, at full volume, as the game plays it
        }
        const int x = (int)float_at(rdram, object + 0x14), y = (int)float_at(rdram, object + 0x18), z = (int)float_at(rdram, object + 0x1C);
        const uint32_t game_volume = (uint32_t)ctx->r6 & 0xFFFFu;
        uint32_t volume = 0, pan = 0x40;
        // (the distances func_10010FFC gives a sound on an object without a look camera of its size: full within 500, silent at 4500)
        evaluate(rdram, ctx, k, 0, (uint32_t)x, (uint32_t)y, (uint32_t)z, 500, 4501, &volume, &pan);
        pending.set = true;
        pending.sp = sp;
        pending.ra = from_play_on_object;
        pending.volume = ((game_volume * volume) >> 15) & 0xFFFFu;
        pending.pan = pan & 0x7Fu;
        log_change(3, "a sound on another player", game_volume, 0x40, pending.volume, pan, x, y, z);
    }
}
