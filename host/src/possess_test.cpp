// Test build only (branch possess-test), offline, in a multiplayer match: steps towards a 5th player.
//
//   D-pad Up:   controller 1 drives the bot nearest player 1 (as its AI would), and player 1's camera follows that bot.
//   D-pad Down: the nearest bot among characters 2 to 4 gets player 1's camera and controller and is moved as a person is
//               (camera-relative stick, R-Look, the C-buttons); player 1 stands without a camera.
//   Either again: back to player 1.
//   D-pad Left: a camera the game doesn't count (slot 5 of its cameras, made for character 5, a bot: a 5th player's) is made and
//               run each frame, and the screen shows it; again: the screen shows player 1's again (the camera keeps running).
//   D-pad Right: controller 1 plays character 5 through that camera (made first if need be), as a 5th player would; again: back.
// The game is told of one player all along: the extra camera is the step towards a 5th player's.
//
// How the game drives a bot: its AI (the script commands, func_1509DFC4, e.g. func_1508CAD8 "go to") never touches a controller. It
// writes its "hands" into the character's brain (character + 0x31C):
//   +0x84 set: driven by the AI;  +0x85 / +0x86 the stick's X / Y;  +0x8A the buttons held;  +0x8C the buttons pressed this frame.
// The movement code (func_1504CB98, from 0x1504CC9C) copies those into the character's controller (D_800CC284), and walking goes the
// way and speed the path finder put in the character (+0x78, +0x44) (at 0x1504E0A0).
//
// A camera (0x9A0 bytes, D_800DBFF0's, one per player, func_150054C4) is tied to its character both ways: character + 0x318 the
// camera (0 for a bot), camera + 0x3D0 the character and + 0x3D4 its brain. The camera reads its controller itself (+0x36C, and the
// pressed buttons' slot +0x368), and +0x23D is its view.
//
// Hooks (conker.toml):
//   - conker_possess_pad (func_1504A730, 0x1504A974, a character's controller picked): player 1's input kept and player 1 given an
//     empty controller; the driven character of D-pad Down given controller 1; controller 1 never left to a later bot (the AI's
//     copy would write into it).
//   - conker_possess_brain (func_1504CB98, 0x1504CC9C): D-pad Up's bot, controller 1 put in its brain before it is copied.
//   - conker_possess_move (func_1504CB98, 0x1504E0A4): D-pad Up's bot walks by the stick, not its path finder.
// Writes conker_possess.log next to the game.

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "recomp.h"

#include "conker.hpp"

namespace {
    constexpr uint32_t characters = 0x800CC2D0;      // the characters, 0x32C apart, 25 of them
    constexpr uint32_t character_size = 0x32C;
    constexpr int character_count = 25;
    constexpr uint32_t controller_now = 0x800CC284;  // D_800CC284: the controller of the character being moved
    constexpr uint32_t pressed_table = 0x800BE710;   // the buttons pressed this frame, a halfword per controller (4)
    constexpr uint32_t multiplayer = 0x800BE616;     // D_800BE616: a multiplayer arena

    constexpr uint16_t button_start = 0x1000;
    constexpr uint16_t button_dpad_up = 0x0800;
    constexpr uint16_t button_dpad_down = 0x0400;

    enum Mode { off, drive, own };
    Mode mode = off;
    int target = -1;
    uint16_t held = 0, pressed = 0;
    int8_t stick_x = 0, stick_y = 0;
    uint32_t frames = 0;
    uint32_t pad0 = 0;              // controller 1's record
    bool pressed_hidden = false;    // player 1's pressed buttons taken off the table for its own move, to put back after

    // What D-pad Down changed, to put back.
    uint32_t camera0 = 0, saved_brain_flag = 0;

    // D-pad Left / Right: the extra camera (slot 1, for character 1).
    constexpr uint16_t button_dpad_left = 0x0200;
    constexpr uint16_t button_dpad_right = 0x0100;
    constexpr uint32_t camera_base = 0x800DBFF0;     // D_800DBFF0: the cameras, 0x9A0 apart
    constexpr uint32_t camera_size = 0x9A0;
    constexpr uint32_t camera_buffers = 0x800BE2B0;  // D_800BE2B0: a buffer per camera (2 x the frame's width)
    constexpr uint32_t camera_bytes = 0x800DBFF4;    // D_800DBFF4: a byte per camera, 0 at the start
    constexpr uint32_t frame_width = 0x800BE620;     // D_800BE620
    constexpr uint32_t pads = 0x800BE728;            // D_800BE728: the controllers' records, a pointer each
    constexpr uint32_t matrices = 0x800DC2A0;        // D_800DC2A0[2]: the cameras' view matrices (0x40 each), this frame's by
    constexpr uint32_t matrix_buffer = 0x800BE9C0;   //   D_800BE9C0
    constexpr uint32_t last_player = 0x80082FA0;     // D_80082FA0: players - 1
    constexpr uint32_t extra_buffer = 0x80FD0000;    // ours, past the game's 8 MB: the extra camera's buffer
    constexpr int extra = 4;                         // the extra camera's slot and character: a 5th player's
    constexpr uint32_t extra_record = 0x80FD0800;    // ours: the extra player's controller record (the game has 4)
    bool have_args = false;
    uint32_t camera_arg2 = 0, camera_arg3 = 0;       // func_150054C4's last two, as the game made camera 0
    bool extra_wanted = false, extra_ready = false, extra_shown = false, extra_played = false;
    uint32_t extra_brain_flag = 1;
    uint16_t extra_pressed = 0;

    uint32_t extra_camera(uint8_t* rdram) {
        return (uint32_t)MEM_W(0, (gpr)(int32_t)camera_base) + (uint32_t)extra * camera_size;
    }
    uint32_t pad_record(uint8_t* rdram, int k) {
        if (k >= 4) {
            return extra_record;
        }
        const uint32_t table = (uint32_t)MEM_W(0, (gpr)(int32_t)pads);
        return (table >> 24) == 0x80 ? (uint32_t)MEM_W(4 * k, (gpr)(int32_t)table) : 0;
    }

