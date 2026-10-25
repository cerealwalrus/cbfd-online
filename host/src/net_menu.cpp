// The online pause menu. The game's own pause screen (the PAUSED logo, its stats row, the CONT.. and QUIT buttons) is
// drawn and run by the game's code while D_800BEAC0 is set, and D_800BEAC0 also stops the whole game, which online
// every machine has to go on with. So the flag stays 0 in the game's memory, and while this machine's player has the
// menu open the hooks below (conker.toml) make just the two pieces of code that draw and run the pause screen think
// it's set:
//   which 1, func_151EB06C (the HUD) at 0x151EB0CC: the normal multiplayer HUD (the scores) isn't drawn, the pause
//   screen's own copy of it is;
//   which 2, func_151EB06C at 0x151EB128: the pause screen is drawn;
//   which 3, func_151DDC20 (the pause menu's logic) at 0x151DDFD0: the menu runs, reading its input from a player slot
//   that nobody uses (set_input; D_800E0A98 is the player the menu listens to) so that this player's own controller
//   drives it while the game itself gets a still one (net_session.cpp).
//   which 4, 5, 6, func_151DDC20 at 0x151DE2F0, 0x151DE334 and 0x151DE34C: the RESTART button is drawn below the screen
//   (it would restart this machine's game alone), QUIT is drawn where it was, and the highlight RESTART would have
//   had goes to QUIT.
// net_session.cpp opens and closes it (the game's own unpause sets D_8008FEF0, which it sees) and ends this
// machine's part of the match when QUIT is confirmed.

#include <atomic>
#include <cstdint>

#include "recomp.h"

#include "conker.hpp"

namespace {
    std::atomic<bool> menu_open = false;
    std::atomic<uint32_t> input_buttons = 0;
    std::atomic<int> input_x = 0;
    std::atomic<int> input_y = 0;
    std::atomic<int> input_slot = 3;
    // the menu reads a slot a player uses (all four are in): its values are kept here while the menu has its own, and put back
    std::atomic<bool> borrowed = false;
    bool saved = false;
    int16_t saved_buttons = 0;
    int8_t saved_x = 0, saved_y = 0;
    int saved_slot = 0;

    void put_back(uint8_t* rdram) {
        if (saved) {
            saved = false;
            MEM_H(0, (gpr)(int32_t)(0x800BE930u + 2u * (uint32_t)saved_slot)) = saved_buttons;
            MEM_B(2, (gpr)(int32_t)(0x800BE918u + 6u * (uint32_t)saved_slot)) = saved_x;
            MEM_B(3, (gpr)(int32_t)(0x800BE918u + 6u * (uint32_t)saved_slot)) = saved_y;
        }
    }
}

void conker::netmenu::set_borrowed(bool value) {
    borrowed = value;
}

void conker::netmenu::set_open(bool open) {
    menu_open = open;
}

bool conker::netmenu::is_open() {
    return menu_open;
}

void conker::netmenu::set_input(int slot, uint16_t buttons, int x, int y) {
    input_slot = slot;
    input_x = x;
    input_y = y;
    input_buttons = buttons;
}

extern "C" void conker_net_pause_hook(uint8_t* rdram, recomp_context* ctx, int which) {
    if (!menu_open) {
        return;
    }
    if (which == 1) {
        ctx->r13 = 1;
    } else if (which == 2) {
        ctx->r2 = 1;
    } else if (which == 4) {
        put_back(rdram);   // (the menu has read its input by now: the slot's own goes back)
        // before the jal: its delay slot makes $a1 (the y of RESTART's button, in the game's 320 x 240 screen) from $t9
        ctx->r25 = (gpr)(int64_t)(450 << 16);
    } else if (which == 5) {
        // QUIT's y is 0x1F below RESTART's: it goes up to RESTART's place
        ctx->r5 = (gpr)(int64_t)(int32_t)((int32_t)ctx->r5 - 0x1F);
    } else if (which == 6) {
        // $v1 is the picked button: RESTART's picks QUIT's highlight
        if ((int32_t)ctx->r3 == 1) {
            ctx->r3 = 2;
        }
    } else if (which == 3) {
        ctx->r24 = 1;
        const int slot = input_slot;
        // the player slot the menu listens to, with this player's controller in it (D_800BE930: each slot's buttons, 2
        // bytes; D_800BE918: 6 bytes, the stick at +2 and +3)
        MEM_B(0, (gpr)(int32_t)0x800E0A98) = (int8_t)slot;
        if (borrowed.load()) {
            saved = true;
            saved_slot = slot;
            saved_buttons = MEM_H(0, (gpr)(int32_t)(0x800BE930u + 2u * (uint32_t)slot));
            saved_x = MEM_B(2, (gpr)(int32_t)(0x800BE918u + 6u * (uint32_t)slot));
            saved_y = MEM_B(3, (gpr)(int32_t)(0x800BE918u + 6u * (uint32_t)slot));
        }
        MEM_H(0, (gpr)(int32_t)(0x800BE930u + 2u * (uint32_t)slot)) = (int16_t)(uint16_t)input_buttons.load();
        MEM_B(2, (gpr)(int32_t)(0x800BE918u + 6u * (uint32_t)slot)) = (int8_t)input_x.load();
        MEM_B(3, (gpr)(int32_t)(0x800BE918u + 6u * (uint32_t)slot)) = (int8_t)input_y.load();
    }
}


