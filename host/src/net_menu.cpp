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
        MEM_H(0, (gpr)(int32_t)(0x800BE930u + 2u * (uint32_t)slot)) = (int16_t)(uint16_t)input_buttons.load();
        MEM_B(2, (gpr)(int32_t)(0x800BE918u + 6u * (uint32_t)slot)) = (int8_t)input_x.load();
        MEM_B(3, (gpr)(int32_t)(0x800BE918u + 6u * (uint32_t)slot)) = (int8_t)input_y.load();
    }
}
