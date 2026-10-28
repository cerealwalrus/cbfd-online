// Online: each machine hears the game from its own player's camera.
//
// The split screen hears a sound in the world from whichever camera is nearest to it (the listeners, D_80041F68, one per
// look camera; func_1000F6B8 and func_100114D0 take the nearest), and a sound played on a player's character (an object
// with a look camera) in the middle, at full volume, for everybody. On one television that is right; online it is every
// player's sounds at full volume on every machine.
//
// What the game decides from a sound's volume stays as it was: whether a sound is started (func_10010E78, the moving
// sounds' list in func_10011624, func_10010BE8's "too quiet to play" below 100), stopped or taken off the list, and the
// values the list keeps. Those decide which sound slots are used, and the slots and their handles are game state, the same
// on every machine. Only the volume and pan the sound player is given are this machine's:
//   - func_10010BE8 (start a sound) is given this machine's volume and pan when it is called from
//       func_10010154, func_10010344 and func_10010FFC for another player's character (its place, as this machine's player
//         hears it); such a sound is then followed every frame (func_10011BB8), as the character moves;
//       func_10010E78 (a sound at a point);
//       func_10011624 (a sound of the moving sounds' list: objects, rockets, explosions);
//   - the list's changes of a playing sound's volume and pan (func_1000F85C) are this machine's, and are sent when this
//     machine's change, not only when the nearest listener's did;
//   - func_1000F91C and func_1000F9D4 (the volume and pan of a sound that is playing, from its point).
//   - the music's layer played with each shot (func_151C2050's request to the music manager): its volume and pan, from this
//     machine's player.
// The volume and pan go to the sound player's own state and events (and the music manager's), which nothing in the game reads.
//
// CONKER_NET_LOCAL_AUDIO=0 turns it off (every machine hears what the split screen would).

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <SDL.h>

#include "recomp.h"

#include "conker.hpp"

