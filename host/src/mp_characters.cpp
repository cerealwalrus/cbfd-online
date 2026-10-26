// Online: every multiplayer character is there for every player, without the cheat codes.
//
// The game keeps the cheats entered in its Cheats menu in one word, D_800E9D00, a bit each (func_151EE184 sets the code's bit when
// the typed letters match it), in memory only. The multiplayer character select (func_151E0B70, at 0x151E0CD0) adds characters to
// the ones a mode offers for seven of those bits:
//   0x10, 0x20, 0x40, 0x80, 0x100, 0x10000 and 0x20000
// (Conker, Neo Conker, Gregg the Grim Reaper, the cavemen, the weasel henchmen, the villagers and zombies, the sergeant and the Tediz
// leader). Online those bits are set once a frame (func_10011BB8) on every machine (the same frame for all, so the game stays the
// same everywhere); no other cheat is touched.

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "recomp.h"

#include "conker.hpp"

namespace {
    constexpr uint32_t cheats = 0x800E9D00;
    constexpr uint32_t character_cheats = 0x10u | 0x20u | 0x40u | 0x80u | 0x100u | 0x10000u | 0x20000u;
}

// Every frame (func_10011BB8, the game thread).
extern "C" void conker_mp_characters(uint8_t* rdram) {
    if (!conker::netsession::active()) {
        return;
    }
    const uint32_t value = (uint32_t)MEM_W(0, (gpr)(int32_t)cheats);
    if ((value & character_cheats) != character_cheats) {
        MEM_W(0, (gpr)(int32_t)cheats) = (int32_t)(value | character_cheats);
    }
}

// 0.10.23: online, the teams screen (func_151E1744) with three or four players lets anyone change to either team.
//
// The screen takes its limits from the mode's team table, 12 bytes an entry at 0x800AB710 (the mode's index at D_800AB68C + mode * 10
// + 5): the number of teams, then for under three players and for three or more the fewest characters, the most characters and the most
// players on one team, then the second team's own most, and the teams' colours. With four players the war's is two a team, so with
// two on each nobody could change (a change to a full team is undone). Online the most players on a team is raised to all but one,
// at the point both paths have loaded it ($a1, at 0x151E18E4).
//
// The screen then fills each team up to the larger team's size with computer players, as many as the mode's most characters allows
// for the team size, but not past the larger team: three against one in the war (four characters at most) would have had a fifth
// character, a computer player with the one. Those added past the mode's most characters are taken off the end of the list again,
// right after they are added (at 0x151E1E4C, $s0 the list's length), so the screen and the game see the teams as they were chosen.
namespace {
    constexpr uint32_t team_humans = 0x8008FE40;     // the players on the screen (s8)
    constexpr uint32_t team_list = 0x8008FE44;       // 16 entries: a player, -2 a computer player, -1 none
    constexpr uint32_t team_of = 0x800E0C00;         // each entry's team
    constexpr uint32_t game_state = 0x8008FDD4;      // gGameState (the mode at +0x42)
    constexpr uint32_t modes = 0x800AB68C;           // 10 bytes a mode, the team table's index at +5
    constexpr uint32_t team_tables = 0x800AB710;     // 12 bytes an entry

    int humans_now(uint8_t* rdram) {
        return (int)(int8_t)MEM_B(0, (gpr)(int32_t)team_humans);
    }
}

// func_151E1744 at 0x151E18E4: $a1 the most players on one team.
extern "C" void conker_team_caps(uint8_t* rdram, recomp_context* ctx) {
    if (!conker::netsession::active()) {
        return;
    }
    const int humans = humans_now(rdram);
    if (humans < 3) {
        return;
    }
    // (0.10.26: all of them on one team, the other then the computer players'; and as many characters as the mode has with fewer
    // players, $s3, the most of the entry's two (+2 under three players, +5 three or more), $v0 the entry)
    const int most_before = (int)(int32_t)ctx->r19;
    const int most = std::max(most_before, (int)(int8_t)MEM_B(2, ctx->r2));
    static int logged = 0;
    if (((int32_t)ctx->r5 < humans || most != most_before) && logged < 4) {
        char line[160];
        std::snprintf(line, sizeof(line), "[teams] %d players: most on one team %d raised to %d, most characters %d to %d", humans, (int)(int32_t)ctx->r5, humans,
            most_before, most);
        conker::netsession::log(line);
        logged++;
    }
    if ((int32_t)ctx->r5 < humans) {
        ctx->r5 = (gpr)(int32_t)humans;
    }
    ctx->r19 = (gpr)(int32_t)most;
}

