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

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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
// (0.9.75's presses of the host's A in the bar, to go on to the multiplayer menu, are gone in 0.9.76: they started a one-player game.)
extern "C" void func_1501C730(uint8_t* rdram, recomp_context* ctx);

// co-op (0.10.28 on): see conker_coop_start below
namespace {
    bool coop_pending = false;
    bool coop_pending_frame_set = false;
    bool coop_started = false;   // (0.10.31) the stage was started with two or more players; off at the barrels again
    int coop_players = 0;
    // (0.10.32) the game's frames (counted at func_10011BB8, conker_coop_frame) and the one a scene last finished loading at
    std::atomic<uint32_t> coop_frames{ 0 };
    std::atomic<uint32_t> coop_loaded_at{ 0 };
    constexpr uint32_t coop_settle_frames = 30;
}

namespace {
    bool boot_bar_requested = false;
    bool boot_bar_set_up = false;
}

extern "C" void conker_online_boot_black(uint8_t* rdram, recomp_context* ctx);

extern "C" void conker_online_boot_logos(uint8_t* rdram, recomp_context* ctx) {
    conker_online_boot_black(rdram, ctx);
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
    // (0.10.32) a scene has just loaded (func_15007A70's return): the co-op stage settles before the pause menu is let in again
    coop_loaded_at.store(coop_frames.load());
    if (conker::netmenu::is_open() && coop_started) {
        conker::netmenu::set_open(false);   // (at once, for this frame's HUD; net_session.cpp closes its side at the next input)
        conker::netsession::log("[coop] the stage reloaded with the pause menu open: the menu closed");
    }
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


// 0.9.78: online, on from the bar to the multiplayer menu (the barrels: BACK, RACE, TANK, ...), by the game's own state and no
// presses. Hook at func_151DE8F0's entry (the bar's phase, 3), on every machine at the same frame of the game, so they stay the same:
//   1. Conker's walk into the bar is a cutscene (D_800C35EA 1): the bar's own skip of it (D_8008FD84, what any button there sets,
//      func_1501E05C) is set.
//   2. Once the menu is up and still (the bar's setup done, D_8008FDA4 5 on; zoomed out, gs+8 0, not zooming, gs+0xC 0, not turning,
//      gs+4 0; the one-player menu, gs+0x3E 0) for 10 frames: what func_151E6C1C does when the MULTI barrel (item 0) has been zoomed
//      into (gs+8 1, gs+0x2B 0): gs+0x3E 1, gs+8 D_800ABA88, gs+0xC -D_8008FF90 (zooming out), gs+0x2A 1, and the game zooms out to the
//      multiplayer barrels on its own. With the music the zoom into MULTI starts (func_1000D96C, D_800AB908's first track,
//      D_800E0A97 the one playing), as the A on MULTI does.
// Once a game start; a soft reset or the way back from a match is left to the player.
extern "C" void func_1000D96C(uint8_t* rdram, recomp_context* ctx);

namespace {
    bool boot_multi_done = false;
    bool boot_walk_skipped = false;
    int boot_menu_still = 0;
    float boot_float(uint8_t* rdram, uint32_t address) {
        const uint32_t word = (uint32_t)MEM_W(0, (gpr)(int32_t)address);
        float value;
        std::memcpy(&value, &word, 4);
        return value;
    }
}

extern "C" void conker_online_boot_multi(uint8_t* rdram, recomp_context* ctx) {
    if (!conker::netsession::active() || !boot_bar_set_up || boot_multi_done) {
        return;
    }
    if ((int32_t)MEM_W(0, (gpr)(int32_t)0x800BE9F0) != 0x1D || (uint8_t)MEM_BU(0, (gpr)(int32_t)0x800E0B94) != 3) {
        return;
    }
    const uint32_t state = (uint32_t)MEM_W(0, (gpr)(int32_t)0x8008FDD4);
    if ((state >> 24) != 0x80) {
        return;
    }
    const gpr gs = (gpr)(int32_t)state;
    if ((uint8_t)MEM_BU(0, (gpr)(int32_t)0x800C35EA) == 1) {
        boot_menu_still = 0;
        if (!boot_walk_skipped && MEM_B(0, (gpr)(int32_t)0x8008FD84) == 0) {
            boot_walk_skipped = true;
            MEM_B(0, (gpr)(int32_t)0x8008FD84) = 1;
            conker::netsession::log("[boot] online: the walk into the bar skipped");
        }
        return;
    }
    const bool still = (uint8_t)MEM_BU(0, (gpr)(int32_t)0x8008FDA4) >= 5 && MEM_B(0x3E, gs) == 0 &&
        boot_float(rdram, state + 0x8) == 0.0f && boot_float(rdram, state + 0xC) == 0.0f && boot_float(rdram, state + 0x4) == 0.0f;
    boot_menu_still = still ? boot_menu_still + 1 : 0;
    if (boot_menu_still < 10) {
        return;
    }
    boot_multi_done = true;
    const float zoom = boot_float(rdram, 0x800ABA88);
    const float speed = boot_float(rdram, 0x8008FF90);
    uint32_t bits;
    MEM_B(0x2B, gs) = 0;
    MEM_B(0x3E, gs) = 1;
    std::memcpy(&bits, &zoom, 4);
    MEM_W(0x8, gs) = (int32_t)bits;
    const float back = -speed;
    std::memcpy(&bits, &back, 4);
    MEM_W(0xC, gs) = (int32_t)bits;
    MEM_B(0x2A, gs) = 1;
    const uint8_t track = (uint8_t)MEM_BU(0, (gpr)(int32_t)0x800AB908);
    const uint8_t playing = (uint8_t)MEM_BU(0, (gpr)(int32_t)0x800E0A97);
    const recomp_context saved = *ctx;
    ctx->r29 = (gpr)(int32_t)0x80F4C000;   // a stack of ours, as for the bar's request above
    ctx->r4 = track;
    ctx->r5 = playing;
    ctx->r6 = 0;
    func_1000D96C(rdram, ctx);
    *ctx = saved;
    MEM_B(0, (gpr)(int32_t)0x800E0A97) = (int8_t)track;
    char line[160];
    std::snprintf(line, sizeof(line), "[boot] online: on to the multiplayer menu (zoom %.3f, speed %.4f, music %u after %u)", zoom, speed,
        (unsigned)track, (unsigned)playing);
    conker::netsession::log(line);
}

// 0.9.80: the way there under a black screen. 0.9.78 showed the logos' first frames, the bar, and the zoom out of the MULTI barrel
// (about two seconds): the screen's fade (D_800E0B96, 0xFF black, which the game takes down a step each frame) is held at black from
// the logos (func_151DE6D4's return) and through the bar (func_151DE8F0's return, after the game's own step of the fade) until the
// multiplayer barrels are up and still (gs+0x3E 1, gs+0x2A 0, zoomed out and not moving: gs+0, +4, +8, +0xC, +0x10 0), and then
// left to the game, which fades in from black as it does after the logos. At most 900 frames of the bar (15 seconds), whatever
// happens. Game memory, the same frame on every machine.
namespace {
    bool boot_black_released = false;
    int boot_black_bar_frames = 0;
}

extern "C" void conker_online_boot_black(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    if (!conker::netsession::active() || boot_black_released) {
        return;
    }
    const bool bar = (int32_t)MEM_W(0, (gpr)(int32_t)0x800BE9F0) == 0x1D && (uint8_t)MEM_BU(0, (gpr)(int32_t)0x800E0B94) == 3;
    if (bar && boot_multi_done) {
        const uint32_t state = (uint32_t)MEM_W(0, (gpr)(int32_t)0x8008FDD4);
        if ((state >> 24) == 0x80) {
            const gpr gs = (gpr)(int32_t)state;
            const bool up = MEM_B(0x3E, gs) == 1 && MEM_B(0x2A, gs) == 0 && boot_float(rdram, state + 0x0) == 0.0f &&
                boot_float(rdram, state + 0x4) == 0.0f && boot_float(rdram, state + 0x8) == 0.0f && boot_float(rdram, state + 0xC) == 0.0f &&
                boot_float(rdram, state + 0x10) == 0.0f;
            if (up) {
                boot_black_released = true;
                char line[120];
                std::snprintf(line, sizeof(line), "[boot] online: the multiplayer menu is up, the screen fades in (barrel %d)", (int)MEM_B(0x2B, gs));
                conker::netsession::log(line);
                return;
            }
        }
    }
    if (bar && ++boot_black_bar_frames > 900) {
        boot_black_released = true;
        conker::netsession::log("[boot] online: the multiplayer menu didn't come up in 900 frames: the screen is left to the game");
        return;
    }
    MEM_B(0, (gpr)(int32_t)0x800E0B96) = (int8_t)0xFF;
}

// 0.9.81: no controller during the black screen. Held from the start of an online game until the multiplayer barrels are up (or the
// 900 frames are out), and only in the logos' and the bar's scenes (0x25, 0x1D), so a boot that went some other way is never left
// without controllers. It goes by the game's own state, the same on every machine at the same read.
bool conker::netmenu::boot_screen_held(uint8_t* rdram) {
    // (no netsession::active() here: net_session.cpp's input calls this with its lock held, online only)
    if (rdram == nullptr || boot_black_released) {
        return false;
    }
    const int32_t scene = (int32_t)MEM_W(0, (gpr)(int32_t)0x800BE9F0);
    return scene == 0x25 || scene == 0x1D;
}

// 0.10.28: co-op (stage 1). Set by A on BACK (conker_online_no_back), off again once back at the barrels, zoomed out.
namespace {
    // The scene table's index (D_80087430, 20 bytes each) of the stage: Zombie Grave Shoot (0.10.29), the graveyard, where Gregg gives
    // Conker the shotgun and the zombies come (scene 0x41, entry 0xD004). (Zombie Fest, the mansion's, is 0xAB.)
    constexpr int16_t coop_stage = 0xA7;
}

extern "C" void func_151E30C4(uint8_t* rdram, recomp_context* ctx);

// 0.10.28: the race lobby's start (func_151E4314, the mode's opening scene, or func_151E2834, the match: hook at each's entry), with a
// co-op start asked for: the one-player game's start from the Chapters menu instead, func_151E30C4 with D_8008FDB4 1 (a scene from the
// table) and D_8008FDAC the stage's index, as the Chapters menu leaves them. Every machine at the same frame (the game's state). Returns
// 1 when it has started it (the function hooked then returns at once).
extern "C" int conker_coop_start(uint8_t* rdram, recomp_context* ctx) {
    if (!conker::netsession::active() || !coop_pending) {
        return 0;
    }
    coop_pending = false;
    MEM_B(0, (gpr)(int32_t)0x8008FDB4) = 1;
    MEM_H(0, (gpr)(int32_t)0x8008FDAC) = coop_stage;
    MEM_B(0, (gpr)(int32_t)0x8008FD74) = 3;
    // (0.10.30, stage 2) the players who joined on the race's character select: D_8008FE44's first D_8008FE40 entries, each a
    // controller (0-3), read before the one-player start sets its own counts
    const int joined = std::min(std::max((int)(int8_t)MEM_B(0, (gpr)(int32_t)0x8008FE40), 1), 4);
    uint32_t mask = 0;
    for (int k = 0; k < joined; k++) {
        const int controller = (int)(int8_t)MEM_B(0, (gpr)(int32_t)(0x8008FE44u + (uint32_t)k));
        if (controller >= 0 && controller < 4) {
            mask |= 1u << controller;
        }
    }
    if ((mask & 1u) == 0) {
        mask |= 1u;   // (player 1 always: the one-player game's Conker is the first player's)
    }
    int players = 0;
    for (int k = 0; k < 4; k++) {
        players += (int)((mask >> k) & 1u);
    }
    const recomp_context saved = *ctx;
    func_151E30C4(rdram, ctx);
    *ctx = saved;
    coop_started = players >= 2;
    coop_players = players;
    if (players >= 2) {
        // 0.10.30, stage 2: every player who joined is a player of the stage, each with a Conker of their own (character 0, the
        // one-player game's) and a camera of their own: the scene's request (func_1501C730, mode 6) left with the player mask at
        // D_800BEA00 (1 for the one-player game) and the screen's layout at D_800BEA04 (0, one screen); the scene's load
        // (func_15007A70 -> func_15017300) sets up a player for each bit of the mask with D_80087270's character, as a match's does.
        // The counts a match's start sets (D_8008FD88/8C/90/94, D_800BE618) to the players.
        const uint32_t layout = (uint32_t)(uint8_t)MEM_BU(0, (gpr)(int32_t)(0x8008FF37u + (uint32_t)players));
        MEM_W(0, (gpr)(int32_t)0x800BEA00) = (int32_t)mask;
        MEM_W(0, (gpr)(int32_t)0x800BEA04) = (int32_t)layout;
        for (uint32_t k = 0; k < 4; k++) {
            MEM_B(0, (gpr)(int32_t)(0x80087270u + k)) = 0;
        }
        MEM_B(0, (gpr)(int32_t)0x8008FD88) = (int8_t)players;
        MEM_B(0, (gpr)(int32_t)0x8008FD8C) = (int8_t)players;
        MEM_B(0, (gpr)(int32_t)0x8008FD90) = (int8_t)players;
        MEM_B(0, (gpr)(int32_t)0x8008FD94) = (int8_t)players;
        MEM_B(0, (gpr)(int32_t)0x800BE618) = (int8_t)players;
    }
    char line[200];
    std::snprintf(line, sizeof(line), "[coop] the race's start: the campaign stage (table %02X, scene %02X) instead, %d players (mask %X, layout %u)",
        (unsigned)coop_stage, (unsigned)MEM_W(0, (gpr)(int32_t)0x800BE9F4), players, (unsigned)mask, (unsigned)MEM_W(0, (gpr)(int32_t)0x800BEA04));
    conker::netsession::log(line);
    return 1;
}

// 0.9.82: online, the multiplayer barrels' BACK leads nowhere (the one-player game is out of reach from an online game). Hook at the
// entry of func_151E6C1C, the barrels' camera, which acts on the menu's buttons of the frame (gs+0x20, the bar's: 0x10 A or Start, 0x20
// B). With the multiplayer barrels (gs+0x3E 1): on BACK (gs+0x2B 0) A and B are taken out, so it isn't zoomed into (which turns the
// one-player barrels back on); zoomed out (gs+8 0) B is taken out, which jumps to BACK and zooms into it. Everything else is the game's:
// turning to the other barrels, A on them, B out of them. The game's own state, the same on every machine.
extern "C" void conker_online_no_back(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    if (!conker::netsession::active()) {
        return;
    }
    const uint32_t state = (uint32_t)MEM_W(0, (gpr)(int32_t)0x8008FDD4);
    if ((state >> 24) != 0x80 || (int32_t)MEM_W(0, (gpr)(int32_t)0x800BE9F0) != 0x1D) {
        return;
    }
    const gpr gs = (gpr)(int32_t)state;
    coop_started = false;   // (0.10.31) back in the bar: no co-op stage running
    if (MEM_B(0x3E, gs) != 1) {
        return;
    }
    const uint16_t buttons = (uint16_t)MEM_HU(0x20, gs);
    uint16_t kept = buttons;
    const bool zoomed_out = boot_float(rdram, state + 0x8) == 0.0f;
    if (zoomed_out && !coop_pending_frame_set) {
        coop_pending = false;   // back at the barrels: a co-op start asked for before is off
    }
    coop_pending_frame_set = false;
    if (MEM_B(0x2B, gs) == 0 && zoomed_out && (buttons & 0x10u) != 0 && boot_float(rdram, state + 0x0) == 0.0f &&
            boot_float(rdram, state + 0x4) == 0.0f && boot_float(rdram, state + 0xC) == 0.0f) {
        // 0.10.28: co-op. A (or Start) on BACK, the barrels still: the RACE barrel's lobby (barrel 7, the race's screens, its character
        // select with every player) as the placeholder lobby, and its start goes to the one-player campaign's scene instead
        // (conker_coop_start). The A is the player's own press, kept: the game zooms into the RACE barrel with it.
        coop_pending = true;
        coop_pending_frame_set = true;
        MEM_B(0x2B, gs) = 7;
        conker::netsession::log("[coop] A on BACK: the race's lobby, for the co-op campaign stage");
        return;
    }
    if (MEM_B(0x2B, gs) == 0) {
        kept &= (uint16_t)~0x30u;
    }
    if (boot_float(rdram, state + 0x8) == 0.0f) {
        kept &= (uint16_t)~0x20u;
    }
    if (kept != buttons) {
        MEM_H(0x20, gs) = (int16_t)kept;
        static int logged = 0;
        if (logged < 20) {
            char line[120];
            std::snprintf(line, sizeof(line), "[boot] online: the barrels' %s held back (barrel %d)", (buttons & ~kept & 0x10u) != 0 ? "A on BACK" : "B",
                (int)MEM_B(0x2B, gs));
            conker::netsession::log(line);
            logged++;
        }
    }
}

bool conker::netmenu::coop_running(uint8_t* rdram) {
    // (no netsession::active() here: net_session.cpp's input calls this with its lock held; coop_started is only ever set online)
    if (rdram == nullptr || !coop_started) {
        return false;
    }
    // 0.10.32: not for the first frames after the stage (re)loads (a death, a checkpoint, the way on to the next part): the online
    // pause menu open on this machine as it reloaded had the HUD draw the pause screen over a stage whose data wasn't there yet (a
    // crash, func_1503F4B0 from func_151EDBDC). Not running, the menu closes (net_session.cpp) and the pause screen isn't drawn.
    if (coop_frames.load() - coop_loaded_at.load() < coop_settle_frames) {
        return false;
    }
    const int32_t scene = (int32_t)MEM_W(0, (gpr)(int32_t)0x800BE9F0);
    return scene != 0x1D && scene != 0x25;
}

// 0.10.31, measuring only (stage 3's groundwork): in the co-op stage, the players' characters (gObjects, 0x800CC2D0, 0x32C each) and
// the players' table (D_800D2138, 524 bytes, func_15085710's) are watched every frame, and a byte that changes is logged for its
// first four changes only: what changes rarely (a weapon given, a mode set) shows, the positions and animations that change all the
// time don't. Player 1 is given the shotgun by the stage; player 2 isn't: the difference shows where the game keeps it.
namespace {
    constexpr uint32_t coop_objects = 0x800CC2D0;
    constexpr uint32_t coop_object_size = 0x32C;
    constexpr uint32_t coop_table = 0x800D2138;
    constexpr uint32_t coop_table_size = 524;
    std::vector<uint8_t> coop_last;
    std::vector<uint8_t> coop_changes;
    int coop_lines = 0;
}

extern "C" void func_15063570(uint8_t* rdram, recomp_context* ctx);

namespace {
    uint32_t coop_modes[4] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
    // (0.10.51) frames a player has been without the shotgun while another player has it
    uint32_t coop_without[4] = {};
    constexpr uint32_t coop_give_after = 30;
    // (0.10.52, measuring) the players' control blocks (the character's +0x31C, where the weapon mode is), watched as the characters
    // are; and dumped whole (with the characters) when a player first has the shotgun out, and 90 frames later
    constexpr uint32_t coop_control_size = 0x280;
    std::vector<uint8_t> coop_control_last;
    std::vector<uint8_t> coop_control_changes;
    int coop_control_lines = 0;
    uint32_t coop_dump_at = 0;   // the frame of the second dump (0: none due)
    bool coop_dumped = false;

    void coop_dump(uint8_t* rdram, int count, const char* why) {
        char line[200];
        std::snprintf(line, sizeof(line), "[coop dump] f=%u %s", (unsigned)conker::netsession::frame(), why);
        conker::netsession::log(line);
        for (int k = 0; k < count; k++) {
            const uint32_t object = 0x800CC2D0u + (uint32_t)k * 0x32Cu;
            const uint32_t control = (uint32_t)MEM_W(0x31C, (gpr)(int32_t)object);
            const bool control_ok = (control >> 24) == 0x80 && (control & 0x7FFFFFu) < 0x7FFF00u;
            for (int part = 0; part < 2; part++) {
                const uint32_t base = part == 0 ? object : control;
                const uint32_t size = part == 0 ? 0x32Cu : coop_control_size;
                if (part == 1 && !control_ok) {
                    continue;
                }
                for (uint32_t at = 0; at < size; at += 32) {
                    int n = std::snprintf(line, sizeof(line), "[coop dump] p%d %s+%03X:", k, part == 0 ? "obj" : "ctl", (unsigned)at);
                    for (uint32_t i = at; i < at + 32 && i < size && n < (int)sizeof(line) - 4; i++) {
                        n += std::snprintf(line + n, sizeof(line) - (size_t)n, " %02X", (unsigned)MEM_BU(0, (gpr)(int32_t)(base + i)));
                    }
                    conker::netsession::log(line);
                }
            }
        }
    }
}

extern "C" void conker_coop_frame(uint8_t* rdram, recomp_context* ctx) {
    coop_frames.fetch_add(1);
    if (!conker::netsession::active() || !conker::netmenu::coop_running(rdram)) {
        coop_last.clear();
        for (int k = 0; k < 4; k++) {
            coop_modes[k] = 0xFFFFFFFFu;
            coop_without[k] = 0;
        }
        coop_control_last.clear();
        coop_dump_at = 0;
        coop_dumped = false;
        return;
    }
    // (0.10.32) each player's weapon mode (+0x78 of the character's +0x31C, func_15063570's 0x3B the shotgun), logged as it changes;
    // and should the first player's be the shotgun's with another player's still none (handed out before conker_coop_weapon could
    // see it), that player is given it too, once a stage (every machine at the same frame: the game's state)
    // (0.10.51) The mode is a byte: read as a word it was 0x3B000000, never 0x3B, so nobody else was ever given it (and the stage hands
    // player 1 the shotgun some way conker_coop_weapon's hook doesn't see). Now: any player with it, and another without it (mode 0)
    // for 30 frames in a row, that one is given it too (func_15063570, the game's own: the mode, the ammo); again if they lose it.
    {
        const int count = std::min(std::max(coop_players, 1), 4);
        uint32_t modes[4] = {};
        for (int k = 0; k < count; k++) {
            const uint32_t object = 0x800CC2D0u + (uint32_t)k * 0x32Cu;
            const uint32_t control = (uint32_t)MEM_W(0x31C, (gpr)(int32_t)object);
            modes[k] = ((control >> 24) == 0x80 && (control & 0x7FFFFFu) < 0x7FFF00u) ? (uint32_t)MEM_BU(0x78, (gpr)(int32_t)control) : 0xFFFFFFFEu;
            if (modes[k] != coop_modes[k]) {
                char line[96];
                std::snprintf(line, sizeof(line), "[coop mode] f=%u player %d: weapon mode %X", (unsigned)conker::netsession::frame(), k + 1, (unsigned)modes[k]);
                conker::netsession::log(line);
                coop_modes[k] = modes[k];
            }
        }
        int holder = -1;
        for (int k = 0; k < count; k++) {
            if (modes[k] == 0x3Bu) {
                holder = k;
                break;
            }
        }
        // (0.10.52, measuring) the dumps
        if (holder >= 0 && !coop_dumped) {
            coop_dumped = true;
            coop_dump_at = conker::netsession::frame() + 90;
            char why[64];
            std::snprintf(why, sizeof(why), "player %d has the shotgun out first", holder + 1);
            coop_dump(rdram, count, why);
        }
        if (coop_dump_at != 0 && conker::netsession::frame() >= coop_dump_at) {
            coop_dump_at = 0;
            coop_dump(rdram, count, "90 frames later");
        }
        // (0.10.52, measuring) the control blocks' bytes as they change (each byte's first 4 changes)
        {
            std::vector<uint8_t> now((size_t)count * coop_control_size, 0);
            for (int k = 0; k < count; k++) {
                const uint32_t control = (uint32_t)MEM_W(0x31C, (gpr)(int32_t)(0x800CC2D0u + (uint32_t)k * 0x32Cu));
                if ((control >> 24) != 0x80 || (control & 0x7FFFFFu) >= 0x7FFF00u) {
                    continue;
                }
                for (uint32_t i = 0; i < coop_control_size; i++) {
                    now[(size_t)k * coop_control_size + i] = (uint8_t)MEM_BU(0, (gpr)(int32_t)(control + i));
                }
            }
            if (coop_control_last.size() != now.size()) {
                coop_control_last = now;
                coop_control_changes.assign(now.size(), 0);
            }
            else {
                std::string text;
                for (size_t i = 0; i < now.size() && coop_control_lines < 3000; i++) {
                    if (now[i] != coop_control_last[i] && coop_control_changes[i] < 4) {
                        coop_control_changes[i]++;
                        char part[40];
                        std::snprintf(part, sizeof(part), " p%u+%03X:%02X>%02X", (unsigned)(i / coop_control_size), (unsigned)(i % coop_control_size),
                            (unsigned)coop_control_last[i], (unsigned)now[i]);
                        text += part;
                    }
                }
                for (size_t at = 0; at < text.size(); at += 900) {
                    conker::netsession::log(("[coop ctl] f=" + std::to_string(conker::netsession::frame()) + text.substr(at, 900)).c_str());
                    coop_control_lines++;
                }
                coop_control_last.swap(now);
            }
        }
        for (int k = 0; k < count; k++) {
            if (holder < 0 || modes[k] != 0u) {
                coop_without[k] = 0;
                continue;
            }
            if (++coop_without[k] < coop_give_after || ctx == nullptr) {
                continue;
            }
            coop_without[k] = 0;
            const recomp_context saved = *ctx;
            ctx->r4 = (gpr)(int32_t)(0x800CC2D0u + (uint32_t)k * 0x32Cu);
            func_15063570(rdram, ctx);
            *ctx = saved;
            char line[96];
            std::snprintf(line, sizeof(line), "[coop] f=%u player %d given the shotgun (player %d has it)", (unsigned)conker::netsession::frame(), k + 1,
                holder + 1);
            conker::netsession::log(line);
        }
    }
    const uint32_t players = (uint32_t)std::min(std::max(coop_players, 1), 4);
    const uint32_t total = players * coop_object_size + coop_table_size;
    std::vector<uint8_t> now(total);
    for (uint32_t i = 0; i < players * coop_object_size; i++) {
        now[i] = (uint8_t)MEM_BU(0, (gpr)(int32_t)(coop_objects + i));
    }
    for (uint32_t i = 0; i < coop_table_size; i++) {
        now[players * coop_object_size + i] = (uint8_t)MEM_BU(0, (gpr)(int32_t)(coop_table + i));
    }
    if (coop_last.size() != total) {
        coop_last = now;
        coop_changes.assign(total, 0);
        char line[120];
        std::snprintf(line, sizeof(line), "[coop watch] f=%u watching %u players' characters and the players' table", (unsigned)conker::netsession::frame(),
            (unsigned)players);
        conker::netsession::log(line);
        return;
    }
    std::string text;
    for (uint32_t i = 0; i < total && coop_lines < 4000; i++) {
        if (now[i] == coop_last[i]) {
            continue;
        }
        if (coop_changes[i] < 4) {
            coop_changes[i]++;
            char part[48];
            if (i < players * coop_object_size) {
                std::snprintf(part, sizeof(part), " p%u+%03X:%02X>%02X", (unsigned)(i / coop_object_size), (unsigned)(i % coop_object_size),
                    (unsigned)coop_last[i], (unsigned)now[i]);
            } else {
                std::snprintf(part, sizeof(part), " tbl+%03X:%02X>%02X", (unsigned)(i - players * coop_object_size), (unsigned)coop_last[i], (unsigned)now[i]);
            }
            text += part;
        }
    }
    if (!text.empty()) {
        for (size_t at = 0; at < text.size(); at += 900) {
            conker::netsession::log(("[coop watch] f=" + std::to_string(conker::netsession::frame()) + text.substr(at, 900)).c_str());
            coop_lines++;
        }
    }
    coop_last.swap(now);
}

// 0.10.32: co-op, the weapons the stage's scripts hand to Conker. A script command (func_15097A8C's, its argument at +8 of the
// command, the character in $s1) puts a character into a weapon's mode: 9 with func_15063404, 0x3B (the shotgun: Gregg's, in the
// graveyard) with func_15063570, and takes 9's away with func_150634E4. The scripts are the one-player game's: they hand it to the
// first player's Conker (gObjects[0]) only. Right after each call for the first player's Conker, the same call for every other
// player's (gObjects[1..]), on every machine at the same frame.
extern "C" void func_15063404(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_15063570(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_150634E4(uint8_t* rdram, recomp_context* ctx);

extern "C" void conker_coop_weapon(uint8_t* rdram, recomp_context* ctx, int which) {
    if (!conker::netsession::active() || !coop_started || coop_players < 2) {
        return;
    }
    constexpr uint32_t objects = 0x800CC2D0, object_size = 0x32C;
    if ((uint32_t)ctx->r17 != objects) {
        return;   // (another character's: left to the game)
    }
    for (int k = 1; k < std::min(coop_players, 4); k++) {
        const recomp_context saved = *ctx;
        ctx->r4 = (gpr)(int32_t)(objects + (uint32_t)k * object_size);
        if (which == 0) {
            func_15063404(rdram, ctx);
        } else if (which == 1) {
            func_15063570(rdram, ctx);
        } else {
            func_150634E4(rdram, ctx);
        }
        *ctx = saved;
    }
    char line[120];
    std::snprintf(line, sizeof(line), "[coop] f=%u the script's %s for player 1: the same for players 2 to %d", (unsigned)conker::netsession::frame(),
        which == 0 ? "weapon 9" : which == 1 ? "shotgun" : "weapon 9 taken away", coop_players);
    conker::netsession::log(line);
}

// 0.10.52, measuring: who calls func_15063570 (the shotgun's mode) for a player's character, and from where (its return address)
extern "C" void conker_coop_who(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (!conker::netsession::active() || !coop_started) {
        return;
    }
    const uint32_t object = (uint32_t)ctx->r4;
    const int player = object >= 0x800CC2D0u && object < 0x800CC2D0u + 4u * 0x32Cu ? (int)((object - 0x800CC2D0u) / 0x32Cu) + 1 : 0;
    char line[160];
    std::snprintf(line, sizeof(line), "[coop who] f=%u func_15063570 for %08X (player %d), from %08X", (unsigned)conker::netsession::frame(),
        (unsigned)object, player, (unsigned)(uint32_t)ctx->r31);
    conker::netsession::log(line);
}
