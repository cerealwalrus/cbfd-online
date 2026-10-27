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

// 0.10.88: the pause menu's buttons were drawn into this frame's list (conker_net_pause_hook 3 let them through). The menu can close
// (net_session.cpp, at the frame's input) after the pause menu has drawn but before the view rewrites the frame (net_view.cpp's
// frame_end): the view then took the menu as closed and moved its pictures as the game's HUD (to the corners of this machine's camera,
// QUIT and the stick at the bottom of the screen) for that frame. The view goes by both. Cleared as each frame's list starts.
namespace {
    std::atomic<bool> pause_drawn_frame{ false };
}

bool conker::netmenu::open_for_view() {
    return menu_open.load() || pause_drawn_frame.load();
}

void conker::netmenu::view_frame_begin() {
    pause_drawn_frame = false;
}

void conker::netmenu::set_input(int slot, uint16_t buttons, int x, int y) {
    input_slot = slot;
    input_x = x;
    input_y = y;
    input_buttons = buttons;
}

namespace {
    bool coop_pause_screen();   // (below, with the co-op stage's state)
}

namespace {
    std::atomic<bool> in_pause_screen{ false };   // (0.10.75) func_151EB06C, with this machine's menu open
    std::atomic<bool> in_pause_menu{ false };     // func_151DDC20, the same
}

bool conker::netmenu::local_pause_drawing() {
    return in_pause_screen.load() || in_pause_menu.load();
}

// (0.10.78) the textures' cache (func_1510D0EC loads a texture, func_1510D694 lets one go): the pause screen's textures loaded on
// this machine alone changed the cache's bookkeeping, which the others' games didn't have (the 0.10.77 log: the lowest and highest
// texture in use, D_800D9F58/5C, 0x7D3 and 0x8AB on the host, none on the others, and their games resynced from the host's, which
// crashed the renderer). Now the bookkeeping (the table of textures, each one's level and count, the lowest and highest, a flag) is
// copied as the pause screen's drawing begins and put back as it ends; the textures it loaded are kept aside (in heap_watch.cpp's
// pool) and put in the table again for its next drawing, so they are loaded once. Forgotten when a scene loads or a resync's copy
// comes in.
namespace {
    uint32_t coop_loaded_frame();   // (below, with the co-op stage's state)
    constexpr uint32_t tex_table = 0x800B0E58u, tex_level = 0x800BC448u, tex_refs = 0x800D9F68u, tex_count = 0x1E52u;
    constexpr uint32_t tex_low = 0x800D9F58u, tex_high = 0x800D9F5Cu, tex_flag = 0x800DBDBAu;
    std::vector<uint32_t> tex_saved_table;
    std::vector<uint8_t> tex_saved_bytes;
    uint32_t tex_saved_low = 0, tex_saved_high = 0;
    uint8_t tex_saved_flag = 0;
    bool tex_saved = false;
    std::vector<std::pair<uint32_t, uint32_t>> tex_aside;   // texture, its memory
    uint32_t tex_aside_generation = 0xFFFFFFFFu;
    uint32_t tex_aside_scene = 0xFFFFFFFFu;

    void tex_begin(uint8_t* rdram) {
        const uint32_t generation = conker::netsession::hook_generation();
        const uint32_t scene = coop_loaded_frame();
        if (generation != tex_aside_generation || scene != tex_aside_scene) {
            tex_aside.clear();
            tex_aside_generation = generation;
            tex_aside_scene = scene;
        }
        tex_saved_table.resize(tex_count);
        tex_saved_bytes.resize(tex_count * 2u);
        for (uint32_t i = 0; i < tex_count; i++) {
            tex_saved_table[i] = (uint32_t)MEM_W(0, (gpr)(int32_t)(tex_table + i * 4u));
            tex_saved_bytes[i] = (uint8_t)MEM_BU(0, (gpr)(int32_t)(tex_level + i));
            tex_saved_bytes[tex_count + i] = (uint8_t)MEM_BU(0, (gpr)(int32_t)(tex_refs + i));
        }
        tex_saved_low = (uint32_t)MEM_W(0, (gpr)(int32_t)tex_low);
        tex_saved_high = (uint32_t)MEM_W(0, (gpr)(int32_t)tex_high);
        tex_saved_flag = (uint8_t)MEM_BU(0, (gpr)(int32_t)tex_flag);
        tex_saved = true;
        for (const auto& kept : tex_aside) {
            if (tex_saved_table[kept.first] == 0xFFFFFFFFu) {
                MEM_W(0, (gpr)(int32_t)(tex_table + kept.first * 4u)) = (int32_t)kept.second;
            }
        }
    }

    void tex_end(uint8_t* rdram) {
        if (!tex_saved) {
            return;
        }
        tex_saved = false;
        for (uint32_t i = 0; i < tex_count; i++) {
            const uint32_t now = (uint32_t)MEM_W(0, (gpr)(int32_t)(tex_table + i * 4u));
            if (tex_saved_table[i] == 0xFFFFFFFFu && now != 0xFFFFFFFFu && (now >> 24) == 0x80 && now != 0x80000000u) {
                bool known = false;
                for (const auto& kept : tex_aside) {
                    known = known || kept.first == i;
                }
                if (!known) {
                    tex_aside.push_back({ i, now });
                }
            }
            MEM_W(0, (gpr)(int32_t)(tex_table + i * 4u)) = (int32_t)tex_saved_table[i];
            MEM_B(0, (gpr)(int32_t)(tex_level + i)) = (int8_t)tex_saved_bytes[i];
            MEM_B(0, (gpr)(int32_t)(tex_refs + i)) = (int8_t)tex_saved_bytes[tex_count + i];
        }
        MEM_W(0, (gpr)(int32_t)tex_low) = (int32_t)tex_saved_low;
        MEM_W(0, (gpr)(int32_t)tex_high) = (int32_t)tex_saved_high;
        MEM_B(0, (gpr)(int32_t)tex_flag) = (int8_t)tex_saved_flag;
    }
}

// (0.10.75) the pause screen's two functions begin (where 0: func_151EB06C, 1: func_151DDC20) and end (2, 3)
extern "C" void conker_pause_bounds(uint8_t* rdram, recomp_context* ctx, int where) {
    (void)ctx;
    const bool on = conker::netsession::active() && menu_open.load();
    const bool before = in_pause_screen.load() || in_pause_menu.load();
    switch (where) {
        case 0: in_pause_screen = on; break;
        case 1: in_pause_menu = on; break;
        case 2: in_pause_screen = false; break;
        case 3: in_pause_menu = false; break;
        default: break;
    }
    const bool after = in_pause_screen.load() || in_pause_menu.load();
    if (!before && after) {
        tex_begin(rdram);
    } else if (before && !after) {
        tex_end(rdram);
    }
}

// (0.10.75) func_10010F30 (a sound with no place) begins: the pause menu's own sounds (its cursor, its choices), played on this
// machine alone, took a game's sound slot that the other machines' games didn't: the slots' handles went apart, a character's "is
// my sound still playing?" (func_100107F8, the Gameboy's beeps every 8 frames) answered differently, and with it whether the RNG
// was asked (the 0.10.74 log: the host's game asked once more at frame 3392, from 1506E598). Returns 1 to skip it.
extern "C" int conker_pause_sound(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (!conker::netmenu::local_pause_drawing()) {
        return 0;
    }
    ctx->r2 = 0;
    return 1;
}

