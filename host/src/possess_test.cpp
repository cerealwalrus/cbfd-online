// Test build only (branch possess-test), offline, in a multiplayer match: steps towards a 5th player.
//
//   D-pad Up:   controller 1 drives the bot nearest player 1 (as its AI would), and player 1's camera follows that bot.
//   D-pad Down: the nearest bot among characters 2 to 4 gets player 1's camera and controller and is moved as a person is
//               (camera-relative stick, R-Look, the C-buttons); player 1 stands without a camera.
//   Either again: back to player 1.
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

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

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

    bool in_rdram(uint32_t address) {
        return (address >> 24) == 0x80 && (address & 0xFFFFFFu) < 0x7FFF00u;
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

extern "C" void conker_possess_pad(uint8_t* rdram, recomp_context* ctx) {
    if (conker::netsession::active()) {
        return;
    }
    const int index = (int)(uint32_t)ctx->r17;
    const uint32_t object = (uint32_t)ctx->r16;
    const uint32_t empty = (uint32_t)ctx->r29 + 0x5C;    // func_1504A730's empty controller for the characters without one

    if (index != 0) {
        if (mode == off) {
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
        } else if (word(rdram, controller_now) == pad0) {
            // a bot left with controller 1 (no controller picked for it): an empty one, or its AI's copy would write into it
            for (uint32_t i = 0; i < 6; i++) {
                MEM_B(i, (gpr)(int32_t)empty) = 0;
            }
            MEM_W(0, (gpr)(int32_t)controller_now) = (int32_t)empty;
        }
        return;
    }

    if (MEM_BU(0, (gpr)(int32_t)multiplayer) == 0) {
        if (mode != off) {
            log("f=%u the match is over", (unsigned)frames);
        }
        mode = off;
        target = -1;
        pressed_hidden = false;
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

    const uint16_t toggles = (uint16_t)(pressed & (button_dpad_up | button_dpad_down));
    if (toggles != 0) {
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
        held &= (uint16_t)~(button_dpad_up | button_dpad_down);
        pressed &= (uint16_t)~(button_dpad_up | button_dpad_down);
    }
    if (mode == off) {
        return;
    }
    if (mode == drive) {
        const uint32_t camera = word(rdram, object + 0x318);
        if (in_rdram(camera)) {
            camera_follow(rdram, camera, target);
        }
    } else {
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
    if (mode != drive || conker::netsession::active() || (uint32_t)ctx->r16 != character(target)) {
        return;
    }
    ctx->r13 = 0;
}
