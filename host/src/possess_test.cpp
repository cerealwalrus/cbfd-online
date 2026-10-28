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
// (possess44) Online, a match of 5 players (the lobby's Players: 5): the game is told of 4 (its controllers, slots 1 to 4), and the 5th
// player (slot 5) plays character 5, a bot to the game, through the extra camera, on every machine alike: the camera is made at the same
// frame everywhere, the character is driven by the 5th player's input as the frame has it (conker_possess_pad), and the extra camera's
// view is drawn on every machine (what it marks for the next frame's updates is the game's), its commands kept on the 5th player's
// machine alone (net_view.cpp leaves them be). None of the D-pad's tests online.
// (possess47) Character 5 is the 5th player's own, no computer player's: the match's list gets an entry for it, the 5th (-3), as the
// match is set up (conker_possess_list), the computer players after it, so the 5th player plays with or without them, and the menus
// leave room for it (mp_characters.cpp): 12 characters at most, the 5 players and 7 computer players in the war.
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
// Writes conker_possess.log next to the game (a second window of the game on the same computer, an online test: conker_possess_2.log).

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#if defined(_WIN32)
#include <share.h>
#endif

#include "recomp.h"

#include "conker.hpp"
#include "p5_icon.hpp"
#include "ultramodern/ultramodern.hpp"

// (possess44) ultramodern's (input.cpp): a controller's stick as the game gets it, from the input's x and y, the same on every machine
void convert_to_n64_range(float x, float y, int8_t& stick_x, int8_t& stick_y);

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
    constexpr int extra = 4;                         // the extra camera's slot (and player): a 5th player's
    // (possess54) the character the extra camera plays: offline character 5 (D-pad Left / Right); online the 5th player's own, wherever the
    // match's list has it (conker_possess_list: right after the players, so with 4 players character 5 as ever; with fewer, earlier, no
    // computer players needed before it). Kept by fifth_character_now from the list, the same on every machine.
    int ch5 = extra;
    constexpr uint32_t extra_record = 0x80FD0800;    // ours: the extra player's controller record (the game has 4)
    bool have_args = false;
    uint32_t camera_arg2 = 0, camera_arg3 = 0;       // func_150054C4's last two, as the game made camera 0
    bool extra_wanted = false, extra_ready = false, extra_shown = false, extra_played = false;
    bool use_game_poses = false;   // (possess38 experiment) D-pad Up while playing the extra character
    uint32_t extra_brain_flag = 1;
    uint16_t extra_pressed = 0;
    // (possess44, online) the 5th player's buttons last frame (for those pressed this frame); what this machine was told; the last frame was
    // a 5 player match's
    uint16_t online_prev_buttons = 0;
    bool online_told_playing = false, online_told_no_bot = false;
    uint32_t online_match_frames = 0;   // (the frames of this match so far: the notice of no bot to play waits a few seconds for the bots)
    bool online_was = false;

    // (possess44) online, a match of 5 players: the extra camera and character 5 are the 5th player's, on every machine alike
    bool online5() {
        return conker::netsession::active() && conker::netsession::five_players();
    }
    // the tests' hooks do nothing online but for a match of 5
    bool hooks_off() {
        return conker::netsession::active() && !conker::netsession::five_players();
    }

    // (possess47) the match's list of characters (D_8008FE44, 16 entries: a player's controller 0 to 3, -2 a computer player, -1 none;
    // the players first) and each one's team (D_800E0C00), as the menus leave them; the 5th player's own entry (conker_possess_list)
    constexpr uint32_t match_list = 0x8008FE44;
    constexpr uint32_t match_teams = 0x800E0C00;
    constexpr int8_t fifth_entry = -3;
    constexpr int most_characters = 12;   // the match's most, the 5th player's with them (the war's)
    // (possess54) where the match's list has the 5th player's own character (-1: nowhere)
    int fifth_at(uint8_t* rdram) {
        for (uint32_t i = 0; i < 16; i++) {
            if ((int8_t)MEM_B(0, (gpr)(int32_t)(match_list + i)) == fifth_entry) {
                return (int)i;
            }
        }
        return -1;
    }
    bool list_has_fifth(uint8_t* rdram) {
        return fifth_at(rdram) >= 0;
    }
    // (possess55) With fewer than 4 players in a match the 5th player plays as one of the game's own players: the camera, the views and
    // the HUD the game has tie a player's index to its character's (func_150054C4 makes camera k for character k), so its own
    // character can't come earlier than 5th through the extra camera. Its entry in the list is then a free controller's (one whose port
    // nobody plays in), right after the players, one more player on the screen (D_8008FE40), and that controller's input is the 5th
    // player's (net_session.cpp, conker::possess_test::fifth_port): the game's own player, its camera, view, HUD and pause menu. Kept in
    // ours (+0 0x80 and the controller, +1 its place in the list), the same on every machine, until the menus come back to the list
    // (fifth_unmap: the entry and the extra player taken out again, so the character select is as the players left it).
    constexpr uint32_t fifth_map = 0x80FD0A30;
    constexpr uint32_t screen_players = 0x8008FE40;   // D_8008FE40: the players
    bool fifth_mapped(uint8_t* rdram) {
        return (MEM_BU(0, (gpr)(int32_t)fifth_map) & 0x80) != 0;
    }
    void log(const char* format, ...);
    void fifth_unmap(uint8_t* rdram, const char* where) {
        if (!fifth_mapped(rdram)) {
            return;
        }
        const int port = MEM_BU(0, (gpr)(int32_t)fifth_map) & 3;
        const int place = MEM_BU(1, (gpr)(int32_t)fifth_map);
        MEM_B(0, (gpr)(int32_t)fifth_map) = 0;
        const int players = (int)(int8_t)MEM_B(0, (gpr)(int32_t)screen_players);
        if (place >= 1 && place < 16 && place < players && (int8_t)MEM_B(0, (gpr)(int32_t)(match_list + (uint32_t)place)) == (int8_t)port) {
            for (uint32_t i = (uint32_t)place; i < 15; i++) {
                MEM_B(0, (gpr)(int32_t)(match_list + i)) = MEM_B(0, (gpr)(int32_t)(match_list + i + 1));
                MEM_B(0, (gpr)(int32_t)(match_teams + i)) = MEM_B(0, (gpr)(int32_t)(match_teams + i + 1));
            }
            MEM_B(0, (gpr)(int32_t)(match_list + 15u)) = -1;
            MEM_B(0, (gpr)(int32_t)(match_teams + 15u)) = 0;
            MEM_B(0, (gpr)(int32_t)screen_players) = (int8_t)(players - 1);
            log("(online) %s: the 5th player's entry (controller %d, player %d) taken out of the list again", where, port + 1, place + 1);
        } else {
            log("(online) %s: the 5th player's entry (controller %d, player %d) not in the list as it was left: the list left as it is", where,
                port + 1, place + 1);
        }
    }
    // the character the extra camera plays, from the list (online, a match of 5 players), else character 5
    void fifth_character_now(uint8_t* rdram) {
        const int at = online5() ? fifth_at(rdram) : -1;
        ch5 = at >= 1 ? at : extra;
    }

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
    // (possess49) 7, 8 and 33 always on: the camera's update writes them by its index (func_1501B320 from func_15128CB0: its yaw and pitch
    // eased, and the direction it looks, for its view's lighting), so the extra camera's went past their 4: 7's into 8's first, 8's into
    // the controllers' timer (D_800BE6E0), 33's over D_800DCD3C
        {0x800BE6C0, 0x4, true},   // 7
        {0x800BE6D0, 0x4, true},   // 8
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
        {0x800DCD30, 0x3, true},   // 33 (possess49: always, above)
        {0x800DCDE0, 0x18, true},   // 34
    // (possess45) 35 to 50 always on: a warp's tables (func_15172CA8, func_15172D80) and the HUD's (its fades, a hit's flash of colour,
    // its screen effects: func_1517EE40, func_1517F488, func_15181D00 and the like), which the game writes while the match runs by the
    // player's index (its camera's +0x23D, the 5th player's 4), not only while a view is drawn; and 52, the HUD's D_800DDE3C (conker.toml's
    // possess45 hooks)
        {0x800DD2B0, 0x1, true},   // 35
        {0x800DD2C0, 0x1, true},   // 36
        {0x800DDD78, 0x1, true},   // 37
        {0x800DDD90, 0x3, true},   // 38
        {0x800DDD9C, 0x1, true},   // 39
        {0x800DDDA0, 0x3, true},   // 40
        {0x800DDDAC, 0x1, true},   // 41
        {0x800DDDB0, 0x4, true},   // 42
        {0x800DDDC0, 0x1, true},   // 43
        {0x800DDDC8, 0x4, true},   // 44
        {0x800DDDD8, 0x4, true},   // 45
        {0x800DDDE8, 0x8, true},   // 46
        {0x800DDE10, 0x2, true},   // 47
        {0x800DDE1C, 0x1, true},   // 48
        {0x800DDE20, 0x1, true},   // 49
        {0x800DDE28, 0x4, true},   // 50
        {0x800DF090, 0x10, false},   // 51
        {0x800DDE3C, 0x1, true},   // 52 (possess45)
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

    // (possess19: the last run's log kept as conker_possess.prev.log) (possess44: two windows of the game on one computer, an online
    // test: the log the first has open is kept from the second's writing, which takes conker_possess_2.log)
    FILE* open_log() {
        // (possess58: up to 5 windows, a 5 player test on one computer: conker_possess_3.log to _5.log for the others)
        static const char* const names[][2] = { { "conker_possess.log", "conker_possess.prev.log" }, { "conker_possess_2.log", "conker_possess_2.prev.log" },
            { "conker_possess_3.log", "conker_possess_3.prev.log" }, { "conker_possess_4.log", "conker_possess_4.prev.log" },
            { "conker_possess_5.log", "conker_possess_5.prev.log" } };
        for (const auto& name : names) {
            std::error_code ec;
            if (std::filesystem::exists(name[0], ec)) {
#if defined(_WIN32)
                FILE* probe = _fsopen(name[0], "r+", _SH_DENYWR);
                if (probe == nullptr) {
                    continue;   // another window's log
                }
                std::fclose(probe);
#endif
                std::remove(name[1]);
                std::rename(name[0], name[1]);
            }
#if defined(_WIN32)
            FILE* file = _fsopen(name[0], "w", _SH_DENYWR);
#else
            FILE* file = std::fopen(name[0], "w");
#endif
            if (file != nullptr) {
                return file;
            }
        }
        return nullptr;
    }

    void log(const char* format, ...) {
        static FILE* file = open_log();
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
        const uint32_t brain = brain_of(rdram, ch5);
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
        log("f=%u %s: controller 1 back to player 1 (from character %d on the extra camera)", (unsigned)frames, why, ch5);
    }

    // The extra character is in the match with no camera of its own (a bot; or one D-pad Right has taken from its AI this
    // frame, its camera not made yet): the extra camera may be made for it.
    bool extra_possible(uint8_t* rdram) {
        if (!have_args || !in_match(rdram, ch5) || brain_of(rdram, ch5) == 0 || word(rdram, character(ch5) + 0x318) != 0) {
            return false;
        }
        // (possess47) online, a match of 5 players: the 5th player's own character alone (the list's entry 4, conker_possess_list), never
        // a computer player
        if (online5()) {
            return list_has_fifth(rdram);
        }
        return MEM_BU(0x84, (gpr)(int32_t)brain_of(rdram, ch5)) != 0 || extra_played;
    }
}

// ---- (possess41) The weapons lying about, and who takes them ----
// A weapon on the ground is a prop (D_800DBEF4's records of 0xA0) run by func_1511DF6C, from func_15113E54 at the frame's start, for
// the props some view saw the frame before (+0x6F's 0x40, func_15113180). It spins, and the first character of the match's (gObjects 0
// to D_8008FD8C) that is alive, has moved (+0x300, func_1504A730's last run), is of the team it's for (its +0x84 by the character's
// +0x128) and within reach of it (100 across: the brain's +0x1AF / 64, and 1.4 times that with the AI flag, +0x84) takes it: the prop's
// +0x80 gets the character's +0x3B (its index + 1), and it shrinks away. The match's script (func_1508DC24, D_800D23B0's tables of the
// pickups: +0x1070 each one's state, 0 waiting; +0xEB0 the pickup each character holds) then gives it: the weapon the character wants
// (its brain's +0x75), drawn by func_1506045C. Logged as [weapons] lines: the weapons and pickups as the match starts; any pickup's
// state, any character's held pickup, and player 1's and the extra character's weapon, as they change; and, while player 1 or the
// extra character is within 200 of a weapon, each check func_1511DF6C makes of it (every 10 frames).
namespace {
    constexpr uint32_t weapon_behaviour = 0x1511DF6C;
    constexpr uint32_t props_count = 0x800DBEF0, props_list = 0x800DBEF4, prop_size = 0xA0;
    constexpr uint32_t pickups_block = 0x800D23B0;
    constexpr int pickups_most = 64, props_most = 256;
    bool weapons_listed = false;
    int32_t pickup_state_was[pickups_most];
    int32_t held_was[25];
    int32_t reach_was[25];          // (possess45) each character's reach byte (its brain's +0x1AF), to log its changes
    uint32_t reach_logs = 0;
    uint32_t weapon_was[25];
    int32_t taken_was[props_most];
    uint32_t near_told[props_most][2];
    uint32_t weapons_frame = 0xFFFFFFFFu;

