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
#include <cstring>

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
    // (0.10.93, logging, for the 10 a team: whether the characters past the game's 8 have places of their own) in a multiplayer
    // match (D_800BE616), each character's place (gObjects, +0x14 x, +0x18 y, +0x1C z) where it first stands and each time it jumps
    // there (a spawn or a respawn: over 400 units in a frame), and two that spawn within 40 units of each other
    static bool in_match = false;
    static float last[16][3];
    static bool have[16];
    static int lines = 0;
    const bool match = MEM_BU(0, (gpr)(int32_t)0x800BE616) != 0;
    if (match != in_match) {
        in_match = match;
        for (int k = 0; k < 16; k++) {
            have[k] = false;
        }
    }
    if (!match || lines >= 400) {
        return;
    }
    for (int k = 0; k < 16; k++) {
        const uint32_t object = 0x800CC2D0u + (uint32_t)k * 0x32Cu;
        if ((int32_t)MEM_W(0, (gpr)(int32_t)object) == 0) {
            have[k] = false;
            continue;
        }
        float p[3];
        for (int i = 0; i < 3; i++) {
            const uint32_t bits = (uint32_t)MEM_W(0x14 + 4 * i, (gpr)(int32_t)object);
            std::memcpy(&p[i], &bits, 4);
        }
        const float dx = p[0] - last[k][0], dy = p[1] - last[k][1], dz = p[2] - last[k][2];
        const bool jumped = !have[k] || dx * dx + dy * dy + dz * dz > 400.0f * 400.0f;
        std::memcpy(last[k], p, sizeof(p));
        have[k] = true;
        if (!jumped) {
            continue;
        }
        int close_to = -1;
        for (int j = 0; j < 16; j++) {
            if (j == k || !have[j]) {
                continue;
            }
            const float ex = p[0] - last[j][0], ez = p[2] - last[j][2];
            if (ex * ex + ez * ez < 40.0f * 40.0f) {
                close_to = j;
            }
        }
        char line[200];
        std::snprintf(line, sizeof(line), "[spawn] f=%u character %d (kind %02X) at %.0f %.0f %.0f%s", (unsigned)conker::netsession::frame(), k,
            (unsigned)MEM_BU(4, (gpr)(int32_t)object), p[0], p[1], p[2], close_to >= 0 ? " (on top of another)" : "");
        conker::netsession::log(line);
        if (close_to >= 0) {
            std::snprintf(line, sizeof(line), "[spawn]   within 40 of character %d", close_to);
            conker::netsession::log(line);
        }
        lines++;
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

    // (possess47) online, a match of 5 players (the possess test's): one character place less than the mode has, the 5th player's own
    // character's (possess_test.cpp's conker_possess_list puts it in the list as the match is set up), so the computer players never
    // take it and the characters never pass the mode's most: the war's 12 are the 4 players, the 5th and 7 computer players. Never under
    // the players, nor under 2 (the match's setup makes the room there, taking a computer player off). (possess49) Once the 5th player
    // has joined on the character select (its P5 column, possess_test.cpp): without it the menus are the game's.
    int fifth_place(int most, int humans) {
        if (most <= 2) {
            return most;
        }
        return std::max(most - 1, humans);
    }
}

// func_151E1744 at 0x151E18E4: $a1 the most players on one team.
extern "C" void conker_team_caps(uint8_t* rdram, recomp_context* ctx) {
    if (!conker::netsession::active()) {
        return;
    }
    const int humans = humans_now(rdram);
    const bool five = conker::possess_test::fifth_joined(rdram);   // (possess47: see fifth_place; possess49: once the 5th player has joined)
    if (humans < 2 && !five) {   // (0.10.91: two players as well, with the computer players conker_bots_most lets them have)
        return;
    }
    // (0.10.26: all of them on one team, the other then the computer players'; and as many characters as the mode has with fewer
    // players, $s3, the most of the entry's two (+2 under three players, +5 three or more), $v0 the entry)
    const int most_before = (int)(int32_t)ctx->r19;
    int most = humans >= 2 ? std::max(most_before, (int)(int8_t)MEM_B(2, ctx->r2)) : most_before;
    if (five) {
        most = fifth_place(most, humans);
    }
    const int team_most = humans >= 2 ? std::max((int)(int32_t)ctx->r5, humans) : (int)(int32_t)ctx->r5;
    static int logged = 0;
    if ((team_most != (int)(int32_t)ctx->r5 || most != most_before) && logged < 4) {
        char line[200];
        std::snprintf(line, sizeof(line), "[teams] %d players: most on one team %d raised to %d, most characters %d to %d%s", humans, (int)(int32_t)ctx->r5,
            team_most, most_before, most, five ? " (one for the 5th player's own)" : "");
        conker::netsession::log(line);
        logged++;
    }
    ctx->r5 = (gpr)(int32_t)team_most;
    ctx->r19 = (gpr)(int32_t)most;
}