// func_151E1744 at 0x151E1E4C: $s0 the list's length, with the computer players added.
extern "C" void conker_team_trim(uint8_t* rdram, recomp_context* ctx) {
    if (!conker::netsession::active()) {
        return;
    }
    const int humans = humans_now(rdram);
    if (humans < 3) {
        return;
    }
    const uint32_t state = (uint32_t)MEM_W(0, (gpr)(int32_t)game_state);
    if ((state >> 24) != 0x80) {
        return;
    }
    const int mode = (int)(int8_t)MEM_B(0x42, (gpr)(int32_t)state);
    if (mode < 0 || mode > 32) {
        return;
    }
    const int table = (int)(int8_t)MEM_B(0, (gpr)(int32_t)(modes + (uint32_t)(mode * 10) + 5u));
    if (table < 0 || table > 16) {
        return;
    }
    // (0.10.26: the most characters as conker_team_caps makes it: the larger of the entry's two)
    const uint32_t entry = team_tables + (uint32_t)table * 12u;
    const int most = std::max((int)(int8_t)MEM_B(5, (gpr)(int32_t)entry), (int)(int8_t)MEM_B(2, (gpr)(int32_t)entry));
    int count = (int)(int16_t)(int32_t)ctx->r16;
    const int before = count;
    while (count > most && count > humans && count <= 16 && (int8_t)MEM_B(0, (gpr)(int32_t)(team_list + (uint32_t)(count - 1))) == -2) {
        MEM_B(0, (gpr)(int32_t)(team_list + (uint32_t)(count - 1))) = -1;
        MEM_B(0, (gpr)(int32_t)(team_of + (uint32_t)(count - 1))) = 0;
        count--;
    }
    if (count != before) {
        static int logged = 0;
        if (logged < 4) {
            char line[120];
            std::snprintf(line, sizeof(line), "[teams] %d players: %d computer players past the mode's %d characters taken off", humans, before - count, most);
            conker::netsession::log(line);
            logged++;
        }
        ctx->r16 = (gpr)(int32_t)count;
    }
}

// 0.10.24: online, the race takes up to four players, as the other modes do. The game keeps it to two in three places, each
// with the menu's category (gGameState +0x2C) at 7, the race's:
//  - the character select (func_151E0B70) lets controllers 2 to 4 join only under a count it sets to 2 for the race (4 otherwise),
//    a byte at $sp + 0x33: set to 4 at 0x151E0DA0, where both paths meet;
//  - the select's drawing (func_151EC648, its case 4) draws 2 columns from x 0x70 for the race (4 from 0x28 otherwise): $s5 and the
//    byte at $sp + 0xF7, set to 0x28 and 4 at 0x151ECCA8;
//  - the characters shown on it (func_151E7F60) stand at the places of players 2 and 3 of the four (the player's number + 1): back
//    to the player's own place ($v1 = $t2) at 0x151E8004, as there are four now (a fifth place, past the table's four, is no place).
// The race's own limit on racers (D_800AB74C's row for it: 8 with one player, 4 with two to four) has room for four players.
extern "C" void conker_race_join(uint8_t* rdram, recomp_context* ctx) {
    if (conker::netsession::active()) {
        MEM_B(0x33, ctx->r29) = 4;
    }
}

extern "C" void conker_race_columns(uint8_t* rdram, recomp_context* ctx) {
    if (conker::netsession::active()) {
        ctx->r21 = (gpr)(int32_t)0x28;
        MEM_B(0xF7, ctx->r29) = 4;
    }
}

extern "C" void conker_race_places(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (conker::netsession::active()) {
        ctx->r3 = ctx->r10;
    }
}

// 0.10.26: online, computer players with three or four players too, as many as the mode has with fewer players.
//
// The modes with a character select count their computer players on the screen after it (func_151E1214), up to the most characters
// for the number of players: a row of four (one to four players) at D_800AB74C, the mode's row from D_8008FEE4 (most modes 10, 8, 4, 4;
// the race 8, 4, 4, 4). With four players that was four: no computer players. Online with three or four the most is the row's largest
// ($s4, loaded in the delay slot before 0x151E1350, $v0 the row, $a1 the players).
extern "C" void conker_bots_most(uint8_t* rdram, recomp_context* ctx) {
    if (!conker::netsession::active()) {
        return;
    }
    const int humans = (int)(int32_t)ctx->r5;
    if (humans < 3) {
        return;
    }
    int most = (int)(int32_t)ctx->r20;
    for (uint32_t i = 0; i < 4; i++) {
        most = std::max(most, (int)(int8_t)MEM_B(i, ctx->r2));
    }
    if (most != (int)(int32_t)ctx->r20) {
        static int logged = 0;
        if (logged < 4) {
            char line[120];
            std::snprintf(line, sizeof(line), "[bots] %d players: most characters %d raised to %d", humans, (int)(int32_t)ctx->r20, most);
            conker::netsession::log(line);
            logged++;
        }
        ctx->r20 = (gpr)(int32_t)most;
    }
}

// 0.10.26: online, the teams screen's team sizes (D_8008FDC4, which moves places from one team to the other: L and R on the screen)
// with three or four players too: the game puts it back to 0 with three or more ($t8 the players, compared with 3 at 0x151E1C94).
extern "C" void conker_team_sizes(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (conker::netsession::active()) {
        ctx->r24 = 0;
    }
}