    float float_word(uint8_t* rdram, uint32_t address) {
        const uint32_t bits = word(rdram, address);
        float value;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    void weapons_reset() {
        weapons_listed = false;
        weapons_frame = 0xFFFFFFFFu;
        std::memset(near_told, 0, sizeof(near_told));
        std::memset(taken_was, 0, sizeof(taken_was));
    }

    void weapons_watch(uint8_t* rdram) {
        if (weapons_frame == frames) {
            return;
        }
        weapons_frame = frames;
        const uint32_t count = word(rdram, props_count), list = word(rdram, props_list);
        if (count == 0 || count > 1024 || !in_rdram(list) || !in_rdram(list + count * prop_size)) {
            return;
        }
        auto at = [](uint32_t address) { return (gpr)(int32_t)address; };
        const uint32_t block = word(rdram, pickups_block);
        const bool have_block = in_rdram(block);
        const int32_t pickups = have_block ? std::max(0, std::min((int32_t)MEM_W(0xEA4, at(block)), (int32_t)pickups_most)) : 0;
        const int32_t chars = have_block ? std::max(0, std::min((int32_t)MEM_W(0x10, at(block)), 25)) : 0;
        const uint32_t props = std::min(count, (uint32_t)props_most);
        auto weapon_of = [&](int k) -> uint32_t {
            const uint32_t brain = brain_of(rdram, k);
            return brain != 0 ? (uint32_t)MEM_BU(0x75, at(brain)) | (uint32_t)MEM_BU(0x78, at(brain)) << 8 | (uint32_t)MEM_BU(0x7C, at(brain)) << 16 : 0u;
        };
        if (!weapons_listed) {
            weapons_listed = true;
            std::string out;
            int n = 0;
            for (uint32_t i = 0; i < props; i++) {
                const uint32_t p = list + i * prop_size;
                if (word(rdram, p + 0x38) != weapon_behaviour) {
                    continue;
                }
                char piece[128];
                std::snprintf(piece, sizeof(piece), " | prop %u: id %u type %X team %d at %d %d %d", (unsigned)i, (unsigned)MEM_BU(0x72, at(p)),
                    (unsigned)MEM_HU(0x54, at(p)), (int)MEM_W(0x84, at(p)), (int)MEM_H(0x10, at(p)), (int)MEM_H(0x12, at(p)), (int)MEM_H(0x14, at(p)));
                out += piece;
                n++;
                taken_was[i] = MEM_W(0x80, at(p));
            }
            log("f=%u [weapons] %d weapons lying about (of %u props)%s", (unsigned)frames, n, (unsigned)count, out.c_str());
            if (have_block) {
                std::string pick;
                for (int32_t i = 0; i < pickups; i++) {
                    char piece[128];
                    pickup_state_was[i] = MEM_W(0x1070 + 4 * (uint32_t)i, at(block));
                    std::snprintf(piece, sizeof(piece), " | %d: id %u type %X flags %X state %d", (int)i,
                        (unsigned)MEM_BU(0xEF0 + 4 * (uint32_t)i + 3, at(block)), (unsigned)MEM_W(0x1174 + 4 * (uint32_t)i, at(block)),
                        (unsigned)MEM_W(0x11F4 + 4 * (uint32_t)i, at(block)), (int)pickup_state_was[i]);
                    pick += piece;
                }
                log("f=%u [weapons] the match's pickups (its mode %X, %d characters, %d pickups)%s", (unsigned)frames,
                    (unsigned)(uint16_t)MEM_H(0x16BC, at(block)), (int)chars, (int)pickups, pick.c_str());
            } else {
                log("f=%u [weapons] no table of the match's pickups (D_800D23B0 %08X)", (unsigned)frames, (unsigned)block);
            }
            for (int k = 0; k < 25; k++) {
                held_was[k] = have_block && k < chars ? MEM_W(0xEB0 + 4 * (uint32_t)k, at(block)) : -1;
                weapon_was[k] = weapon_of(k);
                const uint32_t brain = brain_of(rdram, k);
                reach_was[k] = brain != 0 ? (int32_t)MEM_BU(0x1AF, at(brain)) : -1;
            }
            reach_logs = 0;
            log("f=%u [weapons] player 1's weapon: wanted %02X, out %02X, has %02X; the extra character's: wanted %02X, out %02X, has %02X",
                (unsigned)frames, (unsigned)(weapon_was[0] & 0xFF), (unsigned)(weapon_was[0] >> 8 & 0xFF), (unsigned)(weapon_was[0] >> 16),
                (unsigned)(weapon_was[ch5] & 0xFF), (unsigned)(weapon_was[ch5] >> 8 & 0xFF), (unsigned)(weapon_was[ch5] >> 16));
            return;
        }
        for (int32_t i = 0; i < pickups; i++) {
            const int32_t state = MEM_W(0x1070 + 4 * (uint32_t)i, at(block));
            if (state != pickup_state_was[i]) {
                log("f=%u [weapons] pickup %d (id %u, type %X): its state %d -> %d (its holder %d)", (unsigned)frames, (int)i,
                    (unsigned)MEM_BU(0xEF0 + 4 * (uint32_t)i + 3, at(block)), (unsigned)MEM_W(0x1174 + 4 * (uint32_t)i, at(block)),
                    (int)pickup_state_was[i], (int)state, (int)MEM_W(0xF70 + 4 * (uint32_t)i, at(block)));
                pickup_state_was[i] = state;
            }
        }
        for (int k = 0; k < chars; k++) {
            const int32_t held = MEM_W(0xEB0 + 4 * (uint32_t)k, at(block));
            if (held != held_was[k]) {
                log("f=%u [weapons] character %d holds pickup %d (it had %d)", (unsigned)frames, k, (int)held, (int)held_was[k]);
                held_was[k] = held;
            }
        }
        // (possess45) each character's reach for the weapons (its brain's +0x1AF: 0x40 = 100 across; the possess44 test's extra character
        // had 0 for most of the match, whose AI changes its own), as it changes: the players' and the bots'
        for (int k = 0; k < 25; k++) {
            const uint32_t brain = brain_of(rdram, k);
            const int32_t reach = brain != 0 ? (int32_t)MEM_BU(0x1AF, at(brain)) : -1;
            if (reach != reach_was[k]) {
                // (possess46: not the played extra character's, which its AI sets to 0 every other frame and camera_frame back to 0x40)
                if (reach_logs < 150 && !(k == ch5 && extra_played)) {
                    reach_logs++;
                    log("f=%u [weapons] character %d's reach byte (brain +0x1AF) %d -> %d%s (AI %d)", (unsigned)frames, k, (int)reach_was[k], (int)reach,
                        k == ch5 && extra_played ? " (played)" : "", brain != 0 ? (int)MEM_BU(0x84, at(brain)) : -1);
                }
                reach_was[k] = reach;
            }
        }
        for (int k = 0; k < 25; k++) {
            const uint32_t now = weapon_of(k);
            if (now != weapon_was[k]) {
                if (k == 0 || k == ch5) {
                    log("f=%u [weapons] character %d's weapon: wanted %02X, out %02X, has %02X (it was %02X, %02X, %02X)%s", (unsigned)frames, k,
                        (unsigned)(now & 0xFF), (unsigned)(now >> 8 & 0xFF), (unsigned)(now >> 16), (unsigned)(weapon_was[k] & 0xFF),
                        (unsigned)(weapon_was[k] >> 8 & 0xFF), (unsigned)(weapon_was[k] >> 16), k == ch5 && extra_played ? " (played)" : "");
                }
                weapon_was[k] = now;
            }
        }
        for (uint32_t i = 0; i < props; i++) {
            const uint32_t p = list + i * prop_size;
            if (word(rdram, p + 0x38) != weapon_behaviour) {
                continue;
            }
            const int32_t by = MEM_W(0x80, at(p));
            if (by != taken_was[i]) {
                if (by != 0) {
                    log("f=%u [weapons] prop %u (id %u) taken by character %d", (unsigned)frames, (unsigned)i, (unsigned)MEM_BU(0x72, at(p)), (int)by - 1);
                }
                taken_was[i] = by;
            }
        }
        for (int which = 0; which < 2; which++) {
            const int k = which == 0 ? 0 : ch5;
            const uint32_t c = character(k);
            const uint32_t brain = brain_of(rdram, k);
            if (word(rdram, c) == 0 || brain == 0 || (which == 1 && !extra_ready)) {
                continue;
            }
            const float cx = float_word(rdram, c + 0x14), cy = float_word(rdram, c + 0x18), cz = float_word(rdram, c + 0x1C);
            for (uint32_t i = 0; i < props; i++) {
                const uint32_t p = list + i * prop_size;
                if (word(rdram, p + 0x38) != weapon_behaviour) {
                    continue;
                }
                const float dx = (float)MEM_H(0x10, at(p)) - cx, dz = (float)MEM_H(0x14, at(p)) - cz;
                const float away = std::sqrt(dx * dx + dz * dz);
                if (away > 200.0f || (near_told[i][which] != 0 && frames - near_told[i][which] < 10)) {
                    continue;
                }
                near_told[i][which] = frames;
                const bool ai = MEM_BU(0x84, at(brain)) != 0;
                const float reach = 100.0f * (float)MEM_BU(0x1AF, at(brain)) / 64.0f * (ai ? 1.4f : 1.0f);
                const uint32_t own = word(rdram, p + 0x7C);
                const int state = in_rdram(own) ? (int)MEM_HU(4, at(own)) : -1;
                log("f=%u [weapons] character %d%s by prop %u: %.0f away (its reach %.0f), at height %.0f (the prop's %d, the ceiling %.0f); moved %u, "
                    "health %u, team %u (the prop's %d), index %u, id %u, AI %u; the prop hidden %u, +0x6F %02X (0x40: run this frame), +0x70 %02X, "
                    "its state %d, +0x80 %d", (unsigned)frames, k, which == 1 && extra_played ? " (played)" : "", (unsigned)i, away, reach, cy,
                    (int)MEM_H(0x12, at(p)), float_word(rdram, c + 0x17C), (unsigned)MEM_BU(0x300, at(c)), (unsigned)MEM_BU(0x1CA, at(c)),
                    (unsigned)MEM_BU(0x128, at(c)), (int)MEM_W(0x84, at(p)), (unsigned)MEM_BU(0x127, at(c)), (unsigned)MEM_BU(0x3B, at(c)),
                    (unsigned)ai, (unsigned)MEM_BU(0x6E, at(p)), (unsigned)MEM_BU(0x6F, at(p)), (unsigned)MEM_BU(0x70, at(p)), state,
                    (int)MEM_W(0x80, at(p)));
            }
        }
    }
}

// (possess44) online, a match of 5 players: each character's controller as it's picked. Character 5 gets the 5th player's (from the frame's
// input, on every machine alike) and no other character is left with it; player 1 plays on as ever. Once a frame (character 0's): the
// 5th player's input in the extra camera's controller record and pressed buttons, the extra camera made for character 5 the first frame
// the 5th player is there (and character 5 a bot without a camera), and character 5 the bot's again should the 5th player leave.
namespace {
    void online_pad(uint8_t* rdram, recomp_context* ctx) {
        const int index = (int)(uint32_t)ctx->r17;
        const uint32_t empty = (uint32_t)ctx->r29 + 0x5C;    // func_1504A730's empty controller for the characters without one
        if (index != 0) {
            if (MEM_BU(0, (gpr)(int32_t)multiplayer) != 0) {
                weapons_watch(rdram);
            }
            if (extra_played && extra_ready && index == ch5) {
                const uint32_t record = pad_record(rdram, extra);
                if (in_rdram(record)) {
                    MEM_W(0, (gpr)(int32_t)controller_now) = (int32_t)record;
                }
                MEM_H(0, (gpr)(int32_t)shadow_of(pressed_shadow_table)) = (int16_t)extra_pressed;
                const uint32_t brain = brain_of(rdram, ch5);
                if (brain != 0) {
                    MEM_B(0x84, (gpr)(int32_t)brain) = 0;
                }
            } else if (extra_played && word(rdram, controller_now) == pad_record(rdram, extra)) {
                // a character left with the 5th player's record: an empty one (its AI's copy would write into it)
                for (uint32_t i = 0; i < 6; i++) {
                    MEM_B(i, (gpr)(int32_t)empty) = 0;
                }
                MEM_W(0, (gpr)(int32_t)controller_now) = (int32_t)empty;
            }
            return;
        }
        if (MEM_BU(0, (gpr)(int32_t)multiplayer) == 0) {
            if (extra_ready || extra_played) {
                log("f=%u (online) the match is over", (unsigned)frames);
            }
            extra_wanted = extra_ready = extra_shown = extra_played = false;
            online_prev_buttons = 0;
            online_told_playing = online_told_no_bot = false;
            online_match_frames = 0;
            weapons_reset();
            return;
        }
        frames++;
        online_match_frames++;
        fifth_character_now(rdram);   // (possess55)
        uint16_t buttons = 0;
        float x = 0.0f, y = 0.0f;
        const bool here = conker::netsession::fifth_input(&buttons, &x, &y);
        const bool mine = conker::netsession::local_slot() == extra;
        if (!here) {
            if (extra_played) {
                extra_play_stop(rdram, "(online) the 5th player is gone");
                extra_shown = false;
            }
            online_prev_buttons = 0;
            return;
        }
        if (!extra_played) {
            if (fifth_mapped(rdram)) {
                // (possess55) it plays as the game's own player: nothing of the extra camera's
                if (mine && !online_told_playing && online_match_frames > 30) {
                    online_told_playing = true;
                    log("f=%u (online) the 5th player plays as player %d, the game's own way (controller %d)", (unsigned)frames,
                        MEM_BU(1, (gpr)(int32_t)fifth_map) + 1, (MEM_BU(0, (gpr)(int32_t)fifth_map) & 3) + 1);
                }
                return;
            }
            if (!(extra_ready || extra_wanted || extra_possible(rdram))) {
                if (mine && !online_told_no_bot && online_match_frames > 150) {
                    online_told_no_bot = true;
                    // (possess47) the 5th player plays its own character, the list's entry 4 (conker_possess_list), made as the match was set up
                    const int count = (int)(int8_t)MEM_B(0, (gpr)(int32_t)0x8008FD8C);
                    if (!conker::possess_test::fifth_joined(rdram)) {
                        // (possess49) it joins on the character select, as players 2 to 4 do
                        log("f=%u (online) the 5th player hasn't joined on the character select: no character of its own in this match",
                            (unsigned)frames);
                        conker::notices::show("Player 5: press Start on the character select (the P5 column) to play the next match", true);
                    } else if (list_has_fifth(rdram)) {
                        log("f=%u (online) the 5th player's character %d isn't there to play (in the match %d, its camera %08X)", (unsigned)frames, ch5,
                            (int)in_match(rdram, ch5), (unsigned)word(rdram, character(ch5) + 0x318));
                        conker::notices::show("Player 5's character couldn't be set up in this match: it plays from the next", true);
                    } else {
                        log("f=%u (online) no character of the 5th player's in this match (%d characters): it plays from the next", (unsigned)frames,
                            count);
                        conker::notices::show("Player 5 plays from the next match", true);
                    }
                }
                return;
            }
            if (!extra_ready && !extra_wanted) {
                log("f=%u (online) the extra camera is made for character %d, the 5th player's", (unsigned)frames, ch5);
            }
            extra_wanted = extra_shown = extra_played = true;
            const uint32_t brain = brain_of(rdram, ch5);
            extra_brain_flag = brain != 0 ? MEM_BU(0x84, (gpr)(int32_t)brain) : 1;
            if (extra_brain_flag == 0) {
                extra_brain_flag = 1;
            }
            // (possess47) the 5th player's own character has no AI to go back to (the game gave it none): should the 5th player leave, it stands
            if (list_has_fifth(rdram)) {
                extra_brain_flag = 0;
            }
            online_prev_buttons = 0;
            log("f=%u (online) the 5th player plays character %d through the extra camera%s", (unsigned)frames, ch5, mine ? " (this machine's player)" : "");
            if (mine && !online_told_playing) {
                online_told_playing = true;
                conker::notices::show("You're player 5: you play the match's 5th character", true);
            }
        }
        // the 5th player's controller as controller 5: its record (the camera reads it too) and the buttons pressed this frame (not Start)
        const uint16_t held_now = (uint16_t)(buttons & ~button_start);
        int8_t sx = 0, sy = 0;
        convert_to_n64_range(x, y, sx, sy);
        // (possess46) the stick as the game makes its own controllers' (func_1501BBB8, for each of its 4): under 8 nothing, past it 7
        // less. The record had the stick as read, so one at rest a little off its middle (the possess45 test's, 2 to 4) walked the
        // character and turned its aim on its own, and full tilt was 7 past the players'.
        const auto game_stick = [](int8_t v) -> int8_t {
            const int tilt = v < 0 ? -(int)v : (int)v;
            return tilt < 8 ? (int8_t)0 : (int8_t)(v < 0 ? v + 7 : v - 7);
        };
        sx = game_stick(sx);
        sy = game_stick(sy);
        const uint32_t record = pad_record(rdram, extra);
        if (in_rdram(record)) {
            MEM_H(0, (gpr)(int32_t)record) = (int16_t)held_now;
            MEM_B(2, (gpr)(int32_t)record) = sx;
            MEM_B(3, (gpr)(int32_t)record) = sy;
            MEM_B(4, (gpr)(int32_t)record) = 0;
            MEM_B(5, (gpr)(int32_t)record) = 0;
        }
        extra_pressed = (uint16_t)(held_now & ~online_prev_buttons);
        online_prev_buttons = held_now;
    }
}

extern "C" void conker_possess_pad(uint8_t* rdram, recomp_context* ctx) {
    const bool online = conker::netsession::active();
    if (online_was && !online) {
        // (possess44) the session is over (this machine plays on offline): character 5 the bot's again, but on the 5th player's machine,
        // where controller 1 plays it on (as D-pad Right would)
        online_was = false;
        if (conker::netsession::local_slot() != extra) {
            extra_play_stop(rdram, "(online) the session is over");
            extra_shown = false;
        }
        log("f=%u (online) the session is over: offline from here", (unsigned)frames);
    }
    if (online) {
        online_was = online5();
        if (online_was) {
            online_pad(rdram, ctx);
        }
        return;
    }
    const int index = (int)(uint32_t)ctx->r17;
    const uint32_t object = (uint32_t)ctx->r16;
    const uint32_t empty = (uint32_t)ctx->r29 + 0x5C;    // func_1504A730's empty controller for the characters without one
    const bool frozen = mode != off || extra_played;     // player 1 stands while controller 1 plays another

    if (index != 0) {
        // (possess41) once a frame, past player 1's own move (its +0x300 as func_1504A730 left it), before the extra character's
        if (MEM_BU(0, (gpr)(int32_t)multiplayer) != 0) {
            weapons_watch(rdram);
        }
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
        } else if (extra_played && extra_ready && index == ch5) {
            // a second player: controller 2's record (filled from controller 1) and its slot of pressed buttons
            const uint32_t record = pad_record(rdram, extra);
            if (in_rdram(record)) {
                MEM_W(0, (gpr)(int32_t)controller_now) = (int32_t)record;
            }
            MEM_H(0, (gpr)(int32_t)shadow_of(pressed_shadow_table)) = (int16_t)extra_pressed;
            const uint32_t brain = brain_of(rdram, ch5);
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
        weapons_reset();
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

    if (extra_played && (pressed & button_dpad_up) != 0) {
        use_game_poses = !use_game_poses;
        log("f=%u D-pad Up: (experiment) the extra camera's view draws the characters with %s", (unsigned)frames,
            use_game_poses ? "the game's own poses" : "the poses it makes itself");
        conker::notices::show(use_game_poses ? "Test: the game's own poses for the 5th view (D-pad Up: its own)" : "Test: the 5th view's own poses (D-pad Up: the game's)", true);
    }
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
                log("f=%u D-pad Left: the extra camera is made for character %d, and shown", (unsigned)frames, ch5);
                conker::notices::show("Test: an extra camera on the 5th character (D-pad Left to switch view)", true);
            } else {
                log("f=%u D-pad Left: character %d isn't a bot without a camera (or camera 0 not seen made): no extra camera",
                    (unsigned)frames, ch5);
                conker::notices::show("Test: the 5th character isn't a bot: no extra camera", true);
            }
        } else {
            if (extra_played) {
                extra_play_stop(rdram, "D-pad Right");
                conker::notices::show("Test: back to you (still showing the extra camera)", true);
            } else if (extra_ready || extra_wanted || extra_possible(rdram)) {
                if (!extra_ready && !extra_wanted) {
                    log("f=%u D-pad Right: the extra camera is made for character %d", (unsigned)frames, ch5);
                }
                extra_wanted = extra_shown = extra_played = true;
                const uint32_t brain = brain_of(rdram, ch5);
                extra_brain_flag = brain != 0 ? MEM_BU(0x84, (gpr)(int32_t)brain) : 1;
                if (extra_brain_flag == 0) {
                    extra_brain_flag = 1;
                }
                log("f=%u D-pad Right: controller 1 plays character %d through the extra camera", (unsigned)frames, ch5);
                conker::notices::show("Test: you're playing the 5th character on the extra camera (D-pad Right to stop)", true);
            } else {
                log("f=%u D-pad Right: character %d isn't a bot without a camera: nothing to play", (unsigned)frames, ch5);
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
    if (extra_played && !conker::netsession::active() && (uint32_t)ctx->r16 == character(ch5)) {
        static uint32_t last = 0;
        if (frames - last >= 30) {
            last = frames;
            float speed, way_bits;
            const uint32_t sb = (uint32_t)MEM_W(0x1B4, ctx->r29);
            std::memcpy(&speed, &sb, 4);
            (void)way_bits;
            log("f=%u [walk] character %d walks: brain+0x84 %d, stick's speed %.2f way %04X, state %02X", (unsigned)frames, ch5,
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
    if (!extra_played || conker::netsession::active() || (uint32_t)ctx->r16 != character(ch5)) {
        return;
    }
    static uint32_t last = 0;
    if (frames - last < 30) {
        return;
    }
    last = frames;
    const uint32_t pad = word(rdram, controller_now);
    const uint32_t brain = brain_of(rdram, ch5);
    log("f=%u [probe] character %d: controller %08X (ours %08X) buttons %04X stick %d,%d; brain+0x84 %d; state %02X; +0x104 %d; "
        "players %d, last player %d; camera %08X",
        (unsigned)frames, ch5, (unsigned)pad, (unsigned)extra_record, in_rdram(pad) ? (unsigned)MEM_HU(0, (gpr)(int32_t)pad) : 0u,
        in_rdram(pad) ? (int)MEM_B(2, (gpr)(int32_t)pad) : 0, in_rdram(pad) ? (int)MEM_B(3, (gpr)(int32_t)pad) : 0,
        brain != 0 ? (int)MEM_BU(0x84, (gpr)(int32_t)brain) : -1, (unsigned)MEM_BU(0x4, ctx->r16), (int)MEM_BU(0x104, ctx->r16),
        (int)MEM_B(0, (gpr)(int32_t)0x8008FD8C), (int)MEM_W(0, (gpr)(int32_t)last_player), (unsigned)word(rdram, character(ch5) + 0x318));
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
extern "C" void guMtxL2F(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_150A7A48(uint8_t* rdram, recomp_context* ctx);

// (possess49) The extra camera's view matrix by its view's projection, as the game makes its views' each frame before the cameras run
// (func_1510B690, views 0 to D_80082FA0: D_800D9D10[k] = the camera's view matrix, D_800DC2A0[buffer] + 0x40 k, by the view's
// projection, +0x100 + 0x40 buffer of its record, both as floats), in slot 4 of D_800D9D10 (ours, slot4 table 18). A camera looks for
// what stands between it and its character by where its look-at point falls in its view (func_1512AD54: func_1501A764 projects it with
// D_800D9D10): the extra camera's slot was never made, all zeros, so the point came out as not a number, the depth row (func_1512B1B8)
// was scanned from the view's left edge against a depth of -2^31, every sample counted as something in front, and the camera swung
// aside to see past it, always the same way, until the game's pull back behind the character held it about 125 degrees round. It
// showed whenever the game had the camera to itself (after R-Look, until the right stick turned it again), two frames after the look
// (its +0x5FC). The same on every machine: the game's own memory, as the extra camera runs.
namespace {
    constexpr uint32_t view_records = 0x800BE628;        // D_800BE628: the views' records, 0x180 each (room for 5)
    constexpr uint32_t view_projected = 0x800D9D10;      // D_800D9D10: each view's, 0x40 (slot4 table 18)
    constexpr uint32_t view_scratch = 0x80FD0C00;        // ours: the camera's view matrix and the projection, as floats (0x40 each)

    void extra_view_projection(uint8_t* rdram, recomp_context* ctx) {
        const uint32_t out = shadow_of(view_projected);
        const uint32_t buffer = MEM_BU(0, (gpr)(int32_t)matrix_buffer) & 1u;
        const uint32_t block = (uint32_t)MEM_W(4 * buffer, (gpr)(int32_t)matrices);
        const uint32_t records = (uint32_t)MEM_W(0, (gpr)(int32_t)view_records);
        if (out == 0 || (block >> 24) != 0x80 || (records >> 24) != 0x80) {
            return;
        }
        const recomp_context saved = *ctx;
        ctx->r4 = (gpr)(int32_t)view_scratch;
        ctx->r5 = (gpr)(int32_t)(block + 0x40u * (uint32_t)extra);
        guMtxL2F(rdram, ctx);
        *ctx = saved;
        ctx->r4 = (gpr)(int32_t)(view_scratch + 0x40u);
        ctx->r5 = (gpr)(int32_t)(records + 0x180u * (uint32_t)extra + 0x100u + 0x40u * buffer);
        guMtxL2F(rdram, ctx);
        *ctx = saved;
        ctx->r4 = (gpr)(int32_t)view_scratch;
        ctx->r5 = (gpr)(int32_t)(view_scratch + 0x40u);
        ctx->r6 = (gpr)(int32_t)out;
        func_150A7A48(rdram, ctx);
        *ctx = saved;
    }
}

// func_15005290, 0x15005318: $s0 the cameras to make blocks for (players).
extern "C" void conker_possess_cameras_alloc(uint8_t* rdram, recomp_context* ctx) {
    if (hooks_off()) {
        return;
    }
    online_prev_buttons = 0;
    online_told_playing = online_told_no_bot = false;
    online_match_frames = 0;
    if (conker::netsession::active()) {
        const int slot = conker::netsession::local_slot();
        log("(online) a match of 5 players: this machine plays in slot %d (%s)", slot + 1,
            slot == extra ? "the 5th player's: character 5 through the extra camera" : "one of the game's 4 controllers");
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
    if (!hooks_off() && (int32_t)ctx->r4 < 4) {
        ctx->r4 = 4;
    }
}

// func_150054C4, 0x150054C8 (its start, past one instruction): camera ($a0) for character $a1.
extern "C" void conker_possess_camera_args(uint8_t* rdram, recomp_context* ctx) {
    if (!hooks_off() && (int32_t)ctx->r5 == 0) {
        camera_arg2 = (uint32_t)ctx->r6;
        camera_arg3 = (uint32_t)ctx->r7;
        have_args = true;
    }
}

// func_1501A220, 0x1501A258: $s0 the views to make room for.
extern "C" void conker_possess_views_alloc(uint8_t* rdram, recomp_context* ctx) {
    if (!hooks_off() && (int32_t)ctx->r16 < 5) {
        ctx->r16 = 5;
    }
}

// func_1501A220, 0x1501A360: its views made; the rest of the 5 made full screen.
extern "C" void conker_possess_views_make(uint8_t* rdram, recomp_context* ctx) {
    if (hooks_off()) {
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
    if (hooks_off() || MEM_BU(0, (gpr)(int32_t)multiplayer) == 0 || (!extra_wanted && !extra_ready)) {
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
            log("f=%u character %d isn't a bot without a camera any more: no extra camera", (unsigned)frames, ch5);
            extra_play_stop(rdram, "no extra camera");
            extra_wanted = extra_shown = false;
            return;
        }
        // (possess49) its row of depths (what's in front, func_1512B1B8) as "nothing in front" (0xFFFC, as render_fixes.cpp's
        // conker_depth_row_neutral leaves the game's), and no further than ours has room for (extra_record follows at 0x800)
        const uint32_t width = (uint32_t)MEM_W(0, (gpr)(int32_t)frame_width);
        for (uint32_t i = 0; i + 1 < 2 * width && i < 0x800; i += 2) {
            MEM_H(i, (gpr)(int32_t)extra_buffer) = (int16_t)(uint16_t)0xFFFCu;
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
        log("f=%u the extra camera %08X made for character %d (now its camera: %08X)", (unsigned)frames, (unsigned)camera, ch5,
            (unsigned)word(rdram, character(ch5) + 0x318));
    }
    // still the extra character's (something in the match took it off: put back)
    if (word(rdram, character(ch5) + 0x318) != camera || word(rdram, camera + 0x3D0) != character(ch5)) {
        static uint32_t told = 0;
        if (told++ < 10) {
            log("f=%u the extra camera was off character %d (its camera %08X, the camera's character %08X): put back", (unsigned)frames, ch5,
                (unsigned)word(rdram, character(ch5) + 0x318), (unsigned)word(rdram, camera + 0x3D0));
        }
        set_word(rdram, character(ch5) + 0x318, camera);
        camera_follow(rdram, camera, ch5);
    }
    // (possess49) its view's projected view matrix, as the game's views have theirs made before their cameras run
    extra_view_projection(rdram, ctx);
    *ctx = saved;
    {
        static uint32_t told = 0;
        if (told < 2 || (frames % 1800) == 0) {
            told++;
            const uint32_t m = shadow_of(view_projected);
            log("f=%u the extra camera's view matrix by its projection (ours, D_800D9D10's slot 4): %08X %08X %08X %08X / %08X %08X %08X %08X",
                (unsigned)frames, (unsigned)MEM_W(0x00, (gpr)(int32_t)m), (unsigned)MEM_W(0x04, (gpr)(int32_t)m), (unsigned)MEM_W(0x08, (gpr)(int32_t)m),
                (unsigned)MEM_W(0x0C, (gpr)(int32_t)m), (unsigned)MEM_W(0x30, (gpr)(int32_t)m), (unsigned)MEM_W(0x34, (gpr)(int32_t)m),
                (unsigned)MEM_W(0x38, (gpr)(int32_t)m), (unsigned)MEM_W(0x3C, (gpr)(int32_t)m));
        }
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
    // (possess45) the 5th player's HUD state (its tables' slot 4, ours) moved on each frame as the game moves a player's: its hit's flash
    // counted down (func_1517F75C does players 0 to D_80082FA0), its fade toward its end (func_1517F448, as each view is drawn)
    if (slot4_on) {
        const int32_t step = MEM_W(0, (gpr)(int32_t)0x800BE9E4);
        const uint32_t flash = shadow_of(0x800DDE10), fade = shadow_of(0x800DDDB0), fade_end = shadow_of(0x800DDE28);
        const int32_t flash_left = (int32_t)MEM_HU(0, (gpr)(int32_t)flash);
        MEM_H(0, (gpr)(int32_t)flash) = (int16_t)(step < flash_left ? flash_left - step : 0);
        const int32_t fade_now = MEM_W(0, (gpr)(int32_t)fade);
        if (fade_now != MEM_W(0, (gpr)(int32_t)fade_end)) {
            MEM_W(0, (gpr)(int32_t)fade) = fade_now + step;
        }
    }
    // (possess45) the played extra character takes the weapons it comes to as a player's character does: its reach (the brain's +0x1AF,
    // 0x40 = 100 across, func_1511DF6C) as a player's is (func_15082A44, func_1508E89C set 0x40); a bot's AI changes its own (the
    // possess44 test: 0 for most of the match, and the M4 wasn't taken)
    if (extra_played) {
        const uint32_t brain = brain_of(rdram, ch5);
        if (brain != 0 && MEM_BU(0x1AF, (gpr)(int32_t)brain) != 0x40) {
            static uint32_t told = 0;
            if (told++ < 20) {
                log("f=%u the played extra character's reach (brain +0x1AF) was %u: 0x40, a player's", (unsigned)frames,
                    (unsigned)MEM_BU(0x1AF, (gpr)(int32_t)brain));
            }
            MEM_B(0x1AF, (gpr)(int32_t)brain) = 0x40;
        }
    }
    // (possess29 diagnostics) where the extra camera is, against the character it follows: shown, and played
    {
        static uint32_t told_shown = 0, told_played = 0;
        uint32_t& told = extra_played ? told_played : told_shown;
        if (extra_shown && (frames % 20) == 0 && told < (extra_played ? 90u : 15u)) {
            told++;
            const float ex = position(rdram, camera, 0x2EC), ey = position(rdram, camera, 0x2F0), ez = position(rdram, camera, 0x2F4);
            const float lx = position(rdram, camera, 0x2BC), ly = position(rdram, camera, 0x2C0), lz = position(rdram, camera, 0x2C4);
            const float px = position(rdram, camera, 0x2A4), py = position(rdram, camera, 0x2A8), pz = position(rdram, camera, 0x2AC);
            const float d = std::sqrt((ex - lx) * (ex - lx) + (ey - ly) * (ey - ly) + (ez - lz) * (ez - lz));
            // (possess49) and its look past what's in front (+0x3EC the swing, the look-at point as its view sees it: D_800DC1F0/F4/F8)
            log("f=%u [cam] %s: eye %.0f %.0f %.0f (wanted %.0f %.0f %.0f), looks at %.0f %.0f %.0f (%.0f away), pivot %.0f %.0f %.0f; +374 %.1f "
                "+344 %.1f; index %u; brain+197 %u, character's camera %08X; swing %.1f (+5FC %d), look-at on screen %.1f %.1f %.1f",
                (unsigned)frames, extra_played ? "played" : "shown", ex, ey, ez,
                position(rdram, camera, 0x2F8), position(rdram, camera, 0x2FC), position(rdram, camera, 0x300), lx, ly, lz, d, px, py, pz,
                position(rdram, camera, 0x374), position(rdram, camera, 0x344), (unsigned)MEM_BU(0x23D, (gpr)(int32_t)camera),
                (unsigned)MEM_BU(0x197, (gpr)(int32_t)brain_of(rdram, ch5)), (unsigned)word(rdram, character(ch5) + 0x318),
                position(rdram, camera, 0x3EC), (int)MEM_H(0x5FC, (gpr)(int32_t)camera), position(rdram, 0x800DC1F0u, 0),
                position(rdram, 0x800DC1F4u, 0), position(rdram, 0x800DC1F8u, 0));
        }
    }
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

namespace {
    // (possess37 diagnostics) the objects drawn in the extra camera's view this frame: their kind, index and commands
    struct Drawn { uint32_t kind, object, start, end; };
    std::vector<Drawn> drawn_now;
    std::vector<Drawn> game_drawn;   // (possess38) the extra character drawn by the game's own views this frame (player 1's)
    int extra_object = -1;
}

void conker::possess_test::note_drawn(uint32_t kind, uint32_t object, uint32_t start, bool extra_view) {
    std::vector<Drawn>& to = extra_view ? drawn_now : game_drawn;
    if (to.size() >= 400) {
        return;
    }
    // (the game's views: only the extra character kept; the others' as placeholders while open, so ends pair with their starts)
    const bool placeholder = !extra_view && (int)object != extra_object;
    to.push_back({ placeholder ? 0xFFFFFFFFu : kind, object, start, 0 });
}

void conker::possess_test::note_drawn_end(uint32_t end, bool extra_view) {
    std::vector<Drawn>& in = extra_view ? drawn_now : game_drawn;
    for (size_t i = in.size(); i-- > 0;) {
        if (in[i].end == 0) {
            in[i].end = end;
            if (in[i].kind == 0xFFFFFFFFu) {
                in.erase(in.begin() + (std::ptrdiff_t)i);
            }
            break;
        }
    }
}

namespace {
    uint32_t second_pass_at = 0;   // (possess61) where this frame's extra view's second pass began (conker_possess_view_local)
}

uint32_t conker::possess_test::fifth_second_pass() {
    return second_pass_at;
}

namespace {
    bool extra_view_now = false;   // (possess34) the extra camera's view being drawn
    // (possess35) the characters' fades and hidden bits in the extra camera's view (as view 1), kept from frame to frame
    uint8_t extra_fade[25] = {}, extra_hidden[25] = {};
    bool extra_fades_kept = false;
}

bool conker::possess_test::drawing_extra_view() {
    return extra_view_now;
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

    // The extra camera's view is drawn this frame (offline: D-pad Left; online, a match of 5 players: on every machine while the 5th
    // player plays, what it marks for the next frame's updates being the game's; its commands are kept where view_shown_here).
    bool local_view_wanted(uint8_t* rdram) {
        if (!extra_ready || MEM_BU(0, (gpr)(int32_t)multiplayer) == 0) {
            return false;
        }
        if (conker::netsession::active()) {
            return online5() && extra_played;
        }
        return extra_shown;
    }

    // (possess59) online, the match's players on a split screen (each machine's view is one of its parts, net_view.cpp making it full
    // screen): the extra camera's view drawn as view 1 in view 1's own part of the screen (its record's viewport and projection, not
    // the full-screen one's), on every machine alike (what it sees is kept for the next frame's updates); on the 5th player's machine its
    // HUD laid out for that part as the game lays a player's out (the split screen's), and net_view.cpp makes view 1's part this
    // machine's full screen (its field of view, the HUD's places, the reticle's), as it does on player 1's machine.
    bool fifth_split(uint8_t* rdram) {
        return conker::netsession::active() && online5() && (int32_t)MEM_W(0, (gpr)(int32_t)last_player) >= 1;
    }

    // (possess44) the extra camera's view is shown on this machine: offline whenever it's drawn, online on the 5th player's machine alone
    bool view_shown_here() {
        return !conker::netsession::active() || conker::netsession::local_slot() == extra;
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
    if (!hooks_off() && (int32_t)ctx->r4 < 5 * 0x80) {
        ctx->r4 = 5 * 0x80;
    }
}

extern "C" void conker_possess_frame_start(uint8_t* rdram);

// (possess15 diagnostics) at the start of the game's loop over its views for each stage: its memory then, to set what its own views'
// passes write against what the extra view's do.
extern "C" void conker_possess_diff_game(uint8_t* rdram, recomp_context* ctx, int stage) {
    (void)ctx;
    if (extra_view_drawn(rdram)) {
        diff_begin(rdram);
    }
    // (possess19) the game's memory before its views' passes (func_15019130, before func_1510FC34's loop): the extra camera's view is
    // drawn from it, as the game draws its own views (nothing of theirs drawn yet this frame)
    if (stage == 0) {
        conker_possess_frame_start(rdram);
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
extern "C" void func_1510F800(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_1510B690(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_15112A80(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_15113218(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_1502E474(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_151749F8(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_1502BEE4(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_150636F0(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_1515D69C(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_15167310(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_15188B74(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_1516706C(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_151671E8(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_150A0D8C(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_151738C4(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_1502C1A4(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_15113C88(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_15183D28(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_151670C0(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_15177A94(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_151814FC(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_1517F75C(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_1517F7B4(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_15036148(uint8_t* rdram, recomp_context* ctx);

namespace {
    constexpr uint32_t game_size = 0x400000;
    std::vector<uint8_t> local_copy;                       // the game's memory before the view (as the frame ended)
    std::vector<uint8_t> start_copy;                       // (possess19) and before the frame's views were drawn
    bool start_copied = false;
    struct Kept { uint32_t at, size; std::vector<uint8_t> was; };
    std::vector<Kept> kept_back;                           // put back at the next frame's start
    uint32_t local_views = 0;
    constexpr uint32_t objects_count = 0x800DBEF0;   // D_800DBEF0: the objects (0xA0 each, at *D_800DBEF4) func_15112A80 sees to
    constexpr uint32_t objects_list = 0x800DBEF4;
    constexpr uint32_t object_record = 0xA0;
    constexpr uint8_t extra_seen_bit = 0x01;         // (possess32) +0x6F: seen by the extra camera, as view 1 (possess30 had view 4's bit, 0x08: the game drew view 4 from it, which offline doesn't exist, and crashed). func_15112A80 sets view 1's bit again before anything draws by it.
    bool seen_marked = false;

    uint32_t mem_word(uint8_t* rdram, uint32_t address) {
        return (uint32_t)MEM_W(0, (gpr)(int32_t)address);
    }

    // (possess22) The game's message queues and threads (as it makes them: conker_possess_note_queue / _thread). While the view is
    // drawn messages come in to queues (the controllers', the screen's, ...) and threads are woken; the runtime keeps track of those
    // outside the game's memory, so they must not be put back: those pieces are left as they are.
    std::vector<uint32_t> known_queues, known_threads;

    // (possess42) The buffers the RDP draws into: the two framebuffers (D_8002AAE8), the depth buffer (D_800BE9C4) and the blur's copy of
    // the screen (D_800BE570), all in the game's heap. What's in them is the renderer's (RT64 keeps them as framebuffers of its own):
    // the extra camera's view never copies them as textures it draws from (the effects that draw the screen back from one of them drew,
    // from a copy, the memory's old bytes: in the wrong colours), and they're never put back from a snapshot of the game's memory.
    // (possess43) Nor written at all, not even for a moment (put_back copies around them): the renderer writes them on its own thread
    // while the game's draws the extra camera's view (see put_back).
    std::vector<std::pair<uint32_t, uint32_t>> rdp_buffers(uint8_t* rdram) {
        std::vector<std::pair<uint32_t, uint32_t>> out;
        const uint32_t width = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800BE620), height = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800BE624);
        if (width == 0 || width > 640 || height == 0 || height > 480) {
            return out;
        }
        const uint32_t size = width * height * 2u;
        for (const uint32_t pointer : { 0x8002AAE8u, 0x8002AAECu, 0x800BE9C4u, 0x800BE570u }) {
            const uint32_t named = (uint32_t)MEM_W(0, (gpr)(int32_t)pointer);
            // (possess43) as the CPU's address of it, whichever the game keeps (cached, 0x80..., or not, 0xA0...)
            const uint32_t at = (named >> 29) == 4u || (named >> 29) == 5u ? (named & 0x1FFFFFFFu) | 0x80000000u : 0;
            if (at >= 0x80000000u && at + size <= 0x80400000u) {
                out.push_back({ at, size });
            }
        }
        return out;
    }

    // The pieces of the game's memory never put back: each queue (and its messages), each thread.
    std::vector<std::pair<uint32_t, uint32_t>> untouchables(uint8_t* rdram) {
        std::vector<std::pair<uint32_t, uint32_t>> out;
        for (uint32_t q : known_queues) {
            out.push_back({ q, 24 });
            const uint32_t count = mem_word(rdram, q + 16), messages = mem_word(rdram, q + 20);
            if (count > 0 && count < 4096 && (messages >> 24) == 0x80) {
                out.push_back({ messages, count * 4 });
            }
        }
        for (uint32_t t : known_threads) {
            out.push_back({ t, 0x30 });
        }
        for (const auto& b : rdp_buffers(rdram)) {
            out.push_back(b);
        }
        return out;
    }

    // Puts `from` (a copy of the game's first 4 MB) back, but for [keep_from, keep_to) and the untouchables.
    // (possess43) Copied around what's kept, never over it. The renderer (RT64) draws each frame's list a second time at the N64's
    // size, on its own thread, and writes each picture back to the game's memory (the framebuffer and the depth buffer, at each point
    // the list makes it finish what's drawn so far, and at its end), then notes what it wrote; while it does, the game's thread is
    // drawing the next frame, the extra camera's view with it. possess42 kept the RDP's buffers as they were by saving them, copying
    // the snapshot over everything and writing them back: what the renderer wrote in between was undone (or what it read was the
    // snapshot's), and finding its buffers changed, as if by the game, it drew the memory's picture over its own, in the screen's
    // middle (the N64's part of it), in rows: an older one of the frame's pictures (player 1's view, the extra camera's half drawn) or
    // one caught as it was written (its bytes not yet in order: in the wrong colours).
    void put_back(uint8_t* rdram, const std::vector<uint8_t>& from, uint32_t keep_from, uint32_t keep_to) {
        std::vector<std::pair<uint32_t, uint32_t>> spans;   // [start, end) in the game's memory, whole words
        auto add = [&](uint32_t at, uint32_t size) {
            if (size == 0 || at < 0x80000000u || at >= 0x80000000u + game_size) {
                return;
            }
            const uint32_t start = (at - 0x80000000u) & ~3u;
            const uint64_t end = std::min<uint64_t>(((uint64_t)(at - 0x80000000u) + size + 3u) & ~3ull, game_size);
            spans.push_back({ start, (uint32_t)end });
        };
        for (const auto& k : untouchables(rdram)) {
            add(k.first, k.second);
        }
        if (keep_to > keep_from && keep_to - 0x80000000u <= game_size) {
            add(keep_from, keep_to - keep_from);
        }
        std::sort(spans.begin(), spans.end());
        uint32_t at = 0;
        for (const auto& s : spans) {
            if (s.first > at) {
                std::memcpy(rdram + at, from.data() + at, s.first - at);
            }
            at = std::max(at, s.second);
        }
        if (at < game_size) {
            std::memcpy(rdram + at, from.data() + at, game_size - at);
        }
    }

    // The display list's commands from `from` (an RDRAM address), following calls, jumps and branches: every piece of memory they
    // name (with the command that names it), each list they call (its stretch, and the command that calls it), and where each
    // segment was set. (possess24) Texture images (G_SETTIMG, sized by the load after it), G_BRANCH_Z's lists, and the lists'
    // own commands too: what the extra camera's view drew could be any of those.
    struct ListWalk {
        uint8_t* rdram;
        uint32_t seg[16] = {};
        uint32_t seg_set_at[16] = {};   // the command that set it (0: before the walk)
        struct Ref { uint32_t command, at, size; int segment; int body; };
        std::vector<Ref> refs;
        // a list called (or jumped or branched to): its stretch [at, end), the command naming it (in its parent: -1 the frame's
        // list), the parent's index
        struct Body { uint32_t at, end, command; int parent; };
        std::vector<Body> bodies;
        uint32_t commands = 0, textures = 0, branches = 0;
        std::vector<std::pair<uint32_t, uint32_t>> cimgs;   // (possess42) the color images its commands set (address, bytes)
        uint32_t timg_at = 0, timg_w1 = 0, timg_siz = 0, timg_width = 0;
        int timg_body = -1;
        uint32_t resolve(uint32_t a, int* segment) const {
            const uint32_t s = (a >> 24) & 0xFF;
            if (s < 16) {
                *segment = s == 0 ? -1 : (int)s;
                return 0x80000000u | ((seg[s] + (a & 0x00FFFFFFu)) & 0x00FFFFFFu);
            }
            *segment = -1;
            return 0x80000000u | (a & 0x00FFFFFFu);
        }
        void note(int body, uint32_t command, uint32_t w1, uint32_t size) {
            int segment = -1;
            const uint32_t at = resolve(w1, &segment);
            if (at >= 0x80000000u && at < 0x80000000u + game_size && size > 0 && size <= 0x10000) {
                refs.push_back({ command, at, std::min(size, 0x80000000u + game_size - at), segment, body });
            }
        }
        // a texture image's bytes for `texels` of its size
        uint32_t texel_bytes(uint32_t texels) const {
            return timg_siz == 0 ? (texels + 1u) / 2u : texels << (timg_siz - 1u);
        }
        void texture(uint32_t size) {
            if (timg_at == 0) {
                return;
            }
            textures++;
            if (!refs.empty() && refs.back().command == timg_at) {   // (a second load from the same image)
                refs.back().size = std::max(refs.back().size, std::min(size, 0x10000u));
                return;
            }
            note(timg_body, timg_at, timg_w1, size);
        }
        void child(uint32_t command, uint32_t w1, int parent, int depth) {
            if (depth >= 48 || bodies.size() >= 20000) {
                return;
            }
            int segment = -1;
            const uint32_t target = resolve(w1, &segment);
            const int b = (int)bodies.size();
            bodies.push_back({ target, target, command, parent });
            const uint32_t end = run(target, depth + 1, 0, b);
            bodies[b].end = std::max(end, target);
        }
        // walks from `at` (in list `body`, -1 the frame's) to its end, or `stop`; returns where it ended
        uint32_t run(uint32_t at, int depth, uint32_t stop, int body) {
            uint32_t e1_at = 0, e1_w1 = 0;
            while (commands < 400000) {
                if ((at & 7u) != 0 || at < 0x80000000u || at >= 0x80000000u + game_size || (stop != 0 && at >= stop)) {
                    return at;
                }
                const uint32_t w0 = mem_word(rdram, at), w1 = mem_word(rdram, at + 4);
                commands++;
                switch (w0 >> 24) {
                    case 0x01:   // G_VTX
                        note(body, at, w1, ((w0 >> 12) & 0xFFu) * 16u);
                        break;
                    case 0xDA:   // G_MTX
                        note(body, at, w1, 64);
                        break;
                    case 0xDC:   // G_MOVEMEM
                        note(body, at, w1, (((w0 >> 19) & 0x1Fu) + 1u) * 8u);
                        break;
                    case 0xDB:   // G_MOVEWORD: a segment
                        if (((w0 >> 16) & 0xFFu) == 0x06 && (w0 & 0xFFFFu) < 64u) {
                            seg[(w0 & 0xFFFFu) / 4u] = w1 & 0x00FFFFFFu;
                            seg_set_at[(w0 & 0xFFFFu) / 4u] = at;
                        }
                        break;
                    case 0xFD:   // G_SETTIMG: sized by the load after it
                        timg_at = at;
                        timg_w1 = w1;
                        timg_siz = (w0 >> 19) & 3u;
                        timg_width = (w0 & 0xFFFu) + 1u;
                        timg_body = body;
                        break;
                    case 0xF3:   // G_LOADBLOCK
                        texture(texel_bytes(((w1 >> 12) & 0xFFFu) + 1u));
                        break;
                    case 0xF4:   // G_LOADTILE: its rows from the image's start
                        texture((((w1 & 0xFFFu) >> 2) + 1u) * texel_bytes(timg_width));
                        break;
                    case 0xFF: {   // G_SETCIMG: a buffer the RDP draws into (a row of a narrow one: the game's little copies of pixels)
                        int segment = -1;
                        const uint32_t image = resolve(w1, &segment);
                        const uint32_t width = (w0 & 0xFFFu) + 1u, siz = (w0 >> 19) & 3u;
                        const uint32_t row = siz == 0 ? (width + 1u) / 2u : width << (siz - 1u);
                        cimgs.push_back({ image, width >= 32 ? row * 240u : row });
                        break;
                    }
                    case 0xF0:   // G_LOADTLUT
                        texture((((w1 >> 14) & 0x3FFu) + 1u) * 2u);
                        break;
                    case 0xE1:   // G_RDPHALF_1: G_BRANCH_Z's list
                        e1_at = at;
                        e1_w1 = w1;
                        break;
                    case 0x04:   // G_BRANCH_Z
                        if (e1_at != 0) {
                            branches++;
                            child(e1_at, e1_w1, body, depth);
                        }
                        e1_at = 0;
                        break;
                    case 0xDE:   // G_DL: a call (or a jump)
                        child(at, w1, body, depth);
                        if (((w0 >> 16) & 0xFFu) != 0) {
                            return at + 8;
                        }
                        break;
                    case 0xDF:   // G_ENDDL
                        return at + 8;
                    default:
                        break;
                }
                at += 8;
            }
            return at;
        }
    };

    // ours, past the game's 8 MB: copies of what the extra camera's view draws from, two halves (this frame's, the last one's, which
    // may still be being drawn)
    constexpr uint32_t copies_start = 0x80E00000;
    constexpr uint32_t copies_half = 0xE0000;
    uint32_t copies_frame = 0;

    // (the next frame's start) what the view's commands drew from, put back
    void put_kept_back(uint8_t* rdram) {
        for (const Kept& k : kept_back) {
            std::memcpy(rdram + (k.at - 0x80000000u), k.was.data(), k.size);
        }
        kept_back.clear();
    }
}

namespace {
    // (possess23) The game's other threads run alongside its own (the audio thread above all: it changes the heap too), so while the
    // game's memory is copied, drawn from and put back they're stopped: what they change then would be undone, and the heap's lists
    // with it. Each is the host thread of one of the game's (known_threads: its OSThread's context).
    struct OthersStopped {
        std::vector<void*> stopped;
        explicit OthersStopped(uint8_t* rdram);
        ~OthersStopped();
    };
}

// func_1501878C, 0x15018C8C (its frame's display list about to be closed; $s1 the address of the list's end): the view, see above.
namespace {
    // (possess42) What the extra camera's view saw last frame, kept for its next: the level's map (D_800DBE48, a tree of areas: each one's
    // +0x2 has a bit a view, the areas in its sight, set by the view's drawing of the level, func_150A5378 from func_1510B9D0, and read
    // the next frame by func_15112A80's choice of the objects to draw and run, through func_1503F800), and each object's fade in view 1
    // (+0x8B, stepped by func_15112A80). Put back to the game's (player 1's camera's) with the rest of its memory each frame, the objects
    // near the extra character were left out unless player 1's camera had them in sight too: the weapons there weren't drawn, nor run.
    std::vector<uint32_t> kept_areas;
    std::vector<uint8_t> kept_area_bits;
    uint32_t kept_areas_root = 0;
    std::vector<uint8_t> kept_object_fades;
    uint32_t kept_objects_list = 0, kept_objects_count = 0;
    uint32_t blur_left = 0, areas_told = 0;
    uint32_t renderer_wrote = 0;   // (possess43) the views drawn while the renderer wrote its buffers (see put_back)

    template <typename F> void each_area(uint8_t* rdram, F f) {
        uint32_t node = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800DBE48);
        for (int n = 0; n < 65536 && node >= 0x80000000u && node + 0x10u <= 0x80400000u; n++) {
            f(node);
            const int16_t child = MEM_H(0xC, (gpr)(int32_t)node), next = MEM_H(4, (gpr)(int32_t)node);
            if (child != 0) {
                node += (uint32_t)(int32_t)child;
            } else if (next != 0) {
                node += (uint32_t)(int32_t)next;
            } else {
                break;
            }
        }
    }

    void keep_what_was_seen(uint8_t* rdram) {
        kept_areas.clear();
        kept_area_bits.clear();
        kept_areas_root = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800DBE48);
        each_area(rdram, [&](uint32_t node) {
            kept_areas.push_back(node);
            kept_area_bits.push_back((uint8_t)(MEM_HU(2, (gpr)(int32_t)node) & 1u));
        });
        kept_objects_count = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800DBEF0);
        kept_objects_list = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800DBEF4);
        kept_object_fades.clear();
        if (kept_objects_count > 0 && kept_objects_count < 2048 && kept_objects_list >= 0x80000000u &&
            kept_objects_list + kept_objects_count * 0xA0u <= 0x80400000u) {
            for (uint32_t i = 0; i < kept_objects_count; i++) {
                kept_object_fades.push_back((uint8_t)MEM_BU(0x8B, (gpr)(int32_t)(kept_objects_list + i * 0xA0u)));
            }
        }
    }

    // the areas whose bit was put back as the extra camera's
    uint32_t put_back_what_was_seen(uint8_t* rdram) {
        uint32_t changed = 0;
        if (!kept_areas.empty() && kept_areas_root == (uint32_t)MEM_W(0, (gpr)(int32_t)0x800DBE48)) {
            for (size_t i = 0; i < kept_areas.size(); i++) {
                const gpr at = (gpr)(int32_t)kept_areas[i];
                const uint16_t was = MEM_HU(2, at);
                const uint16_t now = (uint16_t)((was & ~1u) | kept_area_bits[i]);
                if (now != was) {
                    MEM_H(2, at) = (int16_t)now;
                    changed++;
                }
            }
        }
        if (!kept_object_fades.empty() && kept_objects_count == (uint32_t)MEM_W(0, (gpr)(int32_t)0x800DBEF0) &&
            kept_objects_list == (uint32_t)MEM_W(0, (gpr)(int32_t)0x800DBEF4)) {
            for (uint32_t i = 0; i < kept_objects_count; i++) {
                MEM_B(0x8B, (gpr)(int32_t)(kept_objects_list + i * 0xA0u)) = (int8_t)kept_object_fades[i];
            }
        }
        return changed;
    }

    void forget_what_was_seen() {
        kept_areas.clear();
        kept_area_bits.clear();
        kept_object_fades.clear();
        kept_areas_root = 0;
    }
}

// (possess48) The 5th player's HUD, over its view on its machine. The game draws a multiplayer match's HUD for its players 0 to
// D_80082FA0, each in its view's rectangle: once a frame (func_150198FC: the match's HUD, func_150918EC, its health, lives, weapon and
// ammo, scores, the radar and the map; and the screen's own effects), then for each view (func_15019BB8: a hit's flash, the tint, the
// fades and the iris, the view's 2D pieces), then the overlays and the text (func_15019E60, func_151E84B0, func_15043384). Here, as the
// extra camera's view is drawn (as view 1, its camera, view and HUD tables in player 1's place already), the same is drawn for one
// player (D_80082FA0 0, D_8008FD90 1: the one-player layout, full screen) with character 5's state in character 1's place: its object
// (gObjects[4]: its health, weapon, ammo, where it is), its line of the match's tallies (func_150859AC's), its bits and bytes in the
// tables kept a character (D_800D2340, D_800E0BCC, D_800D18A0/2, D_80087270, D_800E0AC0, gGameState's scores), and the HUD's tables of 4
// (its fades, its flash) as ours have them now. All of it back as it was after (and the game's memory put back with the view's).
// Not while the game is paused (its pause screen copies the frame), nor on the other machines (the view's commands are let go there).
// CONKER_POSSESS_HUD=0 leaves it out.
extern "C" void func_150198FC(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_15019BB8(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_15019E60(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_151E84B0(uint8_t* rdram, recomp_context* ctx);
extern "C" void conker_net_view_pass(uint8_t* rdram, int gfx, int camera, int kind);   // (possess60, net_view.cpp)
extern "C" void func_1501A490(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_1517F4D8(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_1517F3A0(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_15180580(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_1507DB6C(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_151674F8(uint8_t* rdram, recomp_context* ctx);

namespace {
    // (possess72) func_15019BB8 (a view's HUD pass) for view 1, as the game's own does it, but for its objects of layer 4 (what's drawn
    // over a view for that view alone: R-Look's reticle, a 32 x 32 picture at the view's middle) the view given as the 5th player's, 4:
    // each object's drawing (func_151674F8's calls) is handed the view, and draws its reticle for its own player's view only, so with
    // view 1 the 5th player had none (possess70's log: the reticle's picture in the HUD pass of its player's view, after func_151674F8's
    // DE 8008AC70). Its place is the full-screen view's middle then (view 4's record, the full screen's with a split screen), which
    // net_view.cpp leaves where it is.
    uint32_t fifth_hud_pass(uint8_t* rdram, recomp_context* ctx, uint32_t list) {
        const recomp_context saved = *ctx;
        conker_net_view_pass(rdram, (int32_t)list, 0, 2);
        const uint32_t view_records_now = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800BE628);
        const uint32_t buffer = MEM_BU(0, (gpr)(int32_t)0x800BE9C0);
        MEM_W(0, (gpr)(int32_t)list) = (int32_t)0xDC080008u;
        MEM_W(4, (gpr)(int32_t)list) = (int32_t)(view_records_now + buffer * 16u + 0x40u);
        list += 8;
        // func_1501A490(list, view 0, 0, 0, 0, 0): its two stack arguments below the caller's room
        ctx->r29 = ADD32(saved.r29, -0x40);
        MEM_W(0x10, ctx->r29) = 0;
        MEM_W(0x14, ctx->r29) = 0;
        ctx->r4 = (gpr)(int32_t)list;
        ctx->r5 = 0;
        ctx->r6 = 0;
        ctx->r7 = 0;
        func_1501A490(rdram, ctx);
        list = (uint32_t)ctx->r2;
        *ctx = saved;
        for (auto fn : { func_1517F4D8, func_1517F3A0, func_15180580 }) {
            ctx->r4 = (gpr)(int32_t)list;
            ctx->r5 = 0;
            fn(rdram, ctx);
            list = (uint32_t)ctx->r2;
            *ctx = saved;
        }
        if ((MEM_HU(0, (gpr)(int32_t)0x800D18A0) & 1u) != 0 || (MEM_HU(0, (gpr)(int32_t)0x800D18A2) & 1u) != 0) {
            ctx->r4 = (gpr)(int32_t)list;
            ctx->r5 = 0;
            func_1507DB6C(rdram, ctx);
            list = (uint32_t)ctx->r2;
            *ctx = saved;
        }
        // (possess73) the objects place what they draw by the view's record (D_800BE628, 0x180 each): view 5's is the full screen's
        // with a split screen, so the reticle came out twice the size, low on the screen (possess72's test). For their drawing it is
        // given view 1's (the part of the screen the 5th player's view is drawn in, as player 1's reticle is placed in its own), put
        // back after: the online view then places it as player 1's machine places player 1's.
        // (possess74) and only the reticle's objects are drawn for the 5th player's view: possess72/73 drew every object of layer 4 for it,
        // and some of them (the player's own character among them, drawn for its own player's view) came out dark and blocky over the
        // view. The others are drawn for view 1 as func_15019BB8 draws them (each object type's list, D_800DCE50 (0x1A0 a part, 4 bytes a
        // type), emptied for the drawing that isn't its, and put back; a type is the reticle's by its setup list, its +0x10, D_8008AC70).
        auto draw_layer4 = [&](int view_arg, bool reticle_only) {
            uint32_t heads[2][104];
            for (uint32_t part = 0; part < 2; part++) {
                for (uint32_t t = 0; t < 104; t++) {
                    const gpr at = (gpr)(int32_t)(0x800DCE50u + part * 0x1A0u + t * 4u);
                    heads[part][t] = (uint32_t)MEM_W(0, at);
                    const bool reticle = (uint32_t)MEM_W(0x10, (gpr)(int32_t)(0x8008B4A8u + t * 0x34u)) == 0x8008AC70u;
                    if (reticle != reticle_only) {
                        MEM_W(0, at) = 0;
                    }
                }
            }
            for (int part = 0; part < 2; part++) {
                ctx->r4 = (gpr)(int32_t)list;
                ctx->r5 = 4;
                ctx->r6 = (gpr)view_arg;
                ctx->r7 = (gpr)part;
                func_151674F8(rdram, ctx);
                list = (uint32_t)ctx->r2;
                *ctx = saved;
            }
            for (uint32_t part = 0; part < 2; part++) {
                for (uint32_t t = 0; t < 104; t++) {
                    MEM_W(0, (gpr)(int32_t)(0x800DCE50u + part * 0x1A0u + t * 4u)) = (int32_t)heads[part][t];
                }
            }
        };
        draw_layer4(0, false);
        uint8_t kept_record[0x180];
        const bool records_ok = (view_records_now >> 24) == 0x80 && (view_records_now & 0x7FFFFFu) + 5u * 0x180u < 0x800000u;
        if (records_ok) {
            std::memcpy(kept_record, rdram + (view_records_now + 4u * 0x180u - 0x80000000u), sizeof(kept_record));
            std::memcpy(rdram + (view_records_now + 4u * 0x180u - 0x80000000u), rdram + (view_records_now - 0x80000000u), sizeof(kept_record));
        }
        draw_layer4(4, true);
        if (records_ok) {
            std::memcpy(rdram + (view_records_now + 4u * 0x180u - 0x80000000u), kept_record, sizeof(kept_record));
        }
        return list;
    }
}
extern "C" void func_15043384(uint8_t* rdram, recomp_context* ctx);

namespace {
    constexpr uint32_t humans_now = 0x8008FD90;     // D_8008FD90: the match's players (its human ones, D_8008FE44's 0-3)
    constexpr uint32_t paused_now = 0x800BEAC0;      // D_800BEAC0: the game's pause screen up
    constexpr uint32_t blur_where = 0x800BE575;      // D_800BE575: 1, the blur drawn in func_150198FC
    constexpr uint32_t objects_at_ = 0x800CC2D0;     // gObjects, 0x32C a character
    constexpr uint32_t tallies = 0x800D213C;         // func_150859AC's: 0x1C a character (its lives at +9...)
    constexpr uint32_t tally_size = 0x1C;
    constexpr uint32_t game_state_at = 0x8008FDD4;   // gGameState: a halfword a character at +0x46 (the scores)

    bool fifth_hud_on() {
        static const bool on = []() {
            const char* text = std::getenv("CONKER_POSSESS_HUD");
            return text == nullptr || std::string(text) != "0";
        }();
        return on;
    }

    void swap_word_runs(uint8_t* rdram, uint32_t a, uint32_t b, uint32_t size) {
        for (uint32_t i = 0; i < size; i += 4) {
            const int32_t x = MEM_W(i, (gpr)(int32_t)a), y = MEM_W(i, (gpr)(int32_t)b);
            MEM_W(i, (gpr)(int32_t)a) = y;
            MEM_W(i, (gpr)(int32_t)b) = x;
        }
    }

    void swap_bytes_at(uint8_t* rdram, uint32_t a, uint32_t b) {
        const int8_t x = MEM_B(0, (gpr)(int32_t)a), y = MEM_B(0, (gpr)(int32_t)b);
        MEM_B(0, (gpr)(int32_t)a) = y;
        MEM_B(0, (gpr)(int32_t)b) = x;
    }

    void swap_halves_at(uint8_t* rdram, uint32_t a, uint32_t b) {
        const int16_t x = MEM_H(0, (gpr)(int32_t)a), y = MEM_H(0, (gpr)(int32_t)b);
        MEM_H(0, (gpr)(int32_t)a) = y;
        MEM_H(0, (gpr)(int32_t)b) = x;
    }

    // bits i and j of the halfword at `at`
    void swap_bits_at(uint8_t* rdram, uint32_t at, int i, int j) {
        const uint16_t v = MEM_HU(0, (gpr)(int32_t)at);
        const uint16_t bi = (uint16_t)((v >> i) & 1u), bj = (uint16_t)((v >> j) & 1u);
        const uint16_t cleared = (uint16_t)(v & ~((1u << i) | (1u << j)));
        MEM_H(0, (gpr)(int32_t)at) = (int16_t)(cleared | (uint16_t)(bi << j) | (uint16_t)(bj << i));
    }

    // character 0's state and character k's swapped (twice: back as they were)
    void swap_character_state(uint8_t* rdram, int k) {
        swap_word_runs(rdram, objects_at_, objects_at_ + (uint32_t)k * 0x32Cu, 0x32Cu);
        swap_word_runs(rdram, tallies, tallies + (uint32_t)k * tally_size, tally_size);
        for (uint32_t bits : { 0x800D2340u, 0x800E0BCCu, 0x800D18A0u, 0x800D18A2u }) {
            swap_bits_at(rdram, bits, 0, k);
        }
        for (uint32_t bytes : { 0x80087270u, 0x800E0AC0u }) {
            swap_bytes_at(rdram, bytes, bytes + (uint32_t)k);
        }
        const uint32_t state = (uint32_t)MEM_W(0, (gpr)(int32_t)game_state_at);
        if ((state >> 24) == 0x80 && (state & 0x7FFFFFu) + 0x80u < 0x800000u) {
            swap_halves_at(rdram, state + 0x46u, state + 0x46u + 2u * (uint32_t)k);
        }
    }

    uint32_t fifth_huds = 0;
    // (possess49) the 5th player's machine, its pause menu open: where this frame's overlay (func_15019E60 as the game's frame calls it,
    // the pause screen in it) put its commands, up to its text's flush
    uint32_t overlay_start = 0, overlay_mid = 0, overlay_frame = 0xFFFFFFFFu;

    // the HUD's commands after `list` (the view's), the new end
    uint32_t fifth_hud(uint8_t* rdram, recomp_context* ctx, uint32_t list) {
        if (!fifth_hud_on() || MEM_BU(0, (gpr)(int32_t)paused_now) != 0) {
            return list;
        }
        const recomp_context saved = *ctx;
        const uint32_t before = list;
        // the HUD's tables of 4 as ours have them now (the view's passes stepped player 1's entry, our copy, once more)
        for (int i = 35; i < slot4_count; i++) {
            const Slot4Table& t = slot4_tables[i];
            if (!t.always) {
                continue;
            }
            const uint32_t shadow = slot4_shadow + 0x100u * (uint32_t)i;
            for (uint32_t b = 0; b < t.stride; b++) {
                MEM_B(b, (gpr)(int32_t)t.base) = MEM_B(b, (gpr)(int32_t)shadow);
            }
        }
        const int32_t was_last = MEM_W(0, (gpr)(int32_t)last_player);
        const int8_t was_humans = MEM_B(0, (gpr)(int32_t)humans_now);
        const int8_t was_blur = MEM_B(0, (gpr)(int32_t)blur_where);
        const int32_t was_view = MEM_W(0, (gpr)(int32_t)view_now);
        swap_character_state(rdram, ch5);
        if (!fifth_split(rdram)) {
            // (possess59: with a split screen the game's own players and layout, view 1's part's HUD: see fifth_split)
            MEM_W(0, (gpr)(int32_t)last_player) = 0;
            MEM_B(0, (gpr)(int32_t)humans_now) = 1;
        }
        MEM_B(0, (gpr)(int32_t)blur_where) = 0;
        MEM_W(0, (gpr)(int32_t)view_now) = 0;
        list = call_list(rdram, ctx, func_150198FC, list, 0);
        *ctx = saved;
        if (fifth_split(rdram)) {
            // (possess60) with a split screen, the game's own order (func_1501878C's with more than one view): the view's HUD pass
            // (func_15019BB8), then the overlay (func_15019E60, which makes the full-screen view the one drawn) and the match's HUD
            // (func_151E84B0: the team scores, and with the pause menu open the pause screen and its score table, drawn here as the
            // game draws its own: net_menu.cpp's pause_screen_here), then the text. Those are noted as a HUD pass of the last view's
            // (net_view.cpp), as in the game's frame, so the online view places them as on player 1's machine (possess59 drew the
            // overlay before the HUD pass, where the online view took it for the 3D's: the team scores stayed in the middle of the
            // screen, the pause screen came out cut and moved, its table's icons hidden).
            // (possess72: the HUD pass as func_15019BB8's, its layer 4 objects drawn for the 5th player's view: fifth_hud_pass)
            static const bool own_layer4 = []() { const char* t = std::getenv("CONKER_POSSESS_RETICLE"); return t == nullptr || std::string(t) != "0"; }();
            if (own_layer4) {
                list = fifth_hud_pass(rdram, ctx, list);
            } else {
                list = call_list(rdram, ctx, func_15019BB8, list, 0);
            }
            *ctx = saved;
            conker_net_view_pass(rdram, (int32_t)list, std::max(0, (int)was_last), 2);
            const bool pause_here = conker::netsession::active() && conker::netmenu::is_open();
            if (pause_here) {
                // (possess63: the menu's text is queued again after the pause screen only, below: queued here as well (possess62), each
                // button showed twice, one of them in the wrong colour or picture, and the screen flickered between them)
                static uint32_t told = 0;
                if (told++ < 3) {
                    log("f=%u the 5th player's pause screen drawn over its view as the game draws its own (a split screen)", (unsigned)frames);
                }
            }
            list = call_list(rdram, ctx, func_15019E60, list, 0);
            *ctx = saved;
            list = call_list(rdram, ctx, func_151E84B0, list, 0);
            *ctx = saved;
            if (pause_here) {
                // (possess62) and the menu's text once more after the pause screen (its QUIT and CONT. were still missing over it), for the
                // last flush. (possess71: without the pause screen's pictures set aside put back first, fifth_pictures: those were the game's
                // loads of this frame, which the extra view's memory doesn't have, so the stick's picture between QUIT and CONT. flickered
                // to garbage; the flush loads what it needs itself.)
                conker::netmenu::fifth_pause_text(rdram, ctx);
                *ctx = saved;
            }
            list = call_list(rdram, ctx, func_15043384, list, 0);
            *ctx = saved;
            MEM_W(0, (gpr)(int32_t)view_now) = was_view;
        } else {
            // (possess49) the 5th player's pause menu open: the pause screen as the game drew it this frame, under this view (its overlay's
            // commands, func_15019E60 up to its text's flush: conker_possess_overlay), copied here; its text (the menu's, queued as the game's
            // update ran it, and the score table's, queued as the overlay drew it: both drawn by the game's flush under this view) queued
            // again (net_menu.cpp) and drawn over it by the flush; and, as the game's pause has it, no view's own HUD over the pause screen
            const bool pause_open = conker::netsession::active() && conker::netmenu::is_open() && overlay_frame == frames && overlay_mid > overlay_start &&
                overlay_mid - overlay_start <= 0x10000u && in_rdram(overlay_start);
            if (pause_open) {
                for (uint32_t at = overlay_start; at < overlay_mid; at += 4) {
                    MEM_W(0, (gpr)(int32_t)list) = MEM_W(0, (gpr)(int32_t)at);
                    list += 4;
                }
                static uint32_t told = 0;
                if (told++ < 3) {
                    log("f=%u the 5th player's pause screen: the game's (%u commands at %08X) copied over its view", (unsigned)frames,
                        (unsigned)((overlay_mid - overlay_start) / 8), (unsigned)overlay_start);
                }
                conker::netmenu::fifth_pause_text(rdram, ctx);
                *ctx = saved;
                list = call_list(rdram, ctx, func_15043384, list, 0);
                *ctx = saved;
            } else {
                list = call_list(rdram, ctx, func_15019E60, list, 0);
                *ctx = saved;
                list = call_list(rdram, ctx, func_15019BB8, list, 0);
                *ctx = saved;
            }
            list = call_list(rdram, ctx, func_151E84B0, list, 0);
            *ctx = saved;
            list = call_list(rdram, ctx, func_15043384, list, 0);
            *ctx = saved;
            MEM_W(0, (gpr)(int32_t)view_now) = was_view;
        }
        MEM_B(0, (gpr)(int32_t)blur_where) = was_blur;
        MEM_B(0, (gpr)(int32_t)humans_now) = was_humans;
        MEM_W(0, (gpr)(int32_t)last_player) = was_last;
        swap_character_state(rdram, ch5);
        fifth_huds++;
        if (fifth_huds <= 3 || fifth_huds % 600 == 0) {
            log("f=%u the 5th player's HUD drawn over its view (%u times): %u commands", (unsigned)frames, (unsigned)fifth_huds,
                (unsigned)((list - before) / 8));
        }
        return list;
    }
}

// (possess49) func_15019E60 (the frame's overlay: the HUD over the views, the pause screen in it, then the text's flush) begins (0: $a0
// the list) and is about to flush its text (1: $v0 the list): the game's own call (not fifth_hud's), on the 5th player's machine with
// its pause menu open, for fifth_hud to copy the pause screen over the 5th player's view.
extern "C" void conker_possess_overlay(uint8_t* rdram, recomp_context* ctx, int where) {
    if (extra_view_now || !conker::netmenu::is_open() || !conker::netsession::active() || !conker::possess_test::online_view_here()) {
        return;
    }
    if (where == 0) {
        overlay_start = (uint32_t)ctx->r4;
        overlay_mid = 0;
        overlay_frame = frames;
        conker::netmenu::fifth_overlay_noting(rdram, true);
    } else if (overlay_frame == frames) {
        overlay_mid = (uint32_t)ctx->r2;
        conker::netmenu::fifth_overlay_noting(rdram, false);
    }
}

extern "C" void conker_possess_view_local(uint8_t* rdram, recomp_context* ctx) {
    second_pass_at = 0;   // (possess61: this frame's, once the view's second pass begins)
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
    if (!start_copied) {
        return;
    }
    start_copied = false;
    OthersStopped others(rdram);
    local_copy.assign(rdram, rdram + game_size);
    const uint32_t list_before = mem_word(rdram, head);
    const uint32_t frame_list = mem_word(rdram, list_start + 4 * (MEM_BU(0, (gpr)(int32_t)matrix_buffer) & 1u));
    // the room left in the frame's list (the game's own limit, func_1501878C's: its commands so far, and a margin for its end)
    if (!in_rdram(frame_list) || list_before < frame_list || (int32_t)((list_before - frame_list) >> 3) + 64 > (int32_t)MEM_W(0, (gpr)(int32_t)list_most)) {
        static uint32_t told = 0;
        if (told++ < 10) {
            log("f=%u no room in the frame's list for the extra camera's view (%u commands of %d)", (unsigned)frames,
                (unsigned)((list_before - frame_list) >> 3), (int)MEM_W(0, (gpr)(int32_t)list_most));
        }
        return;
    }
    // the game's memory as before its views were drawn, but for the frame's list (its commands so far) and the list's end
    put_back(rdram, start_copy, frame_list, list_before);
    MEM_W(0, (gpr)(int32_t)head) = (int32_t)list_before;
    // (possess35) each character's fade and hidden bits in view 1 as the extra camera's view left them last frame (func_1502E4C4 fades
    // a character in or out in a view by its distance from the view's camera, 50 a frame, func_150617BC): put back each frame to the
    // game's (player 1's camera's), a character far from player 1 was faded in from nothing every frame, and stayed at 50 of 255
    if (extra_fades_kept) {
        for (int k = 0; k < 25; k++) {
            const gpr c = (gpr)(int32_t)(0x800CC2D0u + (uint32_t)k * 0x32Cu);
            MEM_B(0xB, c) = (int8_t)extra_fade[k];
            MEM_B(0xF, c) = (int8_t)((MEM_BU(0xF, c) & ~0x11u) | extra_hidden[k]);
        }
    }
    // (possess37) and the step each character's fade takes a frame (func_150617BC, from func_150619A8 in func_15018DFC, before the
    // views: for player 1's view alone), for view 1 as the extra camera's: 50 toward shown, or hidden, by its hidden bit (+0xF) for
    // view 1. Without it a character's fade in the extra camera's view stayed where it was (at nothing, if it had faded out).
    for (int k = 0; k < 25; k++) {
        const gpr c = (gpr)(int32_t)(0x800CC2D0u + (uint32_t)k * 0x32Cu);
        if (MEM_W(0, c) == 0) {
            continue;
        }
        const uint32_t fade = MEM_BU(0xB, c);
        const bool hidden = (MEM_BU(0xF, c) & 1u) != 0;
        MEM_B(0xB, c) = (int8_t)(hidden ? (fade > 50u ? fade - 50u : 0u) : (fade < 205u ? fade + 50u : 255u));
    }
    // (possess42) what the extra camera saw last frame (the map's areas in its sight, each object's fade in view 1), for this frame's
    // choice of what's drawn and run in its view
    const uint32_t areas_put_back = put_back_what_was_seen(rdram);
    if (areas_told < 5 || (areas_put_back != 0 && local_views % 600 == 0)) {
        areas_told++;
        log("f=%u the extra camera's last frame: %u of the map's %u areas put back as in its sight (not player 1's), the fades of %u objects",
            (unsigned)frames, (unsigned)areas_put_back, (unsigned)kept_areas.size(), (unsigned)kept_object_fades.size());
    }
    // nothing else of the game's runs while the view is drawn: this thread first in line (its priority as high as it goes)
    const uint32_t self = (uint32_t)ultramodern::this_thread();
    int32_t self_priority = 0;
    if ((self >> 24) == 0x80) {
        self_priority = MEM_W(4, (gpr)(int32_t)self);
        MEM_W(4, (gpr)(int32_t)self) = 0x7FFF;
    }

    // 2. view 1 the extra camera's
    std::memcpy(rdram + (base - 0x80000000u), rdram + (base + (uint32_t)extra * camera_size - 0x80000000u), camera_size);
    MEM_B(0x23D, (gpr)(int32_t)base) = 0;
    if (!fifth_split(rdram)) {   // (possess59: with a split screen, view 1's own record: see fifth_split)
        std::memcpy(rdram + (views - 0x80000000u), rdram + (views + (uint32_t)extra * 0x180u - 0x80000000u), 0x180);
    }
    std::memcpy(rdram + (mats - 0x80000000u), rdram + (mats + (uint32_t)extra * 0x40u - 0x80000000u), 0x40);
    // (possess33) played or not, the extra character drawn as the bot it is while only shown (its brain's AI flag, +0x84; the game's memory
    // is put back after): shown, it's drawn; played (+0x84 0, a player's), it wasn't, with its fade, hidden bits and render item the same
    if (extra_played) {
        const uint32_t brain = brain_of(rdram, ch5);
        if (brain != 0) {
            MEM_B(0x84, (gpr)(int32_t)brain) = (int8_t)extra_brain_flag;
        }
    }
    // (possess25) the cameras' tables of 4 (each camera's entry: what the camera code worked out for it this frame, what's in sight
    // from it among them): the extra camera's is ours (conker_slot4), so camera 1's entry as the extra camera's for the view. (Not
    // the buttons pressed, the cameras' screen areas, nor the table inside camera 1's own block.)
    {
        static uint32_t told = 0;
        uint32_t differed = 0;
        for (int i = 0; i < slot4_count; i++) {
            const Slot4Table& t = slot4_tables[i];
            if (!t.always || t.base == pressed_shadow_table || t.base == 0x800DC200u || t.base == 0x800DBFF4u) {
                continue;
            }
            const uint32_t shadow = slot4_shadow + 0x100u * (uint32_t)i;
            bool differs = false;
            for (uint32_t b = 0; b < t.stride; b++) {
                differs |= MEM_B(b, (gpr)(int32_t)t.base) != MEM_B(b, (gpr)(int32_t)shadow);
                MEM_B(b, (gpr)(int32_t)t.base) = MEM_B(b, (gpr)(int32_t)shadow);
            }
            if (differs) {
                differed |= 1u << (i & 31);
            }
        }
        if (told++ < 3) {
            log("f=%u the cameras' tables: the extra camera's entries as camera 1's for its view (those that differed: %08X)", (unsigned)frames,
                (unsigned)differed);
        }
    }

    // 3. its passes
    drawn_now.clear();
    extra_view_now = true;
    const recomp_context saved = *ctx;
    const int32_t was_view = MEM_W(0, (gpr)(int32_t)view_now);
    MEM_W(0, (gpr)(int32_t)view_now) = 0;
    ctx->r4 = 0;
    func_1510F800(rdram, ctx);
    *ctx = saved;
    ctx->r4 = 0;
    func_1510FC34(rdram, ctx);
    *ctx = saved;
    func_1510B690(rdram, ctx);
    *ctx = saved;
    ctx->r4 = 0;
    func_15094EA0(rdram, ctx);
    *ctx = saved;
    // (possess28) what each object is in sight of (func_15019130's after its views' loops: func_15112A80 and func_15113218 clear
    // and set each object's bit of the views that see it, the bit a view's passes draw it by), for the extra camera as view 1
    // (possess31) and the rest of func_15019130 after its views' loops, as it runs them: the same calls, in the same order (what each
    // object is in sight of; the characters' drawing made ready for the views that see them; the queue of what's drawn), but for the
    // frame's counters at its end. (possess36) All of it now: func_1502BEE4 too (possess31 left it out as it can load from the
    // cartridge), each character's drawing for the frame (func_1502BD84: its pose, +0x1D4, cleared and made anew; the views it's drawn
    // in, +0x74; its fades, func_1502E4C4): without it the extra camera's view drew each character with what player 1's view had left
    // from the frame before (no pose, or an old one, and not at all if player 1 couldn't see it); and func_151749F8, func_150636F0.
    // Then, as func_1501878C runs before its views' passes: func_1515D69C and func_15167310 (each camera's turn, for what faces the
    // camera, and each render item's drawing readied for the frame).
    {
        const bool one_view_way = MEM_BU(0, (gpr)(int32_t)0x800BEAC0) == 0;
        const bool tail_a = MEM_BU(0, (gpr)(int32_t)0x800BE616) == 0;
        auto call0 = [&](void (*fn)(uint8_t*, recomp_context*)) { fn(rdram, ctx); *ctx = saved; };
        auto call1 = [&](void (*fn)(uint8_t*, recomp_context*), int32_t a0) { ctx->r4 = (gpr)a0; fn(rdram, ctx); *ctx = saved; };
        auto call2 = [&](void (*fn)(uint8_t*, recomp_context*), int32_t a0, int32_t a1) {
            ctx->r4 = (gpr)a0;
            ctx->r5 = (gpr)a1;
            fn(rdram, ctx);
            *ctx = saved;
        };
        call1(func_15112A80, 0);
        // (possess37: func_151749F8, the level's own update for the frame, left out again; and func_150636F0 below)
        call1(func_15188B74, 0);
        if (one_view_way) {
            call0(func_1516706C);
            call0(func_151671E8);
        }
        call0(func_1502BEE4);
        if (tail_a) {
            call0(func_150A0D8C);
        }
        call0(func_15113218);
        call1(func_15188B74, 1);
        call1(func_151738C4, MEM_W(0, (gpr)(int32_t)0x800BE9F0));
        call0(func_1502C1A4);
        call0(func_15113C88);
        if (MEM_BU(0, (gpr)(int32_t)0x800BEAC0) == 0) {
            call0(func_15183D28);
            call0(func_151670C0);
            call0(func_15177A94);
        }
        if (MEM_BU(0, (gpr)(int32_t)0x800BEAC0) == 0) {
            call0(func_151814FC);
        }
        call0(func_1517F75C);
        call0(func_1517F7B4);
        call0(func_15036148);
        // func_1501878C's, after func_15019130 (func_1501A680 adds the frame's first commands: not again here)
        call0(func_1515D69C);
        call0(func_15167310);
    }
    // (possess42) the game's blur (func_151D6778, D_800BE574 its strength: the screen as the frame before left it drawn faintly over a view,
    // then the view copied for the next frame) left out of the extra camera's view: it has one buffer (D_800BE570), player 1's view's
    // too, so each view would be drawn over with the other's
    if (MEM_BU(0, (gpr)(int32_t)0x800BE574) != 0) {
        blur_left++;
    }
    MEM_B(0, (gpr)(int32_t)0x800BE574) = 0;
    ctx->r4 = 0;
    func_15018F80(rdram, ctx);
    *ctx = saved;
    uint32_t list = call_list(rdram, ctx, func_15019464, list_before, 0);
    *ctx = saved;
    list = call_list(rdram, ctx, func_1515D6D0, list, 0);
    *ctx = saved;
    list = call_list(rdram, ctx, func_150195A0, list, 0);
    *ctx = saved;
    // (possess27) and its second pass (func_1501878C's second loop of the views: func_1515D6D0 + func_15019CC8, what's drawn in
    // layer 5 of func_151674F8's, after the 3D of the first)
    // (possess61: where it starts, for net_view.cpp's rocket launcher overlay: fifth_second_pass)
    second_pass_at = list;
    // (possess63) with a split screen, the second pass's objects told the view is the 5th player's (D_80082FA4 4): what's drawn there for
    // one player's own view alone, as the gun's reticle in R-Look (a 32 x 32 picture over the view's middle, drawn with the gun in the
    // second pass of its player's view only), is the 5th player's then, not player 1's. The views' tables (their 5th entry ours while
    // view_pass_on) are given view 1's entries, as the view's first pass left them, for whatever reads them by that index.
    // CONKER_POSSESS_VIEW4=0 leaves it out.
    static const bool view4_on = []() { const char* t = std::getenv("CONKER_POSSESS_VIEW4"); return t == nullptr || std::string(t) != "0"; }();
    const bool second_as_view4 = view4_on && fifth_split(rdram);
    if (second_as_view4) {
        for (int i = 0; i < slot4_count; i++) {
            const Slot4Table& t = slot4_tables[i];
            if (t.always || t.stride > 0x100u) {
                continue;
            }
            const uint32_t shadow = slot4_shadow + 0x100u * (uint32_t)i;
            for (uint32_t b = 0; b < t.stride; b++) {
                MEM_B(b, (gpr)(int32_t)shadow) = MEM_B(b, (gpr)(int32_t)t.base);
            }
        }
        view_pass_on = true;
        MEM_W(0, (gpr)(int32_t)view_now) = 4;
        static uint32_t told = 0;
        if (told++ < 3) {
            log("f=%u the extra view's second pass drawn with the view taken for the 5th player's (4)", (unsigned)frames);
        }
    }
    list = call_list(rdram, ctx, func_1515D6D0, list, 0);
    *ctx = saved;
    list = call_list(rdram, ctx, func_15019CC8, list, 0);
    *ctx = saved;
    if (second_as_view4) {
        MEM_W(0, (gpr)(int32_t)view_now) = 0;
        view_pass_on = false;
    }
    // (possess39) and the characters' poses made ready to be drawn, as func_1501878C does once its views are drawn: func_1502E474
    // turns the frame's bone matrices (the pool at D_800C3E80[frame], D_800C3E7A of them) from the floats func_1502BEE4 works them out
    // in to the fixed point the drawing reads (func_150A9984). The view's commands were drawing its characters from the floats: their
    // limbs anywhere (or nowhere to be seen), and the big flat shapes across the screen. (possess38's experiment, the game's own
    // poses, showed it.)
    func_1502E474(rdram, ctx);
    *ctx = saved;
    // (possess42) what the view had in sight, for its next frame
    keep_what_was_seen(rdram);
    MEM_W(0, (gpr)(int32_t)view_now) = was_view;
    extra_view_now = false;
    for (int k = 0; k < 25; k++) {
        const gpr c = (gpr)(int32_t)(0x800CC2D0u + (uint32_t)k * 0x32Cu);
        extra_fade[k] = MEM_BU(0xB, c);
        extra_hidden[k] = MEM_BU(0xF, c) & 0x11u;
    }
    extra_fades_kept = true;
    // (possess48) its HUD over it, on the 5th player's machine (fifth_hud: what's kept for the next frame is above, and the HUD
    // changes none of it)
    if (view_shown_here()) {
        extra_view_now = true;
        list = fifth_hud(rdram, ctx, list);
        extra_view_now = false;
        *ctx = saved;
    }
    // past the list's room (the game's limit, with a margin for its end): the view's commands let go
    bool too_long = (int32_t)((list - frame_list) >> 3) + 64 > (int32_t)MEM_W(0, (gpr)(int32_t)list_most);
    if (too_long) {
        static uint32_t told = 0;
        if (told++ < 10) {
            log("f=%u the extra camera's view let go: the frame's list would have %u commands of %d", (unsigned)frames,
                (unsigned)((list - frame_list) >> 3), (int)MEM_W(0, (gpr)(int32_t)list_most));
        }
        list = list_before;
    }
    // (possess44, online) the view's commands are the 5th player's machine's alone: the others let them go (what the view marks for the
    // next frame's updates, below, every machine keeps alike)
    if (!view_shown_here()) {
        list = list_before;
    }

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
    walker.run(list_before, 0, list, -1);
    // (possess42) the buffers the RDP draws into, as textures the view draws from: left as they are (see rdp_buffers)
    std::vector<std::pair<uint32_t, uint32_t>> rdp = rdp_buffers(rdram);
    rdp.insert(rdp.end(), walker.cimgs.begin(), walker.cimgs.end());
    auto on_rdp = [&](uint32_t at, uint32_t size) {
        for (const auto& b : rdp) {
            if (at < b.first + b.second && b.first < at + size) {
                return true;
            }
        }
        return false;
    };
    uint32_t rdp_left = 0;
    auto changed = [&](uint32_t at, uint32_t size) {
        if (at < 0x80000000u || at >= 0x80000000u + game_size) {
            return false;
        }
        size = std::min(size, 0x80000000u + game_size - at);
        return std::memcmp(local_copy.data() + (at - 0x80000000u), rdram + (at - 0x80000000u), size) != 0;
    };
    const uint32_t copies = copies_start + (copies_frame++ & 1u) * copies_half;
    uint32_t copies_used = 0;
    auto copy_out = [&](uint32_t at, uint32_t size) -> uint32_t {
        const uint32_t room = (size + 15u) & ~15u;
        if (size == 0 || copies_used + room > copies_half) {
            return 0;
        }
        const uint32_t to = copies + copies_used;
        copies_used += room;
        std::memcpy(rdram + (to - 0x80000000u), rdram + (at - 0x80000000u), size);
        return to;
    };
    // (possess24) What the view drew from that its passes changed is copied to ours, and the command naming it pointed at the copy
    // (as a plain address, whatever segment it was named through). A command in a list the view's commands call is pointed in a
    // copy of that list, and the call to it at the copy (up to the view's own commands): so each list holding something to change,
    // or changed itself, is copied with the lists calling it. What can't be (no room) is kept in the game's memory for the frame and
    // put back at the next one's start.
    const int body_count = (int)walker.bodies.size();
    std::vector<uint8_t> need(body_count, 0);
    std::vector<uint8_t> ref_changed(walker.refs.size(), 0);
    for (int b = 0; b < body_count; b++) {
        const auto& bd = walker.bodies[b];
        if (bd.end > bd.at && changed(bd.at, bd.end - bd.at)) {
            need[b] = 1;
        }
    }
    // (possess38 experiment, D-pad Up while playing the extra character) the characters' poses (their bones' matrices, from the pool
    // at D_800C3E8C, D_800C3E7C of 0x40) left as the game made them, not the view's
    const uint32_t pose_pool = mem_word(rdram, 0x800C3E8C), pose_pool_end = pose_pool + 0x40u * (uint32_t)MEM_HU(0, (gpr)(int32_t)0x800C3E7C);
    uint32_t poses_left = 0;
    for (size_t i = 0; i < walker.refs.size(); i++) {
        const auto& r = walker.refs[i];
        if (use_game_poses && r.at >= pose_pool && r.at < pose_pool_end) {
            poses_left++;
            continue;
        }
        if (on_rdp(r.at, r.size)) {
            rdp_left++;
            continue;
        }
        if (changed(r.at, r.size)) {
            ref_changed[i] = 1;
            if (r.body >= 0) {
                need[r.body] = 1;
            }
        }
    }
    if (use_game_poses && (local_views < 3 || local_views % 120 == 0)) {
        log("f=%u (experiment) %u of the view's matrices left as the game's poses (pool %08X to %08X)", (unsigned)frames, (unsigned)poses_left,
            (unsigned)pose_pool, (unsigned)pose_pool_end);
    }
    for (int b = body_count - 1; b >= 0; b--) {
        if (need[b] && walker.bodies[b].parent >= 0) {
            need[walker.bodies[b].parent] = 1;
        }
    }
    std::vector<uint32_t> body_copy(body_count, 0);
    std::vector<std::pair<uint32_t, uint32_t>> patches;   // (address of a command's second word, new value)
    std::vector<std::pair<uint32_t, uint32_t>> keep_ranges;
    // where a command of list `body` is to be changed: in the view's own commands, or in its list's copy (0: nowhere)
    auto place = [&](int body, uint32_t command) -> uint32_t {
        if (body < 0) {
            return command;
        }
        return body_copy[body] != 0 ? body_copy[body] + (command - walker.bodies[body].at) : 0;
    };
    uint32_t relocated = 0, lists_copied = 0;
    for (int b = 0; b < body_count; b++) {   // (parents before their children)
        if (!need[b]) {
            continue;
        }
        const auto& bd = walker.bodies[b];
        const uint32_t at_parent = place(bd.parent, bd.command);
        const uint32_t size = bd.end - bd.at;
        const uint32_t to = at_parent != 0 && size <= 0x20000u ? copy_out(bd.at, size) : 0;
        if (to != 0) {
            body_copy[b] = to;
            patches.push_back({ at_parent + 4, to });
            lists_copied++;
        } else if (changed(bd.at, size)) {
            keep_ranges.push_back({ bd.at, std::min(size, 0x80000000u + game_size - bd.at) });
        }
    }
    std::vector<uint32_t> ref_to(walker.refs.size(), 0);
    for (size_t i = 0; i < walker.refs.size(); i++) {
        if (!ref_changed[i]) {
            continue;
        }
        const auto& r = walker.refs[i];
        const uint32_t at = place(r.body, r.command);
        const uint32_t to = at != 0 ? copy_out(r.at, r.size) : 0;
        if (to != 0) {
            patches.push_back({ at + 4, to });
            ref_to[i] = to;
            relocated++;
        } else {
            keep_ranges.push_back({ r.at, r.size });
        }
    }
    // (possess38 diagnostics) the extra character's matrices in the view's commands: where each was named from (the address its command
    // names, through its segment), whether the view's passes changed it, where it was copied; the first two whole, as the view's
    // passes left them and as the game's memory has them (put back after)
    if (local_views < 3 || local_views % 600 == 0) {
        auto halves = [&](const uint8_t* mem, uint32_t a) {
            std::string out;
            for (int h = 0; h < 32; h++) {
                int16_t v = 0;
                std::memcpy(&v, mem + (((a - 0x80000000u) + 2u * (uint32_t)h) ^ 2u), 2);
                char piece[8];
                std::snprintf(piece, sizeof(piece), "%s%04X", h == 0 ? "" : (h == 16 ? " | " : " "), (unsigned)(uint16_t)v);
                out += piece;
            }
            return out;
        };
        for (const Drawn& d : drawn_now) {
            if (d.object != (uint32_t)ch5 || d.end <= d.start) {
                continue;
            }
            std::vector<uint8_t> inside(walker.bodies.size(), 0);
            for (size_t b = 0; b < walker.bodies.size(); b++) {
                const auto& bd = walker.bodies[b];
                inside[b] = (bd.command >= d.start && bd.command < d.end) || (bd.parent >= 0 && inside[(size_t)bd.parent]);
            }
            int shown = 0;
            for (size_t i = 0; i < walker.refs.size() && shown < 6; i++) {
                const auto& r = walker.refs[i];
                const bool ours_here = r.body < 0 ? (r.command >= d.start && r.command < d.end) : inside[(size_t)r.body] != 0;
                if (!ours_here || (mem_word(rdram, r.command) >> 24) != 0xDA || r.at < 0x80000000u || r.at + 64 > 0x80000000u + game_size) {
                    continue;
                }
                log("f=%u [mtx] the extra character's matrix %d: command %08X (in %s) names %08X (segment %d) = %08X, %s, copied to %08X",
                    (unsigned)frames, shown, (unsigned)r.command, r.body < 0 ? "the view's own commands" : "a list they call",
                    (unsigned)mem_word(rdram, r.command + 4), r.segment, (unsigned)r.at, ref_changed[i] ? "changed by the view" : "as the game has it",
                    (unsigned)ref_to[i]);
                if (shown < 2) {
                    log("f=%u [mtx]   as the view left it: %s", (unsigned)frames, halves(rdram, r.at).c_str());
                    log("f=%u [mtx]   as the game has it:  %s", (unsigned)frames, halves(local_copy.data(), r.at).c_str());
                }
                shown++;
            }
            // and its pose (+0x1D4), the same two ways
            const uint32_t pose = mem_word(rdram, 0x800CC2D0u + (uint32_t)ch5 * 0x32Cu + 0x1D4u);
            if ((pose >> 24) == 0x80 && pose + 64 <= 0x80000000u + game_size) {
                log("f=%u [mtx] its pose %08X as the view left it: %s", (unsigned)frames, (unsigned)pose, halves(rdram, pose).c_str());
                log("f=%u [mtx] its pose %08X as the game has it:  %s", (unsigned)frames, (unsigned)pose, halves(local_copy.data(), pose).c_str());
            }
        }
    }
    const uint32_t by_segment = lists_copied;
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
    // (possess44, online) nothing of the view's kept in the game's memory into the next frame (it would differ from the other machines'):
    // its commands are let go this frame instead
    if (conker::netsession::active() && !keep.empty()) {
        static uint32_t told_kept = 0;
        if (told_kept++ < 20) {
            log("f=%u (online) %u pieces the view draws from couldn't be copied: its commands let go this frame", (unsigned)frames, (unsigned)keep.size());
        }
        keep.clear();
        now.clear();
        list = list_before;
        patches.clear();
    }
    // (possess32 diagnostics) each character as the view's passes left it: in sight (+0x6F of its object), its fades (+7, +0xB: view 1's),
    // hidden-views bits (+0xF), its render item (+0x2D4), against the game's (as the frame ended)
    if (local_views < 3 || local_views % 120 == 0) {
        std::string out;
        for (int k = 0; k < 6; k++) {
            const uint32_t c = 0x800CC2D0u + (uint32_t)k * 0x32Cu;
            const uint32_t li = c - 0x80000000u;
            auto was_b = [&](uint32_t off) { return (unsigned)local_copy[(li + off) ^ 3]; };
            auto was_w = [&](uint32_t off) { uint32_t v; std::memcpy(&v, local_copy.data() + li + off, 4); return v; };
            char piece[260];
            std::snprintf(piece, sizeof(piece), " | c%d: state %u fade %u/%u (was %u/%u) hide %02X (was %02X) views %02X (was %02X) pose %08X (was %08X)",
                k, (unsigned)MEM_BU(5, (gpr)(int32_t)c), (unsigned)MEM_BU(7, (gpr)(int32_t)c), (unsigned)MEM_BU(0xB, (gpr)(int32_t)c), was_b(7),
                was_b(0xB), (unsigned)MEM_BU(0xF, (gpr)(int32_t)c), was_b(0xF), (unsigned)MEM_BU(0x74, (gpr)(int32_t)c), was_b(0x74),
                (unsigned)word(rdram, c + 0x1D4), (unsigned)was_w(0x1D4));
            out += piece;
        }
        log("f=%u [chars] after the view's passes%s", (unsigned)frames, out.c_str());
    }
    // (possess30) What the view saw (func_15112A80's bit of view 1, in each object's +0x6F): kept once the game's memory is put
    // back, as view 4's bit (0x08: no view 4 offline). The next frame's func_15113180 updates (animates, makes the model of) only
    // the objects some view saw (+0x6F & 0xF), func_15113E54 by the 0x40 it sets: the ones only the extra camera saw were left as
    // they were, and drawn so (or not at all).
    const uint32_t seen_count = mem_word(rdram, objects_count), seen_list = mem_word(rdram, objects_list);
    std::vector<uint8_t> seen;
    if (seen_count > 0 && seen_count < 2048 && in_rdram(seen_list) && in_rdram(seen_list + seen_count * object_record)) {
        seen.resize(seen_count);
        for (uint32_t i = 0; i < seen_count; i++) {
            seen[i] = MEM_BU(0x6F, (gpr)(int32_t)(seen_list + i * object_record)) & 1u;
        }
    }
    // (possess26) What the view's passes wrote to the characters that player 1's view didn't (S and E alike there, ours not): kept
    // once the game's memory is put back, as if a 5th view of the game's own had drawn them. The game notes which characters a view
    // has drawn (to animate them, and to draw their models, the next frame): those only the extra camera sees would be dropped.
    constexpr uint32_t objects_at = 0x000CC2D0, objects_size = 25u * 0x32Cu;
    std::vector<std::pair<uint32_t, uint8_t>> drawn_marks;
    for (uint32_t i = objects_at; i < objects_at + objects_size; i++) {
        if (rdram[i] != local_copy[i] && start_copy[i] == local_copy[i]) {
            drawn_marks.push_back({ i, rdram[i] });
        }
    }
    const bool tell = local_views < 3 || local_views % 300 == 0;
    if (tell) {
        // (diagnostics) where the view's passes wrote (outside its commands), and what player 1's view wrote to the characters
        auto ranges = [&](const std::vector<uint8_t>& a, const uint8_t* b, uint32_t from, uint32_t to, const char* what) {
            std::string out;
            uint32_t count = 0, bytes = 0;
            for (uint32_t i = from; i < to; i++) {
                if (a[i] == b[i] || (i >= list_before - 0x80000000u && i < list - 0x80000000u)) {
                    continue;
                }
                uint32_t j = i, last = i;
                while (j < to && j - last < 32) {
                    if (a[j] != b[j]) {
                        last = j;
                    }
                    j++;
                }
                count++;
                bytes += last + 1 - i;
                if (count <= 60 && out.size() < 1800) {
                    char piece[64];
                    if (i >= objects_at && i < objects_at + objects_size) {
                        std::snprintf(piece, sizeof(piece), " c%u+%X..%X", (unsigned)((i - objects_at) / 0x32C), (unsigned)((i - objects_at) % 0x32C),
                            (unsigned)((last - objects_at) % 0x32C));
                    } else {
                        std::snprintf(piece, sizeof(piece), " %06X+%X", (unsigned)i, (unsigned)(last + 1 - i));
                    }
                    out += piece;
                }
                i = last;
            }
            log("f=%u %s: %u pieces, %u bytes:%s", (unsigned)frames, what, (unsigned)count, (unsigned)bytes, out.c_str());
        };
        ranges(local_copy, rdram, 0, game_size, "the extra view wrote");
        ranges(start_copy, local_copy.data(), objects_at, objects_at + objects_size, "player 1's views wrote to the characters");
        log("f=%u %u bytes of the characters the extra view drew kept", (unsigned)frames, (unsigned)drawn_marks.size());
    }
    // (possess43) whether the renderer wrote its buffers while the view was drawn (see put_back): left as it wrote them
    {
        static uint32_t told = 0;
        if (told++ < 2) {
            std::string out;
            for (const auto& b : rdp_buffers(rdram)) {
                char piece[40];
                std::snprintf(piece, sizeof(piece), " %08X+%X", (unsigned)b.first, (unsigned)b.second);
                out += piece;
            }
            log("f=%u the RDP's buffers (the framebuffers, the depth buffer, the blur's), never written by the view's copies:%s", (unsigned)frames,
                out.c_str());
        }
        bool wrote = false;
        for (const auto& b : rdp_buffers(rdram)) {
            const uint32_t at = b.first - 0x80000000u;
            wrote = wrote || std::memcmp(local_copy.data() + at, rdram + at, b.second) != 0;
        }
        if (wrote) {
            renderer_wrote++;
        }
    }
    // the game's memory back, but for the new commands and the list's end
    const uint32_t to = list - 0x80000000u;
    if (list > list_before && to <= game_size) {
        put_back(rdram, local_copy, list_before, list);
    } else {
        put_back(rdram, local_copy, 0, 0);
        list = list_before;
        patches.clear();
    }
    if ((self >> 24) == 0x80) {
        MEM_W(4, (gpr)(int32_t)self) = self_priority;
    }
    MEM_W(0, (gpr)(int32_t)head) = (int32_t)list;
    for (const auto& pt : patches) {
        MEM_W(0, (gpr)(int32_t)pt.first) = (int32_t)pt.second;
    }
    for (size_t i = 0; i < keep.size(); i++) {
        std::memcpy(rdram + (keep[i].at - 0x80000000u), now[i].data(), keep[i].size);
    }
    kept_back = std::move(keep);
    // (possess37 diagnostics) the objects the view drew, and the extra character's drawing: its matrices (where each puts its origin)
    // and vertices (the first of each block), as the commands left to be drawn have them
    if ((local_views < 3 || local_views % 120 == 0) && list > list_before) {
        std::string out;
        for (const Drawn& d : drawn_now) {
            char piece[48];
            std::snprintf(piece, sizeof(piece), " %X:%u(%u)", (unsigned)d.kind, (unsigned)d.object, (unsigned)(d.end > d.start ? (d.end - d.start) / 8 : 0));
            if (out.size() < 1400) {
                out += piece;
            }
        }
        log("f=%u [drawn] %u objects in the extra camera's view:%s", (unsigned)frames, (unsigned)drawn_now.size(), out.c_str());
        auto dump_one = [&](const Drawn& d, const char* where) {
            std::string dump;
            uint32_t seg[16] = {};
            for (int sg = 0; sg < 16; sg++) {
                seg[sg] = walker.seg[sg];
            }
            auto resolve = [&](uint32_t a) -> uint32_t {
                const uint32_t s = (a >> 24) & 0xFFu;
                if (s < 16) {
                    return 0x80000000u | ((seg[s] + (a & 0x00FFFFFFu)) & 0x00FFFFFFu);
                }
                return 0x80000000u | (a & 0x00FFFFFFu);
            };
            int shown = 0;
            std::vector<std::pair<uint32_t, uint32_t>> stack;   // (where to go back to, where that stretch ends)
            uint32_t at = d.start, stop = d.end;
            for (int n = 0; n < 6000 && shown < 48; n++) {
                if (at >= stop || (at & 7u) != 0 || at < 0x80000000u || at >= 0x81000000u) {
                    if (stack.empty()) {
                        break;
                    }
                    at = stack.back().first;
                    stop = stack.back().second;
                    stack.pop_back();
                    continue;
                }
                const uint32_t w0 = mem_word(rdram, at), w1 = mem_word(rdram, at + 4);
                const uint32_t op = w0 >> 24;
                char piece[120];
                piece[0] = 0;
                if (op == 0xDA) {
                    const uint32_t m = resolve(w1);
                    auto t = [&](int i) {
                        return (float)(int16_t)MEM_H(24 + 2 * i, (gpr)(int32_t)m) + (float)(uint16_t)MEM_H(56 + 2 * i, (gpr)(int32_t)m) / 65536.0f;
                    };
                    std::snprintf(piece, sizeof(piece), " M%02X@%08X(%.0f,%.0f,%.0f)", (unsigned)(w0 & 0xFF), (unsigned)w1, t(0), t(1), t(2));
                } else if (op == 0x01) {
                    const uint32_t v = resolve(w1);
                    std::snprintf(piece, sizeof(piece), " V%u@%08X(%d,%d,%d)", (unsigned)((w0 >> 12) & 0xFFu), (unsigned)w1,
                        (int)(int16_t)MEM_H(0, (gpr)(int32_t)v), (int)(int16_t)MEM_H(2, (gpr)(int32_t)v), (int)(int16_t)MEM_H(4, (gpr)(int32_t)v));
                } else if (op == 0xDB && ((w0 >> 16) & 0xFFu) == 0x06 && (w0 & 0xFFFFu) < 64u) {
                    seg[(w0 & 0xFFFFu) / 4u] = w1 & 0x00FFFFFFu;
                    std::snprintf(piece, sizeof(piece), " S%u=%08X", (unsigned)((w0 & 0xFFFFu) / 4u), (unsigned)w1);
                } else if (op == 0xDE) {
                    std::snprintf(piece, sizeof(piece), " D%s@%08X", ((w0 >> 16) & 0xFFu) != 0 ? "j" : "", (unsigned)w1);
                    const uint32_t target = resolve(w1);
                    if (((w0 >> 16) & 0xFFu) == 0) {
                        if (stack.size() < 12) {
                            stack.push_back({ at + 8, stop });
                            at = target;
                            stop = 0xFFFFFFFFu;
                            dump += piece;
                            shown++;
                            continue;
                        }
                    } else {
                        at = target;
                        stop = 0xFFFFFFFFu;
                        dump += piece;
                        shown++;
                        continue;
                    }
                } else if (op == 0xDF) {
                    if (stack.empty()) {
                        break;
                    }
                    at = stack.back().first;
                    stop = stack.back().second;
                    stack.pop_back();
                    continue;
                }
                if (piece[0] != 0) {
                    dump += piece;
                    shown++;
                }
                at += 8;
            }
            log("f=%u [drawn] the extra character %s (kind %X, %u commands):%s", (unsigned)frames, where, (unsigned)d.kind,
                (unsigned)((d.end - d.start) / 8), dump.c_str());
        };
        for (const Drawn& d : drawn_now) {
            if (d.object == (uint32_t)ch5 && d.end > d.start) {
                dump_one(d, "in the extra camera's view");
            }
        }
        for (const Drawn& d : game_drawn) {
            if (d.object == (uint32_t)ch5 && d.end > d.start) {
                dump_one(d, "in player 1's view");
            }
        }
    }
    // (possess32: drawn_marks no longer put in: with the whole of the game's drawing preparation run for the view, they held the render
    // items it made (+0x2D4), which don't exist in the game's memory as put back: the game freed them, and crashed)
    (void)drawn_marks;
    if (!seen.empty() && mem_word(rdram, objects_count) == seen_count && mem_word(rdram, objects_list) == seen_list) {
        uint32_t marked = 0;
        for (uint32_t i = 0; i < seen_count; i++) {
            const gpr at = (gpr)(int32_t)(seen_list + i * object_record);
            const uint8_t was = MEM_BU(0x6F, at);
            MEM_B(0x6F, at) = (int8_t)(seen[i] ? (was | extra_seen_bit) : was);   // (added to player 1's, never taken off)
            marked += seen[i];
        }
        seen_marked = true;
        if (local_views < 3 || local_views % 600 == 0) {
            log("f=%u %u of %u objects seen by the extra camera, marked for the next frame's updates", (unsigned)frames, (unsigned)marked,
                (unsigned)seen_count);
        }
    }
    // (possess41) and the weapons lying near the extra character (within 400 across), in its camera's sight or not: func_1511DF6C, which
    // gives a weapon to whoever walks into it, is run only for the props some view saw
    if (!seen.empty() && extra_ready && mem_word(rdram, objects_count) == seen_count && mem_word(rdram, objects_list) == seen_list) {
        const uint32_t c = character(ch5);
        const float cx = float_word(rdram, c + 0x14), cz = float_word(rdram, c + 0x1C);
        uint32_t added = 0;
        for (uint32_t i = 0; i < seen_count; i++) {
            const uint32_t prop = seen_list + i * object_record;
            if (word(rdram, prop + 0x38) != weapon_behaviour) {
                continue;
            }
            const float dx = (float)MEM_H(0x10, (gpr)(int32_t)prop) - cx, dz = (float)MEM_H(0x14, (gpr)(int32_t)prop) - cz;
            const uint8_t was = MEM_BU(0x6F, (gpr)(int32_t)prop);
            if (dx * dx + dz * dz <= 400.0f * 400.0f && (was & extra_seen_bit) == 0) {
                MEM_B(0x6F, (gpr)(int32_t)prop) = (int8_t)(was | extra_seen_bit);
                added++;
            }
        }
        static uint32_t told_added = 0;
        if (added != 0 && told_added < 40) {
            told_added++;
            log("f=%u [weapons] %u weapons near the extra character, out of its camera's sight, run anyway", (unsigned)frames, (unsigned)added);
        }
    }
    local_views++;
    if (local_views <= 3 || local_views % 600 == 0) {
        log("f=%u (the frame's list: %u commands of %d)", (unsigned)frames, (unsigned)((list - frame_list) >> 3), (int)MEM_W(0, (gpr)(int32_t)list_most));
        log("f=%u the extra camera's view drawn here (%u times): %u commands (%u walked), %u pieces copied to ours, %u lists copied (of %u walked), %u "
            "texture loads (%u from the RDP's buffers, left as they are; %u color images set), %u branches, %u kept for the frame (%u bytes of copies); "
            "the blur left out %u times; the renderer wrote its buffers while %u of the views were drawn (left as it wrote them)",
            (unsigned)frames, (unsigned)local_views, (unsigned)((list - list_before) / 8),
            (unsigned)walker.commands, (unsigned)relocated, (unsigned)by_segment, (unsigned)walker.bodies.size(), (unsigned)walker.textures, (unsigned)rdp_left,
            (unsigned)walker.cimgs.size(), (unsigned)walker.branches, (unsigned)kept_back.size(), (unsigned)copies_used, (unsigned)blur_left,
            (unsigned)renderer_wrote);
    }
}

// func_1501878C's start (the next frame): what the view drew from, back as the game had it.
void conker_possess_heartbeat();

extern "C" void conker_possess_view_local_after(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    conker_possess_heartbeat();
    if (!kept_back.empty()) {
        put_kept_back(rdram);
    }
}

// (possess19) func_15019130 before its views' func_1510FC34 loop: a copy of the game's memory, to draw the extra camera's view from.
extern "C" void conker_possess_frame_start(uint8_t* rdram) {
    game_drawn.clear();
    extra_object = extra_ready ? ch5 : -1;
    if (!local_view_wanted(rdram)) {
        start_copied = false;
        extra_fades_kept = false;
        forget_what_was_seen();
        if (seen_marked && false) {   // (possess32: view 1's bit is the game's own, set again each frame)
            seen_marked = false;
            const uint32_t count = mem_word(rdram, objects_count), list = mem_word(rdram, objects_list);
            if (count > 0 && count < 2048 && in_rdram(list) && in_rdram(list + count * object_record)) {
                for (uint32_t i = 0; i < count; i++) {
                    const gpr at = (gpr)(int32_t)(list + i * object_record);
                    MEM_B(0x6F, at) = (int8_t)(MEM_BU(0x6F, at) & ~extra_seen_bit);
                }
            }
        }
        return;
    }
    start_copy.assign(rdram, rdram + game_size);
    start_copied = true;
}

// ---- (possess21) a watchdog for freezes ----
// While the extra camera is there, the game's thread marks each frame (func_1501878C's start). If no frame comes for 4 seconds, a
// thread of ours stops the game's for a moment, writes where it is (its stack, with the recompiled functions' names and lines: the
// line gives the MIPS instruction) to conker_possess.log and crash.log, and lets it go on. Once.
#if defined(_WIN32)
#include <Windows.h>
#include <DbgHelp.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <string>
#include <filesystem>
#pragma comment(lib, "dbghelp.lib")

namespace {
    std::atomic<uint32_t> heartbeat{ 0 };
    std::atomic<bool> watch_armed{ false };
    HANDLE game_thread = nullptr;

    void dump_game_thread() {
        HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
        SymInitialize(process, nullptr, TRUE);
        if (SuspendThread(game_thread) == (DWORD)-1) {
            return;
        }
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        std::string report = "[host] freeze: the game's thread hasn't finished a frame in 4 seconds; it is at\n";
        if (GetThreadContext(game_thread, &context)) {
            STACKFRAME64 frame{};
            frame.AddrPC.Offset = context.Rip;
            frame.AddrPC.Mode = AddrModeFlat;
            frame.AddrFrame.Offset = context.Rbp;
            frame.AddrFrame.Mode = AddrModeFlat;
            frame.AddrStack.Offset = context.Rsp;
            frame.AddrStack.Mode = AddrModeFlat;
            alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
            SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
            for (int i = 0; i < 40; i++) {
                if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, game_thread, &frame, &context, nullptr, SymFunctionTableAccess64,
                        SymGetModuleBase64, nullptr) || frame.AddrPC.Offset == 0) {
                    break;
                }
                std::memset(buffer, 0, sizeof(buffer));
                symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
                symbol->MaxNameLen = 255;
                DWORD64 offset = 0;
                char line_text[400];
                if (SymFromAddr(process, frame.AddrPC.Offset, &offset, symbol)) {
                    IMAGEHLP_LINE64 line{};
                    line.SizeOfStruct = sizeof(line);
                    DWORD displacement = 0;
                    if (SymGetLineFromAddr64(process, frame.AddrPC.Offset, &displacement, &line)) {
                        std::snprintf(line_text, sizeof(line_text), "  %s+0x%llx (%s:%lu)\n", symbol->Name, (unsigned long long)offset,
                            std::filesystem::path(line.FileName).filename().string().c_str(), line.LineNumber);
                    } else {
                        std::snprintf(line_text, sizeof(line_text), "  %s+0x%llx\n", symbol->Name, (unsigned long long)offset);
                    }
                } else {
                    std::snprintf(line_text, sizeof(line_text), "  %p\n", (void*)frame.AddrPC.Offset);
                }
                report += line_text;
            }
        }
        ResumeThread(game_thread);
        log("%s", report.c_str());
        if (FILE* f = std::fopen("crash.log", "a")) {
            std::fputs(report.c_str(), f);
            std::fputs("\n", f);
            std::fclose(f);
        }
    }

    void start_watchdog() {
        static bool started = false;
        if (started) {
            return;
        }
        started = true;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &game_thread, 0, FALSE, DUPLICATE_SAME_ACCESS);
        std::thread([]() {
            uint32_t last = heartbeat.load();
            auto since = std::chrono::steady_clock::now();
            bool dumped = false;
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                const uint32_t now = heartbeat.load();
                if (now != last || !watch_armed.load()) {
                    last = now;
                    since = std::chrono::steady_clock::now();
                    continue;
                }
                if (!dumped && std::chrono::steady_clock::now() - since > std::chrono::seconds(4)) {
                    dumped = true;
                    dump_game_thread();
                }
            }
        }).detach();
    }
}

void conker_possess_heartbeat() {
    start_watchdog();
    heartbeat.fetch_add(1);
    watch_armed = extra_ready;
}
#else
void conker_possess_heartbeat() {}
#endif

// (possess22) the game making a message queue ($a0) or a thread ($a0): kept, so that the extra camera's view leaves them be.
extern "C" void conker_possess_note_queue(uint8_t* rdram, uint32_t queue) {
    (void)rdram;
    if ((queue >> 24) == 0x80 && std::find(known_queues.begin(), known_queues.end(), queue) == known_queues.end()) {
        known_queues.push_back(queue);
    }
}

extern "C" void conker_possess_note_thread(uint8_t* rdram, uint32_t thread) {
    (void)rdram;
    if ((thread >> 24) == 0x80 && std::find(known_threads.begin(), known_threads.end(), thread) == known_threads.end()) {
        known_threads.push_back(thread);
    }
}

#if defined(_WIN32)
namespace {
    OthersStopped::OthersStopped(uint8_t* rdram) {
        const uint32_t self = (uint32_t)ultramodern::this_thread();
        stopped.reserve(known_threads.size());
        for (uint32_t t : known_threads) {
            if (t == self) {
                continue;
            }
            OSThread* thread = reinterpret_cast<OSThread*>(rdram + (t - 0x80000000u));
            if (thread == nullptr || thread->context == nullptr) {
                continue;
            }
            HANDLE handle = (HANDLE)thread->context->host_thread.native_handle();
            if (handle != nullptr && SuspendThread(handle) != (DWORD)-1) {
                stopped.push_back(handle);
            }
        }
    }
    OthersStopped::~OthersStopped() {
        for (void* handle : stopped) {
            ResumeThread((HANDLE)handle);
        }
    }
}
#else
namespace {
    OthersStopped::OthersStopped(uint8_t* rdram) { (void)rdram; }
    OthersStopped::~OthersStopped() {}
}
#endif

// (possess32) func_1506160C's entry: a character hidden or shown again in view $a3 (& 0xFF). The game has 4 views: another view (the extra
// camera's, 4) would write its fade over the character's hidden-views bits (+0xB + 4 = +0xF). Such a call is let go.
extern "C" int conker_possess_hide_skip(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    // (possess35: the view is its 5th argument, on the stack (0x10($sp)); $a3 is a value. Only its modes 8 and up name a view.)
    const uint32_t view = (uint32_t)MEM_W(0x10, ctx->r29) & 0xFFu;
    if (((uint32_t)ctx->r5 & 0xFFu) < 8 || view < 4) {
        return 0;
    }
    static uint32_t told = 0;
    if (told++ < 20) {
        log("f=%u func_1506160C for character %08X (mode %u) in view %u: let go", (unsigned)frames, (unsigned)ctx->r4,
            (unsigned)((uint32_t)ctx->r5 & 0xFFu), (unsigned)view);
    }
    return 1;
}

// (possess40) func_15172E7C's entry ($a0 the character, $a1 the level it goes to, $a2 the spot there): for the extra character, as
// the game does for a character without a camera: moved at once (func_15172D28), if it's this level.
extern "C" void func_15172D28(uint8_t* rdram, recomp_context* ctx);
extern "C" int conker_possess_warp(uint8_t* rdram, recomp_context* ctx) {
    if (hooks_off() || !extra_ready || (uint32_t)ctx->r4 != character(ch5)) {
        return 0;
    }
    const uint32_t object = (uint32_t)ctx->r4;
    if (MEM_BU(0x1CA, (gpr)(int32_t)object) == 0) {
        return 1;
    }
    const uint32_t brain = word(rdram, object + 0x31C);
    if (brain != 0 && MEM_B(0x23, (gpr)(int32_t)brain) > 0) {
        return 1;
    }
    const int32_t level = (int32_t)ctx->r5, spot = (int32_t)ctx->r6;
    log("f=%u the extra character through a door: to level %d (this one %d), spot %d", (unsigned)frames, (int)level,
        (int)MEM_W(0, (gpr)(int32_t)0x800BE9F0), (int)spot);
    if (level == MEM_W(0, (gpr)(int32_t)0x800BE9F0)) {
        const recomp_context saved = *ctx;
        ctx->r4 = (gpr)(int32_t)object;
        ctx->r5 = (gpr)spot;
        func_15172D28(rdram, ctx);
        *ctx = saved;
    }
    return 1;
}

// ---- (possess44) online, 5 players ----

bool conker::possess_test::online_extra_camera() {
    return extra_ready && online5();
}

bool conker::possess_test::online_view_here() {
    return extra_ready && extra_played && online5() && conker::netsession::local_slot() == extra;
}

bool conker::possess_test::fifth_playing() {
    return extra_ready && extra_played;
}

// (possess47) The 5th player's own character. func_151E2834 sets a multiplayer match up from its list of characters (the menus' Start, a
// restart, playing again): it counts them (D_8008FD8C) and the arena makes character k from entry k (func_15082A44, which gives a computer
// player's, -2, its AI). Before the list is counted (0x151E2914), entry 4 is made the 5th player's: -3, the computer players moved up after
// it. The game's setup and its menus take it as a computer player (they go by a player's controller, 0 to 3, or not), but func_15082A44
// gives it no AI: nothing but the 5th player's input drives it (conker_possess_pad), so the 5th player never takes one of the computer
// players, and plays with none. Its team: the one with the fewest characters (then players); without teams the game gives every character
// one of its own afterwards. The menus leave room for it (mp_characters.cpp: the most characters less one); should the list be full, its
// last computer players are taken off (the mode's most characters, 12 at most: the war's 4 players, the 5th and 7 computer players). Once
// there it stays (a restart, playing again; the menus make the list over), taken out when the 5th player isn't in the frame's input. Not
// with fewer than 4 characters before it: it has to be character 5 (the extra camera's slot, its controller, the HUD's 5th entries).
// The same on every machine at the same frame: the game's own setup and the frame's input.
namespace {
    constexpr uint32_t game_state = 0x8008FDD4;      // gGameState (the mode at +0x42)
    constexpr uint32_t modes = 0x800AB68C;           // 10 bytes a mode: +5 its team table (or, under 0, its row of most characters)
    constexpr uint32_t team_tables = 0x800AB710;     // 12 bytes: +0 the teams, +2 / +5 the most characters (under 3 players / 3 or more)
    constexpr uint32_t mode_rows = 0x8008FEE4;       // without teams: the row of each, by -(+5) - 1
    constexpr uint32_t most_rows = 0x800AB74C;       // 4 a row: the most characters for 1 to 4 players

    // The mode's team table (under 0: none, the row of most characters by it instead); false without a mode.
    bool mode_tables(uint8_t* rdram, int* table) {
        const uint32_t state = word(rdram, game_state);
        if ((state >> 24) != 0x80) {
            return false;
        }
        const int mode = (int)(int8_t)MEM_B(0x42, (gpr)(int32_t)state);
        if (mode < 0 || mode > 32) {
            return false;
        }
        *table = (int)(int8_t)MEM_B(0, (gpr)(int32_t)(modes + (uint32_t)(mode * 10) + 5u));
        return true;
    }

    // The most characters the menus let the players have (online, mp_characters.cpp: for 2 players or more the largest of the mode's), 12 at
    // most; 12 without a mode.
    int mode_most(uint8_t* rdram, int players) {
        int table = 0;
        if (!mode_tables(rdram, &table) || players < 1 || players > 4) {
            return most_characters;
        }
        int most = 0;
        if (table >= 0 && table <= 16) {
            const uint32_t entry = team_tables + (uint32_t)table * 12u;
            const int under_three = (int)(int8_t)MEM_B(2, (gpr)(int32_t)entry), three_more = (int)(int8_t)MEM_B(5, (gpr)(int32_t)entry);
            most = players >= 2 ? std::max(under_three, three_more) : under_three;
        } else if (table < 0 && table >= -16) {
            const int row = (int)(int8_t)MEM_B(0, (gpr)(int32_t)(mode_rows + (uint32_t)(-table - 1)));
            if (row >= 0 && row < 32) {
                const uint32_t at = most_rows + (uint32_t)row * 4u;
                most = (int)(int8_t)MEM_B((int32_t)(players - 1), (gpr)(int32_t)at);
                if (players >= 2) {
                    for (int32_t i = 0; i < 4; i++) {
                        most = std::max(most, (int)(int8_t)MEM_B(i, (gpr)(int32_t)at));
                    }
                }
            }
        }
        return (most >= 2 && most <= most_characters) ? most : most_characters;
    }

    // (possess51) The mode's teams (0 without a team table).
    int mode_teams(uint8_t* rdram) {
        int table = 0;
        if (!mode_tables(rdram, &table) || table < 0 || table > 16) {
            return 0;
        }
        return (int)(int8_t)MEM_B(0, (gpr)(int32_t)(team_tables + (uint32_t)table * 12u));
    }

    // The 5th player's team: of the mode's teams, the one with the fewest characters, then the fewest players, then the first.
    int8_t fifth_team(uint8_t* rdram, const int8_t* list, const int8_t* team, int count) {
        int table = 0;
        if (!mode_tables(rdram, &table) || table < 0 || table > 16) {
            return 0;   // (no teams: func_151E2834 gives each character its own, after this)
        }
        const int teams = (int)(int8_t)MEM_B(0, (gpr)(int32_t)(team_tables + (uint32_t)table * 12u));
        if (teams < 2 || teams > 8) {
            return 0;
        }
        int characters_on[8] = {}, players_on[8] = {};
        for (int i = 0; i < count; i++) {
            if (team[i] >= 0 && team[i] < teams) {
                characters_on[team[i]]++;
                if (list[i] >= 0) {
                    players_on[team[i]]++;
                }
            }
        }
        int best = 0;
        for (int t = 1; t < teams; t++) {
            if (characters_on[t] < characters_on[best] || (characters_on[t] == characters_on[best] && players_on[t] < players_on[best])) {
                best = t;
            }
        }
        return (int8_t)best;
    }
}

// (possess49) The 5th player on the character select (the join screen: func_151E0B70 runs it each frame, func_151EC648's case 4 draws
// it). The game has a column for each of its 4 controllers: "P1" to "P4", Start's sprite and "press start" while the controller hasn't
// joined, and once it has, the character it picked standing over its column. Controllers 2 to 4 join and leave with Start, and pick a
// character with the stick where the mode lets the players pick one (D_800E0B8C); D_8008FE44 has the joined controllers in order,
// D_800E0B90 their characters, D_800E0C88 each column's fade. Online, in a match of 5 players, a 5th column (P5) works the same way from
// the 5th player's input (the frame's, the same on every machine). Its state is kept in ours (join_state, which goes with a resync's
// state): Start joins or leaves, and the stick or the D-pad picks a character. The 5 columns are drawn 56 apart from x 36, centred like
// the game's 4 (72 apart from 40), and the places the characters stand (D_800AB940, 4 of them) move to match. The match is set up with
// the 5th player's character only once it has joined (conker_possess_list), with the character it picked where the mode lets the
// players pick (conker_possess_join_kind), and the menus leave room for it only then (mp_characters.cpp, fifth_joined).
// (Its character isn't drawn over its column yet: the character select's scene has places for the 4 controllers' characters. The scene's
// places are logged for it.)
namespace {
    constexpr uint32_t join_state = 0x80FD0A00;
    // +0 joined, +1 its character, +2 its column's fade (as D_800E0C88's: 255 "press start", 0 joined), +3 the places moved,
    // +4 its buttons the frame before (a halfword), +6 its stick's x the frame before, +7 the stick's hold (as gGameState+0x36's),
    // +8 the places as the game has them (4 x 8)
    constexpr uint32_t places_table = 0x800AB940;    // D_800AB940: where each column's character stands: x, y, z (halfwords), a byte
    constexpr uint32_t join_next = 0x800E0A96;       // D_800E0A96: the screen to go to (-1: this one stays)
    constexpr uint32_t join_picking = 0x800E0B8C;    // D_800E0B8C: the players pick their characters here (1)
    constexpr uint32_t picks_default = 0x8008FEC4;   // a mode's row of 4 bytes: each controller's character as it joins
    constexpr uint32_t picks_allowed = 0x8008FEA4;   // a mode's row: the characters it lets the players pick (bits)
    constexpr uint32_t unlocked = 0x800E9D00;        // D_800E9D00: the characters won (more of them to pick)
    constexpr uint32_t character_kinds = 0x800AB57C; // 8 bytes a character to pick: +6 its kind (D_80087270's)
    constexpr uint32_t frame_step = 0x800BE9E4;      // D_800BE9E4: the frame's length
    constexpr int pick_count = 22;                   // the characters to pick from (0 to 21)
    constexpr int columns_x = 36, columns_step = 56;
    constexpr uint16_t d_left = 0x0200, d_right = 0x0100;
    uint32_t join_logged = 0;

    bool five_columns() {
        return online5();
    }

    bool fifth_is_joined(uint8_t* rdram) {
        return MEM_BU(0, (gpr)(int32_t)join_state) != 0;
    }

    // The characters the mode lets the players pick (as func_151E0B70 works them out: the row's bits, more for the characters won), and
    // the row (under 0: a mode with teams, every character, nothing picked here).
    uint32_t picks_mask(uint8_t* rdram, int* row) {
        int table = 0;
        *row = -1;
        if (!mode_tables(rdram, &table) || table >= 0 || table < -16) {
            return 0xFFFFFFFFu;
        }
        *row = -1 - table;
        uint32_t mask = word(rdram, picks_allowed + 4u * (uint32_t)*row);
        const uint32_t won = word(rdram, unlocked);
        if (won & 0x20u) mask |= 0x8000u;
        if (won & 0x10u) mask |= 0x10000u;
        if (won & 0x40u) mask |= 0x30u;
        if (won & 0x80u) mask |= 0x43C0u;
        if (won & 0x100u) mask |= 0xE0800u;
        if (won & 0x10000u) mask |= 0xAu;
        if (won & 0x20000u) mask |= 0x300000u;
        return mask;
    }

    // from character c, the next one the mask lets be picked in direction dir (as func_151E0B70 steps them)
    int next_pick(int c, int dir, uint32_t mask) {
        for (int i = 0; i < pick_count; i++) {
            c += dir;
            if (c >= pick_count) {
                c = 0;
            } else if (c < 0) {
                c = pick_count - 1;
            }
            if ((mask >> c) & 1u) {
                return c;
            }
        }
        return c;
    }

    // (logged for the 5th column's character to come) the character select scene's places: D_800D20FC's records (0x30 each, D_800D2100 of
    // them): +0x28 the id (the controllers' characters 0x10 to 0x13), +4 the kind, +6 x, y, z
    void log_join_scene(uint8_t* rdram) {
        const uint32_t records = word(rdram, 0x800D20FCu);
        const int count = (int)MEM_BU(0, (gpr)(int32_t)0x800D2100);
        std::string out;
        for (int i = 0; i < count && i < 64 && (records >> 24) == 0x80; i++) {
            const uint32_t r = records + (uint32_t)i * 0x30u;
            char piece[64];
            std::snprintf(piece, sizeof(piece), " %d:id %02X kind %02X at %d %d %d", i, (unsigned)MEM_BU(0x28, (gpr)(int32_t)r),
                (unsigned)MEM_BU(4, (gpr)(int32_t)r), (int)MEM_H(6, (gpr)(int32_t)r), (int)MEM_H(8, (gpr)(int32_t)r), (int)MEM_H(0xA, (gpr)(int32_t)r));
            if (out.size() < 2000) {
                out += piece;
            }
        }
        log("f=%u [join] the character select's scene: %d places at %08X:%s", (unsigned)conker::netsession::frame(), count, (unsigned)records,
            out.c_str());
    }
}

bool conker::possess_test::fifth_split_view(uint8_t* rdram) {
    return online_view_here() && fifth_split(rdram);
}

bool conker::possess_test::fifth_joined(uint8_t* rdram) {
    return online5() && fifth_is_joined(rdram);
}

// func_151E0B70's start (conker.toml): the places the characters stand, for 5 columns or as the game has them (only func_151E7F60 reads
// them, as it makes a column's character; the same on every machine, and with a resync's state).
extern "C" void conker_possess_join_places(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    fifth_unmap(rdram, "the character select");   // (possess55)
    const bool want = five_columns();
    const bool moved = MEM_BU(3, (gpr)(int32_t)join_state) != 0;
    if (want && join_logged < 3) {
        join_logged++;
        log_join_scene(rdram);
    }
    if (want == moved) {
        return;
    }
    if (want) {
        for (uint32_t i = 0; i < 32; i += 4) {
            MEM_W(8 + i, (gpr)(int32_t)join_state) = MEM_W(i, (gpr)(int32_t)places_table);
        }
        // column j's place: along the line from the first's to the 4th's, where its middle is (x 36 + 56j) of the 4 columns' (40 + 72k)
        std::string out;
        for (int j = 0; j < 4; j++) {
            const int32_t along = columns_x + columns_step * j - 0x28;   // over 72 a column, 3 columns from the first to the 4th: / 216
            for (int axis = 0; axis < 3; axis++) {
                const int32_t first = MEM_H(8 + 2 * axis, (gpr)(int32_t)join_state);
                const int32_t last = MEM_H(8 + 24 + 2 * axis, (gpr)(int32_t)join_state);
                const int32_t num = (last - first) * along;
                const int32_t step = num >= 0 ? (num + 108) / 216 : -((-num + 108) / 216);
                MEM_H(8u * (uint32_t)j + 2u * (uint32_t)axis, (gpr)(int32_t)places_table) = (int16_t)(first + step);
            }
            char piece[80];
            std::snprintf(piece, sizeof(piece), " %d: %d %d %d -> %d %d %d", j, (int)MEM_H(8 + 8 * j, (gpr)(int32_t)join_state),
                (int)MEM_H(10 + 8 * j, (gpr)(int32_t)join_state), (int)MEM_H(12 + 8 * j, (gpr)(int32_t)join_state),
                (int)MEM_H(8 * j, (gpr)(int32_t)places_table), (int)MEM_H(2 + 8 * j, (gpr)(int32_t)places_table),
                (int)MEM_H(4 + 8 * j, (gpr)(int32_t)places_table));
            out += piece;
        }
        MEM_B(3, (gpr)(int32_t)join_state) = 1;
        log("f=%u [join] the character select's columns: 5, the characters' places moved:%s", (unsigned)conker::netsession::frame(), out.c_str());
    } else {
        for (uint32_t i = 0; i < 32; i += 4) {
            MEM_W(i, (gpr)(int32_t)places_table) = MEM_W(8 + i, (gpr)(int32_t)join_state);
        }
        MEM_B(3, (gpr)(int32_t)join_state) = 0;
        log("f=%u [join] the character select's columns: the game's 4 again", (unsigned)conker::netsession::frame());
    }
}

// func_151E0B70, 0x151E1200 (its frame done, the screen staying: $sp + 0x54 its fade, 255 up): the 5th player's Start and stick, as
// controllers 2 to 4 have theirs there.
extern "C" void conker_possess_join(uint8_t* rdram, recomp_context* ctx) {
    if (!five_columns()) {
        return;
    }
    const gpr state = (gpr)(int32_t)join_state;
    const uint32_t sp = (uint32_t)ctx->r29;
    bool plugged = false;
    uint16_t buttons = 0;
    float x = 0.0f, y = 0.0f;
    if (!(conker::netsession::input(extra, &plugged, &buttons, &x, &y) && plugged)) {
        buttons = 0;
        x = y = 0.0f;
    }
    const uint16_t before = MEM_HU(4, state);
    const uint16_t pressed = (uint16_t)(buttons & ~before);
    MEM_H(4, state) = (int16_t)buttons;
    const bool up = MEM_W(0x54, (gpr)(int32_t)sp) == 0xFF;
    int row = -1;
    const uint32_t mask = picks_mask(rdram, &row);
    if (up && (pressed & button_start) != 0) {
        if (fifth_is_joined(rdram)) {
            MEM_B(0, state) = 0;
            MEM_B(0x15, state) = 0;   // (possess51: its team on the teams screen chosen again)
            log("f=%u [join] the 5th player leaves", (unsigned)conker::netsession::frame());
        } else {
            // its first character: as a controller's (the mode's row, the 4th's and on), or its number with teams
            int c = extra;
            if (row >= 0) {
                c = next_pick((int)(int8_t)MEM_B(0, (gpr)(int32_t)(picks_default + 4u * (uint32_t)row + 3u)), 1, mask);
            }
            MEM_B(0, state) = 1;
            MEM_B(1, state) = (int8_t)c;
            MEM_B(0x15, state) = 0;   // (possess51: its team on the teams screen chosen as it gets there)
            log("f=%u [join] the 5th player joins (character %d)", (unsigned)conker::netsession::frame(), c);
        }
    }
    // the stick as the game's menus take a controller's (func_151DE8F0): past 50 either way from under it, or held there a while
    int8_t sx = 0, sy = 0;
    convert_to_n64_range(x, y, sx, sy);
    const int8_t was = MEM_B(6, state);
    int dir = 0;
    if (was >= -49 && sx < -49) {
        dir = -1;
    } else if (was < 50 && sx >= 50) {
        dir = 1;
    }
    MEM_B(6, state) = sx;
    if (sx >= -49 && sx < 50) {
        MEM_B(7, state) = 0;
    } else {
        const int32_t held = (int32_t)MEM_BU(7, state) + MEM_W(0, (gpr)(int32_t)frame_step);
        if (held >= 51) {
            MEM_B(7, state) = 40;
            MEM_B(6, state) = 0;
        } else {
            MEM_B(7, state) = (int8_t)held;
        }
    }
    if ((pressed & d_left) != 0) {
        dir = -1;
    } else if ((pressed & d_right) != 0) {
        dir = 1;
    }
    if (dir != 0 && fifth_is_joined(rdram) && MEM_BU(0, (gpr)(int32_t)join_picking) != 0 && row >= 0) {
        const int was_c = (int)(int8_t)MEM_B(1, state);
        const int c = next_pick(was_c >= 0 && was_c < pick_count ? was_c : 0, dir, mask);
        MEM_B(1, state) = (int8_t)c;
        log("f=%u [join] the 5th player picks character %d", (unsigned)conker::netsession::frame(), c);
    }
}

// func_151EC648, case 4 (the character select drawn): 0x151ECCBC, its columns (5); 0x151ECCD8, each column's x ($s5) and the 5th's fade
// ($s6, ours); 0x151ECD1C (0) and 0x151ECDA0 (1): the 5th column joined or not as ours has it ($a2, the fade's way; $s2 no entry of
// D_8008FE44's), past the characters' making ($s1 0 there, as column 1's: its character isn't made here).
extern "C" void conker_possess_join_columns(uint8_t* rdram, recomp_context* ctx) {
    if (five_columns()) {
        MEM_B(0xF7, ctx->r29) = 5;
    }
}

extern "C" void conker_possess_join_column(uint8_t* rdram, recomp_context* ctx) {
    if (!five_columns() || MEM_BU(0xF7, ctx->r29) != 5) {
        return;
    }
    const int column = (int)(int32_t)ctx->r17;
    ctx->r21 = (gpr)(int32_t)(columns_x + columns_step * column);
    if (column == extra) {
        ctx->r22 = (gpr)(int32_t)(join_state + 2u);
    }
}

extern "C" void conker_possess_join_fifth(uint8_t* rdram, recomp_context* ctx, int where) {
    static bool fifth_now = false;
    if (where == 0) {
        fifth_now = false;
        if (!five_columns() || MEM_BU(0xF7, ctx->r29) != 5 || (int32_t)ctx->r17 != extra) {
            return;
        }
        ctx->r18 = (gpr)(int32_t)-1;
        ctx->r6 = fifth_is_joined(rdram) ? (gpr)(int32_t)-1 : (gpr)1;
        ctx->r17 = 0;
        fifth_now = true;
    } else if (fifth_now) {
        ctx->r17 = (gpr)extra;
        fifth_now = false;
    }
}

// The P5 column's label: the game's is text "#" and the column's number, an icon of D_800859E0's (#1 to #4 the P1 to P4 icons, sprites
// 0x894 to 0x897; its #5 is another, 0x87B). Ours, a P5 in their look, is made from the P4 icon (p5_icon.hpp) and drawn in the label's
// place: func_151EC648, after 0x151ECFB0 (the label queued, conker_possess_join_label), notes the text's entry; func_15043384, before and
// after 0x150438C4 (an icon drawn, conker_possess_icon), has that entry's drawn as sprite 0x897; and func_151ED430, before 0x151ED5F4
// (the sprite's texture loaded, conker_possess_icon_texture), has ours drawn instead, put in our memory (0x80FDE000) each time. All of
// it is the same on every machine (the character select is drawn by the game's own frame on each), and nothing of the game's is kept:
// the texture is made from the game's own.
namespace {
    constexpr uint32_t text_tail = 0x800CBD68;      // the queued text's last entry (func_15042ECC)
    constexpr uint32_t icon_entry_5 = 0x80085A00;   // D_800859E0's #5
    constexpr uint32_t icon_sprite = 0x80090060;    // D_80090060: the sprite func_15043384 draws an icon as (its id first)
    constexpr uint32_t p4_sprite = 0x897;
    constexpr uint32_t p5_texture = 0x80FDE000;     // ours: the P5 icon's texture (32 x 32 RGBA32, 0x1000)
    // (possess51) the P5 icons' text entries queued (the character select's label, the teams screen's 5th player), each with the frame
    // it was queued in (an entry is drawn and freed the frame it's queued, or the next: an older one's place may be another's by then)
    struct P5Label {
        uint32_t entry = 0;
        uint32_t frame = 0;
    };
    P5Label p5_labels[4];
    bool p5_drawing = false;                        // a P5 icon being drawn
    uint32_t p5_made[conker::p5icon::size * conker::p5icon::size];
    bool p5_have = false;

    // The text just queued (func_15042ECC's last entry) as a P5 icon, if it's an icon #5 ("#" and 5).
    void note_p5_label(uint8_t* rdram) {
        const uint32_t entry = (uint32_t)MEM_W(0, (gpr)(int32_t)text_tail);
        if ((entry >> 24) != 0x80 || (entry & 0x7FFFFFu) >= 0x800000u - 0x5Cu || MEM_BU(0xD, (gpr)(int32_t)entry) != (uint8_t)(extra + 1)) {
            return;
        }
        const uint32_t now = conker::netsession::frame();
        P5Label* slot = &p5_labels[0];
        for (P5Label& l : p5_labels) {
            if (l.entry == entry || l.entry == 0 || now - l.frame > 1) {
                slot = &l;
                break;
            }
        }
        slot->entry = entry;
        slot->frame = now;
    }
}

extern "C" void conker_possess_join_label(uint8_t* rdram, recomp_context* ctx) {
    if (!five_columns() || (int32_t)ctx->r16 != extra + 1) {
        return;
    }
    note_p5_label(rdram);
}

extern "C" void conker_possess_icon(uint8_t* rdram, recomp_context* ctx, int where) {
    p5_drawing = false;
    if (where != 0 || (uint32_t)ctx->r17 != icon_entry_5) {
        return;
    }
    const uint32_t entry = (uint32_t)ctx->r16, now = conker::netsession::frame();
    for (P5Label& l : p5_labels) {
        if (l.entry == 0 || l.entry != entry) {
            continue;
        }
        const bool fresh = now - l.frame <= 1;
        l.entry = 0;
        if (fresh && MEM_BU(0xD, (gpr)(int32_t)entry) == (uint8_t)(extra + 1)) {
            MEM_W(0, (gpr)(int32_t)icon_sprite) = (int32_t)p4_sprite;
            p5_drawing = true;
        }
        return;
    }
}

extern "C" void conker_possess_icon_texture(uint8_t* rdram, recomp_context* ctx) {
    if (!p5_drawing) {
        return;
    }
    p5_drawing = false;
    const uint32_t texture = (uint32_t)ctx->r9;
    if ((uint32_t)ctx->r20 != p4_sprite || texture == 0x80000000u) {
        return;
    }
    const uint32_t from = texture < 0x00800000u ? texture | 0x80000000u : texture;
    if ((from >> 24) != 0x80 || (from & 0x7FFFFFu) > 0x800000u - 0x1000u) {
        static bool told = false;
        if (!told) {
            told = true;
            log("f=%u [join] the P4 icon's texture at %08X: not one we can read, the P5 label left as the P4's", (unsigned)conker::netsession::frame(),
                (unsigned)texture);
        }
        return;
    }
    constexpr int n = conker::p5icon::size;
    if (!p5_have) {
        uint32_t p4[n * n];
        for (int y = 0; y < n; y++) {
            for (int x = 0; x < n; x++) {
                const int sx = x ^ ((y & 1) ? 2 : 0);   // the texture's odd rows are kept swizzled, for the RDP's TMEM
                p4[y * n + x] = (uint32_t)MEM_W(4 * (y * n + sx), (gpr)(int32_t)from);
            }
        }
        conker::p5icon::make(p4, p5_made);
        p5_have = true;
        log("f=%u [join] the P5 icon made from the P4's (its texture at %08X)", (unsigned)conker::netsession::frame(), (unsigned)texture);
    }
    for (int y = 0; y < n; y++) {
        for (int x = 0; x < n; x++) {
            const int sx = x ^ ((y & 1) ? 2 : 0);
            MEM_W(4 * (y * n + sx), (gpr)(int32_t)p5_texture) = (int32_t)p5_made[y * n + x];
        }
    }
    ctx->r9 = (gpr)(int32_t)(texture < 0x00800000u ? p5_texture & 0x7FFFFFFFu : p5_texture);
}

// (possess51) The 5th player on the teams screen (func_151E1744, the screen after the character select: each team's characters, a
// player's "P1" to "P4" icon, a computer player's figure, icon #5). Each frame the screen counts the players' teams (D_800E0C00) for the
// list's first D_8008FE40 entries (D_8008FE44, the players' controllers), moves a player's team with its stick (left and right; up and
// down the computer players, as gGameState + 0x22 + 2 * controller has them, func_151DE8F0's), fills the teams with computer players
// after the players and draws an icon for each entry. Online, once the 5th player has joined on the character select, it's on the screen
// as a player: put in the list after the players (controller 4) as the screen starts its frame (0x151E1944: $a2 the players, one more
// with it), its team kept in ours (join_state + 0x14) and its stick's presses made as func_151DE8F0 makes a controller's, from the
// frame's input (0x151E19FC: $a1 its presses' place, less 0x22, sent to ours), and its icon the P5 one (0x151E2164: its text entry noted,
// conker_possess_icon). As the screen's frame ends (0x151E2264) it's taken out of the list again (the entries after it moved down), so the
// menus after the screen and the match's setup have the list as before; the match's setup puts its character in with the team it chose
// here (conker_possess_list). Not in a mode whose screen reads the list's first 4 as controllers (its type 8: func_151E24F0).
namespace {
    constexpr uint32_t teams_players = 0x8008FE40;   // D_8008FE40: the players on the screen
    constexpr uint32_t teams_presses = 0x40;         // join_state + 0x40: the 5th player's presses this frame, as the screen reads a controller's
    int teams_fifth_at = -1;                        // where the 5th player is in the list this frame (-1: not)

    bool teams_mode_ok(uint8_t* rdram) {
        const uint32_t state = word(rdram, game_state);
        if ((state >> 24) != 0x80) {
            return false;
        }
        const int mode = (int)(int8_t)MEM_B(0x42, (gpr)(int32_t)state);
        return mode >= 0 && mode <= 32 && (int8_t)MEM_B(0, (gpr)(int32_t)(modes + (uint32_t)(mode * 10) + 8u)) != 8;
    }

    // A stick's way past 50 from under it (or held there a while, then again), as func_151DE8F0 has a controller's: -1, 0 or 1.
    int stick_press(uint8_t* rdram, int8_t now, uint32_t was_at, uint32_t held_at) {
        const gpr state = (gpr)(int32_t)join_state;
        const int8_t was = MEM_B(was_at, state);
        int dir = 0;
        if (was >= -49 && now < -49) {
            dir = -1;
        } else if (was < 50 && now >= 50) {
            dir = 1;
        }
        MEM_B(was_at, state) = now;
        if (now >= -49 && now < 50) {
            MEM_B(held_at, state) = 0;
        } else {
            const int32_t held = (int32_t)MEM_BU(held_at, state) + MEM_W(0, (gpr)(int32_t)frame_step);
            if (held >= 51) {
                MEM_B(held_at, state) = 40;
                MEM_B(was_at, state) = 0;
            } else {
                MEM_B(held_at, state) = (int8_t)held;
            }
        }
        return dir;
    }
}

int conker::possess_test::fifth_port(const uint8_t* ram) {
    // (possess55, see fifth_map) lock-free: net_session.cpp asks with its lock held
    if (ram == nullptr) {
        return -1;
    }
    const uint8_t b = ram[(fifth_map ^ 3u) - 0x80000000u];
    return (b & 0x80) != 0 ? (int)(b & 3) : -1;
}

// (possess55) func_151E1744's start (the teams screen's frame): the 5th player's entry taken out (fifth_unmap), as the screen has its own
extern "C" void conker_possess_teams_start(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    fifth_unmap(rdram, "the teams screen");
}

bool conker::possess_test::fifth_on_teams(uint8_t* rdram) {
    return online5() && fifth_is_joined(rdram) && teams_mode_ok(rdram);
}

extern "C" void conker_possess_teams(uint8_t* rdram, recomp_context* ctx, int where) {
    const gpr state = (gpr)(int32_t)join_state;
    if (where == 0) {
        // the frame's start: the 5th player put in the list after the players
        teams_fifth_at = -1;
        {
            // (the mode, logged as the screen starts: its index and type, the type 8 one's screen without the 5th player)
            static uint32_t last_frame = 0;
            const uint32_t now = conker::netsession::frame();
            if (online5() && now != last_frame + 1) {
                const uint32_t gs = word(rdram, game_state);
                const int mode = (gs >> 24) == 0x80 ? (int)(int8_t)MEM_B(0x42, (gpr)(int32_t)gs) : -1;
                const int type = mode >= 0 && mode <= 32 ? (int)(int8_t)MEM_B(0, (gpr)(int32_t)(modes + (uint32_t)(mode * 10) + 8u)) : -1;
                log("f=%u [teams] the teams screen: mode %d (type %d)%s", (unsigned)now, mode, type,
                    teams_mode_ok(rdram) ? "" : ", its screen without the 5th player");
            }
            last_frame = now;
        }
        if (!conker::possess_test::fifth_on_teams(rdram)) {
            return;
        }
        const int players = (int)(int32_t)ctx->r6;
        if (players < 0 || players >= 15) {
            return;
        }
        int8_t list[16], team[16];
        for (uint32_t i = 0; i < 16; i++) {
            list[i] = (int8_t)MEM_B(0, (gpr)(int32_t)(match_list + i));
            team[i] = (int8_t)MEM_B(0, (gpr)(int32_t)(match_teams + i));
        }
        int8_t chosen = MEM_B(0x14, state);
        if (MEM_BU(0x15, state) == 0) {
            chosen = fifth_team(rdram, list, team, players);
            MEM_B(0x14, state) = chosen;
            MEM_B(0x15, state) = 1;
            MEM_W(0x10, state) = 0;
            log("f=%u [teams] the 5th player on the teams screen: team %d to start with", (unsigned)conker::netsession::frame(), (int)chosen);
        }
        MEM_B(0, (gpr)(int32_t)(match_list + (uint32_t)players)) = (int8_t)extra;
        MEM_B(0, (gpr)(int32_t)(match_teams + (uint32_t)players)) = chosen;
        ctx->r6 = (gpr)(int32_t)(players + 1);
        teams_fifth_at = players;
    } else if (where == 1) {
        // its presses read (the list's entry $v0, controller 4): from ours
        if (teams_fifth_at < 0 || (int32_t)ctx->r2 != extra) {
            return;
        }
        bool plugged = false;
        uint16_t buttons = 0;
        float x = 0.0f, y = 0.0f;
        if (!(conker::netsession::input(extra, &plugged, &buttons, &x, &y) && plugged)) {
            x = y = 0.0f;
        }
        int8_t sx = 0, sy = 0;
        convert_to_n64_range(x, y, sx, sy);
        const int dx = stick_press(rdram, sx, 0x10, 0x12), dy = stick_press(rdram, sy, 0x11, 0x13);
        uint16_t presses = 0;
        presses |= dy < 0 ? 0x2 : 0;   // down
        presses |= dy > 0 ? 0x1 : 0;   // up
        presses |= dx < 0 ? 0x4 : 0;   // left
        presses |= dx > 0 ? 0x8 : 0;   // right
        MEM_H(teams_presses, state) = (int16_t)presses;
        ctx->r5 = (gpr)(int32_t)(join_state + teams_presses - 0x22u);
    } else if (where == 2) {
        // its icon's text entry, just queued
        if (teams_fifth_at >= 0 && (int8_t)MEM_B(0, (gpr)(int32_t)MEM_W(0x44, ctx->r29)) == (int8_t)extra) {
            note_p5_label(rdram);
        }
    } else {
        // the frame's end: its team kept, and it taken out of the list again
        if (teams_fifth_at < 0) {
            return;
        }
        const uint32_t at = (uint32_t)teams_fifth_at;
        teams_fifth_at = -1;
        if ((int8_t)MEM_B(0, (gpr)(int32_t)(match_list + at)) != (int8_t)extra) {
            return;
        }
        const int8_t chosen = MEM_B(0, (gpr)(int32_t)(match_teams + at));
        if (chosen != MEM_B(0x14, state)) {
            log("f=%u [teams] the 5th player's team: %d", (unsigned)conker::netsession::frame(), (int)chosen);
        }
        MEM_B(0x14, state) = chosen;
        for (uint32_t i = at; i < 15; i++) {
            MEM_B(0, (gpr)(int32_t)(match_list + i)) = MEM_B(0, (gpr)(int32_t)(match_list + i + 1));
            MEM_B(0, (gpr)(int32_t)(match_teams + i)) = MEM_B(0, (gpr)(int32_t)(match_teams + i + 1));
        }
        MEM_B(0, (gpr)(int32_t)(match_list + 15u)) = -1;
        MEM_B(0, (gpr)(int32_t)(match_teams + 15u)) = 0;
    }
}

// func_151E2834, 0x151E2E6C (each character's kind made, D_80087270): the 5th player's, the one it picked, where the mode lets the players
// pick (a mode without teams: its row of characters), as the players' are made from theirs.
extern "C" void conker_possess_join_kind(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    // (possess55: wherever the list has it, or the place of its controller's entry, playing as the game's own player)
    const int at = fifth_mapped(rdram) ? (int)MEM_BU(1, (gpr)(int32_t)fifth_map) : fifth_at(rdram);
    if (!online5() || !fifth_is_joined(rdram) || at < 1) {
        return;
    }
    int row = -1;
    (void)picks_mask(rdram, &row);
    const int c = (int)(int8_t)MEM_B(1, (gpr)(int32_t)join_state);
    if (row < 0 || c < 0 || c >= pick_count) {
        return;
    }
    const uint8_t kind = MEM_BU(6, (gpr)(int32_t)(character_kinds + 8u * (uint32_t)c));
    const uint8_t was = MEM_BU(0, (gpr)(int32_t)(0x80087270u + (uint32_t)at));
    MEM_B(0, (gpr)(int32_t)(0x80087270u + (uint32_t)at)) = (int8_t)kind;
    log("f=%u [join] the 5th player's character %d: kind %u (the setup's %u)", (unsigned)conker::netsession::frame(), c, (unsigned)kind,
        (unsigned)was);
}

// func_151E2834, before 0x151E2914 (conker.toml).
extern "C" void conker_possess_list(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    if (!online5()) {
        return;
    }
    fifth_unmap(rdram, "the match's setup");   // (possess55: a restart's list, its entry from the last match)
    int8_t list[16], team[16], list_was[16], team_was[16];
    for (uint32_t i = 0; i < 16; i++) {
        list[i] = list_was[i] = (int8_t)MEM_B(0, (gpr)(int32_t)(match_list + i));
        team[i] = team_was[i] = (int8_t)MEM_B(0, (gpr)(int32_t)(match_teams + i));
    }
    bool plugged = false;
    uint16_t buttons = 0;
    float x = 0.0f, y = 0.0f;
    // (possess49) and only once it has joined on the character select (conker_possess_join)
    const bool in_input = conker::netsession::input(extra, &plugged, &buttons, &x, &y) && plugged;
    const bool joined = fifth_is_joined(rdram);
    const bool here = in_input && joined;
    // the 5th player's entry (a restart's, playing again's, or one the menus left where they made the list over) taken out first, the
    // entries after it moved down, and put back below: so it's always entry 4, after 4 characters, whatever the menus did
    for (int i = 15; i >= 0; i--) {
        if (list[i] == fifth_entry) {
            for (int j = i; j < 15; j++) {
                list[j] = list[j + 1];
                team[j] = team[j + 1];
            }
            list[15] = -1;
            team[15] = 0;
        }
    }
    int count = 0, players = 0;
    while (count < 16 && list[count] != -1) {
        players += list[count] >= 0 ? 1 : 0;
        count++;
    }
    const int most = mode_most(rdram, players);
    const char* what = !in_input ? "the 5th player not in the frame's input: no character for it" :
        "the 5th player hasn't joined on the character select: no character for it";
    int taken = 0;
    // (possess54) its place: right after the players, the computer players after it. With 4 players entry 4, character 5, through the
    // extra camera as ever; with fewer, a free controller's entry, the game's own player (see fifth_map)
    const int place = players;
    int port = -1;
    if (place < 4) {
        bool used[4] = {};
        for (int i = 0; i < players && i < 16; i++) {
            if (list[i] >= 0 && list[i] < 4) {
                used[list[i]] = true;
            }
        }
        for (int k = 1; k < 4 && port < 0; k++) {
            bool k_plugged = false;
            uint16_t k_buttons = 0;
            float k_x = 0.0f, k_y = 0.0f;
            const bool k_in = conker::netsession::input(k, &k_plugged, &k_buttons, &k_x, &k_y) && k_plugged;
            if (!used[k] && !k_in) {
                port = k;
            }
        }
    }
    if (here) {
        if (count < 1 || place < 1) {
            what = "no players in the list: no character for the 5th player";
        } else if (place < 4 && port < 0) {
            what = "fewer than 4 players and no free controller: no character for the 5th player";
        } else {
            // room for it: the last computer players taken off, past the mode's most less one
            while (count >= most && count > place && list[count - 1] == -2) {
                list[count - 1] = -1;
                team[count - 1] = 0;
                count--;
                taken++;
            }
            if (count >= most || count >= 16) {
                what = "no room for the 5th player's character (the mode's most characters, and no computer player to take off)";
            } else {
                int8_t chosen = fifth_team(rdram, list, team, count);
                // (possess51) the team it chose on the teams screen (conker_possess_teams), where there are teams
                const int8_t on_screen = MEM_B(0x14, (gpr)(int32_t)join_state);
                if (MEM_BU(0x15, (gpr)(int32_t)join_state) != 0 && on_screen >= 0 && on_screen < mode_teams(rdram)) {
                    chosen = on_screen;
                }
                for (int j = 15; j > place; j--) {
                    list[j] = list[j - 1];
                    team[j] = team[j - 1];
                }
                list[place] = port >= 0 ? (int8_t)port : fifth_entry;
                team[place] = chosen;
                count++;
                if (port >= 0) {
                    MEM_B(0, (gpr)(int32_t)screen_players) = (int8_t)(players + 1);
                    MEM_B(0, (gpr)(int32_t)fifth_map) = (int8_t)(0x80 | port);
                    MEM_B(1, (gpr)(int32_t)fifth_map) = (int8_t)place;
                    what = "the 5th player as the game's own player right after the players (a free controller's entry, its input the 5th player's)";
                } else {
                    what = "the 5th player's own character right after the players, through the extra camera";
                }
            }
        }
    }
    const bool changed = std::memcmp(list, list_was, sizeof(list)) != 0 || std::memcmp(team, team_was, sizeof(team)) != 0;
    if (changed) {
        for (uint32_t i = 0; i < 16; i++) {
            MEM_B(0, (gpr)(int32_t)(match_list + i)) = list[i];
            MEM_B(0, (gpr)(int32_t)(match_teams + i)) = team[i];
        }
    }
    // (logged each time a match is set up: the list as it was and as it is)
    char was_text[100] = {}, now_text[100] = {};
    int at_was = 0, at_now = 0;
    for (int i = 0; i < 16; i++) {
        if (list_was[i] != -1 && at_was < (int)sizeof(was_text) - 12) {
            at_was += std::snprintf(was_text + at_was, sizeof(was_text) - (size_t)at_was, " %d/%d", (int)list_was[i], (int)team_was[i]);
        }
        if (list[i] != -1 && at_now < (int)sizeof(now_text) - 12) {
            at_now += std::snprintf(now_text + at_now, sizeof(now_text) - (size_t)at_now, " %d/%d", (int)list[i], (int)team[i]);
        }
    }
    log("f=%u (online) the match is set up: %s (the 5th player %s; %d characters, the mode's most %d%s); entries (controller/team):%s ->%s",
        (unsigned)conker::netsession::frame(), what, here ? "in" : (in_input ? "in the input, not joined" : "not in"), count, most,
        taken > 0 ? (taken == 1 ? ", a computer player taken off" : ", computer players taken off") : "", was_text, now_text);
    const std::string line = std::string("[possess] the match is set up: ") + what;
    conker::netsession::log(line.c_str());
}

// A resync's state (save_states.cpp) has the game's 8 MB; what the extra camera keeps outside them (our memory from 0x80FD0000: its
// buffer, its controller's record, the tables' 5th entries, its view's buffers; and what's kept here between frames) goes with it, so the
// machine loading it goes on with the host's. Not taken between a frame's start and its extra camera's view (drawn from a copy of the
// game taken then), nor while pieces the view drew from are kept for the frame.
namespace {
    constexpr uint32_t ours_start = 0x80FD0000u, ours_size = 0x10000u;
    constexpr uint32_t net_state_version = 1;

    template <typename T> void put_value(std::vector<uint8_t>& out, const T& v) {
        const uint8_t* b = reinterpret_cast<const uint8_t*>(&v);
        out.insert(out.end(), b, b + sizeof(T));
    }

    struct ValueReader {
        const uint8_t* data;
        size_t size;
        size_t at = 0;
        bool bad = false;
        template <typename T> T get() {
            T v{};
            if (at + sizeof(T) > size) {
                bad = true;
                return v;
            }
            std::memcpy(&v, data + at, sizeof(T));
            at += sizeof(T);
            return v;
        }
    };
}

bool conker::possess_test::net_state_ready() {
    return !start_copied && kept_back.empty();
}

void conker::possess_test::net_state_save(uint8_t* rdram, std::vector<uint8_t>& out) {
    put_value(out, net_state_version);
    const uint8_t flags = (uint8_t)((have_args ? 1 : 0) | (extra_wanted ? 2 : 0) | (extra_ready ? 4 : 0) | (extra_shown ? 8 : 0) | (extra_played ? 16 : 0) |
        (slot4_on ? 32 : 0) | (extra_fades_kept ? 64 : 0));
    put_value(out, flags);
    put_value(out, camera_arg2);
    put_value(out, camera_arg3);
    put_value(out, extra_brain_flag);
    put_value(out, extra_pressed);
    put_value(out, online_prev_buttons);
    out.insert(out.end(), extra_fade, extra_fade + 25);
    out.insert(out.end(), extra_hidden, extra_hidden + 25);
    put_value(out, kept_areas_root);
    put_value(out, (uint32_t)kept_areas.size());
    for (size_t i = 0; i < kept_areas.size(); i++) {
        put_value(out, kept_areas[i]);
        put_value(out, kept_area_bits[i]);
    }
    put_value(out, kept_objects_list);
    put_value(out, kept_objects_count);
    put_value(out, (uint32_t)kept_object_fades.size());
    out.insert(out.end(), kept_object_fades.begin(), kept_object_fades.end());
    put_value(out, ours_size);
    out.insert(out.end(), rdram + (ours_start - 0x80000000u), rdram + (ours_start - 0x80000000u) + ours_size);
}

bool conker::possess_test::net_state_load(uint8_t* rdram, const uint8_t* data, size_t size) {
    ValueReader r{ data, size };
    const uint32_t version = r.get<uint32_t>();
    if (r.bad || version != net_state_version) {
        log("(online) a resync's state with the extra camera's of another version (%u): left as it is", (unsigned)version);
        return false;
    }
    const uint8_t flags = r.get<uint8_t>();
    const uint32_t arg2 = r.get<uint32_t>(), arg3 = r.get<uint32_t>(), brain_flag = r.get<uint32_t>();
    const uint16_t pressed_now = r.get<uint16_t>(), prev = r.get<uint16_t>();
    uint8_t fades[25], hidden[25];
    for (int k = 0; k < 25; k++) {
        fades[k] = r.get<uint8_t>();
    }
    for (int k = 0; k < 25; k++) {
        hidden[k] = r.get<uint8_t>();
    }
    const uint32_t root = r.get<uint32_t>();
    const uint32_t areas = r.get<uint32_t>();
    if (r.bad || areas > 65536) {
        log("(online) a resync's state with the extra camera's cut short: left as it is");
        return false;
    }
    std::vector<uint32_t> nodes(areas);
    std::vector<uint8_t> bits(areas);
    for (uint32_t i = 0; i < areas; i++) {
        nodes[i] = r.get<uint32_t>();
        bits[i] = r.get<uint8_t>();
    }
    const uint32_t objects_list_now = r.get<uint32_t>(), objects_count_now = r.get<uint32_t>(), fades_count = r.get<uint32_t>();
    if (r.bad || fades_count > 4096 || r.at + fades_count > size) {
        log("(online) a resync's state with the extra camera's cut short: left as it is");
        return false;
    }
    std::vector<uint8_t> object_fades(data + r.at, data + r.at + fades_count);
    r.at += fades_count;
    const uint32_t region = r.get<uint32_t>();
    if (r.bad || region != ours_size || r.at + region > size) {
        log("(online) a resync's state with the extra camera's cut short: left as it is");
        return false;
    }
    have_args = (flags & 1) != 0;
    extra_wanted = (flags & 2) != 0;
    extra_ready = (flags & 4) != 0;
    extra_shown = (flags & 8) != 0;
    extra_played = (flags & 16) != 0;
    slot4_on = (flags & 32) != 0;
    extra_fades_kept = (flags & 64) != 0;
    camera_arg2 = arg2;
    camera_arg3 = arg3;
    extra_brain_flag = brain_flag;
    extra_pressed = pressed_now;
    online_prev_buttons = prev;
    std::memcpy(extra_fade, fades, sizeof(fades));
    std::memcpy(extra_hidden, hidden, sizeof(hidden));
    kept_areas_root = root;
    kept_areas = std::move(nodes);
    kept_area_bits = std::move(bits);
    kept_objects_list = objects_list_now;
    kept_objects_count = objects_count_now;
    kept_object_fades = std::move(object_fades);
    std::memcpy(rdram + (ours_start - 0x80000000u), data + r.at, region);
    start_copied = false;
    kept_back.clear();
    extra_view_now = false;
    view_pass_on = false;
    log("f=%u (online) a resync's state loaded, the extra camera's with it (ready %d, played %d)", (unsigned)frames, (int)extra_ready, (int)extra_played);
    return true;
}