extern "C" void conker_net_pause_hook(uint8_t* rdram, recomp_context* ctx, int which) {
    if (which == 7) {
        conker_pause_bounds(rdram, ctx, 3);   // (0.10.75: func_151DDC20's end)
    }
    if (!menu_open) {
        if (which == 7) {
            put_back(rdram);   // (the menu closed this frame: a slot it borrowed still goes back)
        }
        // (0.10.77) in co-op, once this machine's menu has closed the pause menu's buttons aren't drawn as they leave (the game's own
        // pause is never on there): at 0x151DDFD0 $t8 0 goes straight to the function's end, past its buttons (the stray QUIT and
        // joystick at the bottom of the screen as the menu closed)
        if (which == 3 && coop_pause_screen()) {
            ctx->r24 = 0;
        }
        // (0.10.73) in co-op, the pause screen's last frames as it fades out after the menu has closed keep the layout too: RESTART
        // hidden and QUIT in its place (0.10.72 showed QUIT in its own place, at the bottom, for those frames)
        if (!((which == 4 || which == 5 || which == 6) && coop_pause_screen())) {
            return;
        }
    }
    if (which == 1) {
        ctx->r13 = 1;
    } else if (which == 2) {
        ctx->r2 = 1;
    } else if (which == 4) {
        put_back(rdram);   // (the menu has read its input by now: the slot's own goes back)
        // before the jal: its delay slot makes $a1 (the y of RESTART's button, in the game's 320 x 240 screen) from $t9
        // (0.10.74: 2000, not 450: 450 is the screen's bottom edge, where RESTART's button and the joystick beside it showed in the
        // co-op pause as it closed (a match's pause has its score table over it))
        ctx->r25 = (gpr)(int64_t)(2000 << 16);
    } else if (which == 7) {
        // (0.10.71) func_151DDC20's end: the borrowed slot's own input back, whichever way the menu went (its Y and N screen
        // doesn't pass 0x151DE2F0, so in 0.10.70 the player's own slot kept the menu's stick and buttons: their Conker moved on this
        // machine alone, the RNG went apart and the host resynced it, which showed as the flicker)
        put_back(rdram);
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
        pause_drawn_frame = true;   // (0.10.88: the buttons go into this frame's list)
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
    bool coop_started = false;
    // (0.10.61) the stage's players as it was started (the mask, the screen layout), for its reloads
    bool coop_restart_pending = false;   // (0.10.64) everyone down: the stage asked for again, its load is the co-op one
    uint32_t coop_mask = 0, coop_layout = 0;   // (0.10.31) the stage was started with two or more players; off at the barrels again
    int coop_players = 0;
    int8_t coop_characters[4] = { 0, 0, 0, 0 };   // (0.10.87) each player's character (by controller), from the race's character select
    // (0.10.32) the game's frames (counted at func_10011BB8, conker_coop_frame) and the one a scene last finished loading at
    std::atomic<uint32_t> coop_frames{ 0 };
    std::atomic<uint32_t> coop_loaded_at{ 0 };
    constexpr uint32_t coop_settle_frames = 30;
}

namespace {
    // (0.10.73) the co-op stage is running online: the pause screen (drawn only on a machine whose menu is open, and for its fade
    // after) gets the co-op layout, whether the menu is still open or fading out
    bool coop_pause_screen() {
        return conker::netsession::active() && coop_started && coop_players >= 2;
    }
    uint32_t coop_loaded_frame() {
        return coop_loaded_at.load();
    }
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
    // (0.10.54 tried Horror Intro, 0xA6, the same scene from its start with Gregg's cutscene; 0.10.56 back to Zombie Grave Shoot's start,
    // after it, the players' choice)
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
    // (0.10.55) coop_pending stays on: quitting the stage comes back to the same lobby (the race's screens), and Start there is the
    // co-op stage again, not a race; it's off once the players are back out at the barrels (conker_online_no_back)
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
    // 0.10.87: the characters picked on the race's character select, for the stage's players (as a match's start does, func_151E2834 at
    // 0x151E2CE0: the k-th player who joined, D_8008FE44[k], picked D_800E0B90[k], whose character is +6 of D_800AB57C's 8 byte
    // entry for it). The stage's players are by controller (the mask), so each goes to its controller's place in D_80087270.
    for (int k = 0; k < 4; k++) {
        coop_characters[k] = 0;
    }
    {
        char line[200];
        int at = std::snprintf(line, sizeof(line), "[coop] the characters picked (D_800E0B8C %u):", (unsigned)MEM_BU(0, (gpr)(int32_t)0x800E0B8C));
        for (int k = 0; k < joined; k++) {
            const int controller = (int)(int8_t)MEM_B(0, (gpr)(int32_t)(0x8008FE44u + (uint32_t)k));
            const int pick = (int)(int8_t)MEM_B(0, (gpr)(int32_t)(0x800E0B90u + (uint32_t)k));
            if (controller < 0 || controller >= 4 || pick < 0 || pick >= 32) {
                continue;
            }
            const uint8_t character = (uint8_t)MEM_BU(0, (gpr)(int32_t)(0x800AB57Cu + (uint32_t)pick * 8u + 6u));
            coop_characters[controller] = (int8_t)character;
            if (at > 0 && at < (int)sizeof(line)) {
                at += std::snprintf(line + at, sizeof(line) - (size_t)at, " player %d pick %d character %u;", controller + 1, pick, (unsigned)character);
            }
        }
        conker::netsession::log(line);
    }
    const recomp_context saved = *ctx;
    func_151E30C4(rdram, ctx);
    *ctx = saved;
    coop_started = players >= 2;
    coop_players = players;
    coop_restart_pending = false;
    if (players >= 2) {
        // 0.10.30, stage 2: every player who joined is a player of the stage, each with a Conker of their own (character 0, the
        // one-player game's) and a camera of their own: the scene's request (func_1501C730, mode 6) left with the player mask at
        // D_800BEA00 (1 for the one-player game) and the screen's layout at D_800BEA04 (0, one screen); the scene's load
        // (func_15007A70 -> func_15017300) sets up a player for each bit of the mask with D_80087270's character, as a match's does.
        // The counts a match's start sets (D_8008FD88/8C/90/94, D_800BE618) to the players.
        const uint32_t layout = (uint32_t)(uint8_t)MEM_BU(0, (gpr)(int32_t)(0x8008FF37u + (uint32_t)players));
        MEM_W(0, (gpr)(int32_t)0x800BEA00) = (int32_t)mask;
        MEM_W(0, (gpr)(int32_t)0x800BEA04) = (int32_t)layout;
        coop_mask = mask;
        coop_layout = layout;
        for (uint32_t k = 0; k < 4; k++) {
            MEM_B(0, (gpr)(int32_t)(0x80087270u + k)) = coop_characters[k];
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
    coop_restart_pending = false;
    if (MEM_B(0x3E, gs) != 1) {
        return;
    }
    const uint16_t buttons = (uint16_t)MEM_HU(0x20, gs);
    uint16_t kept = buttons;
    const bool zoomed_out = boot_float(rdram, state + 0x8) == 0.0f;
    // back at the barrels (zoomed out, and no lobby screen: gs+0x3F and +0x40 0; they're 4 and 5 in the lobby, as it comes back from a
    // match): a co-op start asked for before is off (0.10.55: not before, so the lobby the stage comes back to is co-op's still)
    if (zoomed_out && MEM_B(0x3F, gs) == 0 && MEM_B(0x40, gs) == 0 && !coop_pending_frame_set) {
        if (coop_pending) {
            conker::netsession::log("[coop] back at the barrels: the co-op lobby is left");
        }
        coop_pending = false;
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
extern "C" void func_1505E650(uint8_t* rdram, recomp_context* ctx);

namespace {
    uint32_t coop_modes[4] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
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

// 0.10.85: co-op, R-Look with no weapon out: walk while looking, as in the multiplayer games. func_1504CB98 (Conker on the ground)
// moves him with the C-buttons at 0x1504DC84: in a multiplayer game (D_800BE616 set) always, in the one-player game only with the
// shotgun (weapon mode 0x3B, or 0x9 and 0x16). In a multiplayer game with no weapon (mode 0) the look's C-Left and C-Right strafe him
// (look_aim.cpp's no_weapon_walk gives the game those from the left stick, Aiming: Swap Sticks, and sets the way he goes from the
// view). The co-op stage is the one-player game's, so there he stood still. Here, before the test (0x1504DC88, $t6 D_800BE616 as
// read, $s0 Conker, $t0 his control block), a player's Conker with no weapon in the look (+0x198 of the control block set) reads it
// as set. Only the game's own memory, the same on every machine.
extern "C" void conker_coop_look_strafe(uint8_t* rdram, recomp_context* ctx) {
    if (ctx->r14 != 0 || !conker::netsession::active() || !conker::netmenu::coop_running(rdram) || coop_players < 2) {
        return;
    }
    const uint32_t object = (uint32_t)ctx->r16;
    if (object < 0x800CC2D0u || (object - 0x800CC2D0u) % 0x32Cu != 0 || (object - 0x800CC2D0u) / 0x32Cu >= (uint32_t)std::min(coop_players, 4)) {
        return;
    }
    const uint32_t control = (uint32_t)ctx->r8;
    if (control != (uint32_t)MEM_W(0x31C, (gpr)(int32_t)object) || (control >> 24) != 0x80 || (control & 0x7FFFFFu) >= 0x7FFF00u) {
        return;
    }
    if (MEM_BU(0x78, (gpr)(int32_t)control) != 0 || MEM_BU(0x198, (gpr)(int32_t)control) == 0) {
        return;
    }
    ctx->r14 = 1;
    static int logged = 0;
    if (logged < 10) {
        logged++;
        char line[120];
        std::snprintf(line, sizeof(line), "[coop look] f=%u player %d: no weapon in the look, the C-buttons walk him",
            (unsigned)conker::netsession::frame(), (int)((object - 0x800CC2D0u) / 0x32Cu) + 1);
        conker::netsession::log(line);
    }
}

// 0.10.86: co-op, the normal camera nearer Conker (530 across and 400 up, the farthest of the game's distances: far for the stage).
// func_15124B18 sets a follow camera's distance from the game's table (D_800A34B0, by +0x1B4 of the camera): across (+0x374) and
// the height (+0x348, +0x34C), then returns (0x15124C30, $a0 the camera). There, for a player's camera in co-op whose Conker has no
// weapon out (his control block's +0x78 0), they're taken to coop_camera_share of the table's. The look (R), the shotgun's camera and
// the rest set theirs elsewhere and are left as they are. From the game's memory, the same on every machine.
namespace {
    constexpr float coop_camera_share = 0.65f;
}

// (0.10.87) the 0.10.86 log: putting the shotgun away, the hook ran once with the shotgun's camera still on (mode 3, 247 across: 161
// for a frame); and the cameras set while the stage loads (coop_running not yet) kept 530 until R or the shotgun set them again
// (players 2-4 who did neither). So only a camera in its normal mode (+0x0 0, +0x2C's 0x80 clear: not R held) whose distance is
// still the table's own for its preset is brought in, here and once a frame (conker_coop_frame) for every player's camera.
namespace {
    float coop_read_float(uint8_t* rdram, gpr base, int offset) {
        float value;
        const uint32_t bits = (uint32_t)MEM_W(offset, base);
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    void coop_write_float(uint8_t* rdram, gpr base, int offset, float value) {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(bits));
        MEM_W(offset, base) = (int32_t)bits;
    }
    void coop_camera_near(uint8_t* rdram, gpr camera) {
        if (MEM_HU(0x0, camera) != 0 || (MEM_W(0x2C, camera) & 0x80) != 0) {
            return;
        }
        const uint32_t object = (uint32_t)MEM_W(0x3D0, camera);
        if (object < 0x800CC2D0u || (object - 0x800CC2D0u) % 0x32Cu != 0 || (object - 0x800CC2D0u) / 0x32Cu >= (uint32_t)std::min(coop_players, 4)) {
            return;
        }
        const uint32_t control = (uint32_t)MEM_W(0x31C, (gpr)(int32_t)object);
        if ((control >> 24) != 0x80 || (control & 0x7FFFFFu) >= 0x7FFF00u || MEM_BU(0x78, (gpr)(int32_t)control) != 0) {
            return;
        }
        const int preset = (int)MEM_H(0x1B4, camera);
        if (preset < 0 || preset > 8) {
            return;
        }
        const gpr entry = (gpr)(int32_t)(0x800A34B0u + (uint32_t)preset * 8u);
        const float across = coop_read_float(rdram, entry, 0), up = coop_read_float(rdram, entry, 4);
        if (coop_read_float(rdram, camera, 0x374) != across || !(across > 0.0f)) {
            return;   // (already brought in, or a distance of the game's own that isn't the table's)
        }
        coop_write_float(rdram, camera, 0x374, across * coop_camera_share);
        coop_write_float(rdram, camera, 0x348, up * coop_camera_share);
        coop_write_float(rdram, camera, 0x34C, up * coop_camera_share);
        static int logged = 0;
        if (logged < 16) {
            logged++;
            char line[140];
            std::snprintf(line, sizeof(line), "[coop camera] f=%u player %d: the normal camera's distance %.0f -> %.0f (preset %d)", (unsigned)conker::netsession::frame(),
                (int)((object - 0x800CC2D0u) / 0x32Cu) + 1, across, across * coop_camera_share, preset);
            conker::netsession::log(line);
        }
    }
}

extern "C" void conker_coop_camera_distance(uint8_t* rdram, recomp_context* ctx) {
    if (!conker::netsession::active() || !conker::netmenu::coop_running(rdram) || coop_players < 2) {
        return;
    }
    coop_camera_near(rdram, ctx->r4);
}

extern "C" void conker_coop_frame(uint8_t* rdram, recomp_context* ctx) {
    coop_frames.fetch_add(1);
    if (!conker::netsession::active() || !conker::netmenu::coop_running(rdram)) {
        coop_last.clear();
        for (int k = 0; k < 4; k++) {
            coop_modes[k] = 0xFFFFFFFFu;
        }
        coop_control_last.clear();
        coop_dump_at = 0;
        coop_dumped = false;
        return;
    }
    // (0.10.87) every player's normal camera nearer (coop_camera_near): the cameras, D_800DBFF0's four (0x9A0 each)
    {
        const uint32_t cameras = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800DBFF0);
        if ((cameras >> 24) == 0x80 && (cameras & 0x7FFFFFu) + 4u * 0x9A0u <= 0x800000u) {
            for (uint32_t i = 0; i < 4; i++) {
                coop_camera_near(rdram, (gpr)(int32_t)(cameras + i * 0x9A0u));
            }
        }
    }
    // (0.10.80, measuring, to take Conker's idle animations out) each player's character's animation (+0x84) as it changes, with how
    // many frames the player's controller had been still; the idle's animations are the ones that start after a long stillness
    {
        static uint16_t last_anim[4] = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };
        static uint32_t still_since[4] = { 0, 0, 0, 0 };
        static uint32_t last_input[4][3] = {};
        static int anim_logs = 0;
        const int count = std::min(std::max(coop_players, 1), 4);
        const uint32_t now = conker::netsession::frame();
        for (int k = 0; k < count; k++) {
            const uint32_t object = 0x800CC2D0u + (uint32_t)k * 0x32Cu;
            // the player's controller as the game has it this frame (D_800BE930 the buttons, D_800BE918's +2 and +3 the stick)
            const uint32_t input[3] = { (uint32_t)(uint16_t)MEM_HU(0, (gpr)(int32_t)(0x800BE930u + 2u * (uint32_t)k)),
                (uint32_t)(uint8_t)MEM_BU(2, (gpr)(int32_t)(0x800BE918u + 6u * (uint32_t)k)), (uint32_t)(uint8_t)MEM_BU(3, (gpr)(int32_t)(0x800BE918u + 6u * (uint32_t)k)) };
            const bool still = input[0] == 0 && (int8_t)input[1] > -8 && (int8_t)input[1] < 8 && (int8_t)input[2] > -8 && (int8_t)input[2] < 8;
            if (!still || std::memcmp(input, last_input[k], sizeof(input)) != 0) {
                if (!still) {
                    still_since[k] = now;
                }
                std::memcpy(last_input[k], input, sizeof(input));
            }
            const uint16_t anim = (uint16_t)MEM_HU(0x84, (gpr)(int32_t)object);
            if (anim != last_anim[k]) {
                if (last_anim[k] != 0xFFFF && anim_logs < 600) {
                    anim_logs++;
                    char line[160];
                    std::snprintf(line, sizeof(line), "[coop anim] f=%u player %d: animation %04X -> %04X, still for %u frames", (unsigned)now, k + 1,
                        (unsigned)last_anim[k], (unsigned)anim, (unsigned)(now - still_since[k]));
                    conker::netsession::log(line);
                }
                last_anim[k] = anim;
            }
        }
    }
    // (0.10.81) no idle for the players' Conkers: the 0.10.80 log's animations that a still Conker went through on his own (from
    // standing, 0x0F: his fidgets, the Gameboy 0x171-0x17A, the yawns and the sleep 0x87-0x8A...) are taken back to standing as they
    // start, while his controller is still (a player who moves or is hit plays as the game has it). From the game's own state (the
    // animation, the game's copy of the controller) at the same point of every machine's frame, as the game itself does it
    // (func_1505E650(character, 0x0F, 1.0, 6.0, 0, 0, 0), as func_1506B268 does).
    // (0.10.82) and only the idle itself: the animation must come straight from standing (0x0F, 0x0D, 0x0E: the game's idle starts
    // from there) after the controller has been still for 10 s (300 frames; the game's own idle began after about 20 s in the log), so
    // nothing in play is touched (a hit, a fall, a shot while standing). The frames still are counted from the game's own copy of
    // the controller in the game's frame, and start over with the hooks' state (a resync: on every machine at its frame) and with a
    // scene's load, so every machine counts the same.
    // (0.10.83) the 0.10.82 log: players 2-4 still idled (0x55-0x57, 0x11/0x26/0x29, 0x47/0x49/0x81, 0x19F, the Gameboy 0x171 at
    // only 250 frames still after the stage's load) as the list missed most of the idle's animations. So now any animation that a
    // still Conker starts from standing is the idle's, whatever its number: it's taken back to standing at once (the chain that
    // follows never starts), after his controller has been still for 1.5 s (45 frames), while his health hasn't changed for 2 s
    // and he isn't down (control block +0x120 0): a hit, a death or anything the game does to a player is left as the game has it.
    static uint32_t still_frames[4] = { 0, 0, 0, 0 };
    static uint32_t calm_frames[4] = { 0, 0, 0, 0 };
    static uint8_t last_health[4] = { 0, 0, 0, 0 };
    static uint16_t previous_anim[4] = { 0, 0, 0, 0 };
    static uint32_t still_generation = 0xFFFFFFFFu, still_scene = 0xFFFFFFFFu;
    if (still_generation != conker::netsession::hook_generation() || still_scene != coop_loaded_at.load()) {
        still_generation = conker::netsession::hook_generation();
        still_scene = coop_loaded_at.load();
        for (int k = 0; k < 4; k++) {
            still_frames[k] = 0;
            calm_frames[k] = 0;
            last_health[k] = 0;
            previous_anim[k] = 0;
        }
    }
    {
        const int count = std::min(std::max(coop_players, 1), 4);
        for (int k = 0; k < count; k++) {
            const uint32_t object = 0x800CC2D0u + (uint32_t)k * 0x32Cu;
            const uint16_t anim = (uint16_t)MEM_HU(0x84, (gpr)(int32_t)object);
            const uint16_t before = previous_anim[k];
            previous_anim[k] = anim;
            const uint16_t buttons = (uint16_t)MEM_HU(0, (gpr)(int32_t)(0x800BE930u + 2u * (uint32_t)k));
            const int8_t stick_x = (int8_t)MEM_BU(2, (gpr)(int32_t)(0x800BE918u + 6u * (uint32_t)k));
            const int8_t stick_y = (int8_t)MEM_BU(3, (gpr)(int32_t)(0x800BE918u + 6u * (uint32_t)k));
            const bool still = buttons == 0 && stick_x > -8 && stick_x < 8 && stick_y > -8 && stick_y < 8;
            still_frames[k] = still ? still_frames[k] + 1 : 0;
            const uint8_t health = (uint8_t)MEM_BU(0x1CA, (gpr)(int32_t)object);
            calm_frames[k] = (health == last_health[k]) ? calm_frames[k] + 1 : 0;
            last_health[k] = health;
            const auto standing = [](uint16_t a) { return a == 0x0F || a == 0x0D || a == 0x0E; };
            if (!still || still_frames[k] < 45 || calm_frames[k] < 60 || !standing(before) || standing(anim)) {
                continue;
            }
            if ((int32_t)MEM_W(0, (gpr)(int32_t)object) == 0 || health == 0) {
                continue;
            }
            const uint32_t control = (uint32_t)MEM_W(0x31C, (gpr)(int32_t)object);
            if ((control >> 24) != 0x80 || (control & 0x7FFFFFu) >= 0x7FFF00u || (uint8_t)MEM_BU(0x120, (gpr)(int32_t)control) != 0) {
                continue;
            }
            const recomp_context saved = *ctx;
            ctx->r29 = (gpr)(int32_t)0x80F4C000;   // a stack of ours (local_audio.cpp's, not in use while this runs)
            ctx->r4 = (gpr)(int32_t)object;
            ctx->r5 = 0x0F;   // standing
            ctx->r6 = (gpr)(int32_t)0x3F800000;   // 1.0
            ctx->r7 = (gpr)(int32_t)0x40C00000;   // 6.0
            MEM_W(0x10, ctx->r29) = 0;
            MEM_W(0x14, ctx->r29) = 0;
            MEM_W(0x18, ctx->r29) = 0;
            func_1505E650(rdram, ctx);
            *ctx = saved;
            previous_anim[k] = (uint16_t)MEM_HU(0x84, (gpr)(int32_t)object);
            static int logged = 0;
            if (logged < 40) {
                logged++;
                char line[140];
                std::snprintf(line, sizeof(line), "[coop anim] f=%u player %d: the idle's animation %04X taken back to standing %04X (still %u frames)",
                    (unsigned)conker::netsession::frame(), k + 1, (unsigned)anim, (unsigned)before, (unsigned)still_frames[k]);
                conker::netsession::log(line);
            }
        }
    }
    // (0.10.32) each player's weapon mode (+0x78 of the character's +0x31C, func_15063570's 0x3B the shotgun), logged as it changes;
    // and should the first player's be the shotgun's with another player's still none (handed out before conker_coop_weapon could
    // see it), that player is given it too, once a stage (every machine at the same frame: the game's state)
    // (0.10.51) The mode is a byte: read as a word it was 0x3B000000, never 0x3B, so nobody else was ever given it (and the stage hands
    // player 1 the shotgun some way conker_coop_weapon's hook doesn't see). (0.10.53: see below, the weapon each character has.)
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
        // (0.10.53) Not drawn for them any more (0.10.51-52 drew it with func_15063570 every 30 frames, and the game put it away each
        // time: the loop of equipping and unequipping). What a character has is +0x7C of its control block (the mode +0x78 is what's
        // out): the stage sets player 1's to the shotgun's 0x3B as it starts, the others' stay 0, so their button takes out the frying
        // pan (and a shotgun drawn for them is put away at once). So the first player's +0x7C is every player's: their button draws the
        // shotgun as player 1's does. Every machine at the same frame (the game's state).
        {
            const uint32_t first = (uint32_t)MEM_W(0x31C, (gpr)(int32_t)0x800CC2D0u);
            if ((first >> 24) == 0x80 && (first & 0x7FFFFFu) < 0x7FFF00u) {
                const uint8_t weapon = (uint8_t)MEM_BU(0x7C, (gpr)(int32_t)first);
                for (int k = 1; k < count; k++) {
                    const uint32_t control = (uint32_t)MEM_W(0x31C, (gpr)(int32_t)(0x800CC2D0u + (uint32_t)k * 0x32Cu));
                    if ((control >> 24) != 0x80 || (control & 0x7FFFFFu) >= 0x7FFF00u) {
                        continue;
                    }
                    const uint8_t had = (uint8_t)MEM_BU(0x7C, (gpr)(int32_t)control);
                    if (had != weapon) {
                        MEM_B(0x7C, (gpr)(int32_t)control) = (int8_t)weapon;
                        char line[112];
                        std::snprintf(line, sizeof(line), "[coop] f=%u player %d's weapon %02X: player 1's, %02X", (unsigned)conker::netsession::frame(), k + 1,
                            (unsigned)had, (unsigned)weapon);
                        conker::netsession::log(line);
                    }
                }
            }
        }
    }
    // (0.10.59) The zombies after every player. The one-player game's characters keep who they're after in +0x222 (an index into
    // gObjects, 0 being player 1's Conker), which their AI commands read (func_15078900, func_1507839C...), and nothing in the stage ever
    // sets it for them, so it stays 0: player 1. Every 30 frames (counted in this hook, on every machine alike), each other character in
    // use (gObjects 4-24, +0 not 0) after a player is turned to the nearest player who's alive (+0 not 0, health +0x1CA not 0), by
    // where they stand (+0x14, +0x1C), and kept on the one it has unless another is clearly nearer (by a sixth) or its own is down.
    // (0.10.56's log of them read +0 as a byte, the top of a word, so it showed none.)
    if (coop_frames.load() % 30 == 0) {
        const int count = std::min(std::max(coop_players, 1), 4);
        static int switches_logged = 0;
        for (int i = count; i < 25; i++) {
            const uint32_t object = 0x800CC2D0u + (uint32_t)i * 0x32Cu;
            if (MEM_W(0, (gpr)(int32_t)object) == 0) {
                continue;
            }
            const int target = (int)(uint8_t)MEM_BU(0x222, (gpr)(int32_t)object);
            if (target >= count) {
                continue;   // (after something else)
            }
            const auto where = [&](uint32_t base, float* x, float* z) {
                const uint32_t xw = (uint32_t)MEM_W(0x14, (gpr)(int32_t)base), zw = (uint32_t)MEM_W(0x1C, (gpr)(int32_t)base);
                std::memcpy(x, &xw, 4);
                std::memcpy(z, &zw, 4);
            };
            float ox, oz;
            where(object, &ox, &oz);
            int best = -1;
            float best_d = 0.0f, target_d = -1.0f;
            for (int k = 0; k < count; k++) {
                const uint32_t player = 0x800CC2D0u + (uint32_t)k * 0x32Cu;
                if (MEM_W(0, (gpr)(int32_t)player) == 0 || MEM_BU(0x1CA, (gpr)(int32_t)player) == 0) {
                    continue;
                }
                float px, pz;
                where(player, &px, &pz);
                const float d = (px - ox) * (px - ox) + (pz - oz) * (pz - oz);
                if (k == target) {
                    target_d = d;
                }
                if (best < 0 || d < best_d) {
                    best = k;
                    best_d = d;
                }
            }
            if (best < 0 || best == target || (target_d >= 0.0f && best_d > target_d * 0.7f)) {
                continue;
            }
            MEM_B(0x222, (gpr)(int32_t)object) = (int8_t)best;
            if (switches_logged < 200) {
                switches_logged++;
                char line[128];
                std::snprintf(line, sizeof(line), "[coop chase] f=%u character %d (type %02X) after player %d now, not player %d",
                    (unsigned)conker::netsession::frame(), i, (unsigned)MEM_BU(4, (gpr)(int32_t)object), best + 1, target + 1);
                conker::netsession::log(line);
            }
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

// 0.10.59: the zombies' "near the player" test (func_150779D4: with its argument D_800D1892 0 it measures to player 1's Conker,
// gObjects[0], whoever the character is after; with 1, to the one it's after, +0x222). In co-op, for a character that isn't a player's,
// it's made 1 for the test (so it's the one it's after, which the frame hook above keeps the nearest player), and put back before the
// command it may go on to (func_15075400) and at the end. Every machine alike.
namespace {
    bool coop_near_test_changed = false;
}

extern "C" void conker_coop_near_test(uint8_t* rdram, recomp_context* ctx, int where) {
    (void)ctx;
    if (where != 0) {
        if (coop_near_test_changed) {
            coop_near_test_changed = false;
            MEM_B(0, (gpr)(int32_t)0x800D1892) = 0;
        }
        return;
    }
    coop_near_test_changed = false;
    if (!conker::netsession::active() || !coop_started || coop_players < 2) {
        return;
    }
    const int current = (int)(int8_t)MEM_B(0, (gpr)(int32_t)0x800C3E78);   // gCurrentObjectIndex
    if (current < coop_players || MEM_BU(0, (gpr)(int32_t)0x800D1892) != 0) {
        return;
    }
    MEM_B(0, (gpr)(int32_t)0x800D1892) = 1;
    coop_near_test_changed = true;
}

// 0.10.60: co-op, a HUD of their own for players 2 to 4 (health and the rest), as player 1's. The one-player game's HUD (its module at
// 0x1508F060-0x15093xxx) is for one Conker: its state is a block of the game's memory (D_800D23D0-D_800D28A0: what it shows, its
// timers, its matrices), its update (func_1508F0A4, from func_15019414, every frame of the one-player game) and its drawing
// (func_1508FD38, from func_150198FC, the full screen's pass) read player 1's character (gObjects[0], its health +0x1CA...).
// So for each other player, every frame, on every machine alike (all of it the game's own state, so the machines stay the same):
// their own copy of the HUD's block is put in its place and their character swapped into gObjects[0]'s place (and back after), and
// the update run, then the drawing (each one's commands after player 1's, in the same pass). Each machine's view then shows its own
// player's (net_view.cpp, with the HUD ranges: coop_hud_range).
extern "C" void func_1508F0A4(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_1508FD38(uint8_t* rdram, recomp_context* ctx);

namespace {
    constexpr uint32_t hud_block = 0x800D23D0, hud_block_end = 0x800D28A0;
    constexpr uint32_t hud_list_head = 0x800D245C;
    constexpr uint32_t hud_mirror_start = 0x80FA0000;   // (0.10.61) players 2-4's blocks as drawn, 0x800 each, two each   // the HUD's list of things it has allocated (its pieces flying off), in the block
    std::vector<uint8_t> hud_copies[4];
    uint32_t hud_copies_for = 0xFFFFFFFFu;   // the scene load (coop_loaded_at) the copies were made at
    uint32_t hud_ranges[4][2] = {};          // this frame's drawing for each player: [start, end) in the display list
    int hud_ranges_frame_players = 0;

    void hud_swap_block(uint8_t* rdram, std::vector<uint8_t>& copy) {
        for (uint32_t i = 0; i < hud_block_end - hud_block; i++) {
            const gpr address = (gpr)(int32_t)(hud_block + i);
            const uint8_t was = (uint8_t)MEM_BU(0, address);
            MEM_B(0, address) = (int8_t)copy[i];
            copy[i] = was;
        }
    }

    void hud_swap_characters(uint8_t* rdram, int k) {
        const uint32_t a = 0x800CC2D0u, b = 0x800CC2D0u + (uint32_t)k * 0x32Cu;
        for (uint32_t i = 0; i < 0x32Cu; i += 4) {
            const int32_t x = MEM_W(i, (gpr)(int32_t)a), y = MEM_W(i, (gpr)(int32_t)b);
            MEM_W(i, (gpr)(int32_t)a) = y;
            MEM_W(i, (gpr)(int32_t)b) = x;
        }
    }

    // the HUD's state for players 2-4: made from player 1's as a stage (re)loads, without player 1's allocated pieces
    bool hud_ready(uint8_t* rdram) {
        if (!conker::netsession::active() || !conker::netmenu::coop_running(rdram) || coop_players < 2) {
            return false;
        }
        if (MEM_BU(0, (gpr)(int32_t)0x800BE616) != 0) {
            return false;   // (a multiplayer match: no one-player HUD)
        }
        const uint32_t loaded = coop_loaded_at.load();
        if (hud_copies_for != loaded) {
            hud_copies_for = loaded;
            for (int k = 1; k < 4; k++) {
                hud_copies[k].assign(hud_block_end - hud_block, 0);
                for (uint32_t i = 0; i < hud_block_end - hud_block; i++) {
                    hud_copies[k][i] = (uint8_t)MEM_BU(0, (gpr)(int32_t)(hud_block + i));
                }
                for (int i = 0; i < 4; i++) {
                    hud_copies[k][hud_list_head - hud_block + (uint32_t)i] = 0;
                }
            }
            conker::netsession::log("[coop hud] the other players' HUDs made from player 1's");
        }
        return true;
    }
}

// at func_15019414's entry (the frame's HUD update, which runs player 1's after this)
extern "C" void conker_coop_hud_update(uint8_t* rdram, recomp_context* ctx) {
    if (!hud_ready(rdram)) {
        return;
    }
    const int count = std::min(std::max(coop_players, 1), 4);
    const recomp_context saved = *ctx;
    for (int k = 1; k < count; k++) {
        hud_swap_block(rdram, hud_copies[k]);
        hud_swap_characters(rdram, k);
        func_1508F0A4(rdram, ctx);
        *ctx = saved;
        hud_swap_characters(rdram, k);
        hud_swap_block(rdram, hud_copies[k]);
    }
}

// after func_150198FC's call of func_1508FD38 (player 1's HUD drawn: $v0 the list's end), the others' drawn after it
extern "C" void conker_coop_hud_draw(uint8_t* rdram, recomp_context* ctx) {
    hud_ranges_frame_players = 0;
    if (!hud_ready(rdram)) {
        return;
    }
    const int count = std::min(std::max(coop_players, 1), 4);
    const recomp_context saved = *ctx;
    uint32_t gfx = (uint32_t)ctx->r2;
    hud_ranges[0][1] = gfx;
    for (int k = 1; k < count; k++) {
        hud_swap_block(rdram, hud_copies[k]);
        hud_swap_characters(rdram, k);
        ctx->r4 = (gpr)(int32_t)gfx;
        hud_ranges[k][0] = gfx;
        func_1508FD38(rdram, ctx);
        gfx = (uint32_t)ctx->r2;
        hud_ranges[k][1] = gfx;
        *ctx = saved;
        // (0.10.61) the list points into the HUD's block for its matrices (the chocolate bar's pieces, D_800D2590...), which is
        // player 1's again by the time the picture is drawn: so player k's block is copied to a place of its own (past the game's
        // 8 MB, one for each of the two frames in flight, D_800BE9C0) and player k's commands pointed there
        {
            const uint32_t buffer = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800BE9C0) & 1u;
            const uint32_t mirror = hud_mirror_start + ((uint32_t)k * 2u + buffer) * 0x800u;
            for (uint32_t i = 0; i < hud_block_end - hud_block; i += 4) {
                MEM_W(i, (gpr)(int32_t)mirror) = MEM_W(i, (gpr)(int32_t)hud_block);
            }
            for (uint32_t a = hud_ranges[k][0]; a + 8 <= gfx; a += 8) {
                const uint32_t w1 = (uint32_t)MEM_W(4, (gpr)(int32_t)a);
                const uint32_t physical = w1 & 0x1FFFFFFFu;
                if (physical >= (hud_block & 0x1FFFFFFFu) && physical < (hud_block_end & 0x1FFFFFFFu) && ((w1 >> 24) == 0x80 || (w1 >> 24) == 0x00)) {
                    const uint32_t moved = (mirror & 0x1FFFFFFFu) + (physical - (hud_block & 0x1FFFFFFFu));
                    MEM_W(4, (gpr)(int32_t)a) = (int32_t)((w1 & 0xE0000000u) | moved);
                }
            }
        }
        hud_swap_characters(rdram, k);
        hud_swap_block(rdram, hud_copies[k]);
    }
    ctx->r2 = (gpr)(int32_t)gfx;
    hud_ranges_frame_players = count;
    // (measuring) every 600 frames: each player's range, and its viewports (DC 080008) and scissors (ED) as drawn
    if (coop_frames.load() % 600 == 0) {
        for (int k = 1; k < count; k++) {
            char line[600];
            int n = std::snprintf(line, sizeof(line), "[coop hud] f=%u player %d's HUD %08X..%08X:", (unsigned)conker::netsession::frame(), k + 1,
                (unsigned)hud_ranges[k][0], (unsigned)hud_ranges[k][1]);
            int shown = 0;
            for (uint32_t a = hud_ranges[k][0]; a < hud_ranges[k][1] && shown < 24 && n < (int)sizeof(line) - 24; a += 8) {
                const uint32_t w0 = (uint32_t)MEM_W(0, (gpr)(int32_t)a), w1 = (uint32_t)MEM_W(4, (gpr)(int32_t)a);
                const uint32_t op = w0 >> 24;
                if (op == 0xDC || op == 0xED || op == 0xDE || op == 0xE4) {
                    n += std::snprintf(line + n, sizeof(line) - (size_t)n, " %08X %08X", (unsigned)w0, (unsigned)w1);
                    shown++;
                }
            }
            conker::netsession::log(line);
        }
    }
}

// (0.10.61) before func_150198FC's call of func_1508FD38: where player 1's HUD starts ($v0, the list so far)
extern "C" void conker_coop_hud_draw_start(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    hud_ranges[0][0] = (uint32_t)ctx->r2;
    hud_ranges_frame_players = 0;
}

// net_view.cpp, once it has used this frame's ranges (a frame whose HUD isn't drawn leaves none behind)
void conker::netmenu::coop_hud_clear() {
    hud_ranges_frame_players = 0;
}

// net_view.cpp: this frame's HUD drawing for player `k` (0-3), or false
bool conker::netmenu::coop_hud_range(int k, uint32_t* start, uint32_t* end) {
    if (k < 0 || k >= hud_ranges_frame_players || k >= 4) {
        return false;
    }
    *start = hud_ranges[k][0];
    *end = hud_ranges[k][1];
    return *end > *start;
}


// 0.10.61: co-op, the stage reloaded (every player down, or a checkpoint): func_15007A70(layout, mask, scene) loads a scene with the
// players of the mask; the one-player game's reload asks for its own (player 1 alone, one screen), so the other players were left
// watching player 1. In co-op, a load of the stage's scene gets the stage's players and layout again (as conker_coop_start set them).
extern "C" void conker_coop_scene_load(uint8_t* rdram, recomp_context* ctx) {
    if (!conker::netsession::active() || !coop_started || coop_players < 2 || coop_mask == 0) {
        return;
    }
    const uint32_t scene = (uint32_t)ctx->r6;
    const uint32_t want_scene = (uint32_t)(uint16_t)MEM_HU(8, (gpr)(int32_t)(0x80087430u + (uint32_t)coop_stage * 20u));
    const bool restart = coop_restart_pending;
    coop_restart_pending = false;
    {
        char seen[160];
        std::snprintf(seen, sizeof(seen), "[coop] f=%u a scene load: scene %X (the stage's %X), mask %X, layout %u, a co-op restart %d",
            (unsigned)conker::netsession::frame(), (unsigned)scene, (unsigned)want_scene, (unsigned)(uint32_t)ctx->r5, (unsigned)(uint32_t)ctx->r4, (int)restart);
        conker::netsession::log(seen);
    }
    if (scene != want_scene && !restart) {
        return;
    }
    // (0.10.63) every load of the stage's scene, not only one asked with other players: the counts are put back as well (0.10.60's
    // reload had player 1 alone, whichever of them it was)
    char line[240];
    std::snprintf(line, sizeof(line), "[coop] f=%u the stage loads with mask %X, layout %u, counts %d %d %d %d %d: the co-op stage's (mask %X, layout %u, %d players)",
        (unsigned)conker::netsession::frame(), (unsigned)(uint32_t)ctx->r5, (unsigned)(uint32_t)ctx->r4, (int)MEM_B(0, (gpr)(int32_t)0x8008FD88),
        (int)MEM_B(0, (gpr)(int32_t)0x8008FD8C), (int)MEM_B(0, (gpr)(int32_t)0x8008FD90), (int)MEM_B(0, (gpr)(int32_t)0x8008FD94),
        (int)MEM_B(0, (gpr)(int32_t)0x800BE618), (unsigned)coop_mask, (unsigned)coop_layout, coop_players);
    conker::netsession::log(line);
    ctx->r4 = (gpr)(int32_t)coop_layout;
    ctx->r5 = (gpr)(int32_t)coop_mask;
    MEM_W(0, (gpr)(int32_t)0x800BEA00) = (int32_t)coop_mask;
    MEM_W(0, (gpr)(int32_t)0x800BEA04) = (int32_t)coop_layout;
    for (uint32_t k = 0; k < 4; k++) {
        MEM_B(0, (gpr)(int32_t)(0x80087270u + k)) = coop_characters[k];
    }
    const int8_t players = (int8_t)coop_players;
    MEM_B(0, (gpr)(int32_t)0x8008FD88) = players;
    MEM_B(0, (gpr)(int32_t)0x8008FD8C) = players;
    MEM_B(0, (gpr)(int32_t)0x8008FD90) = players;
    MEM_B(0, (gpr)(int32_t)0x8008FD94) = players;
    MEM_B(0, (gpr)(int32_t)0x800BE618) = players;
}


// 0.10.63: co-op, a player down. func_1507C8FC runs each player's death: the character's control block +0x120 goes 2, 3, 4 (the
// fall), 5 (the iris closed round them, waiting), and +0x122 set (the player's A, or by itself on the last life) takes it on through
// 6-9 to func_1507D4F8, which in the one-player game (D_800BE616 0, as the co-op stage is) restarts the stage from its checkpoint
// (func_1501C730 mode 2). The 0.10.61 test waited at 5 for player 1's A, which nobody knew to press. In co-op: a player down waits
// in the iris while anyone is still up; once every player is down (+0x120 5 or past), player 1's continue is set by itself and the
// stage restarts for everyone (conker_coop_scene_load keeps them all). Players 2-4 never restart it themselves. The game's own
// state, every machine at the same frame.
namespace {
    bool coop_down_all(uint8_t* rdram) {
        const int count = std::min(std::max(coop_players, 1), 4);
        for (int k = 0; k < count; k++) {
            const uint32_t control = (uint32_t)MEM_W(0x31C, (gpr)(int32_t)(0x800CC2D0u + (uint32_t)k * 0x32Cu));
            if ((control >> 24) != 0x80 || (control & 0x7FFFFFu) >= 0x7FFF00u) {
                return false;
            }
            if ((uint8_t)MEM_BU(0x120, (gpr)(int32_t)control) < 5) {
                return false;
            }
        }
        return true;
    }
    uint8_t coop_down_logged[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
}

// func_1507C8FC at 0x1507CBA0: player $s2's control block's +0x122 (its continue) is read next; $s1 the character
extern "C" void conker_coop_continue(uint8_t* rdram, recomp_context* ctx) {
    if (!conker::netsession::active() || !coop_started || coop_players < 2) {
        return;
    }
    const int i = (int)(int32_t)ctx->r18;
    if (i < 0 || i >= std::min(coop_players, 4)) {
        return;
    }
    const uint32_t control = (uint32_t)MEM_W(0x31C, (gpr)(int32_t)ctx->r17);
    if ((control >> 24) != 0x80 || (control & 0x7FFFFFu) >= 0x7FFF00u) {
        return;
    }
    const uint8_t state = (uint8_t)MEM_BU(0x120, (gpr)(int32_t)control);
    const bool all = coop_down_all(rdram);
    if (state != coop_down_logged[i]) {
        coop_down_logged[i] = state;
        char line[160];
        std::snprintf(line, sizeof(line), "[coop down] f=%u player %d: state %u, continue %u, lives %d, everyone down %d", (unsigned)conker::netsession::frame(),
            i + 1, (unsigned)state, (unsigned)(uint8_t)MEM_BU(0x122, (gpr)(int32_t)control),
            (int)(int8_t)MEM_B(0, (gpr)(int32_t)(0x800D2144u + (uint32_t)i * 28u)), (int)all);
        conker::netsession::log(line);
    }
    MEM_B(0x122, (gpr)(int32_t)control) = (i == 0 && all && state >= 5) ? 1 : 0;
}

// func_1507D4F8's entry ($a0 the player): the player is out. Returns 1 to skip it (the function returns at once), always in co-op:
// the one-player game's own restart (0.10.63's) is its checkpoint's, with player 1 alone on one screen, and froze 150 frames later
// in the 0.10.63 test. (0.10.64) Instead, once everyone is down, player 1's call starts the stage again as the race lobby's Start did
// (conker_coop_start: func_151E30C4 for the table's stage, then the players' mask, the screens' layout and the counts), once; the
// load that follows is the co-op stage's (conker_coop_scene_load). Every machine at the same frame.
extern "C" int conker_coop_out(uint8_t* rdram, recomp_context* ctx) {
    if (!conker::netsession::active() || !coop_started || coop_players < 2) {
        return 0;
    }
    const int i = (int)(int16_t)(uint16_t)(uint32_t)ctx->r4;
    if (i != 0 || coop_restart_pending || !coop_down_all(rdram)) {
        return 1;
    }
    coop_restart_pending = true;
    MEM_B(0, (gpr)(int32_t)0x8008FDB4) = 1;
    MEM_H(0, (gpr)(int32_t)0x8008FDAC) = coop_stage;
    MEM_B(0, (gpr)(int32_t)0x8008FD74) = 3;
    const recomp_context saved = *ctx;
    func_151E30C4(rdram, ctx);
    *ctx = saved;
    MEM_W(0, (gpr)(int32_t)0x800BEA00) = (int32_t)coop_mask;
    MEM_W(0, (gpr)(int32_t)0x800BEA04) = (int32_t)coop_layout;
    for (uint32_t k = 0; k < 4; k++) {
        MEM_B(0, (gpr)(int32_t)(0x80087270u + k)) = coop_characters[k];
    }
    const int8_t players = (int8_t)coop_players;
    MEM_B(0, (gpr)(int32_t)0x8008FD88) = players;
    MEM_B(0, (gpr)(int32_t)0x8008FD8C) = players;
    MEM_B(0, (gpr)(int32_t)0x8008FD90) = players;
    MEM_B(0, (gpr)(int32_t)0x8008FD94) = players;
    MEM_B(0, (gpr)(int32_t)0x800BE618) = players;
    for (int k = 0; k < 4; k++) {
        coop_down_logged[k] = 0xFF;
    }
    char line[200];
    std::snprintf(line, sizeof(line), "[coop down] f=%u everyone is down: the co-op stage starts again (table %02X, scene %X, %d players, mask %X, layout %u)",
        (unsigned)conker::netsession::frame(), (unsigned)coop_stage, (unsigned)MEM_W(0, (gpr)(int32_t)0x800BE9F4), coop_players, (unsigned)coop_mask, (unsigned)coop_layout);
    conker::netsession::log(line);
    return 1;
}


// 0.10.67: co-op, the pause screen without the one-player game's HUD. The pause screen (func_151EB06C, drawn on the machine whose
// pause menu is open only) draws Conker's chocolate bar (func_150900F0), the sprites beside it and the lives count (func_150859AC,
// func_151EADFC) itself, between 0x151EB5E8 and 0x151EB81C; so 0.10.66's hiding of the HUD's own drawing left them. In co-op, with
// this machine's menu open, the drawing commands those parts emit are taken out (vertices, triangles, matrices, display list calls,
// rectangles), the state they set (colours, modes, textures) left as it is for what the pause screen draws after them. The DE and
// EF the pause screen writes at 0x151EB768 (its own setup for the rest) are kept. This machine's display list only.
namespace {
    uint32_t pause_hud_start = 0, pause_hud_middle = 0;

    void pause_hud_strip(uint8_t* rdram, uint32_t from, uint32_t to) {
        if ((from >> 24) != 0x80 || to <= from || to - from > 0x4000u) {
            return;
        }
        for (uint32_t a = from; a + 8 <= to; a += 8) {
            const uint32_t op = (uint32_t)MEM_W(0, (gpr)(int32_t)a) >> 24;
            switch (op) {
                case 0x01: case 0x05: case 0x06: case 0x07:   // vertices, triangles
                case 0xDA: case 0xD8:                          // matrices
                case 0xDE:                                     // display list calls
                case 0xE4: case 0xE5: case 0xF6:               // rectangles
                    MEM_W(0, (gpr)(int32_t)a) = 0;
                    MEM_W(4, (gpr)(int32_t)a) = 0;
                    break;
                default:
                    break;
            }
        }
    }

    bool pause_hud_wanted() {
        return coop_pause_screen();
    }
}

extern "C" void conker_coop_pause_hud(uint8_t* rdram, recomp_context* ctx, int where) {
    if (!pause_hud_wanted()) {
        pause_hud_start = pause_hud_middle = 0;
        return;
    }
    if (where == 0) {
        pause_hud_start = (uint32_t)ctx->r16;
        pause_hud_middle = 0;
    } else if (where == 1) {
        pause_hud_middle = (uint32_t)ctx->r2;
    } else if (where == 2) {
        const uint32_t end = (uint32_t)ctx->r16;
        if (pause_hud_start != 0 && pause_hud_middle >= pause_hud_start && end >= pause_hud_middle + 16u) {
            pause_hud_strip(rdram, pause_hud_start, pause_hud_middle);
            pause_hud_strip(rdram, pause_hud_middle + 16u, end);
            static int logged = 0;
            if (logged < 3) {
                logged++;
                char line[160];
                std::snprintf(line, sizeof(line), "[coop] the pause screen's chocolate and lives taken out: %08X..%08X and ..%08X",
                    (unsigned)pause_hud_start, (unsigned)pause_hud_middle, (unsigned)end);
                conker::netsession::log(line);
            }
        }
        pause_hud_start = pause_hud_middle = 0;
    }
}


// 0.10.68: co-op, the pause menu laid out as a match's. func_151DDC20 (the pause menu, run on the machine whose menu is open only)
// lays out the one-player game's two buttons (CONT and RESTART) where D_800BE616 is 0, as the co-op stage is, and a match's three
// (CONT, RESTART, QUIT) otherwise; the online menu's hooks (conker_net_pause_hook) are made for the match's: RESTART hidden, QUIT in
// its place and lit when it's picked, its confirmation QUIT with Y and N. Before the branch at 0x151DDFE4 ($t9 the flag): in co-op,
// with this machine's menu open, the match's layout. What QUIT then does is the menu's own (net_session.cpp, the quit state 7).
extern "C" void conker_coop_pause_layout(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (coop_pause_screen()) {
        ctx->r25 = 1;
    }
}

// 0.10.70: and at L_151DE00C, the buttons where the one-player game has them (the y offset at $sp+0x8C, 5; the match's -0x24 makes
// room for its score table, which co-op hasn't): below PAUSED, not beside it
extern "C" void conker_coop_pause_buttons_y(uint8_t* rdram, recomp_context* ctx) {
    if (coop_pause_screen()) {
        MEM_W(0x8C, ctx->r29) = 5;
    }
}

// 0.10.72: co-op, the pause screen's two sprites not loaded. func_151EB06C (the pause screen, on the machine whose menu is open
// only) loads two sprites for the one-player game's HUD on it (func_151ED90C: assets read in, the game's heap) the first time it's
// drawn, kept at D_80090058/5C until it closes. On that machine alone: its heap and objects went apart from the others' at once
// (the 0.10.71 log: the host opened its menu at frame 752, "the objects differ" from 766, everyone else resynced from it), and a
// resync shows as a flash of another player's picture. Before the branch at 0x151EB5B0 ($t5 the first sprite, none loaded yet):
// in co-op, with this machine's menu open, as if loaded, so nothing is; the sprites' drawing further on is skipped for want of them.
extern "C" void conker_coop_pause_sprites(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (coop_pause_screen() && ctx->r13 == 0) {
        ctx->r13 = 1;
        static int logged = 0;
        if (logged < 2) {
            logged++;
            conker::netsession::log("[coop] the pause screen's sprites left unloaded (this machine's menu)");
        }
    }
}

// 0.10.73: the pause menu's Y and N, N: func_151DDC20 at L_151DE538 reads 16 bytes of the cartridge (osPiStartDma, func_151DDB94's
// address) and checks them against two constants; if they don't match, it copies 256 bytes from a random place on the cartridge
// (random_u32, twice) to a random place in the game's memory (0x80300000 on): the game's copy protection. Here the check doesn't
// match, so every N corrupted the game's memory, on that machine alone, with two of the RNG's numbers: the co-op test's desyncs (the
// flicker was the resyncs), and the renderer's crash (a gfx task overwritten). As the check passing does: the picked button 2 back
// (L_151DE670), nothing read, nothing written.

// (0.10.76) 0.10.75 skipped func_151DDC20 in co-op while this machine's menu was closed: it runs every frame, not only for the pause
// (its start resets the game's state for a black box, D_800E0B96 among it), and every player's screen had a black box over it. It
// runs as the game has it again.
