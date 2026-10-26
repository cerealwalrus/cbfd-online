// Online: every multiplayer character is there for every player, without the cheat codes.
//
// The game keeps the cheats entered in its Cheats menu in one word, D_800E9D00, a bit each (func_151EE184 sets the code's bit when
// the typed letters match it), in memory only. The multiplayer character select (func_151E0B70, at 0x151E0CD0) adds characters to
// the ones a mode offers for seven of those bits:
//   0x10, 0x20, 0x40, 0x80, 0x100, 0x10000 and 0x20000
// (Conker, Neo Conker, Gregg the Grim Reaper, the cavemen, the weasel henchmen, the villagers and zombies, the sergeant and the Tediz
// leader). Online those bits are set once a frame (func_10011BB8) on every machine (the same frame for all, so the game stays the
// same everywhere); no other cheat is touched.

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
    if (humans >= 3 && (int32_t)ctx->r5 < humans - 1) {
        static int logged = 0;
        if (logged < 4) {
            char line[120];
            std::snprintf(line, sizeof(line), "[teams] %d players: most on one team %d raised to %d", humans, (int)(int32_t)ctx->r5, humans - 1);
            conker::netsession::log(line);
            logged++;
        }
        ctx->r5 = (gpr)(int32_t)(humans - 1);
    }
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
    const int most = (int)(int8_t)MEM_B(5, (gpr)(int32_t)(team_tables + (uint32_t)table * 12u));
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
