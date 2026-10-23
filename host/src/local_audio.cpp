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
// The volume and pan go to the sound player's own state and events, which nothing in the game reads.
//
// CONKER_NET_LOCAL_AUDIO=0 turns it off (every machine hears what the split screen would).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <SDL.h>

#include "recomp.h"

#include "conker.hpp"

// The volume (0..0x7FFF) and pan of a sound at a point, for the nearest listener (init_EB00.c).
extern "C" void func_1000F6B8(uint8_t* rdram, recomp_context* ctx);
// The same with the list's arguments: (x, y, z, volume, near, far, &pan, &volume, &distance).
extern "C" void func_100114D0(uint8_t* rdram, recomp_context* ctx);
// Sets one of a playing sound's values: (handle, what: 8 volume, 4 pan, ...; value).
extern "C" void func_1000F85C(uint8_t* rdram, recomp_context* ctx);

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
        if (!in_list) {
            return owner_by_current(rdram, object);
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

    uint32_t owner_by_current(uint8_t* rdram, uint32_t object) {
        if (character_player(rdram, object) == 0) {
            const uint32_t current = word(rdram, current_object);
            if (current != object && character_player(rdram, current) > 0) {
                return current;
            }
        }
        return object;
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
        d.owner = owner_by_current(rdram, object);
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
    void set_sound_value(uint8_t* rdram, recomp_context* ctx, uint32_t handle, uint32_t what, uint32_t value) {
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

    // (a test: who starts which sounds, and how loud they come out on this machine. Each place, caller and sound: its first two
    // starts and every 25th, with the volume and pan the sound player gets and where the sound is, when that is known.)
    struct Census {
        uint32_t ra = 0, outer = 0, sound = 0;
        int who = -2;   // the player whose character the game is running (-1: none)
        unsigned count = 0;
    };
    Census census[320];
    bool source_known = false;
    uint32_t source_ra = 0;    // the start it is for (func_10010BE8's return address)
    int source_x = 0, source_y = 0, source_z = 0;
    uint32_t source_flags = 0;

    void set_source(uint32_t ra, int x, int y, int z, uint32_t flags) {
        source_known = true;
        source_ra = ra;
        source_x = x;
        source_y = y;
        source_z = z;
        source_flags = flags;
    }

    int local_listener(uint8_t* rdram);

    void count_start(uint8_t* rdram, const char* what, uint32_t ra, uint32_t outer, uint32_t sound, uint32_t game_volume, uint32_t game_pan,
                     uint32_t volume, uint32_t pan, uint32_t object, int player) {
        Census* c = nullptr;
        const int who = character_player(rdram, word(rdram, 0x800D154C));
        for (Census& entry : census) {
            if ((entry.ra == ra && entry.outer == outer && entry.sound == sound && entry.who == who) || entry.ra == 0) {
                c = &entry;
                break;
            }
        }
        const bool known = source_known && source_ra == ra;
        source_known = false;
        if (c == nullptr) {
            return;
        }
        c->ra = ra;
        c->outer = outer;
        c->sound = sound;
        c->who = who;
        c->count++;
        if (c->count <= 2 || c->count % 25 == 0) {
            // (and the object the game is running, gCurrentObject: a sound with no place may still be a character's doing)
            const uint32_t current = word(rdram, 0x800D154C);
            const int current_player = character_player(rdram, current);
            const int k = local_listener(rdram);
            int lx = 0, ly = 0, lz = 0;
            if (k >= 0) {
                const uint32_t entry = listeners + (uint32_t)k * listener_size;
                lx = (int)word(rdram, entry + 0xC);
                ly = (int)word(rdram, entry + 0x10);
                lz = (int)word(rdram, entry + 0x14);
            }
            char where[96];
            if (known) {
                std::snprintf(where, sizeof(where), "at %d %d %d (flags %X)", source_x, source_y, source_z, (unsigned)source_flags);
            } else {
                std::snprintf(where, sizeof(where), "with no place");
            }
            char line[420];
            std::snprintf(line, sizeof(line), "[audio] %s from %08X (its caller %08X): sound %u %s, the game's volume %u pan %u, played at %u pan %u%s; object %08X%s, the game running %08X%s; this machine's listener at %d %d %d (%u so far)",
                          what, (unsigned)ra, (unsigned)outer, (unsigned)sound, where, (unsigned)game_volume, (unsigned)(game_pan & 0x7F), (unsigned)volume,
                          (unsigned)(pan & 0x7F), (volume != game_volume || (pan & 0x7F) != (game_pan & 0x7F)) ? " (this machine's)" : "", (unsigned)object,
                          player >= 0 ? (std::string(" (player ") + std::to_string(player + 1) + "'s)").c_str() : "", (unsigned)current,
                          current_player >= 0 ? (std::string(" (player ") + std::to_string(current_player + 1) + "'s)").c_str() : "", lx, ly, lz, c->count);
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

    void on_start(uint8_t* rdram, recomp_context* ctx) {
        const uint32_t sp = (uint32_t)ctx->r29;
        const uint32_t ra = word(rdram, sp + 0x2C);
        const uint32_t game_volume = (uint32_t)ctx->r6 & 0xFFFFu, game_pan = (uint32_t)ctx->r7 & 0xFFu;
        on_start_inner(rdram, ctx);
        if (conker::netsession::active()) {
            const uint32_t caller_s0 = word(rdram, sp + 0x24);
            // (the caller's caller: its return address in the caller's frame, where the callers that are wrappers keep it)
            uint32_t outer = 0;
            if (ra == 0x10010F78) {
                outer = word(rdram, sp + 0x50 + 0x24);   // func_10010F30
            }
            else if (ra == from_play_at_point) {
                outer = word(rdram, sp + 0x50 + 0x24);   // func_10010E78
            }
            else if (ra == from_character || ra == from_character_loop) {
                outer = word(rdram, sp + 0x50 + 0x3C);   // func_10010154, func_10010344
            }
            count_start(rdram, "a sound started", ra, outer, (uint32_t)ctx->r5 & 0xFFFFu, game_volume, game_pan, (uint32_t)ctx->r6 & 0xFFFFu,
                        (uint32_t)ctx->r7 & 0xFFu, caller_s0, character_player(rdram, caller_s0));
        }
    }

    void on_start_inner(uint8_t* rdram, recomp_context* ctx) {
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
                set_source(from_no_place, (int16_t)half(rdram, entry + 2), (int16_t)half(rdram, entry + 4), (int16_t)half(rdram, entry + 6), word(rdram, entry + 0x10));
            }
            return;
        }
        if (ra == from_no_place && (outer == 0x15127730u || outer == 0x1512777Cu)) {
            // func_15127520, the list's callback for the sniper rifle's laser: its beep, with no place, in the middle at 20000. The
            // entry (its $s0) has the character holding the rifle at +0x18: another player's beep is heard from where that player is.
            const uint32_t holder = word(rdram, caller_s0 + 0x18);
            const int holder_player = character_player(rdram, holder);
            const int k_now = local_listener(rdram);
            if (k_now >= 0 && holder_player >= 0 && holder_player != conker::netsession::local_slot()) {
                uint32_t volume = 0, pan = 0x40;
                set_source(ra, (int)float_at(rdram, holder + 0x14), (int)float_at(rdram, holder + 0x18), (int)float_at(rdram, holder + 0x1C), 2u);
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
        if (k < 0 || player < 0 || player == conker::netsession::local_slot()) {
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
        set_source(ra, (int)float_at(rdram, owner + 0x14), (int)float_at(rdram, owner + 0x18), (int)float_at(rdram, owner + 0x1C), owner != caller_s0 ? 1u : 0u);
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
            if (k < 0 || !slot_has(rdram, handle) || player < 0 || player == conker::netsession::local_slot()) {
                f.object = 0;
                continue;
            }
            Sent& s = sent[handle & 0xFu];
            if (s.handle != (uint16_t)handle) {
                remember_sent(handle, f.first_volume, f.first_pan);   // its first frame: what it was started with
            }
            uint32_t volume = 0, pan = 0x40;
            character_sound(rdram, ctx, k, f.owner, f.volume, f.near_distance, f.far_distance, &volume, &pan);
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
        const uint32_t game_volume = (uint32_t)ctx->r5, game_pan = (uint32_t)ctx->r6;
        const char* done = "as the game plays it";
        uint32_t object = 0;
        int player = -1;
        if (ra == 0x10012760u || ra == 0x100127BCu) {
            object = word(rdram, sp + 0x44);
            player = character_player(rdram, object);
            const int k = local_listener(rdram);
            const bool own = ra == 0x10012760u && player == conker::netsession::local_slot();
            if (k >= 0 && ram_pointer(object) && !own && (ra == 0x100127BCu || player >= 0)) {
                const int x = (int)float_at(rdram, object + 0x14), y = (int)float_at(rdram, object + 0x18), z = (int)float_at(rdram, object + 0x1C);
                uint32_t volume = 0, pan = 0x40;
                evaluate_list(rdram, ctx, k, (uint32_t)x, (uint32_t)y, (uint32_t)z, word(rdram, sp + 0x48), half(rdram, sp + 0x52),
                              (uint32_t)(int32_t)(int16_t)half(rdram, sp + 0x4E), true, &volume, &pan);
                ctx->r5 = (gpr)(int32_t)volume;
                ctx->r6 = (gpr)(int32_t)(pan & 0x7Fu);
                done = "this machine's volume and pan given";
            }
        }
        (void)done;
        if (conker::netsession::active()) {
            count_start(rdram, "a streamed sound started", ra, 0, (uint32_t)ctx->r4 & 0xFFFFu, game_volume, game_pan, (uint32_t)ctx->r5, (uint32_t)ctx->r6, object, player);
        }
    }
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
//   15, 16: func_10011BB8 calls func_10011624 (the moving sounds' list), and it has returned.
extern "C" void conker_local_audio(uint8_t* rdram, recomp_context* ctx, int which) {
    const uint32_t sp = (uint32_t)ctx->r29;
    switch (which) {
        case 4:
            on_start(rdram, ctx);
            return;
        case 5:
            pending.set = false;
            return;
        case 12:
            follow_characters(rdram, ctx);
            return;
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
        set_source(from_list, (int16_t)half(rdram, entry + 2), (int16_t)half(rdram, entry + 4), (int16_t)half(rdram, entry + 6),
                   ((uint32_t)ctx->r17 & 0x7FFFFFFFu) | ((k < 0 || list_entry.entry != entry || list_entry.entry == 0) ? 0x80000000u : 0u));
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
            set_source(from_play_at_point, (int16_t)x, (int16_t)y, (int16_t)z, 0);
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
