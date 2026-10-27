// Test build only (branch possess-test): offline, in a multiplayer match, D-pad Up hands controller 1 to the bot nearest player 1, and
// D-pad Up again gives it back. Proves a bot can be driven by a person's input, the first step towards a 5th player.
//
// How the game drives a bot: its AI (the script commands, func_1509DFC4, e.g. func_1508CAD8 "go to") never touches a controller. It
// writes its "hands" into the character's brain (character + 0x31C):
//   +0x84 set: driven by the AI;  +0x85 / +0x86 the stick's X / Y;  +0x8A the buttons held;  +0x8C the buttons pressed this frame.
// The movement code (func_1504CB98, from 0x1504CC9C) copies those into the character's controller (D_800CC284) and from then on
// runs exactly as for a person. So here:
//   - conker_possess_pad (func_1504A730, 0x1504A970, a character's controller just picked): for player 1, keeps this frame's
//     controller 1 and gives player 1 an empty one (Start kept, for the pause) while a bot is driven;
//   - conker_possess_brain (func_1504CB98, 0x1504CC9C, about to copy the brain into the controller): for the driven bot, puts
//     controller 1 into its brain first.
//   - conker_possess_move (func_1504CB98, 0x1504E0A4): walking, an AI-driven character goes the way and speed its path finder put
//     in the character (+0x78, +0x44), not the stick's; for the driven bot $t5 (brain + 0x84) reads 0, so the stick's are used.
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
    constexpr uint32_t pressed_table = 0x800BE710;   // the buttons pressed this frame, a halfword per controller
    constexpr uint32_t multiplayer = 0x800BE616;     // D_800BE616: a multiplayer arena

    constexpr uint16_t button_start = 0x1000;
    constexpr uint16_t button_dpad_up = 0x0800;

    bool driving = false;
    int target = -1;
    uint16_t held = 0, pressed = 0;
    int8_t stick_x = 0, stick_y = 0;
    uint32_t frames = 0;

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

    // A character the AI drives (a bot), in the match. 0 if not.
    uint32_t bot_brain(uint8_t* rdram, int k) {
        const uint32_t object = character(k);
        if (MEM_BU(0x127, (gpr)(int32_t)object) == 0xFF || MEM_BU(0x2FA, (gpr)(int32_t)object) == 0) {
            return 0;
        }
        const uint32_t brain = (uint32_t)MEM_W(0x31C, (gpr)(int32_t)object);
        if (!in_rdram(brain) || MEM_BU(0x84, (gpr)(int32_t)brain) == 0) {
            return 0;
        }
        return brain;
    }

    float position(uint8_t* rdram, uint32_t object, uint32_t offset) {
        const uint32_t bits = (uint32_t)MEM_W(offset, (gpr)(int32_t)object);
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    int nearest_bot(uint8_t* rdram) {
        const uint32_t me = character(0);
        const float x = position(rdram, me, 0x14), y = position(rdram, me, 0x18), z = position(rdram, me, 0x1C);
        int best = -1;
        float best_distance = 0.0f;
        for (int k = 1; k < character_count; k++) {
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
}

extern "C" void conker_possess_pad(uint8_t* rdram, recomp_context* ctx) {
    if (conker::netsession::active() || (int)(uint32_t)ctx->r17 != 0) {
        return;
    }
    if (MEM_BU(0, (gpr)(int32_t)multiplayer) == 0) {
        if (driving) {
            log("f=%u the match is over: controller 1 back to player 1", (unsigned)frames);
        }
        driving = false;
        target = -1;
        return;
    }
    frames++;
    const uint32_t pad = (uint32_t)MEM_W(0, (gpr)(int32_t)controller_now);
    if (!in_rdram(pad)) {
        return;
    }
    held = (uint16_t)MEM_HU(0, (gpr)(int32_t)pad);
    stick_x = (int8_t)MEM_B(2, (gpr)(int32_t)pad);
    stick_y = (int8_t)MEM_B(3, (gpr)(int32_t)pad);
    pressed = (uint16_t)MEM_HU(0, (gpr)(int32_t)pressed_table);

    if ((pressed & button_dpad_up) != 0) {
        if (driving) {
            driving = false;
            log("f=%u D-pad Up: controller 1 back to player 1 (from character %d)", (unsigned)frames, target);
            conker::notices::show("Test: controller 1 back to you", true);
            target = -1;
        } else {
            target = nearest_bot(rdram);
            if (target < 0) {
                log("f=%u D-pad Up: no bot in the match to drive", (unsigned)frames);
                conker::notices::show("Test: no bot to drive", true);
            } else {
                driving = true;
                log("f=%u D-pad Up: controller 1 drives character %d (brain %08X)", (unsigned)frames, target, (unsigned)bot_brain(rdram, target));
                conker::notices::show("Test: you're driving the nearest bot (D-pad Up to stop)", true);
            }
        }
        held &= (uint16_t)~button_dpad_up;
        pressed &= (uint16_t)~button_dpad_up;
    }
    if (!driving) {
        return;
    }
    // Player 1 stands still: the game's own empty controller for the characters without one (func_1504A730's sp+0x5C), Start
    // kept so the pause still opens.
    const uint32_t empty = (uint32_t)ctx->r29 + 0x5C;
    for (uint32_t i = 0; i < 6; i++) {
        MEM_B(i, (gpr)(int32_t)empty) = 0;
    }
    MEM_H(0, (gpr)(int32_t)empty) = (int16_t)(held & button_start);
    MEM_W(0, (gpr)(int32_t)controller_now) = (int32_t)empty;
    MEM_H(0, (gpr)(int32_t)pressed_table) = (int16_t)(pressed & button_start);
}

extern "C" void conker_possess_brain(uint8_t* rdram, recomp_context* ctx) {
    if (!driving || target < 0 || conker::netsession::active()) {
        return;
    }
    if ((uint32_t)ctx->r16 != character(target)) {
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
    if (!driving || target < 0 || conker::netsession::active() || (uint32_t)ctx->r16 != character(target)) {
        return;
    }
    ctx->r13 = 0;
    static uint32_t logged = 0;
    if (logged < 3) {
        logged++;
        log("f=%u character %d walks by the stick, not its path finder", (unsigned)frames, target);
    }
}