    // ---- Slot 4 of the game's tables of 4 (one entry per player or camera) ----
    // The camera code keeps its cameras' data in a dozen tables of 4 at fixed addresses; a 5th camera's entry would be what
    // comes after each (other tables, other things). Every place that reads or writes one of them by a camera's index
    // (found from the code: the index from the camera's +0x23D, +0x368, +0x3CC, or the moving character's) has a hook
    // (conker_slot4) that sends slot 4 to an entry of ours instead.
    // always: a camera's table (on while the extra camera is there); otherwise a view's (on only while the extra view is drawn)
    struct Slot4Table { uint32_t base, stride; bool always; };
    constexpr Slot4Table slot4_tables[] = {
    // Generated (release/gen2.py): the tables of 4 whose 5th entry is kept in ours. always: the cameras' (on while the extra camera
    // is there); otherwise the views' (on only while the extra view is drawn).
        {0x80089240, 0x4, false},   // 0
        {0x80089524, 0x4, true},   // 1
        {0x80089534, 0x4, true},   // 2
        {0x80089560, 0x4, true},   // 3
        {0x80089570, 0x4, true},   // 4
        {0x80089580, 0x4, true},   // 5
        {0x800BE2B0, 0x4, true},   // 6
        {0x800BE6C0, 0x4, false},   // 7
        {0x800BE6D0, 0x4, false},   // 8
        {0x800BE710, 0x2, true},   // 9
        {0x800CC5D4, 0x4, false},   // 10
        {0x800D18A4, 0x1, false},   // 11
        {0x800D2CA8, 0x40, false},   // 12
        {0x800D2E30, 0x4, true},   // 13
        {0x800D9B68, 0x3, false},   // 14
        {0x800D9B78, 0x3, false},   // 15
        {0x800D9BD0, 0x10, false},   // 16
        {0x800D9C10, 0x40, false},   // 17
        {0x800D9D10, 0x40, true},   // 18
        {0x800D9E10, 0x4, false},   // 19
        {0x800D9E70, 0x6, false},   // 20
        {0x800D9E88, 0x3, false},   // 21
        {0x800D9E98, 0x3, false},   // 22
        {0x800D9EA8, 0x3, false},   // 23
        {0x800D9EB8, 0x3, false},   // 24
        {0x800DBE30, 0x2, false},   // 25
        {0x800DBE88, 0x8, false},   // 26
        {0x800DBEE8, 0x2, false},   // 27
        {0x800DBFF4, 0x1, true},   // 28
        {0x800DC028, 0x24, true},   // 29
        {0x800DC0C0, 0x4C, true},   // 30
        {0x800DC200, 0x20, true},   // 31
        {0x800DC2C0, 0x68, true},   // 32
        {0x800DCD30, 0x3, false},   // 33
        {0x800DCDE0, 0x18, true},   // 34
        {0x800DD2B0, 0x1, false},   // 35
        {0x800DD2C0, 0x1, false},   // 36
        {0x800DDD78, 0x1, false},   // 37
        {0x800DDD90, 0x3, false},   // 38
        {0x800DDD9C, 0x1, false},   // 39
        {0x800DDDA0, 0x3, false},   // 40
        {0x800DDDAC, 0x1, false},   // 41
        {0x800DDDB0, 0x4, false},   // 42
        {0x800DDDC0, 0x1, false},   // 43
        {0x800DDDC8, 0x4, false},   // 44
        {0x800DDDD8, 0x4, false},   // 45
        {0x800DDDE8, 0x8, false},   // 46
        {0x800DDE10, 0x2, false},   // 47
        {0x800DDE1C, 0x1, false},   // 48
        {0x800DDE20, 0x1, false},   // 49
        {0x800DDE28, 0x4, false},   // 50
        {0x800DF090, 0x10, false},   // 51
    };
    constexpr int slot4_count = (int)(sizeof(slot4_tables) / sizeof(slot4_tables[0]));
    constexpr uint32_t slot4_shadow = 0x80FD1000;    // ours: 0x100 a table
    bool slot4_on = false;          // the cameras' tables
    bool view_pass_on = false;      // the views' tables: the extra view is being drawn
    uint32_t slot4_moves = 0;

    uint32_t shadow_of(uint32_t base) {
        for (int i = 0; i < slot4_count; i++) {
            if (slot4_tables[i].base == base) {
                return slot4_shadow + 0x100u * (uint32_t)i;
            }
        }
        return 0;
    }
    constexpr uint32_t pressed_shadow_table = 0x800BE710;   // the buttons pressed this frame, per controller
    constexpr uint32_t view_buffers = 0x800DBE88;           // D_800DBE88: two buffers a view (what it draws, a frame each)
    constexpr uint32_t view_buffer_ours = 0x80FD8000;       // ours: the extra view's (up to 0x4000)
    constexpr uint32_t view_lights = 0x800D9E10;            // D_800D9E10: a block of 11 x 0x60 a view (func_150006E0, func_1515D480)
    constexpr uint32_t view_lights_ours = 0x80FDC000;       // ours: the extra view's

    void log(const char* format, ...);

    void slot4_reset(uint8_t* rdram) {
        for (int i = 0; i < slot4_count; i++) {
            const uint32_t shadow = slot4_shadow + 0x100u * (uint32_t)i;
            for (uint32_t b = 0; b < 0x100; b += 4) {
                MEM_W(b, (gpr)(int32_t)shadow) = 0;
            }
            // the cameras' screen areas (func_1512ABF8), and a view's tables: as camera / view 1's (the same scene, full screen); the
            // views' pointers (their buffers) apart, below
            const uint32_t base = slot4_tables[i].base;
            if (base == 0x800DC200 || (!slot4_tables[i].always && base != view_buffers && base != view_lights)) {
                for (uint32_t b = 0; b < slot4_tables[i].stride; b++) {
                    MEM_B(b, (gpr)(int32_t)shadow) = MEM_B(b, (gpr)(int32_t)slot4_tables[i].base);
                }
            }
        }
        for (uint32_t b = 0; b < 8; b++) {
            MEM_B(b, (gpr)(int32_t)extra_record) = 0;
        }
        // The extra view's block of func_150006E0's (func_1515D480(11): 11 x 0x60, zeroed): ours.
        for (uint32_t b = 0; b < 11 * 0x60; b += 4) {
            MEM_W(b, (gpr)(int32_t)view_lights_ours) = 0;
        }
        if (const uint32_t lights = shadow_of(view_lights)) {
            MEM_W(0, (gpr)(int32_t)lights) = (int32_t)view_lights_ours;
        }
        // The extra view's two buffers (func_15000AD0 makes the views' at the scene's start): as large as view 1's, of ours. Two of 0x40
        // with what's in view 1's (that kind of scene sets them up once), or two halves of one block the views fill each frame.
        const uint32_t first = (uint32_t)MEM_W(0, (gpr)(int32_t)view_buffers), second = (uint32_t)MEM_W(4, (gpr)(int32_t)view_buffers);
        const uint32_t shadow = shadow_of(view_buffers);
        if (shadow != 0 && (first >> 24) == 0x80 && second > first) {
            const uint32_t half = second - first;
            const uint32_t size = half == 0x40 ? 0x80u : std::min(2u * half, 0x4000u);
            for (uint32_t b = 0; b < size; b += 4) {
                MEM_W(b, (gpr)(int32_t)view_buffer_ours) = half == 0x40 ? MEM_W(b, (gpr)(int32_t)first) : 0;
            }
            MEM_W(0, (gpr)(int32_t)shadow) = (int32_t)view_buffer_ours;
            MEM_W(4, (gpr)(int32_t)shadow) = (int32_t)(view_buffer_ours + std::min(half, size / 2));
            log("the extra view's buffers: ours, %X bytes (view 1's %08X, %08X)", (unsigned)size, (unsigned)first, (unsigned)second);
        }
    }

    void log(const char* format, ...) {
        static FILE* file = std::fopen("conker_possess.log", "w");
        if (file == nullptr) {
            return;
        }
        va_list args;
        va_start(args, format);
        std::vfprintf(file, format, args);
        va_end(args);
        std::fputc('\n', file);
        std::fflush(file);
    }

    // An address in the game's memory, or in ours past its 8 MB (0x80F00000 on: the extra player's controller record and the like).
    bool in_rdram(uint32_t address) {
        return (address >> 24) == 0x80 && ((address & 0xFFFFFFu) < 0x7FFF00u || (address & 0xFFFFFFu) >= 0xF00000u);
    }

    uint32_t character(int k) {
        return characters + (uint32_t)k * character_size;
    }

    uint32_t word(uint8_t* rdram, uint32_t address) {
        return (uint32_t)MEM_W(0, (gpr)(int32_t)address);
    }

    void set_word(uint8_t* rdram, uint32_t address, uint32_t value) {
        MEM_W(0, (gpr)(int32_t)address) = (int32_t)value;
    }

    uint32_t brain_of(uint8_t* rdram, int k) {
        const uint32_t brain = word(rdram, character(k) + 0x31C);
        return in_rdram(brain) ? brain : 0;
    }

    bool in_match(uint8_t* rdram, int k) {
        const uint32_t object = character(k);
        return MEM_BU(0x127, (gpr)(int32_t)object) != 0xFF && MEM_BU(0x2FA, (gpr)(int32_t)object) != 0;
    }

    // A character the AI drives (a bot), in the match. 0 if not.
    uint32_t bot_brain(uint8_t* rdram, int k) {
        if (!in_match(rdram, k)) {
            return 0;
        }
        const uint32_t brain = brain_of(rdram, k);
        if (brain == 0 || MEM_BU(0x84, (gpr)(int32_t)brain) == 0 || word(rdram, character(k) + 0x318) != 0) {
            return 0;
        }
        return brain;
    }