// Online, straight to the multiplayer menu (0.9.75). The boot's intro is skipped on every machine, as mods/skip_intro does it (see there):
// once the logos (scene 0x25, phase 0xB) have run 10 frames, the bar (scene 0x1D) is asked for instead of the chainsaw opening, and once
// it has loaded, what the opening's end leaves behind is set. Hooks at func_151DE6D4's and func_15007A70's returns (conker.toml). Every
// machine does it at the same frame (the game's own frame counts), so they stay the same; with the mod as well, the first to ask wins.
// The host's controller then takes the menus to the multiplayer one (net_session.cpp, the boot's menus).
extern "C" void func_1501C730(uint8_t* rdram, recomp_context* ctx);

namespace {
    bool boot_bar_requested = false;
    bool boot_bar_set_up = false;
}

extern "C" void conker_online_boot_logos(uint8_t* rdram, recomp_context* ctx) {
    if (!conker::netsession::active() || boot_bar_requested) {
        return;
    }
    const int32_t logo_frames = MEM_W(0, (gpr)(int32_t)0x800E0A90);
    if ((uint8_t)MEM_BU(0, (gpr)(int32_t)0x800E0B94) != 0x0B || (int32_t)MEM_W(0, (gpr)(int32_t)0x800BE9F0) != 0x25 || logo_frames < 10) {
        return;
    }
    boot_bar_requested = true;
    MEM_B(0, (gpr)(int32_t)0x8008FE28) = 2;
    MEM_B(0, (gpr)(int32_t)0x800E0B94) = 1;
    MEM_B(0, (gpr)(int32_t)0x800D2E40) = 0;
    const recomp_context saved = *ctx;
    ctx->r29 = (gpr)(int32_t)0x80F4C000;   // a stack of ours (local_audio.cpp's, not in use here)
    ctx->r4 = 6;
    ctx->r5 = 0x1D;
    ctx->r6 = 0;
    ctx->r7 = 0;
    MEM_W(0x10, ctx->r29) = 1;
    func_1501C730(rdram, ctx);
    *ctx = saved;
    MEM_B(0, (gpr)(int32_t)0x800E0B96) = (int8_t)0xFF;
    conker::netsession::log("[boot] online: the intro skipped, the bar asked for");
}

extern "C" void conker_online_boot_scene(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    if (!boot_bar_requested || boot_bar_set_up || (int32_t)MEM_W(0, (gpr)(int32_t)0x800BE9F0) != 0x1D) {
        return;
    }
    boot_bar_set_up = true;
    MEM_B(0, (gpr)(int32_t)0x800D2E40) = 0;
    MEM_B(0, (gpr)(int32_t)0x800E0B94) = 3;
    MEM_B(0, (gpr)(int32_t)0x8008FD80) = 1;
    MEM_B(0, (gpr)(int32_t)0x8008FE28) = 2;
    MEM_B(0, (gpr)(int32_t)0x8008FDA4) = 0;
    const uint32_t state = (uint32_t)MEM_W(0, (gpr)(int32_t)0x8008FDD4);
    if ((state >> 24) == 0x80) {
        MEM_B(0x3E, (gpr)(int32_t)state) = 0;
        MEM_B(0x2B, (gpr)(int32_t)state) = 5;
        MEM_B(0x2C, (gpr)(int32_t)state) = 5;
    }
    conker::netsession::log("[boot] online: the bar loaded");
}