// The volume (0..0x7FFF) and pan of a sound at a point, for the nearest listener (init_EB00.c).
extern "C" void func_1000F6B8(uint8_t* rdram, recomp_context* ctx);
// The same with the list's arguments: (x, y, z, volume, near, far, &pan, &volume, &distance).
extern "C" void func_100114D0(uint8_t* rdram, recomp_context* ctx);
// Sets one of a playing sound's values: (handle, what: 8 volume, 4 pan, ...; value).
extern "C" void func_1000F85C(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_10017594(uint8_t* rdram, recomp_context* ctx);
// the game's heap, kept the same on every machine online (heap_watch.cpp)
extern "C" void conker_heap_watch(uint8_t* rdram, recomp_context* ctx);
// online, every multiplayer character for everybody (mp_characters.cpp)
extern "C" void conker_mp_characters(uint8_t* rdram);
extern "C" void conker_coop_frame(uint8_t* rdram, recomp_context* ctx);

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
    constexpr uint32_t sound_slots = 0x800425E0;     // D_800425E0: 16 slots of 12 bytes (u16 handle, ..., the voice at +8)
    // The game's functions are called with a stack of ours past the game's 8 MB (nothing else there: net_view's regions end
    // at 0x00F46000 and start again at 0x00F50000), so the game's own stack isn't written below its pointer.
    constexpr uint32_t scratch_sp = 0x80F4C000;
    constexpr uint32_t scratch_out = 0x80F4C800;     // three words: pan, volume, distance

    // func_10010BE8's return addresses: who is starting the sound
    constexpr uint32_t from_play_at_point = 0x10010F18;    // func_10010E78
    constexpr uint32_t from_play_on_object = 0x10011078;   // func_10010FFC, an object with a look camera
    constexpr uint32_t from_character = 0x100101E0;        // func_10010154, an object with a look camera (its handle at +0x8E)
    constexpr uint32_t from_character_loop = 0x1001040C;   // func_10010344, the same (its handle at +0x8C)
    constexpr uint32_t from_list = 0x10011970;             // func_10011624, the moving sounds' list
    constexpr uint32_t from_no_place = 0x10010F78;         // func_10010F30, a sound with no place (its caller's return address at 0x24 of its frame)
    constexpr uint32_t no_place_character = 0x10010690;    // func_10010630 calls func_10010F30 for an object with a look camera
    constexpr uint32_t current_object = 0x800D154C;        // gCurrentObject: the object the game is running

    // The volume and pan func_10010BE8 is to give the sound player for func_10010E78's call from the frame at `sp`.
    struct Pending {
        bool set = false;
        uint32_t sp = 0;
        uint32_t volume = 0;
        uint32_t pan = 0;
    };
    Pending pending;

    // func_10011624's entry being worked on: the nearest listener's volume and pan as func_100114D0 gave them, and this
    // machine's; then (once the entry's callback has had its say) what this machine sends.
    struct ListEntry {
        uint32_t entry = 0;
        uint32_t nearest_volume = 0;
        uint32_t local_volume = 0;
        uint32_t local_pan = 0x40;
        bool has_pan = false;
        bool ready = false;      // volume and pan below are this machine's for the entry
        uint32_t volume = 0;
        uint32_t pan = 0x40;
    };
    ListEntry list_entry;

    // What this machine last gave the sound player for each slot's sound (handle 0: nothing known).
    struct Sent {
        uint16_t handle = 0;
        uint32_t volume = 0;
        uint32_t pan = 0;
    };
    Sent sent[16];

    // Sounds started on another player's character: followed every frame while the character's handle field still has them.
    struct Followed {
        uint32_t object = 0;     // where the game keeps the sound's handle
        uint32_t owner = 0;      // the character the sound is heard from (see owner_of)
        uint32_t field = 0;      // 0x8E or 0x8C
        uint32_t volume = 0;     // the game's volume for it
        int32_t near_distance = 500;
        int32_t far_distance = 4501;
        uint32_t first_volume = 0, first_pan = 0;   // what it was started with
    };
    Followed followed[16];

    int logged_listener = -2;
    int logged_kind[8] = {};
    unsigned counts[8] = {};

    uint32_t word(uint8_t* rdram, uint32_t address) {
        return (uint32_t)MEM_W(0, (gpr)(int32_t)address);
    }

    float float_at(uint8_t* rdram, uint32_t address);

    uint32_t half(uint8_t* rdram, uint32_t address) {
        return (uint32_t)MEM_HU(0, (gpr)(int32_t)address);
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

    bool ram_pointer(uint32_t address) {
        return (address >> 24) == 0x80 && (address & 0x7FFFFFu) < 0x7FFFF0u;
    }

    // The listener of this machine's player, or -1 (offline, one listener, or no look camera of this player's).
    int local_listener(uint8_t* rdram) {
        if (!switched_on() || !conker::netsession::active()) {
            return -1;
        }
        const int32_t last = (int32_t)word(rdram, last_listener);
        const uint32_t base = word(rdram, look_cameras);
        int found = -1;
        const int local = conker::netsession::view_player();
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
        }
        return found;
    }

    // The player whose character an object is (its look camera's player), or -1.
    int character_player(uint8_t* rdram, uint32_t object) {
        if (!ram_pointer(object)) {
            return -1;
        }
        const uint32_t record = word(rdram, object + object_look);
        const uint32_t base = word(rdram, look_cameras);
        if (!ram_pointer(record) || record < base || (record - base) % look_size != 0 || (record - base) / look_size >= 4) {
            return -1;
        }
        return (int)MEM_BU(look_player, (gpr)(int32_t)record);
    }

    // Delayed sounds (func_10010558, func_10010720: an entry of the moving sounds' list whose callback plays the sound on the
    // object later, from func_10011624, when gCurrentObject is just the last object the game ran): whose they were, from when
    // they were asked for.
    struct Delayed {
        uint32_t object = 0;
        uint32_t sound = 0;
        uint32_t owner = 0;
    };
    Delayed delayed[32];
    unsigned delayed_next = 0;
    bool in_list = false;   // func_10011624 is running

    uint32_t owner_by_current(uint8_t* rdram, uint32_t object);

    // The character a sound on `object` is heard from. The game plays some sounds of whoever is acting on the first player's
    // character (gObjects[0]: a single player's habit, the split screen's sounds being everybody's), such as the weapon pickups:
    // a sound on the first player's character while the game is running another player's character is that player's. In the
    // moving sounds' list, gCurrentObject means nothing: a delayed sound's owner is the one it was asked for by.
    uint32_t owner_of(uint8_t* rdram, uint32_t object, uint32_t sound) {
        (void)rdram;
        if (!in_list) {
            return object;   // (0.9.23's log: the pickups are played on the picker's own character; no guessing from the game's loop)
        }
        int match = -1;
        for (int i = 0; i < 32; i++) {
            const int at = (int)((delayed_next + 31u - (unsigned)i) % 32u);   // the newest first
            if (delayed[at].object == object && delayed[at].object != 0) {
                if (delayed[at].sound == sound) {
                    match = at;
                    break;
                }
                if (match < 0) {
                    match = at;
                }
            }
        }
        if (match < 0) {
            return object;
        }
        const uint32_t owner = delayed[match].owner;
        if (delayed[match].sound == sound) {
            delayed[match].object = 0;
        }
        return owner;
    }

    constexpr uint32_t objects = 0x800CC2D0;          // gObjects: 0x32C bytes each
    constexpr uint32_t object_size = 0x32C;
    constexpr uint32_t object_count = 25;
    constexpr uint32_t current_index = 0x800C3E78;    // gCurrentObjectIndex: the object the game's loop is at (gCurrentObject is
                                                      // only set for some, and keeps the last one otherwise)

    float distance2(uint8_t* rdram, uint32_t a, uint32_t b) {
        const float dx = float_at(rdram, a + 0x14) - float_at(rdram, b + 0x14);
        const float dy = float_at(rdram, a + 0x18) - float_at(rdram, b + 0x18);
        const float dz = float_at(rdram, a + 0x1C) - float_at(rdram, b + 0x1C);
        return dx * dx + dy * dy + dz * dz;
    }

    // Whose a sound on the first player's character is: the character the game's loop is at, or, at another object (a weapon
    // pickup), the player's character nearest to it (within 1500: the one touching it); otherwise the first player's.
    uint32_t owner_by_current(uint8_t* rdram, uint32_t object) {
        if (character_player(rdram, object) != 0) {
            return object;
        }
        const uint32_t index = (uint32_t)MEM_BU(0, (gpr)(int32_t)current_index);
        if (index >= object_count) {
            return object;
        }
        const uint32_t at = objects + index * object_size;
        if (character_player(rdram, at) >= 0) {
            return at;
        }
        if (word(rdram, at) == 0) {
            return object;
        }
        uint32_t best = object;
        float best_d = 1500.0f * 1500.0f;
        for (uint32_t i = 0; i < object_count; i++) {
            const uint32_t candidate = objects + i * object_size;
            if (word(rdram, candidate) != 0 && character_player(rdram, candidate) >= 0) {
                const float d = distance2(rdram, candidate, at);
                if (d < best_d) {
                    best_d = d;
                    best = candidate;
                }
            }
        }
        return best;
    }

    // func_1000FA64 (put an entry on the moving sounds' list) begins: from func_10010558 or func_10010720, a delayed sound on the
    // object at 0x24($sp) (its caller's frame: func_1000FA64 hasn't made its own), whose owner is noted.
    void on_list_add(uint8_t* rdram, recomp_context* ctx) {
        const uint32_t ra = (uint32_t)ctx->r31;
        if (ra != 0x1001061Cu && ra != 0x100107E4u) {
            return;
        }
        const uint32_t object = word(rdram, (uint32_t)ctx->r29 + 0x24);
        if (character_player(rdram, object) < 0) {
            return;
        }
        Delayed& d = delayed[delayed_next % 32u];
        delayed_next++;
        d.object = object;
        d.sound = (uint32_t)ctx->r4 & 0xFFFFu;
        d.owner = object;
    }

    // Listener k alone in the game's table while `call` runs; the table is as it was afterwards.
    template <typename Call>
    void with_listener(uint8_t* rdram, int k, Call call) {
        uint32_t saved_entry[listener_words];
        for (uint32_t i = 0; i < listener_words; i++) {
            saved_entry[i] = word(rdram, listeners + i * 4);
        }
        const uint32_t saved_last = word(rdram, last_listener);
        for (uint32_t i = 0; i < listener_words; i++) {
            put_word(rdram, listeners + i * 4, word(rdram, listeners + (uint32_t)k * listener_size + i * 4));
        }
        put_word(rdram, last_listener, 0);
        call();
        put_word(rdram, last_listener, saved_last);
        for (uint32_t i = 0; i < listener_words; i++) {
            put_word(rdram, listeners + i * 4, saved_entry[i]);
        }
    }

    // func_1000F6B8 for listener `k` alone: its volume (0..0x7FFF) and pan. The game's memory is as it was afterwards.
    void evaluate_point(uint8_t* rdram, recomp_context* ctx, int k, uint32_t listener_arg, uint32_t x, uint32_t y, uint32_t z,
                        uint32_t near_distance, uint32_t far_distance, uint32_t* volume, uint32_t* pan) {
        with_listener(rdram, k, [&]() {
            const recomp_context saved = *ctx;
            ctx->r29 = (gpr)(int32_t)scratch_sp;
            ctx->r4 = (gpr)(int32_t)listener_arg;
            ctx->r5 = (gpr)(int32_t)x;
            ctx->r6 = (gpr)(int32_t)y;
            ctx->r7 = (gpr)(int32_t)z;
            put_word(rdram, scratch_sp + 0x10, scratch_out);
            put_word(rdram, scratch_sp + 0x14, near_distance);
            put_word(rdram, scratch_sp + 0x18, far_distance);
            put_word(rdram, scratch_out, 0x40);
            func_1000F6B8(rdram, ctx);
            *volume = (uint32_t)ctx->r2;
            *pan = word(rdram, scratch_out);
            *ctx = saved;
        });
    }

    // func_100114D0 for listener `k` alone, with the list's arguments: the volume and pan it gives.
    void evaluate_list(uint8_t* rdram, recomp_context* ctx, int k, uint32_t x, uint32_t y, uint32_t z, uint32_t volume_in,
                       uint32_t arg4, uint32_t arg5, bool want_pan, uint32_t* volume, uint32_t* pan) {
        with_listener(rdram, k, [&]() {
            const recomp_context saved = *ctx;
            ctx->r29 = (gpr)(int32_t)scratch_sp;
            ctx->r4 = (gpr)(int32_t)x;
            ctx->r5 = (gpr)(int32_t)y;
            ctx->r6 = (gpr)(int32_t)z;
            ctx->r7 = (gpr)(int32_t)volume_in;
            put_word(rdram, scratch_sp + 0x10, arg4);
            put_word(rdram, scratch_sp + 0x14, arg5);
            put_word(rdram, scratch_sp + 0x18, want_pan ? scratch_out : 0);
            put_word(rdram, scratch_sp + 0x1C, scratch_out + 4);
            put_word(rdram, scratch_sp + 0x20, scratch_out + 8);
            put_word(rdram, scratch_out, 0x40);
            put_word(rdram, scratch_out + 4, 0);
            func_100114D0(rdram, ctx);
            *pan = word(rdram, scratch_out);
            *volume = word(rdram, scratch_out + 4);
            *ctx = saved;
        });
    }

    // func_1000F85C(handle, what, value), from the host.
    bool own_call = false;   // set_sound_value is calling func_1000F85C (hook 17 leaves it alone)

    void set_sound_value(uint8_t* rdram, recomp_context* ctx, uint32_t handle, uint32_t what, uint32_t value) {
        own_call = true;
        struct Clear { ~Clear() { own_call = false; } } clear;
        const recomp_context saved = *ctx;
        ctx->r29 = (gpr)(int32_t)scratch_sp;
        ctx->r4 = (gpr)(int32_t)handle;
        ctx->r5 = (gpr)(int32_t)what;
        ctx->r6 = (gpr)(int32_t)value;
        func_1000F85C(rdram, ctx);
        *ctx = saved;
    }

    bool slot_has(uint8_t* rdram, uint32_t handle) {
        if (handle == 0) {
            return false;
        }
        const uint32_t slot = sound_slots + (handle & 0xFu) * 12u;
        return half(rdram, slot) == handle && word(rdram, slot + 8) != 0;
    }

    void remember_sent(uint32_t handle, uint32_t volume, uint32_t pan) {
        Sent& s = sent[handle & 0xFu];
        s.handle = (uint16_t)handle;
        s.volume = volume;
        s.pan = pan;
    }

    // Sends this machine's volume and pan for a playing sound when they aren't what it was last given.
    void send_if_changed(uint8_t* rdram, recomp_context* ctx, uint32_t handle, uint32_t volume, uint32_t pan) {
        Sent& s = sent[handle & 0xFu];
        const bool known = s.handle == (uint16_t)handle;
        if (!known || s.volume != volume) {
            set_sound_value(rdram, ctx, handle, 8, volume);
        }
        if (!known || s.pan != pan) {
            set_sound_value(rdram, ctx, handle, 4, pan);
        }
        remember_sent(handle, volume, pan);
    }

    void log_change(int which, const char* what, uint32_t game_volume, uint32_t game_pan, uint32_t volume, uint32_t pan, int x, int y, int z) {
        counts[which]++;
        if (logged_kind[which] < 6 || counts[which] % 500 == 0) {
            logged_kind[which]++;
            char line[240];
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

    // A sound on another player's character, as this machine's player hears it at the character's place.
    void character_sound(uint8_t* rdram, recomp_context* ctx, int k, uint32_t object, uint32_t game_volume, int32_t near_distance,
                         int32_t far_distance, uint32_t* volume, uint32_t* pan) {
        if (near_distance <= 0 || far_distance <= near_distance) {
            near_distance = 500;
            far_distance = 4501;
        }
        const int x = (int)float_at(rdram, object + 0x14), y = (int)float_at(rdram, object + 0x18), z = (int)float_at(rdram, object + 0x1C);
        uint32_t attenuation = 0, p = 0x40;
        evaluate_point(rdram, ctx, k, 0, (uint32_t)x, (uint32_t)y, (uint32_t)z, (uint32_t)near_distance, (uint32_t)far_distance, &attenuation, &p);
        *volume = ((game_volume * attenuation) >> 15) & 0xFFFFu;
        *pan = p & 0x7Fu;
    }

    // func_10010BE8 is about to start a sound ($a2 the volume, $a3 the pan; its caller's $s0 saved at 0x24($sp)).
    void on_start_inner(uint8_t* rdram, recomp_context* ctx);

    // (0.10.77) Conker's Gameboy: its tune (0x296-0x29F) and 0x3A0-0x3AB, which idle Conkers played every 20 frames in the logs
    bool gameboy_sound(uint32_t sound) {
        return (sound >= 0x296u && sound <= 0x29Fu) || (sound >= 0x3A0u && sound <= 0x3ABu);
    }

    void on_start(uint8_t* rdram, recomp_context* ctx) {
        on_start_inner(rdram, ctx);
    }

    void on_start_inner(uint8_t* rdram, recomp_context* ctx) {
        // (0.10.75) co-op: Conker's Gameboy (his idle, sounds 0x29C-0x29F every 8 frames). Only this machine's volume: the game's slots
        // and handles are as it has them.
        // (0.10.77) only another player's: this player hears their own Conker's Gameboy (its tune 0x296-0x29F, and 0x3A0-0x3AB, which
        // the idle players' Conkers played every 20 frames in the logs), and never the others'
        if (conker::netmenu::coop_running(rdram)) {
            const uint32_t sound = (uint32_t)ctx->r5 & 0xFFFFu;
            if (gameboy_sound(sound)) {
                ctx->r6 = 0;   // (0.10.79: every player's, this player's own too: the Gameboy isn't heard in co-op)
                return;
            }
        }
        const uint32_t sp = (uint32_t)ctx->r29;
        const uint32_t ra = word(rdram, sp + 0x2C);
        const uint32_t caller_s0 = word(rdram, sp + 0x24);
        if (ra == from_play_at_point) {
            if (pending.set && sp + 0x50u == pending.sp) {
                ctx->r6 = (gpr)(int32_t)pending.volume;
                ctx->r7 = (gpr)(int32_t)pending.pan;
            }
            pending.set = false;
            return;
        }
        pending.set = false;
        if (ra == from_list) {
            if (list_entry.ready && caller_s0 == list_entry.entry) {
                ctx->r6 = (gpr)(int32_t)list_entry.volume;
                ctx->r7 = (gpr)(int32_t)list_entry.pan;
            }
            return;
        }
        const uint32_t outer = ra == from_no_place ? word(rdram, sp + 0x50 + 0x24) : 0;
        if (ra == from_no_place && (outer == 0x1000F0BC || outer == 0x1000F0F0 || outer == 0x1000F128 || outer == 0x1000F15C || outer == 0x1000F194)) {
            // func_1000EFB4, the list's callback for a bullet that has ended (it hit something): its impact and a ricochet, with no
            // place, at the nearest listener's volume (the ricochet at 16000) and pan. The entry is func_1000EFB4's first argument
            // (0x20 of its frame, 0x28 above func_10010F30's); func_10011624 has just worked out its volumes for it (hook 6).
            const uint32_t entry = word(rdram, sp + 0x50 + 0x28 + 0x20);
            if (local_listener(rdram) >= 0 && list_entry.entry == entry && entry != 0) {
                const uint32_t game_volume = (uint32_t)ctx->r6 & 0xFFFFu;
                uint32_t volume = 0;
                if (list_entry.nearest_volume != 0) {
                    const uint64_t scaled = (uint64_t)game_volume * list_entry.local_volume / list_entry.nearest_volume;
                    volume = scaled > 0xFFFFu ? 0xFFFFu : (uint32_t)scaled;
                }
                ctx->r6 = (gpr)(int32_t)volume;
                if (list_entry.has_pan) {
                    ctx->r7 = (gpr)(int32_t)(list_entry.local_pan & 0x7Fu);
                }
            }
            return;
        }
        if (ra == from_no_place && (outer == 0x15127730u || outer == 0x1512777Cu)) {
            // func_15127520, the list's callback for the sniper rifle's laser: its beep, with no place, in the middle at 20000. The
            // entry (its $s0) has the character holding the rifle at +0x18: another player's beep is heard from where that player is.
            // (+0x18 is the holder's look camera record, 0.9.21's log: 0x9A0 apart from D_800DBFF0's, its player at +0x23D; its
            // listener, the same index, is where the player's camera is)
            const uint32_t holder = word(rdram, caller_s0 + 0x18);
            const uint32_t base = word(rdram, look_cameras);
            const int k_now = local_listener(rdram);
            if (k_now >= 0 && ram_pointer(holder) && holder >= base && (holder - base) % look_size == 0 && (holder - base) / look_size < 4) {
                const uint32_t n = (holder - base) / look_size;
                const int holder_player = (int)MEM_BU(look_player, (gpr)(int32_t)holder);
                if (holder_player != conker::netsession::view_player() && (int32_t)n <= (int32_t)word(rdram, last_listener)) {
                    const uint32_t entry = listeners + n * listener_size;
                    const uint32_t x = word(rdram, entry + 0xC), y = word(rdram, entry + 0x10), z = word(rdram, entry + 0x14);
                    uint32_t attenuation = 0, pan = 0x40;
                    evaluate_point(rdram, ctx, k_now, 0, x, y, z, 500, 4501, &attenuation, &pan);
                    ctx->r6 = (gpr)(int32_t)((((uint32_t)ctx->r6 & 0xFFFFu) * attenuation) >> 15);
                    ctx->r7 = (gpr)(int32_t)(pan & 0x7Fu);
                }
            }
            return;
        }
        if (ra == from_no_place && outer == 0x151007B4u) {
            // func_15100600 (the sniper rifle's laser dot, for a character with a look camera): its sound with no place. The
            // character's index in gObjects is at 0x2D9 of its frame (0x28 above func_10010F30's).
            const uint32_t index = (uint32_t)MEM_BU(0, (gpr)(int32_t)(sp + 0x50 + 0x28 + 0x2D9));
            const uint32_t holder = index < 25 ? 0x800CC2D0u + index * 0x32Cu : 0;
            const int holder_player = holder != 0 ? character_player(rdram, holder) : -1;
            const int k_now = local_listener(rdram);
            if (k_now >= 0 && holder_player >= 0 && holder_player != conker::netsession::view_player()) {
                uint32_t volume = 0, pan = 0x40;
                character_sound(rdram, ctx, k_now, holder, (uint32_t)ctx->r6 & 0xFFFFu, 500, 4501, &volume, &pan);
                ctx->r6 = (gpr)(int32_t)volume;
                ctx->r7 = (gpr)(int32_t)pan;
            }
            return;
        }
        const bool no_place = ra == from_no_place && outer == no_place_character;
        if (ra != from_character && ra != from_character_loop && ra != from_play_on_object && !no_place) {
            return;
        }
        const int k = local_listener(rdram);
        const uint32_t owner = owner_of(rdram, caller_s0, (uint32_t)ctx->r5 & 0xFFFFu);
        const int player = character_player(rdram, owner);
        if (k < 0 || player < 0 || player == conker::netsession::view_player()) {
            return;   // this machine's own player's: in the middle, at full volume, as the game plays it
        }
        const uint32_t caller_sp = sp + 0x50;
        int32_t near_distance = 500, far_distance = 4501;
        if (ra == from_character) {
            near_distance = (int16_t)half(rdram, caller_sp + 0x5E);
            far_distance = (int16_t)half(rdram, caller_sp + 0x62);
        }
        else if (ra == from_character_loop) {
            near_distance = (int16_t)half(rdram, caller_sp + 0x66);
            far_distance = (int16_t)half(rdram, caller_sp + 0x6A);
        }
        else if (no_place) {
            // func_10010630's frame, 0x28 above func_10010F30's: (sound, object, volume, near, far)
            near_distance = (int16_t)half(rdram, caller_sp + 0x28 + 0x4E);
            far_distance = (int16_t)half(rdram, caller_sp + 0x28 + 0x52);
        }
        const uint32_t game_volume = (uint32_t)ctx->r6 & 0xFFFFu;
        uint32_t volume = 0, pan = 0x40;
        character_sound(rdram, ctx, k, owner, game_volume, near_distance, far_distance, &volume, &pan);
        ctx->r6 = (gpr)(int32_t)volume;
        ctx->r7 = (gpr)(int32_t)pan;
        if (ra == from_character || ra == from_character_loop) {
            // followed from the next frame on, through the handle the caller puts in the object's field
            const uint32_t field = ra == from_character ? 0x8Eu : 0x8Cu;
            int free_index = -1;
            for (int i = 0; i < 16; i++) {
                if (followed[i].object == caller_s0 && followed[i].field == field) {
                    free_index = i;
                    break;
                }
                if (free_index < 0 && followed[i].object == 0) {
                    free_index = i;
                }
            }
            if (free_index >= 0) {
                Followed& f = followed[free_index];
                f.object = caller_s0;
                f.owner = owner;
                f.field = field;
                f.volume = game_volume;
                f.near_distance = near_distance;
                f.far_distance = far_distance;
                f.first_volume = volume;
                f.first_pan = pan;
            }
        }
        const int x = (int)float_at(rdram, owner + 0x14), y = (int)float_at(rdram, owner + 0x18), z = (int)float_at(rdram, owner + 0x1C);
        log_change(3, owner != caller_s0 ? "a sound of another player's on the first player's character" : "a sound on another player", game_volume, 0x40, volume, pan, x, y, z);
    }

    // Every frame (func_10011BB8): the followed sounds of other players' characters, as the characters move.
    void follow_characters(uint8_t* rdram, recomp_context* ctx) {
        const int k = local_listener(rdram);
        for (Followed& f : followed) {
            if (f.object == 0) {
                continue;
            }
            const uint32_t handle = ram_pointer(f.object) ? half(rdram, f.object + f.field) : 0;
            const int player = character_player(rdram, f.owner);
            if (k < 0 || !slot_has(rdram, handle) || player < 0 || player == conker::netsession::view_player()) {
                f.object = 0;
                continue;
            }
            Sent& s = sent[handle & 0xFu];
            if (s.handle != (uint16_t)handle) {
                remember_sent(handle, f.first_volume, f.first_pan);   // its first frame: what it was started with
            }
            uint32_t volume = 0, pan = 0x40;
            character_sound(rdram, ctx, k, f.owner, f.volume, f.near_distance, f.far_distance, &volume, &pan);
            // (0.10.78) co-op: another player's Gameboy stays silent here too (the character's handle is followed from an earlier
            // sound of theirs, and its volume was worked out again every frame for the Gameboy's sound that followed: heard)
            if (conker::netmenu::coop_running(rdram) && gameboy_sound(half(rdram, sound_slots + (handle & 0xFu) * 12u + 4u))) {
                volume = 0;
            }
            send_if_changed(rdram, ctx, handle, volume, pan);
        }
    }
}

namespace {
    // func_1001263C (the streamed sounds' player, D_800E0E08's: one sound at a time, its volume and pan read only by the audio
    // code) begins: (sound, volume, pan). From func_10012718 (a sound on an object: in the middle at full volume for a character,
    // else from the nearest listener), this machine's volume and pan are given instead.
    void on_stream_start(uint8_t* rdram, recomp_context* ctx) {
        const uint32_t sp = (uint32_t)ctx->r29;   // func_1001263C hasn't made its frame yet: its caller's
        const uint32_t ra = (uint32_t)ctx->r31;
        // (0.10.75, measuring) co-op: the streamed sounds started, to tell which is the Gameboy's music
        if (conker::netmenu::coop_running(rdram)) {
            static int logged = 0;
            if (logged < 60) {
                logged++;
                const uint32_t object = (ra == 0x10012760u || ra == 0x100127BCu) ? word(rdram, sp + 0x44) : 0;
                char line[160];
                std::snprintf(line, sizeof(line), "[audio] f=%u co-op: a streamed sound starts: a0 %08X a1 %08X a2 %08X from %08X, object %08X (player %d)",
                    (unsigned)conker::netsession::frame(), (unsigned)(uint32_t)ctx->r4, (unsigned)(uint32_t)ctx->r5, (unsigned)(uint32_t)ctx->r6, (unsigned)ra,
                    (unsigned)object, object != 0 ? character_player(rdram, object) : -1);
                conker::netsession::log(line);
            }
        }
        uint32_t object = 0;
        int player = -1;
        if (ra == 0x10012760u || ra == 0x100127BCu) {
            object = word(rdram, sp + 0x44);
            player = character_player(rdram, object);
            const int k = local_listener(rdram);
            const bool own = ra == 0x10012760u && player == conker::netsession::view_player();
            if (k >= 0 && ram_pointer(object) && !own && (ra == 0x100127BCu || player >= 0)) {
                const int x = (int)float_at(rdram, object + 0x14), y = (int)float_at(rdram, object + 0x18), z = (int)float_at(rdram, object + 0x1C);
                uint32_t volume = 0, pan = 0x40;
                evaluate_list(rdram, ctx, k, (uint32_t)x, (uint32_t)y, (uint32_t)z, word(rdram, sp + 0x48), half(rdram, sp + 0x52),
                              (uint32_t)(int32_t)(int16_t)half(rdram, sp + 0x4E), true, &volume, &pan);
                ctx->r5 = (gpr)(int32_t)volume;
                ctx->r6 = (gpr)(int32_t)(pan & 0x7Fu);
            }
        }
    }
}

namespace {
    // func_1000F85C (set a playing sound's value) begins: ($a0 handle, $a1 what, $a2 value). The game's own changes of the volume
    // or pan of a sound on another player's character (func_1001091C: a character's sound made louder or quieter as it plays)
    // were the split screen's: they are made this machine's.
    void on_set_value(uint8_t* rdram, recomp_context* ctx) {
        if (own_call) {
            return;
        }
        const uint32_t handle = (uint32_t)ctx->r4 & 0xFFFFu, what = (uint32_t)ctx->r5 & 0xFFFFu;
        if ((what != 8 && what != 4) || handle == 0) {
            return;
        }
        const int k = local_listener(rdram);
        if (k < 0) {
            return;
        }
        for (Followed& f : followed) {
            if (f.object == 0 || !ram_pointer(f.object) || half(rdram, f.object + f.field) != handle) {
                continue;
            }
            const int player = character_player(rdram, f.owner);
            if (player < 0 || player == conker::netsession::view_player()) {
                return;
            }
            if (what == 8) {
                f.volume = (uint32_t)ctx->r6 & 0xFFFFu;
            }
            uint32_t volume = 0, pan = 0x40;
            character_sound(rdram, ctx, k, f.owner, f.volume, f.near_distance, f.far_distance, &volume, &pan);
            ctx->r6 = (gpr)(int32_t)(what == 8 ? volume : pan);
            Sent& sv = sent[handle & 0xFu];
            if (sv.handle == (uint16_t)handle) {
                if (what == 8) {
                    sv.volume = volume;
                } else {
                    sv.pan = pan;
                }
            }
            static int logged = 0;
            if (logged < 6) {
                logged++;
                char line[160];
                std::snprintf(line, sizeof(line), "[audio] the game set the %s of another player's sound %04X: this machine's %u", what == 8 ? "volume" : "pan",
                              (unsigned)handle, (unsigned)(what == 8 ? volume : pan));
                conker::netsession::log(line);
            }
            return;
        }
    }
}

namespace {
    // The music's shot layer (func_151C2050, the game thread): when a gun is fired the game asks the music manager
    // (func_1000E7A0(8, ...): D_80041F08/D_80041F0C, read by func_1000C530) to play a layer of the music, its volume and pan
    // packed in the request: (weapon + 1) | volume << 8 | pan << 16. The split screen works them out
    //   - for a shot from an object without a look camera (or on some maps): at the bullet's point, from the nearest
    //     listener (func_100114D0: near 100, far 1000), sent when louder than 0x1000;
    //   - for a player's character: from the shooter's own values, as the shooter's own camera hears it.
    // Online, every machine heard every shot's layer: this machine's player hears it from its own listener instead (the
    // point's, or the shooter's place for another player's shot). Only the volume and pan sent change; whether the request
    // is sent stays the game's (the music manager's state is the same on every machine).
    uint32_t shot_music_object = 0;

    void shot_music_start(uint8_t* rdram, recomp_context* ctx) {
        (void)rdram;
        shot_music_object = (uint32_t)ctx->r4;
    }

    // The music's shot layer is one sound for every shot: each request replaces what it plays. In a multiplayer match every shot's
    // request is for its point (the game sends it when the point is within 1000 of any player). With each machine's own volume,
    // another player's shot far from this one came out at volume 0 (or nearly), and it replaced this player's own gun as it fired:
    // the gun's sound stopped short. So a request this player can't hear isn't sent at all (as the game does for a shot nobody
    // hears), and another player's shot quieter than what the layer was given in the last few frames doesn't replace it. This player's
    // own shots are always sent: each request plays its shot, and running, the volume of each shot of a burst was a little lower than
    // the one before (the camera falls behind), so in 0.9.44 every other shot of a burst was held back and the gun's sound stopped short.
    uint32_t shot_layer_volume = 0;
    uint32_t shot_layer_frame = 0;
    constexpr uint32_t shot_layer_hold_frames = 8;

    // `character`: about to send the request for a player's character (at 0x151C2284: $t8 (weapon + 1) | volume << 8,
    // $t1 pan << 16, which the call's delay slot puts together in $a1); else for a point (at 0x151C214C, $a1 ready).
    // False: the request for the point is not to be sent (see shot_layer_volume).
    bool shot_music_send(uint8_t* rdram, recomp_context* ctx, bool character) {
        const int k = local_listener(rdram);
        if (k < 0) {
            return true;
        }
        const uint32_t sp = (uint32_t)ctx->r29;
        uint32_t request = character ? (((uint32_t)ctx->r24 & 0xFFFFu) | ((uint32_t)ctx->r9 & 0xFF0000u)) : (uint32_t)ctx->r5;
        const uint32_t game_volume = (request >> 8) & 0xFFu, game_pan = (request >> 16) & 0xFFu;
        uint32_t volume = game_volume, pan = game_pan;
        int player = -1;
        if (character) {
            player = character_player(rdram, shot_music_object);
            if (player < 0 || player == conker::netsession::view_player()) {
                return true;   // this machine's own player's shot: as the game plays it
            }
            uint32_t attenuation = 0, p = 0x40;
            const int x = (int)float_at(rdram, shot_music_object + 0x14), y = (int)float_at(rdram, shot_music_object + 0x18),
                      z = (int)float_at(rdram, shot_music_object + 0x1C);
            evaluate_point(rdram, ctx, k, 0, (uint32_t)x, (uint32_t)y, (uint32_t)z, 500, 4501, &attenuation, &p);
            volume = (game_volume * attenuation) >> 15;
            pan = p & 0x7Fu;
        } else {
            const uint32_t point = word(rdram, sp + 0x54);
            if (!ram_pointer(point)) {
                return true;
            }
            uint32_t attenuation = 0, p = 0x40;
            const int x = (int)float_at(rdram, point), y = (int)float_at(rdram, point + 4), z = (int)float_at(rdram, point + 8);
            evaluate_point(rdram, ctx, k, 0, (uint32_t)x, (uint32_t)y, (uint32_t)z, 100, 1000, &attenuation, &p);
            volume = attenuation > 0x1000u ? (attenuation >> 7) & 0xFFu : 0u;
            pan = p & 0x7Fu;
            const uint32_t now = (uint32_t)conker::netsession::frame();
            const bool recent = now - shot_layer_frame < shot_layer_hold_frames;
            const int shooter = character_player(rdram, shot_music_object);
            const bool own = shooter >= 0 && shooter == conker::netsession::view_player();
            const bool skip = !own && (volume == 0 || (recent && volume < shot_layer_volume));
            static int logged = 0;
            if (logged < 60) {
                logged++;
                char line[160];
                std::snprintf(line, sizeof(line), "[shot layer] f=%u shooter %d (%s), volume %u (game's %u), last %u %u frames ago: %s", now, shooter,
                    own ? "this player" : "another", volume, game_volume, shot_layer_volume, now - shot_layer_frame, skip ? "held back" : "sent");
                conker::netsession::log(line);
            }
            if (skip) {
                return false;
            }
            if (own && volume == 0) {
                volume = game_volume;   // (this player's own shot is never silenced: then as the game had it)
            }
            shot_layer_volume = volume;
            shot_layer_frame = now;
        }
        request = (request & 0xFFu) | (volume & 0xFFu) << 8 | (pan & 0xFFu) << 16;
        if (character) {
            ctx->r24 = (gpr)(int32_t)(request & 0xFFFFu);
            ctx->r9 = (gpr)(int32_t)(request & 0xFF0000u);
        } else {
            ctx->r5 = (gpr)(int32_t)request;
        }
        return true;
    }
}

namespace {
    // A character's sounds (func_10010154 and func_10010FFC: shots, footsteps, voices) share one handle (+0x8E): starting one stops the
    // one before if it is still playing (func_10010BE8, 0x10010C5C). Running, each footstep cut the gunshot fired just before it short.
    // Online, when the new sound is a different one, the one before isn't stopped there: it plays on by itself (its slot is cleared all
    // the same, so the game goes on exactly as before) and is stopped tail_frames later if it is still playing then (a loop would
    // otherwise play for ever). The same sound again (a gun's next shot) stops the one before, as the game has it. The characters'
    // looping sounds (func_10010344, their own handle) aren't touched.
    struct Tail {
        uint32_t voice = 0;
        uint16_t sound = 0;       // the voice's sound (+0x44) when it was let play on
        uint32_t until = 0;
    };
    std::vector<Tail> tails;
    constexpr uint32_t tail_frames = 24;      // 0.8 s
    constexpr uint32_t voices_in_use = 0x8002BA20u;

    // (0.10.96) voices to stop: the host's notes took them off their slots (net_session.cpp), with their sound then
    std::mutex detached_mutex;
    std::vector<std::pair<uint32_t, uint16_t>> detached;
    uint32_t detached_stopped = 0, loose_stopped = 0;
    // (0.10.96) voices playing that no slot holds and no tail is: since which frame (stopped after loose_frames)
    std::vector<std::pair<uint32_t, std::pair<uint16_t, uint32_t>>> loose;
    constexpr uint32_t loose_frames = 150;   // 5 s
    int tails_logged = 0;
    unsigned tails_kept = 0, tails_stopped = 0;

    // func_10010BE8 at 0x10010C5C: $s0 the slot whose sound is still playing, about to be stopped (the call's delay slot loads its
    // voice into $a0: cleared here, the stop does nothing).
    void on_replace(uint8_t* rdram, recomp_context* ctx) {
        if (!conker::netsession::active() || !switched_on()) {
            return;
        }
        const uint32_t sp = (uint32_t)ctx->r29;
        const uint32_t slot = (uint32_t)ctx->r16;
        const uint32_t ra = word(rdram, sp + 0x2C);
        const uint32_t voice = word(rdram, slot + 8);
        if (!ram_pointer(voice) || (ra != from_character && ra != from_play_on_object)) {
            return;
        }
        const uint32_t old_sound = half(rdram, slot + 4) & 0x7FFFu;
        const uint32_t new_sound = word(rdram, sp + 0x54) & 0x7FFFu;
        const int player = character_player(rdram, word(rdram, sp + 0x24));
        const bool keep = old_sound != new_sound;
        if (tails_logged < 80) {
            tails_logged++;
            char line[200];
            std::snprintf(line, sizeof(line), "[audio] f=%u player %d's sound %04X (voice %08X) replaced by %04X: %s", (unsigned)conker::netsession::frame(), player + 1,
                old_sound, voice, new_sound, keep ? "it plays on" : "stopped (the same sound)");
            conker::netsession::log(line);
        }
        if (!keep) {
            return;
        }
        if (tails.size() >= 24) {
            return;   // (as the game has it)
        }
        tails.push_back({ voice, (uint16_t)half(rdram, voice + 0x44), (uint32_t)conker::netsession::frame() + tail_frames });
        put_word(rdram, slot + 8, 0);
        tails_kept++;
    }

    bool voice_in_use(uint8_t* rdram, uint32_t voice) {
        uint32_t current = word(rdram, voices_in_use);
        for (int n = 0; n < 256 && current != 0 && ram_pointer(current); n++) {
            if (current == voice) {
                return true;
            }
            current = word(rdram, current);
        }
        return false;
    }

    bool voice_held(uint8_t* rdram, uint32_t voice) {
        for (uint32_t k = 0; k < 16; k++) {
            if (word(rdram, sound_slots + k * 12 + 8) == voice) {
                return true;
            }
        }
        return false;
    }

    void stop_voice(uint8_t* rdram, recomp_context* ctx, uint32_t voice) {
        // its link (+0x38, the slot's voice field it was started for) pointed at a word of ours first: no slot holds it, and whatever
        // the sound player does there as it ends (a slot's field now another sound's) is done to nothing of the game's
        put_word(rdram, voice + 0x38, scratch_out + 0x20);
        put_word(rdram, scratch_out + 0x20, voice);
        const recomp_context saved = *ctx;
        ctx->r4 = (gpr)(int32_t)voice;
        ctx->r29 = (gpr)(int32_t)scratch_sp;
        func_10017594(rdram, ctx);
        *ctx = saved;
    }

    // (0.10.96) Every frame (the game thread): the voices the host's notes took off their slots are stopped if they still play the same
    // sound and nothing holds them; and any voice that has played for 5 seconds with no slot holding it and no tail of ours (one the game
    // can't stop any more: on the host there are none) is stopped. Only this machine's sound player: nothing of the game's reads it.
    void stop_loose(uint8_t* rdram, recomp_context* ctx) {
        if (!conker::netsession::active()) {
            std::lock_guard lock{ detached_mutex };
            detached.clear();
            loose.clear();
            return;
        }
        std::vector<std::pair<uint32_t, uint16_t>> now_detached;
        {
            std::lock_guard lock{ detached_mutex };
            now_detached.swap(detached);
        }
        for (const auto& d : now_detached) {
            if (ram_pointer(d.first) && voice_in_use(rdram, d.first) && !voice_held(rdram, d.first) && MEM_BU(0x54, (gpr)(int32_t)d.first) != 0 &&
                    (uint16_t)half(rdram, d.first + 0x44) == d.second) {
                stop_voice(rdram, ctx, d.first);
                detached_stopped++;
            }
        }
        const uint32_t now = (uint32_t)conker::netsession::frame();
        if (now % 15 != 0) {
            return;
        }
        std::vector<std::pair<uint32_t, std::pair<uint16_t, uint32_t>>> next;
        uint32_t current = word(rdram, voices_in_use);
        for (int n = 0; n < 256 && current != 0 && ram_pointer(current); n++) {
            const uint32_t voice = current;
            current = word(rdram, current);
            if (MEM_BU(0x54, (gpr)(int32_t)voice) == 0 || voice_held(rdram, voice)) {
                continue;
            }
            bool tail = false;
            for (const Tail& t : tails) {
                tail |= t.voice == voice;
            }
            if (tail) {
                continue;
            }
            const uint16_t sound = (uint16_t)half(rdram, voice + 0x44);
            uint32_t since = now;
            for (const auto& l : loose) {
                if (l.first == voice && l.second.first == sound) {
                    since = l.second.second;
                }
            }
            if (now - since >= loose_frames) {
                stop_voice(rdram, ctx, voice);
                loose_stopped++;
                if (loose_stopped <= 40) {
                    char line[140];
                    std::snprintf(line, sizeof(line), "[audio] f=%u voice %08X (sound %04X) played %u frames with no slot holding it: stopped", now, voice, sound, now - since);
                    conker::netsession::log(line);
                }
                continue;
            }
            next.push_back({ voice, { sound, since } });
        }
        loose.swap(next);
        static uint32_t last_report = 0;
        if (now / 1800u != last_report && (detached_stopped != 0 || loose_stopped != 0)) {
            last_report = now / 1800u;
            char line[140];
            std::snprintf(line, sizeof(line), "[audio] f=%u stopped: %u voices the host's notes took off their slots, %u left playing with no slot", now,
                detached_stopped, loose_stopped);
            conker::netsession::log(line);
        }
    }

    // Every frame (the game thread): the sounds let play on whose time is up are stopped, if they still play and are still theirs.
    void stop_tails(uint8_t* rdram, recomp_context* ctx) {
        stop_loose(rdram, ctx);   // (0.10.96)
        if (tails.empty()) {
            return;
        }
        const uint32_t now = (uint32_t)conker::netsession::frame();
        for (size_t i = 0; i < tails.size();) {
            const Tail t = tails[i];
            if (conker::netsession::active() && now < t.until && now + tail_frames * 4 > t.until) {
                i++;
                continue;
            }
            tails.erase(tails.begin() + (ptrdiff_t)i);
            if (!conker::netsession::active()) {
                continue;
            }
            // still in use, playing, the same sound, and no slot's
            bool in_use = false;
            uint32_t current = word(rdram, voices_in_use);
            for (int n = 0; n < 256 && current != 0 && ram_pointer(current); n++) {
                if (current == t.voice) {
                    in_use = true;
                    break;
                }
                current = word(rdram, current);
            }
            bool held = false;
            for (uint32_t k = 0; k < 16; k++) {
                held |= word(rdram, sound_slots + k * 12 + 8) == t.voice;
            }
            if (!in_use || held || MEM_BU(0x54, (gpr)(int32_t)t.voice) == 0 || (uint16_t)half(rdram, t.voice + 0x44) != t.sound) {
                continue;
            }
            const recomp_context saved = *ctx;
            ctx->r4 = (gpr)(int32_t)t.voice;
            ctx->r29 = (gpr)(int32_t)scratch_sp;
            func_10017594(rdram, ctx);
            *ctx = saved;
            tails_stopped++;
        }
        static uint32_t last_report = 0;
        if (now / 1800u != last_report) {
            last_report = now / 1800u;
            char line[160];
            std::snprintf(line, sizeof(line), "[audio] f=%u sounds let play on after another sound of the same character: %u, stopped at the end of their time: %u", now,
                tails_kept, tails_stopped);
            conker::netsession::log(line);
        }
    }
}

void conker::localaudio::stop_detached(uint32_t voice, uint16_t sound) {
    std::lock_guard lock{ detached_mutex };
    if (detached.size() < 64) {
        detached.push_back({ voice, sound });
    }
}

// func_151C2050 at 0x151C214C, about to send a point's request for the music's shot layer: 1 when it isn't to be sent, the function
// then returning as it does after the call (its frame: $ra at 0x2C($sp), 0x50 bytes).
extern "C" int conker_local_audio_shot(uint8_t* rdram, recomp_context* ctx) {
    if (shot_music_send(rdram, ctx, false)) {
        return 0;
    }
    ctx->r31 = (gpr)(int32_t)MEM_W(0x2C, ctx->r29);
    ctx->r29 = (gpr)(int32_t)((uint32_t)ctx->r29 + 0x50u);
    return 1;
}

// The hooks (conker.toml):
//   0: func_1000F91C, 1: func_1000F9D4, 2: func_10010E78, just after their call to func_1000F6B8 (its arguments are still in the
//      caller's frame: the listener argument and x, y, z at 0x0-0xC($sp), the pan's address at 0x10, near and far at 0x14, 0x18);
//   4: func_10010BE8, about to give the sound player the volume ($a2) and pan ($a3);
//   5: func_10010E78 returns;
//   6: func_10011624, just after its call to func_100114D0 for the entry at $s0 (its stack arguments still at 0x10-0x20($sp));
//   7: func_10011624, the entry's volume (0xB4($sp)) and pan (0xBC($sp)) final, the game about to decide from them;
//   8: func_10011624, func_10010BE8 has returned the started sound's handle ($v0);
//   9, 10: func_10011624, about to send a playing sound's volume ($t3, which the call's delay slot moves to $a2) or pan ($t5);
//   11: func_10011624, done with a playing sound's changes (handle $s2);
//   12: func_10011BB8 (every frame) begins;
//   13: func_1001263C (start a streamed sound) begins;
//   14: func_1000FA64 (put an entry on the moving sounds' list) begins;
//   15, 16: func_10011BB8 calls func_10011624 (the moving sounds' list), and it has returned;
//   17: func_1000F85C (set a playing sound's value) begins;
//   22: func_151C2050 (a shot's request for the music's shot layer) begins; 23, 24: it is about to send the request;
//   25: func_10010BE8, about to stop the sound still playing in the slot it was given the handle of (0x10010C5C).
extern "C" void conker_local_audio(uint8_t* rdram, recomp_context* ctx, int which) {
    const uint32_t sp = (uint32_t)ctx->r29;
    switch (which) {
        case 4:
            on_start(rdram, ctx);
            return;
        case 5:
            pending.set = false;
            return;
        case 25:
            on_replace(rdram, ctx);
            return;
        case 12: {
            stop_tails(rdram, ctx);
            conker_heap_watch(rdram, ctx);
            conker_mp_characters(rdram);
            conker_coop_frame(rdram, ctx);
            follow_characters(rdram, ctx);
            return;
        }
        case 13:
            on_stream_start(rdram, ctx);
            return;
        case 14:
            on_list_add(rdram, ctx);
            return;
        case 15:
            in_list = true;
            return;
        case 16:
            in_list = false;
            return;
        case 17:
            on_set_value(rdram, ctx);
            return;
        case 22:
            shot_music_start(rdram, ctx);
            return;
        case 23:
            return;   // (0x151C214C's hook calls conker_local_audio_shot, which can skip the request)
        case 24:
            shot_music_send(rdram, ctx, true);
            return;
        case 8: {
            const uint32_t handle = (uint32_t)ctx->r2 & 0xFFFFu;
            if (handle != 0 && list_entry.ready && list_entry.entry == (uint32_t)ctx->r16) {
                remember_sent(handle, list_entry.volume, list_entry.pan);
            }
            return;
        }
        case 9:
            if (list_entry.ready && list_entry.entry == (uint32_t)ctx->r16) {
                ctx->r11 = (gpr)(int32_t)list_entry.volume;
                Sent& s = sent[(uint32_t)ctx->r18 & 0xFu];
                if (s.handle == (uint16_t)ctx->r18) {
                    s.volume = list_entry.volume;
                }
            }
            return;
        case 10:
            if (list_entry.ready && list_entry.entry == (uint32_t)ctx->r16) {
                ctx->r13 = (gpr)(int32_t)list_entry.pan;
                Sent& s = sent[(uint32_t)ctx->r18 & 0xFu];
                if (s.handle == (uint16_t)ctx->r18) {
                    s.pan = list_entry.pan;
                }
            }
            return;
        case 11: {
            const uint32_t handle = (uint32_t)ctx->r18 & 0xFFFFu;
            if (list_entry.ready && list_entry.entry == (uint32_t)ctx->r16 && slot_has(rdram, handle)) {
                send_if_changed(rdram, ctx, handle, list_entry.volume, list_entry.pan);
            }
            return;
        }
        default:
            break;
    }
    const int k = local_listener(rdram);
    if (which == 6) {
        list_entry = ListEntry{};
        if (k < 0) {
            return;
        }
        const uint32_t entry = (uint32_t)ctx->r16;
        const uint32_t pan_address = word(rdram, sp + 0x18);
        list_entry.entry = entry;
        list_entry.has_pan = pan_address != 0;
        list_entry.nearest_volume = word(rdram, sp + 0xB4);
        uint32_t volume = 0, pan = 0x40;
        evaluate_list(rdram, ctx, k, (uint32_t)(int32_t)(int16_t)half(rdram, entry + 2), (uint32_t)(int32_t)(int16_t)half(rdram, entry + 4),
                      (uint32_t)(int32_t)(int16_t)half(rdram, entry + 6), word(rdram, entry + 0xC), word(rdram, sp + 0x10), word(rdram, sp + 0x14),
                      list_entry.has_pan, &volume, &pan);
        list_entry.local_volume = volume;
        list_entry.local_pan = pan;
        return;
    }
    if (which == 7) {
        const uint32_t entry = (uint32_t)ctx->r16;
        // (flags: the entry's; 0x80000000 when this machine's volume and pan were not worked out for it)
        if (k < 0 || list_entry.entry != entry || list_entry.entry == 0) {
            list_entry.ready = false;
            return;
        }
        const uint32_t game_volume = word(rdram, sp + 0xB4);
        const uint32_t game_pan = word(rdram, sp + 0xBC);
        uint32_t volume = game_volume;
        if (list_entry.nearest_volume != 0) {
            // what the entry's callback (or the game's mute) made of the nearest listener's volume, applied to this machine's
            const uint64_t scaled = (uint64_t)game_volume * list_entry.local_volume / list_entry.nearest_volume;
            volume = scaled > 0xFFFFu ? 0xFFFFu : (uint32_t)scaled;
        }
        uint32_t pan = game_pan & 0x7Fu;
        if (list_entry.has_pan) {
            pan = list_entry.local_pan & 0x7Fu;
            if (((uint32_t)ctx->r17 & 0x200u) != 0) {
                // (as the game turns the pan round for these)
                pan = 0x80u - pan;
                if (pan == 0x80u) {
                    pan = 0x7Fu;
                }
            }
        }
        list_entry.volume = volume & 0xFFFFu;
        list_entry.pan = pan;
        list_entry.ready = true;
        if (game_volume != 0 && half(rdram, sp + 0xA6) != 0) {
            log_change(6, "a sound of the moving sounds' list", game_volume, game_pan, list_entry.volume, pan,
                       (int16_t)half(rdram, entry + 2), (int16_t)half(rdram, entry + 4), (int16_t)half(rdram, entry + 6));
        }
        return;
    }
    if (k < 0) {
        return;
    }
    if (which <= 2) {
        const uint32_t listener_arg = word(rdram, sp), x = word(rdram, sp + 4), y = word(rdram, sp + 8), z = word(rdram, sp + 0xC);
        const uint32_t pan_address = word(rdram, sp + 0x10);
        if (pan_address != sp + 0x2Cu) {
            return;
        }
        const uint32_t game_volume = (uint32_t)ctx->r2, game_pan = word(rdram, pan_address);
        uint32_t volume = 0, pan = 0x40;
        evaluate_point(rdram, ctx, k, listener_arg, x, y, z, word(rdram, sp + 0x14), word(rdram, sp + 0x18), &volume, &pan);
        if (which == 2) {
            // the game decides from its own volume whether to start the sound (and func_10010BE8, below 100); started, it has this one
            const uint32_t base_volume = (uint32_t)MEM_HU(0x3A, (gpr)(int32_t)sp);
            pending.set = true;
            pending.sp = sp;
            pending.volume = ((base_volume * volume) >> 15) & 0xFFFFu;
            pending.pan = pan & 0x7Fu;
            log_change(2, "a sound played at a point", (base_volume * game_volume) >> 15, game_pan, pending.volume, pan, (int16_t)x, (int16_t)y, (int16_t)z);
        } else {
            ctx->r2 = (gpr)(int32_t)volume;
            put_word(rdram, pan_address, pan);
            log_change(which, which == 0 ? "a playing sound's volume" : "a playing sound's pan", game_volume, game_pan, volume, pan, (int16_t)x, (int16_t)y, (int16_t)z);
        }
    }
}


// 0.10.14: online, footsteps in a 3 or 4 player game as in a 2 player one. The game plays a character's footstep (func_1506BF5C, at
// 0x1506C05C) only while D_80082FA0, the last listener in use, is under 2: one or two players (the Nintendo 64's sound in a 4 way split
// screen). Online each machine hears its own player's place (above), so they're played for every player count; every machine alike,
// so the sound slots stay the same everywhere.
extern "C" void conker_net_footsteps(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (conker::netsession::active() && (int32_t)ctx->r12 > 1) {
        ctx->r12 = 1;
    }
}