    float position(uint8_t* rdram, uint32_t object, uint32_t offset) {
        const uint32_t bits = word(rdram, object + offset);
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    int nearest_bot(uint8_t* rdram, int last) {
        const uint32_t me = character(0);
        const float x = position(rdram, me, 0x14), y = position(rdram, me, 0x18), z = position(rdram, me, 0x1C);
        int best = -1;
        float best_distance = 0.0f;
        for (int k = 1; k <= last; k++) {
            if (bot_brain(rdram, k) == 0) {
                continue;
            }
            const uint32_t object = character(k);
            const float dx = position(rdram, object, 0x14) - x, dy = position(rdram, object, 0x18) - y, dz = position(rdram, object, 0x1C) - z;
            const float distance = dx * dx + dy * dy + dz * dz;
            if (best < 0 || distance < best_distance) {
                best = k;
                best_distance = distance;
            }
        }
        return best;
    }

    // The camera follows character k (its object and brain).
    void camera_follow(uint8_t* rdram, uint32_t camera, int k) {
        set_word(rdram, camera + 0x3D0, character(k));
        set_word(rdram, camera + 0x3D4, brain_of(rdram, k));
    }

    // D-pad Down: character k takes player 1's camera and is a person to the game.
    void own_apply(uint8_t* rdram) {
        const uint32_t bot = character(target);
        set_word(rdram, bot + 0x318, camera0);
        set_word(rdram, character(0) + 0x318, 0);
        camera_follow(rdram, camera0, target);
        const uint32_t brain = brain_of(rdram, target);
        if (brain != 0) {
            MEM_B(0x84, (gpr)(int32_t)brain) = 0;
        }
    }

    void stop(uint8_t* rdram, const char* why) {
        if (mode == own) {
            set_word(rdram, character(target) + 0x318, 0);
            set_word(rdram, character(0) + 0x318, camera0);
            const uint32_t brain = brain_of(rdram, target);
            if (brain != 0) {
                MEM_B(0x84, (gpr)(int32_t)brain) = (int8_t)saved_brain_flag;
            }
            if (target < 4) {
                MEM_H(2 * (uint32_t)target, (gpr)(int32_t)pressed_table) = 0;
            }
        }
        if (mode != off) {
            const uint32_t camera = word(rdram, character(0) + 0x318);
            if (in_rdram(camera)) {
                camera_follow(rdram, camera, 0);
            }
            log("f=%u %s: back to player 1 (from character %d)", (unsigned)frames, why, target);
        }
        mode = off;
        target = -1;
    }
}

namespace {
    void extra_play_stop(uint8_t* rdram, const char* why) {
        if (!extra_played) {
            return;
        }
        extra_played = false;
        const uint32_t brain = brain_of(rdram, extra);
        if (brain != 0) {
            MEM_B(0x84, (gpr)(int32_t)brain) = (int8_t)extra_brain_flag;
        }
        const uint32_t record = pad_record(rdram, extra);
        if (in_rdram(record)) {
            for (uint32_t i = 0; i < 6; i++) {
                MEM_B(i, (gpr)(int32_t)record) = 0;
            }
        }
        MEM_H(0, (gpr)(int32_t)shadow_of(pressed_shadow_table)) = 0;
        log("f=%u %s: controller 1 back to player 1 (from character %d on the extra camera)", (unsigned)frames, why, extra);
    }