// func_151E1744 at 0x151E1E4C: $s0 the list's length, with the computer players added.
extern "C" void conker_team_trim(uint8_t* rdram, recomp_context* ctx) {
    if (!conker::netsession::active()) {
        return;
    }
    const int humans = humans_now(rdram);
    const bool five = conker::possess_test::fifth_joined(rdram);   // (possess47: see fifth_place; possess49: once the 5th player has joined)
    if (humans < 2 && !five) {   // (0.10.91: two players as well, with the computer players conker_bots_most lets them have)
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
    // (0.10.26: the most characters as conker_team_caps makes it: the larger of the entry's two; with one player the game's own, +2)
    const uint32_t entry = team_tables + (uint32_t)table * 12u;
    int most = humans >= 2 ? std::max((int)(int8_t)MEM_B(5, (gpr)(int32_t)entry), (int)(int8_t)MEM_B(2, (gpr)(int32_t)entry))
                           : (int)(int8_t)MEM_B(2, (gpr)(int32_t)entry);
    if (five) {
        most = fifth_place(most, humans);
    }
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
// (0.10.91: and with two: as many as with one, the row's largest: two players against eight in the war)
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
    const bool five = conker::possess_test::fifth_joined(rdram);   // (possess47: see fifth_place; possess49: once the 5th player has joined)
    // (0.10.91: with two players too: the war's row gives two players four computer players, three and four players eight)
    if (humans < 2 && !five) {
        return;
    }
    int most = (int)(int32_t)ctx->r20;
    if (humans >= 2) {
        for (uint32_t i = 0; i < 4; i++) {
            most = std::max(most, (int)(int8_t)MEM_B(i, ctx->r2));
        }
    }
    if (five) {
        most = fifth_place(most, humans);
    }
    if (most != (int)(int32_t)ctx->r20) {
        static int logged = 0;
        if (logged < 4) {
            char line[160];
            std::snprintf(line, sizeof(line), "[bots] %d players: most characters %d to %d%s", humans, (int)(int32_t)ctx->r20, most,
                five ? " (one for the 5th player's own)" : "");
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

// 0.10.93: online, up to 10 characters on one team (the game's 8), as long as the match's most characters isn't passed: the teams
// screen (func_151E1744) works out each team's size from the base size and the balance (D_8008FDC4), then keeps each between 1 and
// 8, $v1 (or the second team's own most, the mode's), at 0x151E1D40 for each team. Online 8 is the most characters ($s3, 12 in the
// war) less the players (so 2 players against 10 computer players, 3 against 9, 4 against 8: never more characters than the mode
// allows), up to 10. The screen draws a figure for each, and the game sets up the characters the list has.
extern "C" void conker_team_most(uint8_t* rdram, recomp_context* ctx) {
    if (!conker::netsession::active() || (int32_t)ctx->r3 != 8) {
        return;
    }
    const int humans = humans_now(rdram);
    const int most = std::min(10, (int)(int32_t)ctx->r19 - std::max(humans, 1));
    if (most > 8) {
        ctx->r3 = (gpr)(int32_t)most;
    }
}

// 0.10.93: online, the teams screen's move sound (0x4FF, $sp + 0x74, played at 0x151E21C0) only for a press that changed something:
// past a limit (a full team, the bots or the balance at their most) the game still played it and, for the balance, kept counting
// (D_8008FDC4), so the presses back did nothing for a while. At 0x151E21C0 the frame's list of characters (D_8008FE44) and their
// teams (D_800E0C00) are made: if they're as they were last frame, the sound is taken out and the bots' count (D_8008FDA0) and the
// balance are put back as they were. The game's own state, the same on every machine.
namespace {
    constexpr uint32_t team_bots = 0x8008FDA0;
    constexpr uint32_t team_balance = 0x8008FDC4;
}

extern "C" void conker_team_sound(uint8_t* rdram, recomp_context* ctx) {
    static int8_t prev_list[16], prev_team[16], prev_bots = 0, prev_balance = 0;
    static uint32_t prev_frame = 0;
    static bool have = false;
    if (!conker::netsession::active()) {
        have = false;
        return;
    }
    int8_t list[16], team[16];
    for (uint32_t i = 0; i < 16; i++) {
        list[i] = (int8_t)MEM_B(0, (gpr)(int32_t)(team_list + i));
        team[i] = (int8_t)MEM_B(0, (gpr)(int32_t)(team_of + i));
    }
    const uint32_t now = conker::netsession::frame();
    const bool same = have && now == prev_frame + 1 && std::memcmp(list, prev_list, sizeof(list)) == 0 && std::memcmp(team, prev_team, sizeof(team)) == 0;
    if (same && (int32_t)MEM_W(0x74, ctx->r29) == 0x4FF) {
        MEM_W(0x74, ctx->r29) = 0;
        MEM_B(0, (gpr)(int32_t)team_bots) = prev_bots;
        MEM_B(0, (gpr)(int32_t)team_balance) = prev_balance;
    }
    std::memcpy(prev_list, list, sizeof(list));
    std::memcpy(prev_team, team, sizeof(team));
    prev_bots = (int8_t)MEM_B(0, (gpr)(int32_t)team_bots);
    prev_balance = (int8_t)MEM_B(0, (gpr)(int32_t)team_balance);
    prev_frame = now;
    have = true;
}