    // The extra character is in the match with no camera of its own (a bot; or one D-pad Right has taken from its AI this
    // frame, its camera not made yet): the extra camera may be made for it.
    bool extra_possible(uint8_t* rdram) {
        if (!have_args || !in_match(rdram, extra) || brain_of(rdram, extra) == 0 || word(rdram, character(extra) + 0x318) != 0) {
            return false;
        }
        return MEM_BU(0x84, (gpr)(int32_t)brain_of(rdram, extra)) != 0 || extra_played;
    }
}

extern "C" void conker_possess_pad(uint8_t* rdram, recomp_context* ctx) {
    if (conker::netsession::active()) {
        return;
    }
    const int index = (int)(uint32_t)ctx->r17;
    const uint32_t object = (uint32_t)ctx->r16;
    const uint32_t empty = (uint32_t)ctx->r29 + 0x5C;    // func_1504A730's empty controller for the characters without one
    const bool frozen = mode != off || extra_played;     // player 1 stands while controller 1 plays another

    if (index != 0) {
        if (!frozen) {
            return;
        }
        if (pressed_hidden) {
            // player 1 has moved: its pressed buttons back on the table for the rest of the frame (its camera's C-buttons)
            pressed_hidden = false;
            MEM_H(0, (gpr)(int32_t)pressed_table) = (int16_t)pressed;
        }
        if (mode == own && index == target) {
            MEM_W(0, (gpr)(int32_t)controller_now) = (int32_t)pad0;
            if (target < 4) {
                MEM_H(2 * (uint32_t)target, (gpr)(int32_t)pressed_table) = (int16_t)(pressed & ~button_start);
            }
            const uint32_t brain = brain_of(rdram, target);
            if (brain != 0) {
                MEM_B(0x84, (gpr)(int32_t)brain) = 0;
            }
        } else if (extra_played && extra_ready && index == extra) {
            // a second player: controller 2's record (filled from controller 1) and its slot of pressed buttons
            const uint32_t record = pad_record(rdram, extra);
            if (in_rdram(record)) {
                MEM_W(0, (gpr)(int32_t)controller_now) = (int32_t)record;
            }
            MEM_H(0, (gpr)(int32_t)shadow_of(pressed_shadow_table)) = (int16_t)extra_pressed;
            const uint32_t brain = brain_of(rdram, extra);
            if (brain != 0) {
                MEM_B(0x84, (gpr)(int32_t)brain) = 0;
            }
        } else {
            const uint32_t now = word(rdram, controller_now);
            if (now == pad0 || (extra_played && now == pad_record(rdram, extra))) {
                // a bot left with controller 1's (or 2's) record (no controller picked for it): an empty one, or its AI's copy
                // would write into it
                for (uint32_t i = 0; i < 6; i++) {
                    MEM_B(i, (gpr)(int32_t)empty) = 0;
                }
                MEM_W(0, (gpr)(int32_t)controller_now) = (int32_t)empty;
            }
        }
        return;
    }

    if (MEM_BU(0, (gpr)(int32_t)multiplayer) == 0) {
        if (mode != off || extra_ready) {
            log("f=%u the match is over", (unsigned)frames);
        }
        mode = off;
        target = -1;
        pressed_hidden = false;
        extra_wanted = extra_ready = extra_shown = extra_played = false;
        return;
    }
    frames++;
    const uint32_t pad = word(rdram, controller_now);
    if (!in_rdram(pad)) {
        return;
    }
    pad0 = pad;
    held = (uint16_t)MEM_HU(0, (gpr)(int32_t)pad);
    stick_x = (int8_t)MEM_B(2, (gpr)(int32_t)pad);
    stick_y = (int8_t)MEM_B(3, (gpr)(int32_t)pad);
    pressed = (uint16_t)MEM_HU(0, (gpr)(int32_t)pressed_table);
    constexpr uint16_t dpad = button_dpad_up | button_dpad_down | button_dpad_left | button_dpad_right;

    const uint16_t toggles = (uint16_t)(pressed & (button_dpad_up | button_dpad_down));
    if (toggles != 0 && !extra_played) {
        if (mode != off) {
            stop(rdram, (toggles & button_dpad_up) != 0 ? "D-pad Up" : "D-pad Down");
            conker::notices::show("Test: back to you", true);
        } else if ((toggles & button_dpad_up) != 0) {
            target = nearest_bot(rdram, character_count - 1);
            if (target < 0) {
                log("f=%u D-pad Up: no bot in the match to drive", (unsigned)frames);
                conker::notices::show("Test: no bot to drive", true);
            } else {
                mode = drive;
                log("f=%u D-pad Up: controller 1 drives character %d (brain %08X), player 1's camera follows it", (unsigned)frames, target,
                    (unsigned)bot_brain(rdram, target));
                conker::notices::show("Test: driving the nearest bot, your camera follows it (D-pad Up to stop)", true);
            }
        } else {
            camera0 = word(rdram, object + 0x318);
            target = in_rdram(camera0) ? nearest_bot(rdram, 3) : -1;
            if (target < 0) {
                log("f=%u D-pad Down: no bot among characters 2 to 4 (or no camera, %08X)", (unsigned)frames, (unsigned)camera0);
                conker::notices::show("Test: no bot among characters 2 to 4", true);
            } else {
                saved_brain_flag = MEM_BU(0x84, (gpr)(int32_t)brain_of(rdram, target));
                mode = own;
                own_apply(rdram);
                ctx->r2 = 0;    // func_1504A730 has read player 1's camera already
                log("f=%u D-pad Down: character %d (brain %08X) has player 1's camera %08X and controller 1", (unsigned)frames, target,
                    (unsigned)brain_of(rdram, target), (unsigned)camera0);
                conker::notices::show("Test: the bot has your camera and controls (D-pad Down to stop)", true);
            }
        }
    }
    const uint16_t extra_toggles = (uint16_t)(pressed & (button_dpad_left | button_dpad_right));
    if (extra_toggles != 0 && mode == off) {
        const bool left = (extra_toggles & button_dpad_left) != 0;
        if (left) {
            if (extra_played) {
                extra_play_stop(rdram, "D-pad Left");
                extra_shown = false;
                conker::notices::show("Test: back to you", true);
            } else if (extra_ready || extra_wanted) {
                extra_shown = !extra_shown;
                log("f=%u D-pad Left: the screen shows %s", (unsigned)frames, extra_shown ? "the extra camera" : "player 1's camera");
                conker::notices::show(extra_shown ? "Test: showing the extra camera" : "Test: showing your camera", true);
            } else if (extra_possible(rdram)) {
                extra_wanted = extra_shown = true;
                log("f=%u D-pad Left: the extra camera is made for character %d, and shown", (unsigned)frames, extra);
                conker::notices::show("Test: an extra camera on the 5th character (D-pad Left to switch view)", true);
            } else {
                log("f=%u D-pad Left: character %d isn't a bot without a camera (or camera 0 not seen made): no extra camera",
                    (unsigned)frames, extra);
                conker::notices::show("Test: the 5th character isn't a bot: no extra camera", true);
            }
        } else {
            if (extra_played) {
                extra_play_stop(rdram, "D-pad Right");
                conker::notices::show("Test: back to you (still showing the extra camera)", true);
            } else if (extra_ready || extra_wanted || extra_possible(rdram)) {
                if (!extra_ready && !extra_wanted) {
                    log("f=%u D-pad Right: the extra camera is made for character %d", (unsigned)frames, extra);
                }
                extra_wanted = extra_shown = extra_played = true;
                const uint32_t brain = brain_of(rdram, extra);
                extra_brain_flag = brain != 0 ? MEM_BU(0x84, (gpr)(int32_t)brain) : 1;
                if (extra_brain_flag == 0) {
                    extra_brain_flag = 1;
                }
                log("f=%u D-pad Right: controller 1 plays character %d through the extra camera", (unsigned)frames, extra);
                conker::notices::show("Test: you're playing the 5th character on the extra camera (D-pad Right to stop)", true);
            } else {
                log("f=%u D-pad Right: character %d isn't a bot without a camera: nothing to play", (unsigned)frames, extra);
                conker::notices::show("Test: the 5th character isn't a bot: nothing to play", true);
            }
        }
    }
    if ((pressed & dpad) != 0) {
        held &= (uint16_t)~dpad;
        pressed &= (uint16_t)~dpad;
    }
    if (extra_played) {
        // controller 1 as controller 2: its record (the camera reads it too) and pressed buttons; Start stays player 1's
        const uint32_t record = pad_record(rdram, extra);
        if (in_rdram(record)) {
            MEM_H(0, (gpr)(int32_t)record) = (int16_t)(held & ~button_start);
            MEM_B(2, (gpr)(int32_t)record) = stick_x;
            MEM_B(3, (gpr)(int32_t)record) = stick_y;
            MEM_B(4, (gpr)(int32_t)record) = 0;
            MEM_B(5, (gpr)(int32_t)record) = 0;
        }
        extra_pressed = (uint16_t)(pressed & ~button_start);
    }
    if (mode == off && !extra_played) {
        return;
    }
    if (mode == drive) {
        const uint32_t camera = word(rdram, object + 0x318);
        if (in_rdram(camera)) {
            camera_follow(rdram, camera, target);
        }
    } else if (mode == own) {
        own_apply(rdram);
        ctx->r2 = 0;
    }
    // Player 1 stands still: an empty controller, Start kept so the pause still opens, and nothing pressed until it has moved.
    for (uint32_t i = 0; i < 6; i++) {
        MEM_B(i, (gpr)(int32_t)empty) = 0;
    }
    MEM_H(0, (gpr)(int32_t)empty) = (int16_t)(held & button_start);
    MEM_W(0, (gpr)(int32_t)controller_now) = (int32_t)empty;
    MEM_H(0, (gpr)(int32_t)pressed_table) = (int16_t)(pressed & button_start);
    pressed_hidden = true;
}

extern "C" void conker_possess_brain(uint8_t* rdram, recomp_context* ctx) {
    if (mode != drive || conker::netsession::active() || (uint32_t)ctx->r16 != character(target)) {
        return;
    }
    const uint32_t brain = bot_brain(rdram, target);
    if (brain == 0) {
        return;
    }
    const uint16_t buttons = (uint16_t)(held & ~button_start);
    MEM_B(0x85, (gpr)(int32_t)brain) = stick_x;
    MEM_B(0x86, (gpr)(int32_t)brain) = stick_y;
    MEM_H(0x8A, (gpr)(int32_t)brain) = (int16_t)buttons;
    MEM_H(0x8C, (gpr)(int32_t)brain) = (int16_t)(pressed & ~button_start);
    static uint16_t last_buttons = 0xFFFF;
    if (buttons != last_buttons) {
        last_buttons = buttons;
        log("f=%u character %d: buttons %04X stick %d,%d", (unsigned)frames, target, (unsigned)buttons, (int)stick_x, (int)stick_y);
    }
}

extern "C" void conker_possess_move(uint8_t* rdram, recomp_context* ctx) {
    if (extra_played && !conker::netsession::active() && (uint32_t)ctx->r16 == character(extra)) {
        static uint32_t last = 0;
        if (frames - last >= 30) {
            last = frames;
            float speed, way_bits;
            const uint32_t sb = (uint32_t)MEM_W(0x1B4, ctx->r29);
            std::memcpy(&speed, &sb, 4);
            (void)way_bits;
            log("f=%u [walk] character %d walks: brain+0x84 %d, stick's speed %.2f way %04X, state %02X", (unsigned)frames, extra,
                (int)(uint32_t)ctx->r13, speed, (unsigned)MEM_HU(0x1BC, ctx->r29), (unsigned)MEM_BU(0x4, ctx->r16));
        }
    }
    if (mode != drive || conker::netsession::active() || (uint32_t)ctx->r16 != character(target)) {
        return;
    }
    ctx->r13 = 0;
}

// func_1504CB98, 0x1504CC54: the moving character's controller is set (D_800CC284), its brain's AI flag about to be read.
extern "C" void conker_possess_probe(uint8_t* rdram, recomp_context* ctx) {
    if (!extra_played || conker::netsession::active() || (uint32_t)ctx->r16 != character(extra)) {
        return;
    }
    static uint32_t last = 0;
    if (frames - last < 30) {
        return;
    }
    last = frames;
    const uint32_t pad = word(rdram, controller_now);
    const uint32_t brain = brain_of(rdram, extra);
    log("f=%u [probe] character %d: controller %08X (ours %08X) buttons %04X stick %d,%d; brain+0x84 %d; state %02X; +0x104 %d; "
        "players %d, last player %d; camera %08X",
        (unsigned)frames, extra, (unsigned)pad, (unsigned)extra_record, in_rdram(pad) ? (unsigned)MEM_HU(0, (gpr)(int32_t)pad) : 0u,
        in_rdram(pad) ? (int)MEM_B(2, (gpr)(int32_t)pad) : 0, in_rdram(pad) ? (int)MEM_B(3, (gpr)(int32_t)pad) : 0,
        brain != 0 ? (int)MEM_BU(0x84, (gpr)(int32_t)brain) : -1, (unsigned)MEM_BU(0x4, ctx->r16), (int)MEM_BU(0x104, ctx->r16),
        (int)MEM_B(0, (gpr)(int32_t)0x8008FD8C), (int)MEM_W(0, (gpr)(int32_t)last_player), (unsigned)word(rdram, character(extra) + 0x318));
}

// ---- The extra camera (D-pad Left / Right) ----
//
// The game makes its cameras as a match loads (func_15005290): as many as players (D_80082FA0 + 1), in one block (D_800DBFF0), each
// made by func_150054C4(camera, k, ...) for character k; with them blocks a camera each (D_800DC2B0 0xB0, the view matrices
// D_800DC2A0[2] 0x40, D_800DC010 0x9A0 and one more). The views (D_800BE628, 0x180 each, and D_800BE62C, 16) are made by func_1501A220.
// Offline here, all of those have room for 5 (the cameras the game counts are still the players'), and the views 1 to 4 that
// weren't made are made as the extra one is (func_1501A8C0, full screen). Each frame the cameras run (func_15122AE0, cameras 0 to
// D_80082FA0); then the extra one runs the same way; shown, its view matrix is put in camera 0's and camera 0's view looks for what to
// draw from its eye (func_1510FC34).

extern "C" void func_150054C4(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_1501A8C0(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_151239CC(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_15122C5C(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_15123934(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_1512C490(uint8_t* rdram, recomp_context* ctx);

// func_15005290, 0x15005318: $s0 the cameras to make blocks for (players).
extern "C" void conker_possess_cameras_alloc(uint8_t* rdram, recomp_context* ctx) {
    if (conker::netsession::active()) {
        return;
    }
    if ((int32_t)ctx->r16 < 5) {
        log("cameras made for %d players: blocks with room for 5", (int)(int32_t)ctx->r16);
        ctx->r16 = 5;
    }
    extra_wanted = extra_ready = extra_shown = extra_played = false;
    have_args = false;
    if (slot4_on) {
        log("slot 4: %u accesses sent to ours this match", (unsigned)slot4_moves);
    }
    slot4_on = false;
    slot4_moves = 0;
}

// func_15005290, 0x15005464: $a0 = D_80082FA0, for the last block (cameras + 1).
extern "C" void conker_possess_cameras_alloc2(uint8_t* rdram, recomp_context* ctx) {
    if (!conker::netsession::active() && (int32_t)ctx->r4 < 4) {
        ctx->r4 = 4;
    }
}

// func_150054C4, 0x150054C8 (its start, past one instruction): camera ($a0) for character $a1.
extern "C" void conker_possess_camera_args(uint8_t* rdram, recomp_context* ctx) {
    if (!conker::netsession::active() && (int32_t)ctx->r5 == 0) {
        camera_arg2 = (uint32_t)ctx->r6;
        camera_arg3 = (uint32_t)ctx->r7;
        have_args = true;
    }
}

// func_1501A220, 0x1501A258: $s0 the views to make room for.
extern "C" void conker_possess_views_alloc(uint8_t* rdram, recomp_context* ctx) {
    if (!conker::netsession::active() && (int32_t)ctx->r16 < 5) {
        ctx->r16 = 5;
    }
}

// func_1501A220, 0x1501A360: its views made; the rest of the 5 made full screen.
extern "C" void conker_possess_views_make(uint8_t* rdram, recomp_context* ctx) {
    if (conker::netsession::active()) {
        return;
    }
    const int last = (int)MEM_W(0, (gpr)(int32_t)last_player);
    const int first = last == 0 ? 1 : last + 2;
    if (first > 4) {
        return;
    }
    const recomp_context saved = *ctx;
    for (int k = first; k <= 4; k++) {
        ctx->r4 = (gpr)k;
        ctx->r5 = 0;
        ctx->r6 = 0x3FF;
        ctx->r7 = 0;
        func_1501A8C0(rdram, ctx);
        *ctx = saved;
    }
    log("views %d to 4 made (full screen) for the extra camera", first);
}

// func_15122AE0, 0x15122C18: the game's cameras have run this frame.
extern "C" void conker_possess_camera_frame(uint8_t* rdram, recomp_context* ctx) {
    if (conker::netsession::active() || MEM_BU(0, (gpr)(int32_t)multiplayer) == 0 || (!extra_wanted && !extra_ready)) {
        return;
    }
    const uint32_t base = (uint32_t)MEM_W(0, (gpr)(int32_t)camera_base);
    if (!in_rdram(base)) {
        return;
    }
    const uint32_t camera = base + (uint32_t)extra * camera_size;
    const recomp_context saved = *ctx;
    if (!extra_ready) {
        if (!extra_possible(rdram)) {
            log("f=%u character %d isn't a bot without a camera any more: no extra camera", (unsigned)frames, extra);
            extra_play_stop(rdram, "no extra camera");
            extra_wanted = extra_shown = false;
            return;
        }
        const uint32_t width = (uint32_t)MEM_W(0, (gpr)(int32_t)frame_width);
        for (uint32_t i = 0; i < 2 * width && i < 0x4000; i++) {
            MEM_B(i, (gpr)(int32_t)extra_buffer) = 0;
        }
        slot4_reset(rdram);
        slot4_on = true;
        MEM_B(extra, (gpr)(int32_t)camera_bytes) = 0;   // D_800DBFF4's 5th byte: nothing else names it
        ctx->r4 = (gpr)(int32_t)camera;
        ctx->r5 = (gpr)extra;
        ctx->r6 = (gpr)(int32_t)camera_arg2;
        ctx->r7 = (gpr)(int32_t)camera_arg3;
        func_150054C4(rdram, ctx);
        *ctx = saved;
        // its buffer (func_150054C4 read it from D_800BE2B0[4], past that table's 4)
        MEM_W(0x8BC, (gpr)(int32_t)camera) = (int32_t)extra_buffer;
        extra_ready = true;
        log("f=%u the extra camera %08X made for character %d (now its camera: %08X)", (unsigned)frames, (unsigned)camera, extra,
            (unsigned)word(rdram, character(extra) + 0x318));
    }
    // still the extra character's (something in the match took it off: put back)
    if (word(rdram, character(extra) + 0x318) != camera || word(rdram, camera + 0x3D0) != character(extra)) {
        static uint32_t told = 0;
        if (told++ < 10) {
            log("f=%u the extra camera was off character %d (its camera %08X, the camera's character %08X): put back", (unsigned)frames, extra,
                (unsigned)word(rdram, character(extra) + 0x318), (unsigned)word(rdram, camera + 0x3D0));
        }
        set_word(rdram, character(extra) + 0x318, camera);
        camera_follow(rdram, camera, extra);
    }
    // as func_15122AE0 runs each of its cameras
    MEM_W(0x7B4, (gpr)(int32_t)camera) = MEM_W(0x7B4, (gpr)(int32_t)base);
    const bool other_way = MEM_BU(0, (gpr)(int32_t)0x800BEAC0) != 0 && MEM_BU(0, (gpr)(int32_t)0x800C35EA) == 0 &&
        MEM_W(0, (gpr)(int32_t)0x800D2DB4) == 0;
    if (other_way) {
        ctx->r4 = (gpr)(int32_t)camera;
        ctx->r5 = 0x2000;
        ctx->r6 = 0;
        ctx->r7 = (gpr)MEM_W(0x134, (gpr)(int32_t)camera);
        MEM_W(0x10, ctx->r29) = 5;
        func_15123934(rdram, ctx);
        *ctx = saved;
        ctx->r4 = (gpr)(int32_t)camera;
        func_1512C490(rdram, ctx);
    } else {
        ctx->r4 = (gpr)(int32_t)camera;
        ctx->r5 = 5;
        func_151239CC(rdram, ctx);
        *ctx = saved;
        ctx->r4 = (gpr)(int32_t)camera;
        func_15122C5C(rdram, ctx);
    }
    *ctx = saved;
    static uint32_t logged = 0;
    if (logged < 3 || (frames % 600) == 0) {
        logged++;
        log("f=%u the extra camera ran (%s); for character %08X, at %.0f %.0f %.0f", (unsigned)frames, other_way ? "other way" : "usual way",
            (unsigned)word(rdram, camera + 0x3D0), position(rdram, camera, 0x2EC), position(rdram, camera, 0x2F0), position(rdram, camera, 0x2F4));
    }
}

// func_1510FC34, 0x1510FC74: view $s0's camera ($v0) about to give its eye, for what the view draws.
extern "C" void conker_possess_view_eye(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    (void)ctx;   // (possess13: the extra view is drawn as a view of its own now)
}

int conker::possess_test::played_camera() {
    return extra_played && extra_ready ? extra : 0;
}

// Before an access to slot4_tables[table] through register *reg with offset off: slot 4 sent to our entry.
extern "C" void conker_slot4(uint8_t* rdram, gpr* reg, int32_t off, int table) {
    if (table < 0 || table >= slot4_count) {
        return;
    }
    const Slot4Table& t = slot4_tables[table];
    if (!(t.always ? slot4_on : view_pass_on)) {
        return;
    }
    const uint32_t at = (uint32_t)*reg + (uint32_t)off;
    const uint32_t from = t.base + 4u * t.stride;
    if (at >= from && at < from + t.stride) {
        *reg = (gpr)(int32_t)((uint32_t)*reg - from + slot4_shadow + 0x100u * (uint32_t)table);
        if (slot4_moves++ < 40) {
            log("slot 4: table %d (%08X x%X) +%X sent to ours", table, (unsigned)t.base, (unsigned)t.stride, (unsigned)(at - from));
        }
    }
}

// Before the camera keeps its controller's record (+0x36C) from the game's 4: the extra camera's is ours.
extern "C" void conker_slot4_pad(uint8_t* rdram, gpr* value, gpr camera) {
    if (slot4_on && (uint32_t)camera == extra_camera(rdram)) {
        *value = (gpr)(int32_t)extra_record;
    }
}

// ---- The extra view (possess13) ----
// What a view draws feeds back into the game (what each pass culls, the random numbers it draws: net_view.cpp's notes), so a 5th
// player's view has to be drawn as a view of the game's own on every machine. Each frame, after the game's own views, the same
// passes run for view 4, the extra camera's (D_80082FA4 the view being drawn, as the game's loops set it):
//   func_15019130: func_1510FC34 (what's in sight from the view's camera), func_15094EA0 (the view's matrices);
//   func_1501878C: func_15018F80 + func_15019464 (the first pass), func_1515D6D0 + func_150195A0 (the 3D), func_1515D6D0 +
//   func_15019CC8 (the second).
// The views' tables of 4 are sent to ours while it's drawn (conker_slot4, the views' set). The view is full screen (func_1501A8C0's
// layout 0), so offline, drawn after the game's, it is what's seen.
extern "C" void func_1510FC34(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_15094EA0(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_15018F80(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_15019464(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_1515D6D0(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_150195A0(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_15019CC8(uint8_t* rdram, recomp_context* ctx);

namespace {
    constexpr uint32_t view_now = 0x80082FA4;        // D_80082FA4: the view being drawn
    constexpr uint32_t list_start = 0x800BE9C8;      // D_800BE9C8[2]: the start of this frame's display list
    constexpr uint32_t list_most = 0x800BEBA4;       // D_800BEBA4: the most commands it may have
    uint32_t passes = 0, passes_dropped = 0;

    // (possess13-16's 5th view, drawn as a view of the game's own in the middle of its frame: off. possess17 draws it on this machine
    // alone at the end of the frame and puts the game's memory back, conker_possess_view_local.)
    bool extra_view_drawn(uint8_t* rdram) {
        (void)rdram;
        return false;
    }

    // This machine draws the extra camera's view (offline: D-pad Left; online: the 5th player's machine).
    bool local_view_wanted(uint8_t* rdram) {
        if (!extra_ready || MEM_BU(0, (gpr)(int32_t)multiplayer) == 0) {
            return false;
        }
        return !conker::netsession::active() && extra_shown;
    }

    uint32_t call_list(uint8_t* rdram, recomp_context* ctx, void (*fn)(uint8_t*, recomp_context*), uint32_t list, int view) {
        ctx->r4 = (gpr)(int32_t)list;
        ctx->r5 = (gpr)view;
        fn(rdram, ctx);
        return (uint32_t)ctx->r2;
    }
}

namespace {
    // (possess14 diagnostics) the game's memory before and after each of the extra view's passes, its first 2 frames: what they wrote
    // outside of ours, in the log (to find a table of 4 still written past its end).
    std::vector<uint32_t> diff_before;
    uint32_t diff_frames = 0;
    constexpr uint32_t diff_size = 0x400000;   // the game's first 4 MB (its data and heap)

    void diff_begin(uint8_t* rdram) {
        if (diff_frames >= 2) {
            return;
        }
        diff_before.assign(reinterpret_cast<const uint32_t*>(rdram), reinterpret_cast<const uint32_t*>(rdram) + diff_size / 4);
    }

    void diff_end(uint8_t* rdram, int stage) {
        if (diff_frames >= 2 || diff_before.size() != diff_size / 4) {
            return;
        }
        const uint32_t* now = reinterpret_cast<const uint32_t*>(rdram);
        int ranges = 0;
        uint32_t words = 0;
        for (uint32_t i = 0; i < diff_size / 4; i++) {
            if (now[i] == diff_before[i]) {
                continue;
            }
            uint32_t j = i;
            while (j + 1 < diff_size / 4 && (now[j + 1] != diff_before[j + 1] || (j + 2 < diff_size / 4 && now[j + 2] != diff_before[j + 2]))) {
                j++;
            }
            words += j - i + 1;
            if (ranges++ < 150) {
                log("  [diff] stage %d: %08X-%08X (%u words) first %08X -> %08X", stage, 0x80000000u + i * 4, 0x80000000u + j * 4 + 3,
                    j - i + 1, diff_before[i], now[i]);
            }
            i = j;
        }
        log("  [diff] stage %d: %d ranges, %u words changed", stage, ranges, (unsigned)words);
        diff_before.clear();
    }
}

// stage 0: func_15019130 after its views' func_1510FC34; 1: after their func_15094EA0; 2, 3, 4: func_1501878C after each of its loops
// over the views ($s1 the address of the display list's end).
extern "C" void conker_possess_view_pass(uint8_t* rdram, recomp_context* ctx, int stage) {
    if (!extra_view_drawn(rdram)) {
        return;
    }
    diff_end(rdram, 10 + stage);   // the game's own pass for its views (begun at the loop's start, conker_possess_diff_game)
    const recomp_context saved = *ctx;
    const uint32_t head = (uint32_t)ctx->r17;
    const int32_t was = MEM_W(0, (gpr)(int32_t)view_now);
    MEM_W(0, (gpr)(int32_t)view_now) = extra;
    view_pass_on = true;
    diff_begin(rdram);
    switch (stage) {
        case 0:
            ctx->r4 = extra;
            func_1510FC34(rdram, ctx);
            break;
        case 1:
            ctx->r4 = extra;
            func_15094EA0(rdram, ctx);
            break;
        case 2: {
            ctx->r4 = extra;
            func_15018F80(rdram, ctx);
            *ctx = saved;
            MEM_W(0, (gpr)(int32_t)head) = (int32_t)call_list(rdram, ctx, func_15019464, (uint32_t)MEM_W(0, (gpr)(int32_t)head), extra);
            break;
        }
        case 3: {
            const uint32_t before = (uint32_t)MEM_W(0, (gpr)(int32_t)head);
            uint32_t list = call_list(rdram, ctx, func_1515D6D0, before, extra);
            *ctx = saved;
            list = call_list(rdram, ctx, func_150195A0, list, extra);
            // as the game's loop does: past the list's room, the view's commands are let go
            const uint32_t buffer = MEM_BU(0, (gpr)(int32_t)matrix_buffer) & 1u;
            const uint32_t start = (uint32_t)MEM_W(4 * buffer, (gpr)(int32_t)list_start);
            if ((int32_t)((list - start) >> 3) > (int32_t)MEM_W(0, (gpr)(int32_t)list_most)) {
                list = before;
                passes_dropped++;
            }
            MEM_W(0, (gpr)(int32_t)head) = (int32_t)list;
            passes++;
            break;
        }
        case 4: {
            uint32_t list = call_list(rdram, ctx, func_1515D6D0, (uint32_t)MEM_W(0, (gpr)(int32_t)head), extra);
            *ctx = saved;
            list = call_list(rdram, ctx, func_15019CC8, list, extra);
            MEM_W(0, (gpr)(int32_t)head) = (int32_t)list;
            break;
        }
        default:
            break;
    }
    view_pass_on = false;
    MEM_W(0, (gpr)(int32_t)view_now) = was;
    diff_end(rdram, stage);
    if (stage == 4) {
        diff_frames++;
    }
    *ctx = saved;
    if (stage == 3 && (passes == 1 || passes % 600 == 0)) {
        log("f=%u the extra view drawn (%u times, %u let go for the list's room)", (unsigned)frames, (unsigned)passes, (unsigned)passes_dropped);
    }
}

// func_15000AD0, before its allocations of a 0x80 block a view ($a0, D_80082FA0 * 0x80 + 0x80, kept at D_800DBEB0): room for 5.
extern "C" void conker_possess_view_alloc(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (!conker::netsession::active() && (int32_t)ctx->r4 < 5 * 0x80) {
        ctx->r4 = 5 * 0x80;
    }
}

// (possess15 diagnostics) at the start of the game's loop over its views for each stage: its memory then, to set what its own views'
// passes write against what the extra view's do.
extern "C" void conker_possess_diff_game(uint8_t* rdram, recomp_context* ctx, int stage) {
    (void)ctx;
    (void)stage;
    if (extra_view_drawn(rdram)) {
        diff_begin(rdram);
    }
}

// ---- possess17: the extra camera's view, drawn on this machine alone ----
// What a view's passes write feeds the game (net_view.cpp's notes), and the game keeps its views' data in many places sized for 4
// (tables, each object's own), so a 5th view drawn by the game as one of its own wrote past them. Instead, at the end of the frame
// (all of its logic done, its display list about to be closed), this machine alone:
//   1. keeps a copy of the game's first 4 MB;
//   2. makes view 1 the extra camera's for a moment: camera 1 a copy of the extra camera (its index 0), view 1's record the full-screen
//      one's (view 4: its viewport and projection), the extra camera's view matrix in view 1's slot;
//   3. runs view 1's passes (func_1510FC34, func_15094EA0, func_15018F80 + func_15019464, func_1515D6D0 + func_150195A0), which add
//      to the frame's display list;
//   4. puts the game's memory back as it was, but for those new commands, the list's end, and what they draw from (each matrix,
//      vertex block, light or viewport they name, followed through the lists they call and the segments set): those are put back
//      at the start of the next frame, once the frame has been drawn.
// So the game's memory is the same as on any other machine, and the new commands, drawn last, are the extra camera's view.
extern "C" void func_15094EA0(uint8_t* rdram, recomp_context* ctx);

namespace {
    constexpr uint32_t game_size = 0x400000;
    std::vector<uint8_t> local_copy;                       // the game's memory before the view
    struct Kept { uint32_t at, size; std::vector<uint8_t> was; };
    std::vector<Kept> kept_back;                           // put back at the next frame's start
    uint32_t local_views = 0;

    uint32_t mem_word(uint8_t* rdram, uint32_t address) {
        return (uint32_t)MEM_W(0, (gpr)(int32_t)address);
    }

    // The display list's commands from `from` (an RDRAM address), following calls: every piece of memory they name (with the command
    // that names it, and the segment it's named through), and where each segment was set.
    struct ListWalk {
        uint8_t* rdram;
        uint32_t seg[16] = {};
        uint32_t seg_set_at[16] = {};   // the command that set it (0: before the walk)
        struct Ref { uint32_t command, at, size; int segment; };
        std::vector<Ref> refs;
        uint32_t commands = 0;
        uint32_t resolve(uint32_t a, int* segment) const {
            const uint32_t s = (a >> 24) & 0xFF;
            if (s < 16) {
                *segment = s == 0 ? -1 : (int)s;
                return 0x80000000u | ((seg[s] + (a & 0x00FFFFFFu)) & 0x00FFFFFFu);
            }
            *segment = -1;
            return 0x80000000u | (a & 0x00FFFFFFu);
        }
        void note(uint32_t command, uint32_t w1, uint32_t size) {
            int segment = -1;
            const uint32_t at = resolve(w1, &segment);
            if (at >= 0x80000000u && at < 0x80000000u + game_size && size > 0 && size <= 0x10000) {
                refs.push_back({ command, at, std::min(size, 0x80000000u + game_size - at), segment });
            }
        }
        void walk(uint32_t at, int depth, uint32_t stop) {
            if (depth > 12) {
                return;
            }
            while (commands < 400000) {
                if ((at & 7u) != 0 || at < 0x80000000u || at >= 0x80000000u + game_size || (stop != 0 && at >= stop)) {
                    return;
                }
                const uint32_t w0 = mem_word(rdram, at), w1 = mem_word(rdram, at + 4);
                commands++;
                switch (w0 >> 24) {
                    case 0x01:   // G_VTX
                        note(at, w1, ((w0 >> 12) & 0xFFu) * 16u);
                        break;
                    case 0xDA:   // G_MTX
                        note(at, w1, 64);
                        break;
                    case 0xDC:   // G_MOVEMEM
                        note(at, w1, (((w0 >> 19) & 0x1Fu) + 1u) * 8u);
                        break;
                    case 0xDB:   // G_MOVEWORD: a segment
                        if (((w0 >> 16) & 0xFFu) == 0x06 && (w0 & 0xFFFFu) < 64u) {
                            seg[(w0 & 0xFFFFu) / 4u] = w1 & 0x00FFFFFFu;
                            seg_set_at[(w0 & 0xFFFFu) / 4u] = at;
                        }
                        break;
                    case 0xDE: {  // G_DL: a call (or a jump)
                        int segment = -1;
                        const uint32_t target = resolve(w1, &segment);
                        if (((w0 >> 16) & 0xFFu) != 0) {
                            at = target;
                            stop = 0;
                            continue;
                        }
                        walk(target, depth + 1, 0);
                        break;
                    }
                    case 0xDF:   // G_ENDDL
                        return;
                    default:
                        break;
                }
                at += 8;
            }
        }
    };

    // ours, past the game's 8 MB: copies of what the extra camera's view draws from, two halves (this frame's, the last one's, which
    // may still be being drawn)
    constexpr uint32_t copies_start = 0x80E00000;
    constexpr uint32_t copies_half = 0x80000;
    uint32_t copies_frame = 0;

    // (the next frame's start) what the view's commands drew from, put back
    void put_kept_back(uint8_t* rdram) {
        for (const Kept& k : kept_back) {
            std::memcpy(rdram + (k.at - 0x80000000u), k.was.data(), k.size);
        }
        kept_back.clear();
    }
}

// func_1501878C, 0x15018C8C (its frame's display list about to be closed; $s1 the address of the list's end): the view, see above.
extern "C" void conker_possess_view_local(uint8_t* rdram, recomp_context* ctx) {
    if (!local_view_wanted(rdram)) {
        return;
    }
    const uint32_t base = mem_word(rdram, camera_base);
    const uint32_t views = mem_word(rdram, 0x800BE628);
    const uint32_t buffer = MEM_BU(0, (gpr)(int32_t)matrix_buffer) & 1u;
    const uint32_t mats = mem_word(rdram, matrices + 4 * buffer);
    const uint32_t head = (uint32_t)ctx->r17;
    if (!in_rdram(base) || !in_rdram(views) || !in_rdram(mats) || !in_rdram(head)) {
        return;
    }
    put_kept_back(rdram);   // (none should be left; to be safe)
    local_copy.assign(rdram, rdram + game_size);
    const uint32_t list_before = mem_word(rdram, head);

    // 2. view 1 the extra camera's
    std::memcpy(rdram + (base - 0x80000000u), rdram + (base + (uint32_t)extra * camera_size - 0x80000000u), camera_size);
    MEM_B(0x23D, (gpr)(int32_t)base) = 0;
    std::memcpy(rdram + (views - 0x80000000u), rdram + (views + (uint32_t)extra * 0x180u - 0x80000000u), 0x180);
    std::memcpy(rdram + (mats - 0x80000000u), rdram + (mats + (uint32_t)extra * 0x40u - 0x80000000u), 0x40);

    // 3. its passes
    const recomp_context saved = *ctx;
    const int32_t was_view = MEM_W(0, (gpr)(int32_t)view_now);
    MEM_W(0, (gpr)(int32_t)view_now) = 0;
    ctx->r4 = 0;
    func_1510FC34(rdram, ctx);
    *ctx = saved;
    ctx->r4 = 0;
    func_15094EA0(rdram, ctx);
    *ctx = saved;
    ctx->r4 = 0;
    func_15018F80(rdram, ctx);
    *ctx = saved;
    uint32_t list = call_list(rdram, ctx, func_15019464, list_before, 0);
    *ctx = saved;
    list = call_list(rdram, ctx, func_1515D6D0, list, 0);
    *ctx = saved;
    list = call_list(rdram, ctx, func_150195A0, list, 0);
    *ctx = saved;
    MEM_W(0, (gpr)(int32_t)view_now) = was_view;

    // 4. what the new commands draw from: what the view's passes changed is copied to ours and the commands pointed at the copies (a
    // command of the new ones, directly; one of a list they call, through the segment a new command set); what can't be is kept
    // in the game's memory for the frame and put back at the next one's start
    ListWalk walker{ rdram };
    // the segments as the frame's list set them before (its top level, from its start)
    const uint32_t start = mem_word(rdram, list_start + 4 * buffer);
    for (uint32_t a = start; in_rdram(a) && a + 8 <= list_before && a - start < 0x80000; a += 8) {
        const uint32_t w0 = mem_word(rdram, a);
        if ((w0 >> 24) == 0xDB && ((w0 >> 16) & 0xFFu) == 0x06 && (w0 & 0xFFFFu) < 64u) {
            walker.seg[(w0 & 0xFFFFu) / 4u] = mem_word(rdram, a + 4) & 0x00FFFFFFu;
        }
    }
    walker.walk(list_before, 0, list);
    auto changed = [&](uint32_t at, uint32_t size) {
        return std::memcmp(local_copy.data() + (at - 0x80000000u), rdram + (at - 0x80000000u), size) != 0;
    };
    auto ours = [&](uint32_t command) { return command >= list_before && command < list; };
    const uint32_t copies = copies_start + (copies_frame++ & 1u) * copies_half;
    uint32_t copies_used = 0;
    auto copy_out = [&](uint32_t at, uint32_t size) -> uint32_t {
        const uint32_t room = (size + 15u) & ~15u;
        if (copies_used + room > copies_half) {
            return 0;
        }
        const uint32_t to = copies + copies_used;
        copies_used += room;
        std::memcpy(rdram + (to - 0x80000000u), rdram + (at - 0x80000000u), size);
        return to;
    };
    std::vector<std::pair<uint32_t, uint32_t>> patches;   // (address of a command's second word, new value)
    std::vector<std::pair<uint32_t, uint32_t>> keep_ranges;
    uint32_t relocated = 0, by_segment = 0;
    // segments set by a new command and used by a called list: the stretch they're used over
    struct Window { uint32_t lo = 0xFFFFFFFFu, hi = 0; bool change = false; };
    Window windows[16];
    for (const auto& r : walker.refs) {
        if (ours(r.command)) {
            if (changed(r.at, r.size)) {
                if (const uint32_t to = copy_out(r.at, r.size)) {
                    patches.push_back({ r.command + 4, to });
                    relocated++;
                } else {
                    keep_ranges.push_back({ r.at, r.size });
                }
            }
        } else if (r.segment > 0 && ours(walker.seg_set_at[r.segment])) {
            Window& w = windows[r.segment];
            w.lo = std::min(w.lo, r.at);
            w.hi = std::max(w.hi, r.at + r.size);
            w.change |= changed(r.at, r.size);
        } else if (changed(r.at, r.size)) {
            keep_ranges.push_back({ r.at, r.size });
        }
    }
    for (int sgm = 1; sgm < 16; sgm++) {
        Window& w = windows[sgm];
        if (!w.change || w.hi <= w.lo) {
            continue;
        }
        const uint32_t base_phys = mem_word(rdram, walker.seg_set_at[sgm] + 4) & 0x00FFFFFFu;
        const uint32_t lo_off = (w.lo & 0x00FFFFFFu) - base_phys;
        const uint32_t to = w.hi - w.lo <= 0x40000u ? copy_out(w.lo, w.hi - w.lo) : 0;
        if (to != 0 && (to & 0x00FFFFFFu) >= lo_off) {
            patches.push_back({ walker.seg_set_at[sgm] + 4, (to & 0x00FFFFFFu) - lo_off });
            by_segment++;
        } else {
            keep_ranges.push_back({ w.lo, w.hi - w.lo });
        }
    }
    std::vector<Kept> keep;
    for (const auto& r : keep_ranges) {
        Kept k{ r.first, r.second, {} };
        k.was.assign(local_copy.begin() + (r.first - 0x80000000u), local_copy.begin() + (r.first - 0x80000000u) + r.second);
        keep.push_back(std::move(k));
    }
    std::vector<std::vector<uint8_t>> now;
    now.reserve(keep.size());
    for (const Kept& k : keep) {
        now.emplace_back(rdram + (k.at - 0x80000000u), rdram + (k.at - 0x80000000u) + k.size);
    }
    // the game's memory back, but for the new commands and the list's end
    const uint32_t from = list_before - 0x80000000u, to = list - 0x80000000u;
    if (list > list_before && to <= game_size) {
        std::memcpy(rdram, local_copy.data(), from);
        std::memcpy(rdram + to, local_copy.data() + to, game_size - to);
    } else {
        std::memcpy(rdram, local_copy.data(), game_size);
        list = list_before;
        patches.clear();
    }
    MEM_W(0, (gpr)(int32_t)head) = (int32_t)list;
    for (const auto& pt : patches) {
        MEM_W(0, (gpr)(int32_t)pt.first) = (int32_t)pt.second;
    }
    for (size_t i = 0; i < keep.size(); i++) {
        std::memcpy(rdram + (keep[i].at - 0x80000000u), now[i].data(), keep[i].size);
    }
    kept_back = std::move(keep);
    local_views++;
    if (local_views <= 3 || local_views % 600 == 0) {
        log("f=%u the extra camera's view drawn here (%u times): %u commands (%u walked), %u pieces copied to ours, %u segments moved, %u kept "
            "for the frame (%u bytes of copies)", (unsigned)frames, (unsigned)local_views, (unsigned)((list - list_before) / 8),
            (unsigned)walker.commands, (unsigned)relocated, (unsigned)by_segment, (unsigned)kept_back.size(), (unsigned)copies_used);
    }
}

// func_1501878C's start (the next frame): what the view drew from, back as the game had it.
extern "C" void conker_possess_view_local_after(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    if (!kept_back.empty()) {
        put_kept_back(rdram);
    }
}
