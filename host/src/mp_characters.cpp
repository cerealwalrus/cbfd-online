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
#include <cctype>
#include <cstdlib>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
#include <map>
#include <set>
#include <string>

#include "recomp.h"
#include "miniz.h"

#include "conker.hpp"

#if defined(CONKER_RT64)
#include "recompui/recompui.h"
#include "util/file.h"
#endif

namespace {
    constexpr uint32_t cheats = 0x800E9D00;
    constexpr uint32_t character_cheats = 0x10u | 0x20u | 0x40u | 0x80u | 0x100u | 0x10000u | 0x20000u;
}

// ---- (0.11.4, for Koala Kong's slot) The characters' models surveyed: characters.log ----
// Each character (gObjects, 0x800CC2D0 + k*0x32C) is drawn with a model by number: its list of them at +0x2C4 (+0x2C8 how many, from
// the most detailed down, model_detail.cpp) and the one in use at +0x4, which func_150837D4 sets (the object's index in $a0, the model's
// number in $a1) and looks up in D_800D1C90's table of loaded models (a pointer each, by number). Once a match has its characters, and
// again whenever a character's model changes, each one's numbers, the loaded model's address and its first 0x40 bytes as words (its
// layout: pointers and sizes, to find the skeleton the next build rigs Kong to). In memory only, written on this machine.
namespace {
    std::FILE* survey_file = nullptr;
    void survey(const char* line) {
        if (survey_file == nullptr) {
            survey_file = std::fopen("characters.log", "w");
        }
        if (survey_file != nullptr) {
            std::fprintf(survey_file, "%s\n", line);
            std::fflush(survey_file);
        }
    }
    uint8_t surveyed_model[16] = {};
    bool surveyed_have[16] = {};
    uint32_t survey_lines = 0;
    std::set<uint32_t> records_told;
    char* line_buffer() {
        static char b[64];
        return b;
    }

    void characters_survey(uint8_t* rdram) {
        if (MEM_BU(0, (gpr)(int32_t)0x800BE616) == 0 || survey_lines > 600) {
            for (bool& h : surveyed_have) {
                h = false;
            }
            return;
        }
        for (int k = 0; k < 16; k++) {
            const uint32_t object = 0x800CC2D0u + (uint32_t)k * 0x32Cu;
            const gpr o = (gpr)(int32_t)object;
            if ((int32_t)MEM_W(0, o) == 0) {
                continue;
            }
            const uint8_t model = MEM_BU(4, o);
            if (surveyed_have[k] && surveyed_model[k] == model) {
                continue;
            }
            surveyed_have[k] = true;
            surveyed_model[k] = model;
            const uint32_t list = (uint32_t)MEM_W(0x2C4, o);
            const int count = (int)MEM_BU(0x2C8, o);
            std::string models;
            for (int i = 0; i < count && i < 8 && (list >> 24) == 0x80; i++) {
                char piece[8];
                std::snprintf(piece, sizeof(piece), " %02X", (unsigned)MEM_BU((uint32_t)i, (gpr)(int32_t)list));
                models += piece;
            }
            const uint32_t record = (uint32_t)MEM_W(4u * model, (gpr)(int32_t)0x800D1C90);
            char line[256];
            std::snprintf(line, sizeof(line), "character %d: model %02X (its models:%s), loaded at %08X, team %u, player slot %d", k, (unsigned)model,
                models.c_str(), (unsigned)record, (unsigned)MEM_BU(0x127, o), (int)(int8_t)MEM_B(0x1CA, o));
            survey(line);
            survey_lines++;
            if ((record >> 24) != 0x80 || records_told.count(record) != 0) {
                continue;
            }
            records_told.insert(record);
            std::string words;
            for (uint32_t w = 0; w < 0x40; w += 4) {
                char piece[12];
                std::snprintf(piece, sizeof(piece), " %08X", (unsigned)MEM_W(w, (gpr)(int32_t)record));
                words += piece;
            }
            survey(("  model " + std::to_string(model) + "'s record:" + words).c_str());
            // and the first 0x40 bytes at each pointer in it (one level), to see which is the skeleton
            for (uint32_t w = 0; w < 0x40; w += 4) {
                const uint32_t to = (uint32_t)MEM_W(w, (gpr)(int32_t)record);
                if ((to >> 24) != 0x80 || (to & 3u) != 0 || (to & 0x00FFFFFFu) >= 0x800000u) {
                    continue;
                }
                std::string inner;
                for (uint32_t x = 0; x < 0x20; x += 4) {
                    char piece[12];
                    std::snprintf(piece, sizeof(piece), " %08X", (unsigned)MEM_W(x, (gpr)(int32_t)to));
                    inner += piece;
                }
                char head[64];
                std::snprintf(head, sizeof(head), "    +%02X -> %08X:", (unsigned)w, (unsigned)to);
                survey((head + inner).c_str());
            }
            survey_lines += 16;
        }
    }
}

// (0.11.4, for Koala Kong's slot, the second look) The characters' skeletons: once, 3 seconds into a match, the game's memory is scanned
// for skeleton tables as the model bank files have them (16 bytes a bone: its parent, its number twice, 0, then its offset from the
// parent, 3 floats; bone 0's parent 0xFF, the numbers counting up), and for each one found: where, how many bones, their parents, the
// model file around it (the file's header gives the skeleton's offset at +0x10 and its size at +0x14) and which characters have a word
// pointing into that file, and where in the character. Its layout only, no positions.
namespace {
    void skeleton_survey(uint8_t* rdram) {
        static uint32_t match_frames = 0;
        static bool done = false;
        if (MEM_BU(0, (gpr)(int32_t)0x800BE616) == 0) {
            match_frames = 0;
            done = false;
            return;
        }
        if (done || ++match_frames < 90) {
            return;
        }
        done = true;
        survey("skeletons in memory:");
        int found = 0;
        for (uint32_t a = 0x80000400u; a < 0x80800000u - 16u * 8u && found < 40; a += 4) {
            const gpr at = (gpr)(int32_t)a;
            if ((uint32_t)MEM_W(0, at) != 0xFF000000u) {   // bone 0: parent FF, number 0, 0, 0
                continue;
            }
            int n = 1;
            while (n < 80 && a + 16u * (uint32_t)n < 0x80800000u) {
                const uint32_t w = (uint32_t)MEM_W(16u * (uint32_t)n, at);
                const uint32_t parent = w >> 24, number = (w >> 16) & 0xFFu, twice = (w >> 8) & 0xFFu, zero = w & 0xFFu;
                // (the third look: a bone's second number is allowed to differ, its mirror's maybe: the 38 bones found had no legs)
                if (number != (uint32_t)n || twice >= 96u || zero != 0 || parent >= (uint32_t)n) {
                    break;
                }
                n++;
            }
            if (n < 6) {
                continue;
            }
            found++;
            std::string parents;
            for (int i = 1; i < n; i++) {
                const uint32_t w = (uint32_t)MEM_W(16u * (uint32_t)i, at);
                char piece[16];
                if (((w >> 8) & 0xFFu) != (uint32_t)i) {
                    std::snprintf(piece, sizeof(piece), " %u(%u)", (unsigned)(w >> 24), (unsigned)((w >> 8) & 0xFFu));
                } else {
                    std::snprintf(piece, sizeof(piece), " %u", (unsigned)(w >> 24));
                }
                parents += piece;
            }
            {
                // and what follows the last bone (why the run stopped)
                char piece[64];
                std::snprintf(piece, sizeof(piece), " | next %08X %08X", (unsigned)MEM_W(16u * (uint32_t)n, at), (unsigned)MEM_W(16u * (uint32_t)n + 4u, at));
                parents += piece;
            }
            // the model file it's in: a start S below it whose +0x10 is the skeleton's offset and +0x14 its size
            uint32_t file = 0;
            for (uint32_t back = 0x38; back < 0x40000u && back <= a - 0x80000000u; back += 4) {
                const gpr st = (gpr)(int32_t)(a - back);
                if ((uint32_t)MEM_W(0x10, st) == back && (uint32_t)MEM_W(0x14, st) == 16u * (uint32_t)n) {
                    file = a - back;
                    break;
                }
            }
            char line[400];
            std::snprintf(line, sizeof(line), "  at %08X: %d bones, parents:%s", (unsigned)a, n, parents.c_str());
            survey(line);
            // (the third look) who points at it: every word in memory from 0x100 below it to its end, and for each, the characters
            // pointing at that word's own block (within 0x400 below it), or at the word itself
            int holders = 0;
            for (uint32_t h = 0x80000400u; h < 0x80800000u && holders < 12; h += 4) {
                const uint32_t v = (uint32_t)MEM_W(0, (gpr)(int32_t)h);
                if (v < a - 0x100u || v >= a + 16u * (uint32_t)n) {
                    continue;
                }
                holders++;
                std::string who;
                for (int k = 0; k < 16; k++) {
                    const uint32_t object = 0x800CC2D0u + (uint32_t)k * 0x32Cu;
                    if ((int32_t)MEM_W(0, (gpr)(int32_t)object) == 0) {
                        continue;
                    }
                    if (h >= object && h < object + 0x32Cu) {
                        char piece[48];
                        std::snprintf(piece, sizeof(piece), " character %d's own +%03X", k, (unsigned)(h - object));
                        who += piece;
                    }
                    for (uint32_t off = 0; off < 0x32Cu; off += 4) {
                        const uint32_t p2 = (uint32_t)MEM_W(off, (gpr)(int32_t)object);
                        if (p2 <= h && h - p2 < 0x400u) {
                            char piece[64];
                            std::snprintf(piece, sizeof(piece), " character %d's +%03X -> %08X (+%X)", k, (unsigned)off, (unsigned)p2, (unsigned)(h - p2));
                            who += piece;
                        }
                    }
                }
                std::snprintf(line, sizeof(line), "    %08X holds %08X (+%X from the bones)%s", (unsigned)h, (unsigned)v, (unsigned)(v - (a - 0x100u)) , "");
                survey((line + who).c_str());
            }
            if (file != 0) {
                std::string words;
                for (uint32_t w = 0; w < 0x38; w += 4) {
                    char piece[12];
                    std::snprintf(piece, sizeof(piece), " %08X", (unsigned)MEM_W(w, (gpr)(int32_t)file));
                    words += piece;
                }
                survey(("    in the model file at " + std::to_string(file - 0x80000000u) + " (hex " + [&] { char b[12]; std::snprintf(b, sizeof(b), "%08X", (unsigned)file); return std::string(b); }() + "), its header:" + words).c_str());
                const uint32_t end = file + (uint32_t)MEM_W(0x30, (gpr)(int32_t)file);
                for (int k = 0; k < 16; k++) {
                    const uint32_t object = 0x800CC2D0u + (uint32_t)k * 0x32Cu;
                    if ((int32_t)MEM_W(0, (gpr)(int32_t)object) == 0) {
                        continue;
                    }
                    for (uint32_t off = 0; off < 0x32Cu; off += 4) {
                        const uint32_t v = (uint32_t)MEM_W(off, (gpr)(int32_t)object);
                        if (v >= file && v < std::max(end, file + 0x100u)) {
                            std::snprintf(line, sizeof(line), "    character %d's +%03X points into it (+%X)", k, (unsigned)off, (unsigned)(v - file));
                            survey(line);
                        }
                    }
                }
            }
        }
        std::snprintf(line_buffer(), 64, "%d skeletons found", found);
        survey(line_buffer());
    }
}

// Every frame (func_10011BB8, the game thread).
extern "C" void conker_kill_feed_frame(uint8_t* rdram);
void conker_cave_menu(uint8_t* rdram);   // (below, with the cave)
void conker_cave_match(uint8_t* rdram);  // (below: the saved spawn)

extern "C" void conker_mp_characters(uint8_t* rdram) {
    conker_cave_menu(rdram);
    conker_cave_match(rdram);
    conker_kill_feed_frame(rdram);   // (0.11.4, the kill feed: kill_feed.cpp, offline too)
    characters_survey(rdram);   // (0.11.4, offline too)
    skeleton_survey(rdram);
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
    // (0.11.4, 5 players) where each character first stood in the match, and the match's frames so far: the 5th player's character
    // (character 4) was put on top of player 1's a frame after the start (f=1161, both at 2009 -6199), its own place being 1280 -6222
    static float first[16][3];
    static bool have_first[16];
    static uint32_t match_frames = 0;
    const bool match = MEM_BU(0, (gpr)(int32_t)0x800BE616) != 0;
    if (match != in_match) {
        in_match = match;
        match_frames = 0;
        for (int k = 0; k < 16; k++) {
            have[k] = false;
            have_first[k] = false;
        }
    }
    if (!match) {
        return;
    }
    match_frames++;
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
        if (!have_first[k]) {
            std::memcpy(first[k], p, sizeof(p));
            have_first[k] = true;
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
        // put on top of another as the match starts (its first 2 seconds): back where it first stood, if that was a place of its own
        if (close_to >= 0 && match_frames < 60 && have_first[k]) {
            bool free_place = true;
            for (int j = 0; j < 16; j++) {
                if (j != k && have[j]) {
                    const float fx = first[k][0] - last[j][0], fz = first[k][2] - last[j][2];
                    free_place = free_place && (fx * fx + fz * fz >= 40.0f * 40.0f);
                }
            }
            if (free_place) {
                for (int i = 0; i < 3; i++) {
                    uint32_t bits;
                    std::memcpy(&bits, &first[k][i], 4);
                    MEM_W(0x14 + 4 * i, (gpr)(int32_t)object) = (int32_t)bits;
                }
                std::memcpy(last[k], first[k], sizeof(p));
                if (lines < 400) {
                    char moved[160];
                    std::snprintf(moved, sizeof(moved), "[spawn] f=%u character %d put on top of character %d: back to its own place, %.0f %.0f %.0f",
                        (unsigned)conker::netsession::frame(), k, close_to, first[k][0], first[k][1], first[k][2]);
                    conker::netsession::log(moved);
                    lines++;
                }
                continue;
            }
        }
        if (lines >= 400) {
            continue;
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
    // (possess51) the 5th player on the screen as a player (possess_test.cpp, conker_possess_teams): a player more, and no place kept for it
    const bool shown = conker::possess_test::fifth_on_teams(rdram);
    const int players = humans + (shown ? 1 : 0);
    if (humans < 2 && !five) {   // (0.10.91: two players as well, with the computer players conker_bots_most lets them have)
        return;
    }
    // (0.10.26: all of them on one team, the other then the computer players'; and as many characters as the mode has with fewer
    // players, $s3, the most of the entry's two (+2 under three players, +5 three or more), $v0 the entry)
    const int most_before = (int)(int32_t)ctx->r19;
    int most = humans >= 2 ? std::max(most_before, (int)(int8_t)MEM_B(2, ctx->r2)) : most_before;
    if (five && !shown) {
        most = fifth_place(most, humans);
    }
    const int team_most = players >= 2 ? std::max((int)(int32_t)ctx->r5, players) : (int)(int32_t)ctx->r5;
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
    const bool shown = conker::possess_test::fifth_on_teams(rdram);   // (possess51: in the list as a player, see conker_team_caps)
    if (five && !shown) {
        most = fifth_place(most, humans);
    }
    const int players = humans + (shown ? 1 : 0);
    int count = (int)(int16_t)(int32_t)ctx->r16;
    const int before = count;
    while (count > most && count > players && count <= 16 && (int8_t)MEM_B(0, (gpr)(int32_t)(team_list + (uint32_t)(count - 1))) == -2) {
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
    const int humans = humans_now(rdram) + (conker::possess_test::fifth_on_teams(rdram) ? 1 : 0);   // (possess51: the 5th player's too)
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


// possess51: online, the teams screen's computer players with two teams, a team at a time (func_151E1744, before 0x151E1D74, where both
// ways of working out the teams' sizes meet: $sp + 0x78 and 0x79 the two teams' sizes, $sp + 0xA0 and 0xA1 the players on each, $s3
// the most characters, $ra the second team's own most). The game's are the same number on each team (the stick up and down, any
// player's), then C-left and C-right moving places from one team to the other (D_8008FDC4). Online, the host's D-pad Left (0.11.4; C-left
// until then, gGameState + 0x20's 0x40) adds a computer player to the left team and D-pad Right (C-right, 0x80) one to the right team, and past the most
// the team can have (its most characters, the players on it counted, and the match's most characters, the other team's counted) the
// team's start again from none (from one for a team with no player: a team never has no one). The stick does nothing to them. The counts
// are kept in ours (0x80FD0A20, with the 5th player's state: they go with a resync's state, the same on every machine), started from the
// teams as the game had them when the screen begins (the frame before it not one of the screen's).
namespace {
    constexpr uint32_t team_bot_state = 0x80FD0A20;   // +0, +1 each team's computer players; +4 the screen's last frame; +8 the D-pad held
                                                      // last frame (0.11.4)
}

extern "C" void conker_team_bots(uint8_t* rdram, recomp_context* ctx) {
    if (!conker::netsession::active() || (int32_t)ctx->r20 != 2) {
        return;
    }
    const gpr sp = ctx->r29, ours = (gpr)(int32_t)team_bot_state;
    const int most = (int)(int32_t)ctx->r19;
    int players_on[2], size[2];
    for (int t = 0; t < 2; t++) {
        players_on[t] = (int)(int8_t)MEM_B(0xA0 + t, sp);
        size[t] = (int)(int8_t)MEM_B(0x78 + t, sp);
    }
    const int players = players_on[0] + players_on[1];
    // each team's most, as the screen keeps it (8, or the second team's own; online more, conker_team_most)
    int cap[2];
    for (int t = 0; t < 2; t++) {
        int c = 8;
        if (t == 1 && (int8_t)(int32_t)ctx->r31 != 0) {
            c = (int)(int8_t)(int32_t)ctx->r31;
        }
        if (c == 8) {
            const int online_most = std::min(10, most - std::max(players, 1));
            if (online_most > 8) {
                c = online_most;
            }
        }
        cap[t] = c;
    }
    const uint32_t now = conker::netsession::frame();
    int bots[2];
    // (0.11.4) the host's D-pad Left and Right add them now, in place of C-Left and C-Right: the frame's agreed buttons (port 1's, the
    // host's, as gGameState + 0x20's presses are), pressed this frame against last frame's held (kept in ours, the same on every machine)
    uint16_t dpad = 0;
    {
        bool plugged = false;
        uint16_t held = 0;
        float sx = 0.0f, sy = 0.0f;
        if (conker::netsession::input(0, &plugged, &held, &sx, &sy)) {
            dpad = (uint16_t)(held & 0x0300u);   // (0x0200 left, 0x0100 right)
        }
    }
    const bool screen_going = (uint32_t)MEM_W(4, ours) + 1u == now;
    const uint16_t dpad_before = screen_going ? (uint16_t)MEM_HU(8, ours) : dpad;   // (the screen's first frame: nothing counts as pressed)
    MEM_H(8, ours) = (int16_t)dpad;
    const uint16_t dpad_pressed = (uint16_t)(dpad & ~dpad_before);
    if (!screen_going) {
        for (int t = 0; t < 2; t++) {
            bots[t] = std::max(0, size[t] - players_on[t]);
        }
        char line[120];
        std::snprintf(line, sizeof(line), "[teams] computer players a team at a time: %d and %d to start with", bots[0], bots[1]);
        conker::netsession::log(line);
    } else {
        bots[0] = (int)(int8_t)MEM_B(0, ours);
        bots[1] = (int)(int8_t)MEM_B(1, ours);
    }
    MEM_W(4, ours) = (int32_t)now;
    // (C-Left and C-Right were these until 0.11.4: gGameState + 0x20's presses, 0x40 and 0x80)
    const bool add[2] = { (dpad_pressed & 0x0200u) != 0, (dpad_pressed & 0x0100u) != 0 };
    // (possess52 kept at least 5 characters with the 5th player on the screen, its own then the match's 5th after 4 others; possess54: its
    // own comes right after the players, so none are needed)
    const int need = 0;
    const int was[2] = { bots[0], bots[1] };
    for (int t = 0; t < 2; t++) {
        const int other = 1 - t;
        const int fewest = players_on[t] == 0 ? 1 : 0;
        auto top = [&]() { return std::max(fewest, std::min(cap[t] - players_on[t], most - players - bots[other])); };
        // past its most, back to its fewest: none (one with no player on it), or as many as the characters needed, the other team's counted
        auto low = [&]() { return std::min(top(), std::max(fewest, need - players - bots[other])); };
        if (add[t]) {
            bots[t] = bots[t] + 1 > top() ? low() : bots[t] + 1;
        }
        bots[t] = std::min(std::max(bots[t], fewest), top());
    }
    // (possess52) still short of the characters needed (a player moved): computer players added to the team with fewer characters first
    for (int guard = 0; guard < 16 && players + bots[0] + bots[1] < need; guard++) {
        const int first = players_on[0] + bots[0] <= players_on[1] + bots[1] ? 0 : 1;
        bool added = false;
        for (int k = 0; k < 2 && !added; k++) {
            const int t = k == 0 ? first : 1 - first;
            if (players_on[t] + bots[t] + 1 <= cap[t] && players + bots[0] + bots[1] + 1 <= most) {
                bots[t]++;
                added = true;
            }
        }
        if (!added) {
            break;
        }
    }
    const bool changed = bots[0] != was[0] || bots[1] != was[1];
    for (int t = 0; t < 2; t++) {
        MEM_B(t, ours) = (int8_t)bots[t];
        MEM_B(0x78 + t, sp) = (int8_t)(players_on[t] + bots[t]);
    }
    if (changed) {
        MEM_W(0x74, sp) = 0x4FF;   // the screen's move sound
        char line[120];
        std::snprintf(line, sizeof(line), "[teams] computer players: %d on the left team, %d on the right", bots[0], bots[1]);
        conker::netsession::log(line);
    }
}

// ==== (0.11.4) Koala Kong, first look: drawn in place of the muscular teddy (model B1) ====
// A character's model is loaded by func_1503CF20 (the model's number in $s0): func_1502B6BC walks the ROM's tables to its entry and
// func_1502B350 reads it (the entry DMA'd into a buffer of its own size, 0x28 of its stack, the size 0x20), unpacks it (a u32 size then
// raw deflate) into a buffer of that size and func_1502B4A8 turns the file's offsets into addresses. Here, while model B1 is loaded, its
// entry as read is replaced by Kong's (assets/kong/kong.rzip: his Crash Bash mesh bound to the 28 bones the teddy has, his arms brought
// down as Conker's start), so the game unpacks and sets up Kong as one of its own models and the teddy's animations move him.
// His textures (ids 0x240A, 0x24C6-0x24CD, past the game's 0x1E51) are handed out by func_1510D0EC (a texture by id) from ours at
// 0x80FC8000 (free: the gold weapons' pool ends there, the extra camera's buffer starts at 0x80FD0000), and func_1510D7AC (let go of
// one) does nothing for them. Everyone who has this build sees Kong; the game itself is the same on every machine.
// (For now in place of the teddy, to see the rig; his own slot on the character select is next.)
namespace {
    // (0.11.4) Kong's own model number, his own character (22) on the select screens: 0xBA, the last of the game's model numbers, which
    // no multiplayer arena uses. Its loads are the teddy's (B1's) entries, his model put in place of the teddy's as it's read, so the
    // teddy is the teddy again. See "Koala Kong's own place" below.
    constexpr int kong_model = 0xBA;
    constexpr int teddy_model = 0xB1;
    constexpr uint32_t kong_textures_at = 0x80FC8000u, kong_textures_end = 0x80FD0000u;
    int loading_model = -1;
    bool kong_loaded = false, kong_tried = false;
    std::vector<uint8_t> kong_rzip;
    std::vector<uint8_t> read_asset(const char* name);
    struct KongTexture { uint32_t id; std::vector<uint8_t> data; uint32_t at = 0; };
    std::vector<KongTexture> kong_textures;
    uint32_t kong_told = 0;
    uint32_t models_told = 0;   // (both again from 0 as each level loads, so every match is logged, not only the first)

    void kong_say(const char* line) {
        if (kong_told < 60) {
            kong_told++;
            survey(line);
            conker::netsession::log(line);
        }
    }

    std::vector<uint8_t> read_asset(const char* name) {
        std::vector<uint8_t> bytes;
#if defined(CONKER_RT64)
        std::ifstream file(recompui::file::get_asset_path(name), std::ios::binary);
        if (file) {
            bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        }
#else
        (void)name;
#endif
        return bytes;
    }

    // ---- (0.11.4) The characters added from the game's own models (Berri, the Panther King, ... more go in extras[]): each its own
    // place on the select screens after Koala Kong. For the game each is the striped character after the teddy (model 3B, Conker's
    // skeleton and animations); it's drawn as its own model, rebuilt on 3B's skeleton at its own joints (its own proportions) on the
    // player's own PC from their own ROM (assets/<name>/<name>.bin, made by tools/<name>/<name>_rig.py; nothing of the ROM's in the
    // project), put in place of its entry as it's unpacked. Its model's other banks (its settings, its animations) are 3B's.
    // Its own model number is at +4 while the game draws it and works its bones out (func_1502C974, func_1502DF38), so its bones are its
    // own skeleton's, with 3B's animations (func_15084D00, the animations' groups: its model in 3B's). Only in multiplayer (the match's
    // flag), or on its select screens once it's been put up there (until a level loads outside multiplayer): the story's are the game's.
    struct Extra {
        const char* tag;            // (its log lines')
        const char* asset;          // its rebuilt model
        int model;                  // its own model (bank 1)
        int base_model;             // the game's character for it (3B)
        uint8_t base_character;     // (3B's character byte)
        int character;              // its number on the select screens
        uint32_t entry;             // its entry (scale, model, character), ours
        uint32_t offered_with;      // offered in the modes whose characters include these (3B's, 0x10)
        float camera_scale;         // its camera's look-at height, times 3B's (its height over 3B's 182)
        bool loaded, tried, on_select;
        int draw_now;               // its characters being drawn or updated right now
        std::vector<uint8_t> bin;
    };
    Extra extras[] = {
        { "berri", "berri/berri.bin", 0x72, 0x3B, 0x01, 23, 0x80FE0008u, 0x10u, 1.0f, false, false, false, 0, {} },
        { "alien", "alien/alien.bin", 0x5E, 0x3B, 0x01, 24, 0x80FE0010u, 0x10u, 1.37f, false, false, false, 0, {} },
        { "ogre", "ogre/ogre.bin", 0x54, 0x3B, 0x01, 25, 0x80FE0018u, 0x10u, 1.48f, false, false, false, 0, {} },
        { "professor", "professor/professor.bin", 0x99, 0x3B, 0x01, 26, 0x80FE0020u, 0x10u, 1.3f, false, false, false, 0, {} },
        { "panther", "panther/panther.bin", 0x76, 0x3B, 0x01, 27, 0x80FE0028u, 0x10u, 1.6f, false, false, false, 0, {} },
        // (model 0x0C: 13 bones' room in its own entry, which Conker's 28 ran over (the select screen froze): built with his skeleton at
        // its end, 6056 bytes, and drawn from model 0x16's entry, a story model's of 9352 bytes, whose room it fits)
        { "c0c", "c0c/c0c.bin", 0x16, 0x3B, 0x01, 28, 0x80FE0030u, 0x10u, 1.0f, false, false, false, 0, {} },
        // (made from a mesh with no skeleton, tools/objchar/obj_character.py: its own textures, assets/<name>/tex_XXXX.bin, handed out with
        // the others; drawn from model 0x4B's entry, the seated character's, 42488 bytes' room)
        { "jamjars", "jamjars/jamjars.bin", 0x4B, 0x3B, 0x01, 29, 0x80FE0038u, 0x10u, 1.1f, false, false, false, 0, {} },
        // (the low-poly Banjo, the same way: drawn from model 0x7B's entry, 50840 bytes' room)
        { "banjo", "banjo/banjo.bin", 0x7B, 0x3B, 0x01, 30, 0x80FE0040u, 0x10u, 1.05f, false, false, false, 0, {} },
        // (Metatrix, the same way: drawn from model 0x97's entry, its room raised to his 30848 as it's read. A model's slot must be a
        // character's with its own animations' banks (0x82's has none: the game then loads none of its other banks, and on the select
        // screen he came out huge, all over the screen))
        { "metatrix", "metatrix/metatrix.bin", 0x97, 0x3B, 0x01, 31, 0x80FE0048u, 0x10u, 1.05f, false, false, false, 0, {} },
        // (Naked Snake: drawn from model 0xA4's entry (21160 bytes), its room raised to his 35040 as it's read; sized by his shoulders
        // (--fit=shoulders), 143 tall)
        { "snake", "snake/snake.bin", 0xA4, 0x3B, 0x01, 32, 0x80FE0050u, 0x10u, 1.05f, false, false, false, 0, {} },
        // (the low-poly Snake, from its .dae (tools/objchar/dae2obj.py), and Cloud: drawn from 0x92's and 0x9E's entries, characters' own
        // with their own animations' banks, their rooms raised as they're read)
        { "lowsnake", "lowsnake/lowsnake.bin", 0x92, 0x3B, 0x01, 33, 0x80FE0058u, 0x10u, 1.05f, false, false, false, 0, {} },
        { "cloud", "cloud/cloud.bin", 0x9E, 0x3B, 0x01, 34, 0x80FE0060u, 0x10u, 1.05f, false, false, false, 0, {} },
        // (Dinosaur Planet's Garunda Te, General Scales, King EarthWalker and Rocky, South Park's dead Kenny: from their meshes, their
        // textures 0x2300 on)
        { "garunda", "garunda/garunda.bin", 0x84, 0x3B, 0x01, 35, 0x80FE0068u, 0x10u, 1.05f, false, false, false, 0, {} },
        { "scales", "scales/scales.bin", 0x85, 0x3B, 0x01, 36, 0x80FE0070u, 0x10u, 1.05f, false, false, false, 0, {} },
        { "king", "king/king.bin", 0x86, 0x3B, 0x01, 37, 0x80FE0078u, 0x10u, 1.05f, false, false, false, 0, {} },
        { "kenny", "kenny/kenny.bin", 0x87, 0x3B, 0x01, 38, 0x80FE0080u, 0x10u, 0.7f, false, false, false, 0, {} },
        { "rocky", "rocky/rocky.bin", 0x89, 0x3B, 0x01, 39, 0x80FE0088u, 0x10u, 1.05f, false, false, false, 0, {} },
        { "fox", "fox/fox.bin", 0x8C, 0x3B, 0x01, 40, 0x80FE0090u, 0x10u, 0.95f, false, false, false, 0, {} },
        { "dreamkasco", "dreamkasco/dreamkasco.bin", 0x8E, 0x3B, 0x01, 41, 0x80FE0098u, 0x10u, 1.0f, false, false, false, 0, {} },
    };
    constexpr int kExtras = (int)(sizeof(extras) / sizeof(extras[0]));
    bool extra_now(uint8_t* rdram, const Extra& x) {
        return x.loaded && (MEM_BU(0, (gpr)(int32_t)0x800BE616) != 0 || x.on_select);
    }
    void extra_load(Extra& x) {
        if (x.tried) {
            return;
        }
        x.tried = true;
        x.bin = read_asset(x.asset);
        x.loaded = x.bin.size() > 0x1000;
        char line[160];
        std::snprintf(line, sizeof(line), "[%s] the model %s (%u bytes)", x.tag, x.loaded ? "found" : "not found (make it with its tools/ rig script)",
            (unsigned)x.bin.size());
        kong_say(line);
    }
    void extras_load() {
        for (Extra& x : extras) {
            extra_load(x);
        }
    }
    bool any_extra_loaded() {
        extras_load();
        for (const Extra& x : extras) {
            if (x.loaded) {
                return true;
            }
        }
        return false;
    }

    void kong_load() {
        if (kong_tried) {
            return;
        }
        kong_tried = true;
        kong_rzip = read_asset("kong/kong.rzip");
        static const uint32_t ids[] = { 0x240A, 0x24C6, 0x24C7, 0x24C8, 0x24C9, 0x24CA, 0x24CB, 0x24CC, 0x24CD };
        uint32_t at = kong_textures_at;
        for (uint32_t id : ids) {
            char name[40];
            std::snprintf(name, sizeof(name), "kong/tex_%04X.bin", (unsigned)id);
            KongTexture t{ id, read_asset(name), 0 };
            if (t.data.empty() || at + t.data.size() > kong_textures_end) {
                continue;
            }
            t.at = at;
            at = (at + (uint32_t)t.data.size() + 15u) & ~15u;
            kong_textures.push_back(std::move(t));
        }
        kong_loaded = kong_rzip.size() > 8;
        char line[160];
        std::snprintf(line, sizeof(line), "[kong] his model %s (%u bytes packed), %u of 9 textures", kong_loaded ? "found" : "not found",
            (unsigned)kong_rzip.size(), (unsigned)kong_textures.size());
        kong_say(line);
    }
}

// func_1503CF20 at 0x1503CFA4 ($s0 the model about to be loaded) and at 0x1503CFC8 (loaded)
extern "C" void conker_kong_model_begin(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    loading_model = (int)(int32_t)ctx->r16;
    kong_load();
}

extern "C" void conker_kong_model_end(uint8_t* rdram, recomp_context* ctx) {
    // (for Kong's own head when it's shot off: the head the game throws is a model of its own, 6E by the survey) each model a match
    // loads: its number, its skeleton (bones, their parents and offsets) and its vertices' bounds, for 6E and any model past the first
    // ones of the match
    {
        const uint32_t file = (uint32_t)ctx->r2;
        if ((file >> 24) == 0x80 && models_told < 40 && loading_model >= 0 && loading_model < 0xBB) {
            models_told++;
            const uint32_t skel = (uint32_t)MEM_W(0x10, (gpr)(int32_t)file), skel_size = (uint32_t)MEM_W(0x14, (gpr)(int32_t)file);
            const uint32_t list = (uint32_t)MEM_W(0x8, (gpr)(int32_t)file);
            const uint32_t bones = (skel >> 24) == 0x80 && skel_size < 0x1000u ? skel_size / 16u : 0u;
            char line[200];
            std::snprintf(line, sizeof(line), "[models] model %02X loaded at %08X: %u bones (skeleton at %08X), vertices %08X to %08X", (unsigned)loading_model,
                (unsigned)file, (unsigned)bones, (unsigned)skel, (unsigned)(file + 0x38u), (unsigned)list);
            survey(line);
            if (loading_model == 0x6E && bones > 0 && bones <= 40) {
                for (uint32_t b = 0; b < bones; b++) {
                    const gpr e = (gpr)(int32_t)(skel + 16u * b);
                    float o[3];
                    for (int i = 0; i < 3; i++) {
                        const uint32_t bits = (uint32_t)MEM_W(4u + 4u * (uint32_t)i, e);
                        std::memcpy(&o[i], &bits, 4);
                    }
                    std::snprintf(line, sizeof(line), "    bone %u: parent %u, offset %.1f %.1f %.1f", (unsigned)b, (unsigned)((uint32_t)MEM_W(0, e) >> 24), o[0], o[1], o[2]);
                    survey(line);
                }
                float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
                for (uint32_t v = file + 0x38u; v + 16u <= list && v < file + 0x38u + 16u * 2000u; v += 16) {
                    for (int i = 0; i < 3; i++) {
                        const float c = (float)(int16_t)MEM_H((uint32_t)(2 * i), (gpr)(int32_t)v);
                        lo[i] = std::min(lo[i], c);
                        hi[i] = std::max(hi[i], c);
                    }
                }
                std::snprintf(line, sizeof(line), "    its vertices (local to their bones) from %.0f %.0f %.0f to %.0f %.0f %.0f", lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
                survey(line);
            }
        }
    }
    (void)rdram;
    if (loading_model == kong_model && kong_loaded) {
        char line[120];
        std::snprintf(line, sizeof(line), "[kong] model %02X loaded at %08X", (unsigned)kong_model, (unsigned)(uint32_t)ctx->r2);
        kong_say(line);
    }
    loading_model = -1;
}

// ---- (0.11.4) Pirate Lagoon, from Diddy Kong Racing, in place of the race course ----
// What func_1502B6BC loads is a path through the ROM's tables: its depth in $a3 and each step's index on the stack (0x10, 0x14 of its
// caller's frame, as it starts): a character's model is (1, its number), a level's piece (4, its number). The race's course is bank 4's
// 0x32 by the package (0x33 another version of it), 0x33 the one the race loaded when tried: its entry as read is replaced by Pirate
// Lagoon's (assets/lagoon/lagoon.rzip: the track, its collision, every triangle the track's surface), packed to fit the course's own
// (28626 bytes packed, 62408 unpacked: ours 22161 and 52480, its vertices shared within each load of the vertex cache). Its 26 textures
// (0x1F10-0x1F29) are handed out by func_1510D0EC from their own files (assets/lagoon/tex_XXXX.bin), from high memory as Kong's are.
// The race's checkpoints and start places are still the volcano's: the next step.
namespace {
    uint32_t path_depth = 0, path_a = 0, path_b = 0;
    enum class Swap { None, Kong, Lagoon, Extra, Cave, Weapon };
    Swap swapping = Swap::None;
    int swapping_extra = -1;   // (Swap::Extra: which of extras[])
    // (0.11.4) the sword replaced (tools/weapon/sword.py: assets/sword/pickup.bin, bank 3's 0x52, the prop's model; held.bin, bank 9's
    // 0x3A, the one in the hand on bone 9), each put in place of its entry as it's unpacked, its room raised as it's read
    std::vector<uint8_t> sword_bins[2];
    bool sword_tried = false;
    int swapping_weapon = -1;
    void sword_load() {
        if (sword_tried) {
            return;
        }
        sword_tried = true;
        sword_bins[0] = read_asset("sword/pickup.bin");
        sword_bins[1] = read_asset("sword/held.bin");
    }

    bool lagoon_tried = false, lagoon_loaded = false;
    std::vector<uint8_t> lagoon_rzip;
    // (its textures its own files now, assets/lagoon/tex_XXXX.bin, handed out from high memory as Kong's are: packed in the entry, it
    // was too big for the race course's place, 53401 bytes packed for 28626)
    constexpr uint32_t lagoon_textures_at = 0x80FD0000u, lagoon_textures_end = 0x80FE0000u;
    struct LagoonTexture { uint32_t id; std::vector<uint8_t> data; uint32_t at; };
    std::vector<LagoonTexture> lagoon_textures;
    uint32_t lagoon_base = 0;

    void lagoon_load() {
        if (lagoon_tried) {
            return;
        }
        lagoon_tried = true;
        lagoon_rzip = read_asset("lagoon/lagoon.rzip");
        uint32_t at = lagoon_textures_at;
        for (uint32_t id = 0x1F10; id <= 0x1F29; id++) {
            char name[40];
            std::snprintf(name, sizeof(name), "lagoon/tex_%04X.bin", (unsigned)id);
            LagoonTexture t{ id, read_asset(name), 0 };
            if (t.data.empty() || at + t.data.size() > lagoon_textures_end) {
                continue;
            }
            t.at = at;
            at = (at + (uint32_t)t.data.size() + 15u) & ~15u;
            lagoon_textures.push_back(std::move(t));
        }
        lagoon_loaded = lagoon_rzip.size() > 8;
        char line[160];
        std::snprintf(line, sizeof(line), "[lagoon] the level %s (%u bytes packed), %u textures listed", lagoon_loaded ? "found" : "not found",
            (unsigned)lagoon_rzip.size(), (unsigned)lagoon_textures.size());
        kong_say(line);
    }
}

// ---- (0.11.4) Cave of Horror, the custom map (assets/cave: cave.rzip, its level in bank 4's shape, made by tools/cave/cave_level.py; its
// textures tex_2100.bin on, ids 0x2100 to 0x213F, handed out from 0x80A00000 as Lagoon's are). For now, while it's tested: it takes the
// place of the arena that assets/cave/replace.txt names (its scene, hexadecimal; no file: off), built in that arena's entry's shape as
// Lagoon is in the race course's, and the arena's setup records (its props, weapons and starts) moved onto the cave's floors as the level
// loads (cave_props). Its own place on the map select comes next.
namespace {
    uint8_t* cave_rdram = nullptr;
    bool cave_tried = false, cave_loaded = false;
    // (the custom maps, each its own folder of assets made by tools/cave/cave_level.py, its own textures' ids; picked on the mode select
    // by the byte at cave_picked_at: its key. One loaded at a time, in Bunkers' place.)
    struct CustomMap { const char* folder; const char* name; uint32_t tex_base; uint8_t key; };
    constexpr CustomMap custom_maps[] = {
        { "cave", "CAVE OF HORROR", 0x2100, 0xCA },
        { "stadium", "MAYAN KICKBALL STADIUM", 0x2200, 0xCB },
        { "snowcastle", "SNOW CASTLE", 0x2240, 0xCC },
        { "falls", "DISCOVERY FALLS", 0x2280, 0xCD },
        { "magiccave", "MAGIC CAVE", 0x22C0, 0xCE },
        { "zanador", "ZANADOR", 0x2440, 0xCF },   // (0x2400 to 0x243F not: Kong's 0x240A)
    };
    constexpr int kCustomMaps = (int)(sizeof(custom_maps) / sizeof(custom_maps[0]));
    int cave_map = 0;            // (the one loaded or to load)
    bool cave_middle_known = false;
    std::string cave_asset(const char* file) { return std::string(custom_maps[cave_map].folder) + "/" + file; }
    int cave_scene = -1;
    std::vector<uint8_t> cave_rzip;
    constexpr uint32_t cave_textures_at = 0x80A00000u, cave_textures_end = 0x80A80000u;
    std::vector<LagoonTexture> cave_textures;
    struct CaveTri { float a[3], b[3], c[3]; };
    std::vector<CaveTri> cave_tris;
    float cave_min[3] = { 0, 0, 0 }, cave_max[3] = { 0, 0, 0 };
    // (fitted into the arena's own space as it's built: the game draws and collides a level only inside the arena's own areas, its
    // other files' rooms; at its own place, 0 to 2212 high, the cave was black and the players fell through it. Its vertices scaled by
    // cave_fit_scale about its middle and moved so that its middle is the arena's and its floor the arena's floor.)
    float cave_fit_scale = 1.0f, cave_fit_from[3] = { 0, 0, 0 }, cave_fit_to[3] = { 0, 0, 0 };
    std::vector<CaveTri> cave_tris0;          // (as built, before any fitting)
    float cave_min0[3] = { 0, 0, 0 }, cave_max0[3] = { 0, 0, 0 };
    void cave_fit_point(float* p) {
        for (int q = 0; q < 3; q++) {
            p[q] = (p[q] - cave_fit_from[q]) * cave_fit_scale + cave_fit_to[q];
        }
    }
    void cave_load() {
        if (cave_tried) {
            return;
        }
        cave_tried = true;
        cave_loaded = false;
        cave_middle_known = false;
        cave_rzip.clear();
        cave_textures.clear();
        cave_tris.clear();
        // (Bunkers, scene 34, by default: the deathmatch arena whose setup it takes; assets/cave/replace.txt another scene, for tests)
        const std::vector<uint8_t> want = read_asset("cave/replace.txt");
        cave_scene = want.empty() ? 0x34 : (int)std::strtol(std::string(want.begin(), want.end()).c_str(), nullptr, 16);
        cave_rzip = read_asset(cave_asset("cave.rzip").c_str());
        uint32_t at = cave_textures_at;
        const uint32_t tex_base = custom_maps[cave_map].tex_base;
        for (uint32_t id = tex_base; id <= tex_base + 0x3F; id++) {
            char name[40];
            std::snprintf(name, sizeof(name), "tex_%04X.bin", (unsigned)id);
            LagoonTexture t{ id, read_asset(cave_asset(name).c_str()), 0 };
            if (t.data.empty() || at + t.data.size() > cave_textures_end) {
                continue;
            }
            t.at = at;
            at = (at + (uint32_t)t.data.size() + 15u) & ~15u;
            cave_textures.push_back(std::move(t));
        }
        // its triangles, for the floors (the chunk's display list: G_VTX into the vertex buffer, G_TRI1 and G_TRI2 out of it)
        if (cave_rzip.size() > 8) {
            const uint32_t n = ((uint32_t)cave_rzip[0] << 24) | ((uint32_t)cave_rzip[1] << 16) | ((uint32_t)cave_rzip[2] << 8) | cave_rzip[3];
            std::vector<uint8_t> e(n);
            const size_t got = tinfl_decompress_mem_to_mem(e.data(), e.size(), cave_rzip.data() + 4, cave_rzip.size() - 4, 0);
            if (got != TINFL_DECOMPRESS_MEM_TO_MEM_FAILED && got >= 0x40) {
                const auto w = [&](size_t a) { return a + 4 <= e.size() ? ((uint32_t)e[a] << 24) | ((uint32_t)e[a + 1] << 16) | ((uint32_t)e[a + 2] << 8) | e[a + 3] : 0u; };
                const uint32_t c = w(0);
                const uint32_t dl = c + w(c), dl_len = w(c + 4);
                float buf[32][3] = {};
                bool first = true;
                const auto take = [&](int i, float* out) { out[0] = buf[i & 31][0]; out[1] = buf[i & 31][1]; out[2] = buf[i & 31][2]; };
                for (uint32_t a = dl; a + 8 <= dl + dl_len && a + 8 <= e.size(); a += 8) {
                    const uint32_t w0 = w(a), w1 = w(a + 4), op = w0 >> 24;
                    if (op == 0x01) {
                        const uint32_t cnt = (w0 >> 12) & 0xFF, end = (w0 & 0xFF) >> 1, v0 = end - cnt;
                        const uint32_t src = c + 0x28 + (w1 & 0xFFFFFF);
                        for (uint32_t k = 0; k < cnt && v0 + k < 32; k++) {
                            const size_t r = src + 16 * k;
                            if (r + 6 > e.size()) break;
                            for (int q = 0; q < 3; q++) {
                                buf[v0 + k][q] = (float)(int16_t)(((uint16_t)e[r + 2 * q] << 8) | e[r + 2 * q + 1]);
                                if (first) { cave_min[q] = cave_max[q] = buf[v0 + k][q]; }
                                cave_min[q] = std::min(cave_min[q], buf[v0 + k][q]);
                                cave_max[q] = std::max(cave_max[q], buf[v0 + k][q]);
                            }
                            first = false;
                        }
                    } else if (op == 0x05 || op == 0x06) {
                        CaveTri t;
                        take((int)((w0 >> 17) & 0x7F), t.a); take((int)((w0 >> 9) & 0x7F), t.b); take((int)((w0 >> 1) & 0x7F), t.c);
                        cave_tris.push_back(t);
                        if (op == 0x06) {
                            take((int)((w1 >> 17) & 0x7F), t.a); take((int)((w1 >> 9) & 0x7F), t.b); take((int)((w1 >> 1) & 0x7F), t.c);
                            cave_tris.push_back(t);
                        }
                    } else if (op == 0xDF) {
                        break;
                    }
                }
            }
        }
        // (the basement, tools/cave/cave_level.py's floor under the whole map for the camera: not a floor of the map's own, left out of
        // the floors the spawns and props are put on)
        {
            const float ex = (cave_max[0] - cave_min[0]) * 0.9f, ez = (cave_max[2] - cave_min[2]) * 0.9f;
            cave_tris.erase(std::remove_if(cave_tris.begin(), cave_tris.end(), [&](const CaveTri& t) {
                const float x0 = std::min({ t.a[0], t.b[0], t.c[0] }), x1 = std::max({ t.a[0], t.b[0], t.c[0] });
                const float z0 = std::min({ t.a[2], t.b[2], t.c[2] }), z1 = std::max({ t.a[2], t.b[2], t.c[2] });
                return x1 - x0 >= ex && z1 - z0 >= ez;
            }), cave_tris.end());
        }
        cave_loaded = cave_rzip.size() > 8 && !cave_tris.empty();
        cave_tris0 = cave_tris;
        for (int q = 0; q < 3; q++) {
            cave_min0[q] = cave_min[q];
            cave_max0[q] = cave_max[q];
        }
        char line[200];
        std::snprintf(line, sizeof(line), "[cave] %s in place of scene %02X: the level %s (%u bytes packed), %u textures, %u triangles, from %.0f %.0f %.0f to %.0f %.0f %.0f",
            custom_maps[cave_map].folder, (unsigned)cave_scene, cave_loaded ? "found" : "not found", (unsigned)cave_rzip.size(), (unsigned)cave_textures.size(), (unsigned)cave_tris.size(),
            cave_min[0], cave_min[1], cave_min[2], cave_max[0], cave_max[1], cave_max[2]);
        kong_say(line);
    }
    // whether (x, z) has a floor of the cave's under it (a triangle facing up), and the floors' middle (by area), for the setup records
    bool cave_over_floor(float x, float z) {
        for (const CaveTri& t : cave_tris) {
            const float ux = t.b[0] - t.a[0], uy = t.b[1] - t.a[1], uz = t.b[2] - t.a[2];
            const float vx = t.c[0] - t.a[0], vy = t.c[1] - t.a[1], vz = t.c[2] - t.a[2];
            const float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
            const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (len < 1e-3f || std::fabs(ny) / len < 0.6f) {
                continue;
            }
            const auto edge = [](const float* p, const float* q, float px, float pz) { return (q[0] - p[0]) * (pz - p[2]) - (q[2] - p[2]) * (px - p[0]); };
            const float e0 = edge(t.a, t.b, x, z), e1 = edge(t.b, t.c, x, z), e2 = edge(t.c, t.a, x, z);
            if ((e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0)) {
                return true;
            }
        }
        return false;
    }
    bool cave_well_inside(float x, float z) {   // (and 250 in from every side, so nobody starts against a wall or over the edge)
        const float r = 250.0f;
        return cave_over_floor(x, z) && cave_over_floor(x + r, z) && cave_over_floor(x - r, z) && cave_over_floor(x, z + r) && cave_over_floor(x, z - r);
    }
    void cave_floor_middle(float& mx, float& mz) {
        double sx = 0, sz = 0, sa = 0;
        for (const CaveTri& t : cave_tris) {
            const float ux = t.b[0] - t.a[0], uy = t.b[1] - t.a[1], uz = t.b[2] - t.a[2];
            const float vx = t.c[0] - t.a[0], vy = t.c[1] - t.a[1], vz = t.c[2] - t.a[2];
            const float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
            const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (len < 1e-3f || std::fabs(ny) / len < 0.6f) {
                continue;
            }
            sx += (t.a[0] + t.b[0] + t.c[0]) / 3.0 * len; sz += (t.a[2] + t.b[2] + t.c[2]) / 3.0 * len; sa += len;
        }
        mx = sa > 0 ? (float)(sx / sa) : (cave_min[0] + cave_max[0]) * 0.5f;
        mz = sa > 0 ? (float)(sz / sa) : (cave_min[2] + cave_max[2]) * 0.5f;
    }
    // the floor under (x, z) nearest to the height y: the cave's triangles facing up (or failing that, the nearest of them)
    float cave_floor(float x, float z, float y) {
        float best = 1e9f, best_y = y;
        float near_d = 1e18f, near_y = cave_min[1];
        for (const CaveTri& t : cave_tris) {
            const float ux = t.b[0] - t.a[0], uy = t.b[1] - t.a[1], uz = t.b[2] - t.a[2];
            const float vx = t.c[0] - t.a[0], vy = t.c[1] - t.a[1], vz = t.c[2] - t.a[2];
            const float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
            const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (len < 1e-3f || std::fabs(ny) / len < 0.6f) {
                continue;
            }
            const float cx = (t.a[0] + t.b[0] + t.c[0]) / 3.0f, cz = (t.a[2] + t.b[2] + t.c[2]) / 3.0f;
            const float d = (cx - x) * (cx - x) + (cz - z) * (cz - z);
            if (d < near_d) {
                near_d = d;
                near_y = (t.a[1] + t.b[1] + t.c[1]) / 3.0f;
            }
            // inside, by the signs of the edges (x, z)
            const auto edge = [](const float* p, const float* q, float px, float pz) { return (q[0] - p[0]) * (pz - p[2]) - (q[2] - p[2]) * (px - p[0]); };
            const float e0 = edge(t.a, t.b, x, z), e1 = edge(t.b, t.c, x, z), e2 = edge(t.c, t.a, x, z);
            if (!((e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0))) {
                continue;
            }
            const float h = t.a[1] - (nx * (x - t.a[0]) + nz * (z - t.a[2])) / ny;
            if (std::fabs(h - y) < best) {
                best = std::fabs(h - y);
                best_y = h;
            }
        }
        return best < 1e9f ? best_y : near_y;
    }
}

// (0.11.4) The cave picked on the mode select: a byte of ours in the game's memory (every machine sets it alike, from player 1's agreed
// buttons in the menu, cave_menu), read as the arena loads. assets/cave/replace.txt (a test) puts it in place of that scene always.
constexpr uint32_t cave_picked_at = 0x80FE0100u;
// the map picked (its index in custom_maps), -1 none
int cave_picked() {
    uint8_t* rdram = cave_rdram;
    const uint8_t key = rdram != nullptr ? MEM_BU(0, (gpr)(int32_t)cave_picked_at) : 0;
    for (int i = 0; i < kCustomMaps; i++) {
        if (custom_maps[i].key == key) {
            return i;
        }
    }
    return read_asset("cave/replace.txt").empty() ? -1 : 0;
}
bool conker::mp_characters::cave_on(int scene) {
    const int m = cave_picked();
    if (m < 0) {
        return false;
    }
    if (m != cave_map) {   // (another map picked: its own files)
        cave_map = m;
        cave_tried = false;
    }
    cave_load();
    return cave_loaded && scene == cave_scene;
}

    // (0.11.4) The mode select (the bar's scene, 0x1D, a barrel zoomed into: gGameState's +0x2C), its Bunkers highlighted (+0x42, mode 4,
    // scene 34): L (player 1's, the frame's agreed buttons online) switches it to Cave of Horror and back. Shown at the top of the screen.
void conker_cave_menu(uint8_t* rdram) {
        cave_rdram = rdram;
        const uint32_t gs = (uint32_t)MEM_W(0, (gpr)(int32_t)0x8008FDD4u);
        const int32_t scene = (int32_t)MEM_W(0, (gpr)(int32_t)0x800BE9F0u);
        static bool shown = false;
        static uint16_t last = 0;
        if ((gs >> 24) != 0x80 || scene != 0x1D || MEM_BU(0x2C, (gpr)(int32_t)gs) == 0 || MEM_BU(0x42, (gpr)(int32_t)gs) != 4) {
            shown = false;
            return;
        }
        uint16_t held = 0;
        if (conker::netsession::active()) {
            bool plugged = false;
            float x = 0.0f, y = 0.0f;
            conker::netsession::input(conker::netsession::port_of_player(0), &plugged, &held, &x, &y);
        } else {
            uint16_t pressed = 0;
            conker::frontend::probe_buttons(0, &held, &pressed);
        }
        const bool l_now = (held & 0x0020) != 0, l_was = (last & 0x0020) != 0;
        last = held;
        // (L: Bunkers, then each custom map in turn, then Bunkers again)
        const int picked = cave_picked();
        if (l_now && !l_was) {
            const int next = picked + 1 < kCustomMaps ? picked + 1 : -1;
            MEM_B(0, (gpr)(int32_t)cave_picked_at) = (int8_t)(next < 0 ? 0 : custom_maps[next].key);
            shown = false;
        }
        if (!shown) {
            shown = true;
            const int now_picked = cave_picked();
            const int next = now_picked + 1 < kCustomMaps ? now_picked + 1 : -1;
            const std::string text = std::string("Map: ") + (now_picked < 0 ? "BUNKERS" : custom_maps[now_picked].name) + "  (L: " +
                (next < 0 ? "Bunkers" : custom_maps[next].name) + ")";
            conker::notices::show_top(text);
        }
    }

// (0.11.4) The custom maps' spawns: the game starts (and brings back) its characters at the arena's own places, which on a custom map
// are outside it, in the dark, falling (30 Oct, Snow Castle: player 1 at z 4150, past the map's 3067). Each character found off the map
// (no floor under it, or below its lowest floor) or come back from a death (its place jumping more than 600 in a frame) is put on one of
// the map's spawn spots: places well inside its floors, spread apart, the first nearest its middle. Worked out alike on every machine
// (the same files, the game's own state), so it holds online.
namespace {
    std::vector<std::array<float, 3>> cave_spawns;
    int cave_spawns_map = -1;
    void cave_make_spawns() {
        cave_spawns.clear();
        cave_spawns_map = cave_map;
        std::vector<std::array<float, 3>> cand;
        const float lowest = cave_min[1];
        for (float x = cave_min[0]; x <= cave_max[0]; x += 250.0f) {
            for (float z = cave_min[2]; z <= cave_max[2]; z += 250.0f) {
                if (!cave_well_inside(x, z)) {
                    continue;
                }
                const float y = cave_floor(x, z, lowest);
                cand.push_back({ x, y, z });
            }
        }
        if (cand.empty()) {
            return;
        }
        float mx = 0.0f, mz = 0.0f;
        cave_floor_middle(mx, mz);
        // (the lower floors first: the one nearest the middle among those within 300 of the lowest found)
        float low = 1e9f;
        for (const auto& c : cand) low = std::min(low, c[1]);
        size_t first = 0;
        float best = 1e18f;
        for (size_t i = 0; i < cand.size(); i++) {
            const float d = (cand[i][0] - mx) * (cand[i][0] - mx) + (cand[i][2] - mz) * (cand[i][2] - mz) + (cand[i][1] > low + 300.0f ? 1e12f : 0.0f);
            if (d < best) { best = d; first = i; }
        }
        cave_spawns.push_back(cand[first]);
        while (cave_spawns.size() < 64 && cave_spawns.size() < cand.size()) {   // (16 spawns, then the setup's weapons and props)
            size_t pick = 0;
            float pick_d = -1.0f;
            for (size_t i = 0; i < cand.size(); i++) {
                float nd = 1e18f;
                for (const auto& sp : cave_spawns) {
                    nd = std::min(nd, (cand[i][0] - sp[0]) * (cand[i][0] - sp[0]) + (cand[i][2] - sp[2]) * (cand[i][2] - sp[2]));
                }
                if (cand[i][1] > low + 300.0f) nd *= 0.25f;   // (higher floors less wanted)
                if (nd > pick_d) { pick_d = nd; pick = i; }
            }
            cave_spawns.push_back(cand[pick]);
        }
        for (size_t i = 0; i < cave_spawns.size() && i < 16; i++) {
            const auto& sp = cave_spawns[i];
            char line[100];
            std::snprintf(line, sizeof(line), "[cave] spawn spot %.0f %.0f %.0f", sp[0], sp[1], sp[2]);
            kong_say(line);
        }
    }
    void cave_put(uint8_t* rdram, uint32_t place, const float* p) {
        for (int q = 0; q < 3; q++) {
            const float v = q == 1 ? p[q] + 5.0f : p[q];
            uint32_t bits;
            std::memcpy(&bits, &v, 4);
            MEM_W(4 * q, (gpr)(int32_t)place) = (int32_t)bits;
        }
    }
    // each character (0 to the game's count, 0x8008FD8C): put on a spawn spot when it's off the map or just come back
    void cave_fix_spawns(uint8_t* rdram, const float* saved, bool use_saved) {
        static float last_pos[16][3];
        static bool have[16] = {};
        static int respawns[16] = {};
        static int off_frames[16] = {};   // (no floor under it this long: off the map; a jump over a gap is shorter)
        if (cave_spawns_map != cave_map || cave_spawns.empty()) {
            cave_make_spawns();
            for (bool& h : have) h = false;
        }
        if (cave_spawns.empty()) {
            return;
        }
        const int count = std::clamp((int)MEM_B(0, (gpr)(int32_t)0x8008FD8Cu), 0, 16);
        for (int k = 0; k < count; k++) {
            const uint32_t c = 0x800CC2D0u + (uint32_t)k * 0x32Cu;
            const uint32_t st = (uint32_t)MEM_W(0x31C, (gpr)(int32_t)c);
            if ((st >> 24) != 0x80) {
                have[k] = false;
                continue;
            }
            const uint32_t place = c + 0x14u;
            float p[3];
            for (int q = 0; q < 3; q++) {
                const uint32_t bits = (uint32_t)MEM_W(4 * q, (gpr)(int32_t)place);
                std::memcpy(&p[q], &bits, 4);
            }
            if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) {
                continue;
            }
            const float jump = have[k] ? std::fabs(p[0] - last_pos[k][0]) + std::fabs(p[1] - last_pos[k][1]) + std::fabs(p[2] - last_pos[k][2]) : 1e9f;
            off_frames[k] = cave_over_floor(p[0], p[2]) ? 0 : off_frames[k] + 1;
            const bool off = !have[k] || off_frames[k] > 45 || p[1] < cave_min[1] - 250.0f;
            if (jump > 600.0f || off) {
                const float* to = (k == 0 && use_saved) ? saved : cave_spawns[(size_t)(k * 5 + respawns[k] * 3) % std::min<size_t>(16, cave_spawns.size())].data();
                respawns[k]++;
                off_frames[k] = 0;
                cave_put(rdram, place, to);
                p[0] = to[0]; p[1] = to[1] + 5.0f; p[2] = to[2];
                static uint32_t told = 0;
                if (told++ < 40) {
                    char line[120];
                    std::snprintf(line, sizeof(line), "[cave] character %d put on a spawn spot %.0f %.0f %.0f (%s)", k, to[0], to[1], to[2], off ? "off the map" : "come back");
                    survey(line);
                }
            }
            for (int q = 0; q < 3; q++) last_pos[k][q] = p[q];
            have[k] = true;
        }
    }
}

// (0.11.4) Cave of Horror: your own spawn point. Crouch (Z) without moving for 3 seconds in the cave and the place is saved
// (assets/cave/spawn.txt); from then on, offline, player 1 starts there and comes back there after each death (the game's start put
// right as it's taken: a jump of the character's place by more than 600 in one frame). Online every machine would need the same file
// (the game runs in step), so it's offline only until the place is put in the map itself.
void conker_cave_match(uint8_t* rdram) {
    static int still = 0;
    static float last[3] = { 0, 0, 0 };
    static bool have_last = false, have_spawn = false;
    static int spawn_read = -1;   // (the map whose spawn was read)
    static float spawn[3] = { 0, 0, 0 };
    const int32_t scene = (int32_t)MEM_W(0, (gpr)(int32_t)0x800BE9F0u);
    if (!cave_loaded || scene != cave_scene || !conker::mp_characters::cave_on(scene)) {
        have_last = false;
        still = 0;
        return;
    }
    if (spawn_read != cave_map) {
        spawn_read = cave_map;
        have_spawn = false;
        const std::vector<uint8_t> t = read_asset(cave_asset("spawn.txt").c_str());
        if (!t.empty()) {
            const std::string text(t.begin(), t.end());
            have_spawn = std::sscanf(text.c_str(), "%f %f %f", &spawn[0], &spawn[1], &spawn[2]) == 3;
        }
    }
    constexpr uint32_t place = 0x800CC2E4u;   // (character 0, player 1: its x, y, z)
    float now[3];
    for (int q = 0; q < 3; q++) {
        const uint32_t bits = (uint32_t)MEM_W(4 * q, (gpr)(int32_t)place);
        std::memcpy(&now[q], &bits, 4);
    }
    if (!std::isfinite(now[0]) || !std::isfinite(now[1]) || !std::isfinite(now[2])) {
        return;
    }
    const float jump = have_last ? std::sqrt((now[0] - last[0]) * (now[0] - last[0]) + (now[1] - last[1]) * (now[1] - last[1]) + (now[2] - last[2]) * (now[2] - last[2])) : 1e9f;
    // (the map going black: Bunkers' visibility table (bank 5, its scene's PVS: per camera cell, which nodes of the level's quadtree
    // are hidden, func_150A6760 writing the bits each frame, func_150A5378 skipping a node so marked) was put on the custom map's own
    // quadtree, a different shape: the root came out hidden from some cells and the whole map wasn't drawn. Its pointer (0x800BE5D0,
    // read by func_150A6760 alone) cleared while a custom map is played: only the frustum test then decides, as with no table)
    // (and the bits it already wrote cleared from every node: the quadtree from 0x800DBE48, each node's +0 low bits (one a viewport),
    // its child at +C and next at +4, signed offsets from the node itself, both 0 at the end; walked as func_150A6760 walks it)
    if ((uint32_t)MEM_W(0, (gpr)(int32_t)0x800BE5D0u) != 0) {
        MEM_W(0, (gpr)(int32_t)0x800BE5D0u) = 0;
        uint32_t node = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800DBE48u);
        const uint32_t vp = std::min<uint32_t>((uint32_t)MEM_W(0, (gpr)(int32_t)0x80082FA0u), 7u);
        const uint16_t mask = (uint16_t)((2u << vp) - 1u);
        for (int guard = 0; (node >> 24) == 0x80 && guard < 200000; guard++) {
            MEM_H(0, (gpr)(int32_t)node) = (int16_t)((uint16_t)MEM_H(0, (gpr)(int32_t)node) & (uint16_t)~mask);
            const int16_t c = (int16_t)MEM_H(0xC, (gpr)(int32_t)node), n = (int16_t)MEM_H(4, (gpr)(int32_t)node);
            if (c != 0) node += (uint32_t)(int32_t)c;
            else if (n != 0) node += (uint32_t)(int32_t)n;
            else break;
        }
        static uint32_t told = 0;
        if (told++ < 10) {
            survey("[cave] Bunkers' visibility table left out (0x800BE5D0 cleared, the quadtree's bits cleared)");
        }
    }
    // (no peeing on the custom maps: Bunkers' match script (bank 20 entry 0xBD) gives B the pee action (context 3, brain +0x75) to
    // anyone inside its trigger zone 0 (Bunkers' toilets, a box |x-302| < 1090, |z+440| < 1247, y -821 to -302), which every custom
    // map, fitted into Bunkers' space, has in its middle. That zone switched off while a custom map is played: its +0x14 set to 1, the
    // game's own "zone off" (func_150A1DA0 then answers "outside" for everyone); the same on every machine, so nothing to send)
    {
        const uint32_t zones = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800D3098u);
        if ((zones >> 24) == 0x80 && (int32_t)MEM_W(0, (gpr)(int32_t)0x800D3094u) > 0 && MEM_BU(0x14, (gpr)(int32_t)zones) != 1) {
            MEM_B(0x14, (gpr)(int32_t)zones) = 1;
            static uint32_t told_zone = 0;
            if (told_zone++ < 10) {
                survey("[cave] Bunkers' pee zone (trigger zone 0) switched off on the custom map");
            }
        }
    }
    // (every character: put on the map's spawn spots when off it or come back; player 1 on the saved spawn, offline)
    if (MEM_BU(0, (gpr)(int32_t)0x800BE616) != 0) {
        cave_fix_spawns(rdram, spawn, have_spawn && !conker::netsession::active());
        for (int q = 0; q < 3; q++) {
            const uint32_t bits = (uint32_t)MEM_W(4 * q, (gpr)(int32_t)place);
            std::memcpy(&now[q], &bits, 4);
        }
    }
    uint16_t held = 0;
    if (conker::netsession::active()) {
        bool plugged = false;
        float x = 0.0f, y = 0.0f;
        conker::netsession::input(conker::netsession::port_of_player(0), &plugged, &held, &x, &y);
    } else {
        uint16_t pressed = 0;
        conker::frontend::probe_buttons(0, &held, &pressed);
    }
    const bool crouched = (held & 0x2000) != 0;
    still = (crouched && jump < 5.0f) ? still + 1 : 0;
    if (still == 90) {
        spawn[0] = now[0]; spawn[1] = now[1]; spawn[2] = now[2];
        have_spawn = true;
#if defined(CONKER_RT64)
        std::ofstream f(recompui::file::get_asset_path(cave_asset("spawn.txt").c_str()));
        f << now[0] << " " << now[1] << " " << now[2] << "\n";
#endif
        char line[120];
        std::snprintf(line, sizeof(line), "[cave] spawn saved at %.0f %.0f %.0f", now[0], now[1], now[2]);
        kong_say(line);
        conker::netsession::log(line);
        conker::notices::show_top("Spawn point saved here");
    }
    // (where player 1 is, now and then: to place things by what's seen in the videos)
    static uint32_t frames = 0;
    if (++frames % 150 == 0) {
        static uint32_t told = 0;
        if (told++ < 200) {
            char line[100];
            std::snprintf(line, sizeof(line), "[cave] player 1 at %.0f %.0f %.0f", now[0], now[1], now[2]);
            survey(line);
        }
    }
    last[0] = now[0]; last[1] = now[1]; last[2] = now[2];
    have_last = true;
}

// The arena's setup records ($a list of count records of 0x44: the place at +0, +2, +4, halfwords) moved onto the cave: the arena's
// own spread (its records' x and z) to the cave's (a tenth in from its sides), its heights in proportion, then each put on the floor
// under it nearest that height. Every machine alike (the same files, the same level).
void conker::mp_characters::cave_props(uint8_t* rdram, uint32_t list, uint32_t count) {
    if (!cave_loaded || count == 0 || (list >> 24) != 0x80) {
        return;
    }
    float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
    for (uint32_t i = 0; i < count; i++) {
        for (int q = 0; q < 3; q++) {
            const float v = (float)MEM_H(2 * q, (gpr)(int32_t)(list + i * 0x44u));
            lo[q] = std::min(lo[q], v);
            hi[q] = std::max(hi[q], v);
        }
    }
    // (each record on its own spot of the map's spread ones, after the 16 spawns: worked out from the map's floors alone, so any map
    // gets its weapons and props spread over all of it, well inside, none on another; the same on every machine)
    static int props_map = -1;
    static size_t next_spot = 16;
    if (cave_spawns_map != cave_map || cave_spawns.empty()) {
        cave_make_spawns();
    }
    if (props_map != cave_map) {
        props_map = cave_map;
        next_spot = 16;
    }
    int moved = 0;
    for (uint32_t i = 0; i < count; i++) {
        const gpr r = (gpr)(int32_t)(list + i * 0x44u);
        float p[3];
        if (!cave_spawns.empty()) {
            const auto& sp = cave_spawns[next_spot % cave_spawns.size()];
            next_spot++;
            p[0] = sp[0]; p[1] = sp[1]; p[2] = sp[2];
        } else {
            for (int q = 0; q < 3; q++) p[q] = (cave_min[q] + cave_max[q]) * 0.5f;
        }
        p[1] = cave_floor(p[0], p[2], p[1]) + 2.0f;
        for (int q = 0; q < 3; q++) {
            MEM_H(2 * q, r) = (int16_t)std::lround(std::clamp(p[q], -32000.0f, 32000.0f));
        }
        moved++;
        if (moved <= 12) {
            char line[140];
            std::snprintf(line, sizeof(line), "[cave] record %u (kind %08X) to %.0f %.0f %.0f", (unsigned)i, (unsigned)(uint32_t)MEM_W(0x10, r), p[0], p[1], p[2]);
            kong_say(line);
        }
    }
    char line[100];
    std::snprintf(line, sizeof(line), "[cave] %d of the arena's setup records moved onto the cave", moved);
    kong_say(line);
}

// func_1502B6BC's start: the path being loaded ($a3 its depth, the steps at 0x10 and 0x14 of the stack)
extern "C" void conker_load_path(uint8_t* rdram, recomp_context* ctx) {
    path_depth = (uint32_t)ctx->r7;
    path_a = path_depth >= 1 ? (uint32_t)MEM_W(0x10, ctx->r29) : 0;
    path_b = path_depth >= 2 ? (uint32_t)MEM_W(0x14, ctx->r29) : 0;
    swapping = Swap::None;
    // (Kong's own model number) whatever is loaded for it is the teddy's: his model (bank 1, read from the teddy's entry and put in place)
    // and everything else a model has (its other banks: its settings, its animations), so he moves as the teddy does
    if (path_depth == 2 && path_b == (uint32_t)kong_model) {
        kong_load();
        if (kong_loaded) {
            MEM_W(0x14, ctx->r29) = teddy_model;
            static uint32_t told = 0;
            if (told < 20) {
                told++;
                char line[96];
                std::snprintf(line, sizeof(line), "[kong] bank %u's %02X loaded as the teddy's (%02X)", (unsigned)path_a, (unsigned)kong_model, (unsigned)teddy_model);
                survey(line);
            }
        }
    }
    // (an added character, in multiplayer: its model's other banks are 3B's; its model, its own entry, rebuilt as it's unpacked,
    // conker_load_unpacked) (only while we draw one as it, draw_now, or in multiplayer: Berri's select screen loaded her with the flag
    // clear, her own 45-bone model drawn on 3B's bones, a jumble)
    if (path_depth >= 1 && path_a == 4 && MEM_BU(0, (gpr)(int32_t)0x800BE616) == 0) {
        for (Extra& x : extras) {
            x.on_select = false;   // (a level outside multiplayer: the story's)
        }
    }
    for (int i = 0; i < kExtras && path_depth == 2; i++) {
        Extra& x = extras[i];
        if (path_b != (uint32_t)x.model || !(x.draw_now > 0 || extra_now(rdram, x))) {
            continue;
        }
        extra_load(x);
        if (!x.loaded) {
            continue;
        }
        if (path_a != 1) {
            MEM_W(0x14, ctx->r29) = x.base_model;
        } else {
            swapping = Swap::Extra;
            swapping_extra = i;
        }
        static uint32_t told = 0;
        if (told < 40) {
            told++;
            char line[96];
            std::snprintf(line, sizeof(line), "[%s] bank %u's %02X loaded %s", x.tag, (unsigned)path_a, (unsigned)x.model,
                path_a == 1 ? "as its own, rebuilt" : "as 3B's");
            survey(line);
        }
    }
    if (path_depth == 2 && ((path_a == 3 && path_b == 0x52) || (path_a == 9 && path_b == 0x3A))) {
        sword_load();
        const int w = path_a == 3 ? 0 : 1;
        if (!sword_bins[w].empty()) {
            swapping = Swap::Weapon;
            swapping_weapon = w;
            return;
        }
    }
    cave_rdram = rdram;
    if (path_depth == 2 && path_a == 4 && conker::mp_characters::cave_on((int)path_b)) {
        swapping = Swap::Cave;
    } else if (path_depth == 2 && path_a == 1 && path_b == (uint32_t)kong_model) {
        swapping = kong_loaded ? Swap::Kong : Swap::None;
    } else if (path_depth == 2 && path_a == 4 && (path_b == 0x32 || path_b == 0x33)) {
        lagoon_load();
        swapping = lagoon_loaded ? Swap::Lagoon : Swap::None;
    }
    if (path_depth >= 1 && path_a == 4) {
        kong_told = 0;
        models_told = 0;
        static uint32_t told = 0;
        if (told < 60) {
            told++;
            char line[120];
            std::snprintf(line, sizeof(line), "[levels] bank 4's %02X loaded (depth %u)%s", (unsigned)path_b, (unsigned)path_depth,
                swapping == Swap::Lagoon ? ": Pirate Lagoon in its place" : "");
            survey(line);
        }
    }
}

// ---- (0.11.4) The gold weapons from the small teddy's view, on the holder's own screen only ----
// A player holding a gold weapon in R-Look (their camera's look stage, +0x73C) or scoped is drawn on their own machine as the small
// teddy (model 75): its model, worked out on its own skeleton, with the player's own animations (their model's group). Only on that
// player's own machine (online: this machine's player; offline: player 1), and only while the teddy's model is loaded already (the
// record table D_800D1C90 has it: a teddy is in the match); everyone else sees the player as their own character. The camera's from the
// teddy's height on every machine (conker_kong_look_height).
namespace {
    constexpr int small_teddy_model = 0x75;
    int teddy_own_model[64];          // (the model put back after, per character; 0 none)
    bool teddy_updating[64] = {}, teddy_drawing[64] = {};
    int teddy_group_of = -1;          // (while one is worked out: the model whose animations' group it takes)
    // (30 Oct test: off. The camera lowered to the teddy's height sat inside a big character's own body, which the gold scope draws in the
    // holder's view: the screen went black; and the holder's own model, no longer at +4 while the teddy was, was let go and loaded again
    // every few frames. Off until the teddy can be drawn without either.)
    constexpr bool gold_teddy_on = false;
    bool gold_teddy_now(uint8_t* rdram, int k) {
        if (!gold_teddy_on) {
            return false;
        }
        if (k < 0 || k >= 25) {
            return false;
        }
        const int own = conker::netsession::active() ? conker::netsession::local_player() : 0;
        if (k != own) {
            return false;
        }
        if (conker::gold_weapons::held_by(rdram, k) < 0) {
            return false;
        }
        const uint32_t ob = 0x800CC2D0u + (uint32_t)k * 0x32Cu;
        const uint32_t cam = (uint32_t)MEM_W(0x318, (gpr)(int32_t)ob);
        const uint32_t aim = (uint32_t)MEM_W(0x31C, (gpr)(int32_t)ob);
        const bool cam_ok = (cam >> 24) == 0x80 && (cam & 0x00FFFFFFu) < 0x007F0000u;
        const bool aim_ok = (aim >> 24) == 0x80 && (aim & 0x00FFFFFFu) < 0x007F0000u;
        const bool looking = (cam_ok && MEM_H(0x73C, (gpr)(int32_t)cam) != 0) || (aim_ok && MEM_BU(0x197, (gpr)(int32_t)aim) != 0);
        if (!looking) {
            return false;
        }
        const uint32_t record = (uint32_t)MEM_W(4u * (uint32_t)small_teddy_model, (gpr)(int32_t)0x800D1C90);
        const bool loaded = (record >> 24) == 0x80;
        static uint32_t told = 0;
        static bool last = false;
        if (loaded != last && told < 20) {
            told++;
            last = loaded;
            char line[140];
            std::snprintf(line, sizeof(line), "[gold] character %d aiming a gold weapon: %s", k,
                loaded ? "drawn as the small teddy on this screen" : "the small teddy's model isn't loaded (no teddy in the match): drawn as itself");
            survey(line);
        }
        return loaded;
    }
}

// func_15084D00 at 0x15084D64: $v1 the model, in none of the animations' groups (D_80087240's seven lists, D_8009D954 their lengths):
// for Berri (0x72) in multiplayer, the group 3B is in, its first model, as the function gives for a model in a group
extern "C" void conker_berri_anim_group(uint8_t* rdram, recomp_context* ctx) {
    // (the small teddy worked out for a gold weapon's holder: the holder's own model's group)
    if ((uint32_t)ctx->r3 == (uint32_t)small_teddy_model && teddy_group_of >= 0) {
        for (uint32_t g = 0; g < 7; g++) {
            const uint32_t list = (uint32_t)MEM_W(4 * g, (gpr)(int32_t)0x80087240u);
            const uint32_t n = MEM_BU(g, (gpr)(int32_t)0x8009D954u);
            if ((list >> 24) != 0x80) {
                continue;
            }
            for (uint32_t i = 0; i < n; i++) {
                if (MEM_BU(i, (gpr)(int32_t)list) == (uint8_t)teddy_group_of) {
                    ctx->r3 = (gpr)(int32_t)MEM_BU(0, (gpr)(int32_t)list);
                    return;
                }
            }
        }
        return;
    }
    const Extra* x = nullptr;
    for (const Extra& e : extras) {
        if ((uint32_t)ctx->r3 == (uint32_t)e.model && e.draw_now > 0) {
            x = &e;
        }
    }
    if (x == nullptr) {
        return;
    }
    for (uint32_t g = 0; g < 7; g++) {
        const uint32_t list = (uint32_t)MEM_W(4 * g, (gpr)(int32_t)0x80087240u);
        const uint32_t n = MEM_BU(g, (gpr)(int32_t)0x8009D954u);
        if ((list >> 24) != 0x80) {
            continue;
        }
        for (uint32_t i = 0; i < n; i++) {
            if (MEM_BU(i, (gpr)(int32_t)list) == (uint8_t)x->base_model) {
                ctx->r3 = (gpr)(int32_t)MEM_BU(0, (gpr)(int32_t)list);
                static uint32_t told = 0;
                if (told < 8) {
                    told++;
                    char line[96];
                    std::snprintf(line, sizeof(line), "[%s] its animations 3B's group's (model %02X)", x->tag, (unsigned)(uint32_t)ctx->r3);
                    survey(line);
                }
                return;
            }
        }
    }
}

// func_1502B6BC at 0x1502B780: the entry unpacked ($v0 its buffer)
extern "C" void conker_load_unpacked(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (swapping == Swap::Weapon && swapping_weapon >= 0 && ((uint32_t)ctx->r2 >> 24) == 0x80) {
        const std::vector<uint8_t>& b = sword_bins[swapping_weapon];
        const uint32_t at = (uint32_t)ctx->r2;
        for (size_t i = 0; i < b.size(); i++) {
            MEM_B((uint32_t)i, (gpr)(int32_t)at) = (int8_t)b[i];
        }
        char line[96];
        std::snprintf(line, sizeof(line), "[sword] the %s put in at %08X (%u bytes)", swapping_weapon == 0 ? "pickup" : "held one", (unsigned)at, (unsigned)b.size());
        kong_say(line);
        swapping = Swap::None;
        return;
    }
    if (swapping == Swap::Extra && swapping_extra >= 0 && swapping_extra < kExtras && ((uint32_t)ctx->r2 >> 24) == 0x80) {
        const Extra& x = extras[swapping_extra];
        const uint32_t at = (uint32_t)ctx->r2;
        const uint32_t theirs = (uint32_t)x.bin.size();
        // (its entry's own unpacked size: ours is the same, its model rebuilt in place)
        for (uint32_t i = 0; i < theirs; i++) {
            MEM_B(i, (gpr)(int32_t)at) = (int8_t)x.bin[i];
        }
        swapping = Swap::None;
        char line[96];
        std::snprintf(line, sizeof(line), "[%s] its model put in at %08X (%u bytes)", x.tag, (unsigned)at, (unsigned)theirs);
        kong_say(line);
        return;
    }
    if (swapping == Swap::Lagoon && ((uint32_t)ctx->r2 >> 24) == 0x80) {
        lagoon_base = (uint32_t)ctx->r2;
        char line[96];
        std::snprintf(line, sizeof(line), "[lagoon] the level unpacked at %08X", (unsigned)lagoon_base);
        kong_say(line);
    }
}

// (Kong's M4 pose) The teddy's own model, from his entry as read, before Kong's is put in its place, once: his skeleton (each bone's
// parent, offset and place in his starting pose) and where his vertices are on each bone, in that pose. The teddy's animations turn his
// bones from that pose, so Kong's arms are rigged to the teddy's (they were to Conker's, and the M4 came out held beside his head).
namespace {
    void teddy_survey(uint8_t* rdram, uint32_t buffer, uint32_t room) {
        static bool told = false;
        if (told || room < 8 || room > 0x10000u) {
            return;
        }
        told = true;
        std::vector<uint8_t> packed(room);
        for (uint32_t i = 0; i < room; i++) {
            packed[i] = (uint8_t)MEM_BU(i, (gpr)(int32_t)buffer);
        }
        const uint32_t size = ((uint32_t)packed[0] << 24) | ((uint32_t)packed[1] << 16) | ((uint32_t)packed[2] << 8) | packed[3];
        if (size < 0x40 || size > 0x40000u) {
            survey("[teddy] his entry's size makes no sense");
            return;
        }
        std::vector<uint8_t> f(size);
        const size_t got = tinfl_decompress_mem_to_mem(f.data(), f.size(), packed.data() + 4, packed.size() - 4, 0);
        if (got == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED || got < 0x40) {
            survey("[teddy] his entry didn't unpack");
            return;
        }
        auto u32 = [&](size_t at) { return at + 4 <= got ? ((uint32_t)f[at] << 24) | ((uint32_t)f[at + 1] << 16) | ((uint32_t)f[at + 2] << 8) | f[at + 3] : 0u; };
        auto s16 = [&](size_t at) { return at + 2 <= got ? (int16_t)(((uint32_t)f[at] << 8) | f[at + 1]) : (int16_t)0; };
        auto f32 = [&](size_t at) { const uint32_t b = u32(at); float v; std::memcpy(&v, &b, 4); return v; };
        const uint32_t dltab = u32(0x8), skel = u32(0x10), skel_size = u32(0x14);
        const uint32_t bones = skel_size / 16u;
        char line[200];
        std::snprintf(line, sizeof(line), "[teddy] his model: %u bytes, display lists' table at %X, skeleton at %X (%u bones)", (unsigned)got,
            (unsigned)dltab, (unsigned)skel, (unsigned)bones);
        survey(line);
        if (bones == 0 || bones > 64 || skel + 16u * bones > got) {
            return;
        }
        std::vector<int> parent(bones);
        std::vector<std::array<float, 3>> offset(bones), at(bones);
        for (uint32_t b = 0; b < bones; b++) {
            const size_t e = skel + 16u * b;
            parent[b] = f[e];
            offset[b] = { f32(e + 4), f32(e + 8), f32(e + 12) };
            at[b] = offset[b];
            if (parent[b] != (int)b && parent[b] < (int)b) {
                for (int i = 0; i < 3; i++) {
                    at[b][i] += at[parent[b]][i];
                }
            }
            std::snprintf(line, sizeof(line), "  bone %2u: id %u, parent %u, offset %.1f %.1f %.1f, at %.1f %.1f %.1f", (unsigned)b, (unsigned)f[e + 1],
                (unsigned)parent[b], offset[b][0], offset[b][1], offset[b][2], at[b][0], at[b][1], at[b][2]);
            survey(line);
        }
        // the display list: DA380003 03000000+bone*0x40 picks the bone, 01 loads that bone's vertices (their offset in the file)
        std::vector<std::array<float, 6>> box(bones, { 1e9f, 1e9f, 1e9f, -1e9f, -1e9f, -1e9f });
        std::vector<uint32_t> count(bones, 0);
        uint32_t list = u32(dltab), bone = 0, commands = 0;
        for (uint32_t p = list; p + 8 <= got && commands < 20000; p += 8, commands++) {
            const uint32_t w0 = u32(p), w1 = u32(p + 4);
            if ((w0 >> 24) == 0xDF) {
                break;
            }
            if ((w0 >> 28) == 1) {
                continue;
            }
            if ((w0 >> 24) == 0xDA && (w1 >> 24) == 0x03) {
                bone = ((w1 & 0xFFFFFFu) / 0x40u);
            } else if ((w0 >> 24) == 0x01 && bone < bones) {
                const uint32_t n = (w0 >> 12) & 0xFFu, v = w1 & 0xFFFFFFu;
                for (uint32_t k = 0; k < n && v + 16u * k + 6 <= got; k++) {
                    for (int i = 0; i < 3; i++) {
                        const float c = (float)s16(v + 16u * k + 2u * (uint32_t)i) + at[bone][i];
                        box[bone][i] = std::min(box[bone][i], c);
                        box[bone][3 + i] = std::max(box[bone][3 + i], c);
                    }
                    count[bone]++;
                }
            }
        }
        for (uint32_t b = 0; b < bones; b++) {
            if (count[b] == 0) {
                continue;
            }
            std::snprintf(line, sizeof(line), "  bone %2u's vertices (%u loaded): %.0f %.0f %.0f to %.0f %.0f %.0f", (unsigned)b, (unsigned)count[b],
                box[b][0], box[b][1], box[b][2], box[b][3], box[b][4], box[b][5]);
            survey(line);
        }
    }
}

// (Kong's aim) func_15124C38 at 0x15124E1C ($s0 the camera, $f14 the height its look-at point keeps above the character): in the look
// mode (hold R, aiming; the camera's +0x2C has 0x100) the height is the teddy's, which for Kong is his chest, so aiming with the M4
// showed nothing but his back. For Kong it's raised by kong_look_lift: the game's own look-at point, so its eye, its crosshair and its
// aim all go from there, the same on every machine.
namespace {
    constexpr float kong_look_lift = 0.0f;   // (85 while he was half again the teddy's size; now he's the teddy's)
}
int conker_extra_of_character(int k);
float conker_extra_camera_scale(int k);
extern "C" void conker_kong_look_height(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t camera = (uint32_t)ctx->r16;
    // (0.11.4) A gold weapon's holder in R-Look (the camera's look stage, +0x73C) or scoped: the camera looks from the small teddy's height
    // (model 75: its record's height, 95, and base, 43, as func_15124C38 works it out from the character's own: +0xCC of the character,
    // +0x114 of its aim's record, the camera's way +0x4E), whoever the character is: the view the gold weapons look best from. Every
    // camera on every machine alike, from the game's memory.
    if ((camera >> 24) == 0x80 && (uint32_t)MEM_W(0x3D0, (gpr)(int32_t)camera) >= 0x800CC2D0u) {
        const uint32_t walker = (uint32_t)MEM_W(0x3D0, (gpr)(int32_t)camera), aim = (uint32_t)MEM_W(0x3D4, (gpr)(int32_t)camera);
        const uint32_t off = walker - 0x800CC2D0u;
        const int k = (off % 0x32Cu == 0 && off / 0x32Cu < 25) ? (int)(off / 0x32Cu) : -1;
        const uint32_t held_aim = k >= 0 ? (uint32_t)MEM_W(0x31C, (gpr)(int32_t)walker) : 0u;
        const bool looking = MEM_H(0x73C, (gpr)(int32_t)camera) != 0 ||
            ((held_aim >> 24) == 0x80 && (held_aim & 0x00FFFFFFu) < 0x007F0000u && MEM_BU(0x197, (gpr)(int32_t)held_aim) != 0);
        if (gold_teddy_on && k >= 0 && looking && (aim >> 24) == 0x80 && conker::gold_weapons::held_by(rdram, k) >= 0) {
            const float own_base = (float)MEM_H(0xCC, (gpr)(int32_t)walker), own_height = (float)MEM_H(0x114, (gpr)(int32_t)aim);
            float teddy_base = 43.0f, teddy_height = 95.0f;
            {
                const uint32_t record = (uint32_t)MEM_W(4u * 0x75u, (gpr)(int32_t)0x800D1C90);
                if ((record >> 24) == 0x80 && (record & 0x00FFFFFFu) < 0x007F0000u) {
                    teddy_base = (float)(uint16_t)((uint32_t)MEM_W(4, (gpr)(int32_t)record) & 0xFFFFu);
                    teddy_height = (float)(uint16_t)((uint32_t)MEM_W(0x10, (gpr)(int32_t)record) >> 16);
                }
            }
            if (own_height > 0.0f && teddy_height > 0.0f) {
                const float was = ctx->f14.fl;
                const bool height_alone = (MEM_BU(0x4E, (gpr)(int32_t)aim) & 0xFu) == 1u;
                ctx->f14.fl = height_alone ? was * (teddy_height / own_height) : teddy_base + (was - own_base) * (teddy_height / own_height);
                static uint32_t told_g = 0;
                if (told_g < 12) {
                    told_g++;
                    char line[200];
                    std::snprintf(line, sizeof(line), "[gold] character %d's camera from the teddy's height: %.1f, not %.1f (its own %d over %d, the teddy's %d over %d)", k,
                        ctx->f14.fl, was, (int)own_height, (int)own_base, (int)teddy_height, (int)teddy_base);
                    survey(line);
                }
                return;
            }
        }
    }
    // (0.11.4) an added character taller than 3B (the alien, the Panther King): its camera's look-at height scaled up with it, in every
    // mode (it was 3B's, low behind him); logged with the camera's mode a few times
    if ((camera >> 24) == 0x80) {
        for (int k = 0; k < 5; k++) {
            const gpr o = (gpr)(int32_t)(0x800CC2D0u + (uint32_t)k * 0x32Cu);
            if ((uint32_t)MEM_W(0x318, o) != camera) {
                continue;
            }
            const float scale = conker_extra_camera_scale(k);
            if (scale != 1.0f) {
                static uint32_t told_x = 0;
                if (told_x < 12) {
                    told_x++;
                    char line[140];
                    std::snprintf(line, sizeof(line), "[extra] character %d's camera: height %.1f to %.1f (mode %08X)", k, ctx->f14.fl, ctx->f14.fl * scale,
                        (unsigned)(uint32_t)MEM_W(0x2C, (gpr)(int32_t)camera));
                    survey(line);
                }
                ctx->f14.fl *= scale;
                return;
            }
            break;
        }
    }
    if (!kong_loaded || (camera >> 24) != 0x80 || ((uint32_t)MEM_W(0x2C, (gpr)(int32_t)camera) & 0x100u) == 0) {
        return;
    }
    for (int k = 0; k < 5; k++) {
        const gpr o = (gpr)(int32_t)(0x800CC2D0u + (uint32_t)k * 0x32Cu);
        if ((uint32_t)MEM_W(0x318, o) != camera) {
            continue;
        }
        if (!conker::mp_characters::kong_drawn_as(k)) {
            return;
        }
        static uint32_t told = 0;
        if (told < 6) {
            told++;
            char line[140];
            std::snprintf(line, sizeof(line), "[kong] his look camera raised: character %d, height %.1f to %.1f", k, ctx->f14.fl, ctx->f14.fl + kong_look_lift);
            survey(line);
        }
        ctx->f14.fl += kong_look_lift;
        return;
    }
}

namespace {
    uint32_t be32(const std::vector<uint8_t>& v, size_t at) {
        return at + 4 <= v.size() ? ((uint32_t)v[at] << 24) | ((uint32_t)v[at + 1] << 16) | ((uint32_t)v[at + 2] << 8) | v[at + 3] : 0u;
    }
    void put32(std::vector<uint8_t>& v, uint32_t x) {
        v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16)); v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x);
    }
    // The course's entry (packed, in the game's buffer) unpacked, and Pirate Lagoon's built in its shape, packed the same way (its size
    // unpacked, big-endian, then raw deflate). Empty when it can't be (logged).
    std::vector<uint8_t> lagoon_entry_for(uint8_t* rdram, uint32_t buffer, uint32_t room, uint32_t theirs_unpacked, const std::vector<uint8_t>& lagoon_rzip, bool keep_others = false) {
        char line[200];
        std::vector<uint8_t> packed(room);
        for (uint32_t i = 0; i < room; i++) {
            packed[i] = (uint8_t)MEM_BU(i, (gpr)(int32_t)buffer);
        }
        std::vector<uint8_t> theirs(theirs_unpacked);
        const size_t got = tinfl_decompress_mem_to_mem(theirs.data(), theirs.size(), packed.data() + 4, packed.size() - 4, 0);
        std::vector<uint8_t> lagoon(be32(lagoon_rzip, 0));
        const size_t got_lagoon = lagoon.empty() ? 0 : tinfl_decompress_mem_to_mem(lagoon.data(), lagoon.size(), lagoon_rzip.data() + 4, lagoon_rzip.size() - 4, 0);
        if (got == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED || got < 16 || got_lagoon == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED || got_lagoon < 16) {
            kong_say("[lagoon] not put in: the course's entry or Lagoon's didn't unpack");
            return {};
        }
        theirs.resize(got);
        lagoon.resize(got_lagoon);
        const uint32_t pieces = be32(theirs, 0) / 8u;
        if (pieces == 0 || pieces > 512 || pieces * 8u > got) {
            std::snprintf(line, sizeof(line), "[lagoon] not put in: the course's entry has %u pieces?", (unsigned)pieces);
            kong_say(line);
            return {};
        }
        {
            std::string list = "[lagoon] the course's pieces (offset, size):";
            for (uint32_t k = 0; k < pieces && k < 24; k++) {
                char piece[32];
                std::snprintf(piece, sizeof(piece), " %X,%08X", (unsigned)be32(theirs, 8u * k), (unsigned)be32(theirs, 8u * k + 4));
                list += piece;
            }
            std::snprintf(line, sizeof(line), " (%u in all)", (unsigned)pieces);
            list += line;
            survey(list.c_str());
        }
        // (each of the course's pieces: its first 12 words, to see which are its track and which other data)
        for (uint32_t k = 0; k < pieces && k < 12; k++) {
            const uint32_t off = be32(theirs, 8u * k), size = be32(theirs, 8u * k + 4) & 0x0FFFFFFFu;
            std::string words;
            for (uint32_t w = 0; w < 12 && off + 4u * w + 4u <= got && 4u * w < size; w++) {
                char piece[12];
                std::snprintf(piece, sizeof(piece), " %08X", (unsigned)be32(theirs, off + 4u * w));
                words += piece;
            }
            std::snprintf(line, sizeof(line), "  piece %u (%u bytes):", (unsigned)k, (unsigned)size);
            survey((std::string(line) + words).c_str());
        }
        // Lagoon's own entry: one piece, the track (its table's first pair: offset and size)
        const uint32_t track_at = be32(lagoon, 0), track_size = be32(lagoon, 4) & 0x0FFFFFFFu;
        if (track_at + track_size > lagoon.size()) {
            kong_say("[lagoon] not put in: Lagoon's entry makes no sense");
            return {};
        }
        // (the cave: fitted into the arena's piece 0's space: its vertices', from 0x28 to its display list)
        if (keep_others && pieces > 0) {
            const uint32_t p0 = be32(theirs, 0), p0_size = be32(theirs, 4) & 0x0FFFFFFFu;
            const uint32_t their_dl = p0 + be32(theirs, p0);
            float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
            for (uint32_t v = p0 + 0x28; v + 16 <= their_dl && v + 16 <= p0 + p0_size && v + 6 <= theirs.size(); v += 16) {
                for (int q = 0; q < 3; q++) {
                    const float c = (float)(int16_t)(((uint16_t)theirs[v + 2 * q] << 8) | theirs[v + 2 * q + 1]);
                    lo[q] = std::min(lo[q], c);
                    hi[q] = std::max(hi[q], c);
                }
            }
            cave_tris = cave_tris0;
            for (int q = 0; q < 3; q++) {
                cave_min[q] = cave_min0[q];
                cave_max[q] = cave_max0[q];
            }
            const float cx = cave_max[0] - cave_min[0], cz = cave_max[2] - cave_min[2];
            if (hi[0] > lo[0] && cx > 1.0f && cz > 1.0f) {
                cave_fit_scale = std::min((hi[0] - lo[0]) / cx, (hi[2] - lo[2]) / cz);
                cave_fit_from[0] = (cave_min[0] + cave_max[0]) * 0.5f; cave_fit_from[1] = cave_min[1]; cave_fit_from[2] = (cave_min[2] + cave_max[2]) * 0.5f;
                cave_fit_to[0] = (lo[0] + hi[0]) * 0.5f; cave_fit_to[1] = lo[1]; cave_fit_to[2] = (lo[2] + hi[2]) * 0.5f;
                const uint32_t c = track_at, dl_at = c + be32(lagoon, c);
                for (uint32_t v = c + 0x28; v + 16 <= dl_at && v + 6 <= lagoon.size(); v += 16) {
                    float pnt[3];
                    for (int q = 0; q < 3; q++) {
                        pnt[q] = (float)(int16_t)(((uint16_t)lagoon[v + 2 * q] << 8) | lagoon[v + 2 * q + 1]);
                    }
                    cave_fit_point(pnt);
                    for (int q = 0; q < 3; q++) {
                        const int16_t w = (int16_t)std::lround(std::clamp(pnt[q], -32000.0f, 32000.0f));
                        lagoon[v + 2 * q] = (uint8_t)((uint16_t)w >> 8);
                        lagoon[v + 2 * q + 1] = (uint8_t)w;
                    }
                }
                for (CaveTri& t : cave_tris) {
                    cave_fit_point(t.a); cave_fit_point(t.b); cave_fit_point(t.c);
                }
                cave_fit_point(cave_min); cave_fit_point(cave_max);
                std::snprintf(line, sizeof(line), "[cave] fitted into the arena's space (%.0f %.0f %.0f to %.0f %.0f %.0f): scale %.3f, now %.0f %.0f %.0f to %.0f %.0f %.0f",
                    lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], cave_fit_scale, cave_min[0], cave_min[1], cave_min[2], cave_max[0], cave_max[1], cave_max[2]);
                kong_say(line);
            }
        }
        // an empty piece: the header (the display list's place and length, the collision's, none), 0x80000000 at 0x24, the list's end
        std::vector<uint8_t> empty;
        for (uint32_t w : { 0x28u, 8u, 0u, 0u, 0x30u, 0u, 0u, 0u, 0u, 0x80000000u, 0xDF000000u, 0u }) {
            put32(empty, w);
        }
        std::vector<uint8_t> entry;
        uint32_t at = pieces * 8u;
        // (the cave: the arena's other pieces kept as they are, its doors and props' models, which its setup goes by: emptied, Bunkers
        // crashed setting them up, func_150A47FC reading segment 0x0A's +0xA)
        const auto their_piece = [&](uint32_t k, uint32_t* at_out) {
            const uint32_t off = be32(theirs, 8u * k), sz = be32(theirs, 8u * k + 4) & 0x0FFFFFFFu;
            *at_out = off;
            return (off + sz <= theirs.size()) ? sz : 0u;
        };
        for (uint32_t k = 0; k < pieces; k++) {
            uint32_t from = 0;
            const uint32_t size = k == 0 ? track_size : keep_others ? their_piece(k, &from) : (uint32_t)empty.size();
            put32(entry, at);
            put32(entry, (be32(theirs, 8u * k + 4) & 0xF0000000u) | size);
            at += (size + 7u) & ~7u;
        }
        for (uint32_t k = 0; k < pieces; k++) {
            uint32_t from = 0;
            if (k == 0) {
                entry.insert(entry.end(), lagoon.begin() + track_at, lagoon.begin() + track_at + track_size);
            } else if (keep_others) {
                const uint32_t size = their_piece(k, &from);
                const size_t start = entry.size();
                entry.insert(entry.end(), theirs.begin() + from, theirs.begin() + from + size);
                // (kept for the setup, but not seen: Bunkers' own doors and props drawn over the custom map. Every vertex of the
                // piece (from its +0x28 to its display list) put on one point, so its triangles have no area and nothing is drawn)
                if (size >= 0x28) {
                    const uint32_t dl = be32(entry, start);
                    for (uint32_t v = 0x28; v + 16 <= dl && v + 16 <= size; v += 16) {
                        for (uint32_t b = 0; b < 6; b++) {
                            entry[start + v + b] = 0;
                        }
                    }
                }
            } else {
                entry.insert(entry.end(), empty.begin(), empty.end());
            }
            while (entry.size() % 8u) {
                entry.push_back(0);
            }
        }
        if (entry.size() > theirs_unpacked) {
            std::snprintf(line, sizeof(line), "[lagoon] not put in: built %u bytes, the course's %u", (unsigned)entry.size(), (unsigned)theirs_unpacked);
            kong_say(line);
            return {};
        }
        size_t out_len = 0;
        void* deflated = tdefl_compress_mem_to_heap(entry.data(), entry.size(), &out_len, TDEFL_DEFAULT_MAX_PROBES);
        std::vector<uint8_t> out;
        if (deflated != nullptr) {
            put32(out, (uint32_t)entry.size());
            out.insert(out.end(), (uint8_t*)deflated, (uint8_t*)deflated + out_len);
            mz_free(deflated);
        }
        if (out.empty() || out.size() > room) {
            std::snprintf(line, sizeof(line), "[lagoon] not put in: packed %u bytes, room %u", (unsigned)out.size(), (unsigned)room);
            kong_say(line);
            return {};
        }
        std::snprintf(line, sizeof(line), "[lagoon] built for the course: %u pieces, %u bytes (%u packed)", (unsigned)pieces, (unsigned)entry.size(),
            (unsigned)out.size());
        kong_say(line);
        return out;
    }
}

// (Kong's aim with the M4 and the SMG) func_15120158 at 0x15120378 ($s0 the camera): func_151216F8 has just set the look's camera for its
// kind (+0x23E): for the M4 and the SMG (0x09, 0x39) 75 behind (+0x784), 5 (+0x190) and 0 (+0x99C). For Kong they're the shotgun's in the
// co-op mansion instead (kind 0x3B there: 106, -12 and -28), the view from over his head the player asked for; the guns' crosshairs are
// their own. From the camera's character's model, the same on every machine.
extern "C" void conker_kong_look_view(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t camera = (uint32_t)ctx->r16;
    if (!kong_loaded || (camera >> 24) != 0x80) {
        return;
    }
    const uint8_t kind = MEM_BU(0x23E, (gpr)(int32_t)camera);
    if (kind != 0x09 && kind != 0x39) {
        return;
    }
    for (int k = 0; k < 5; k++) {
        const gpr o = (gpr)(int32_t)(0x800CC2D0u + (uint32_t)k * 0x32Cu);
        if ((uint32_t)MEM_W(0x318, o) != camera) {
            continue;
        }
        // (and the teddy's: as wide as Kong, the game's own look for the M4 put its camera inside him and hid him)
        const bool kong = conker::mp_characters::kong_drawn_as(k);
        if (!kong && MEM_BU(4, o) != teddy_model) {
            return;
        }
        // (v4) straight behind him, not to the side: off to the side the camera ran into walls in narrow places and the game pulled it in
        // to his head, the crosshair (where the aim meets the world, as seen from the camera) jumping or going off the screen. A little
        // further back and above (-12) to see past his head.
        auto put = [&](uint32_t at, float v) { uint32_t b; std::memcpy(&b, &v, 4); MEM_W(at, (gpr)(int32_t)camera) = (int32_t)b; };
        auto get = [&](uint32_t at) { uint32_t b = (uint32_t)MEM_W(at, (gpr)(int32_t)camera); float v; std::memcpy(&v, &b, 4); return v; };
        const float was_784 = get(0x784), was_190 = get(0x190), was_99c = get(0x99C);
        put(0x784, 120.0f);
        put(0x190, -12.0f);
        put(0x99C, 0.0f);
        static uint32_t told = 0;
        if (told < 40) {
            told++;
            char line[220];
            std::snprintf(line, sizeof(line), "[kong] f=%u %s look with the gun (kind %02X): 120 behind, -12, 0 (the game's %.1f, %.1f, %.1f); a %.1f->%.1f b %.1f->%.1f",
                (unsigned)conker::netsession::frame(), kong ? "Kong's" : "the teddy's", (unsigned)kind, was_784, was_190, was_99c,
                get(0x18C), get(0x190), get(0x194), get(0x198));
            survey(line);
        }
        return;
    }
}

// ---- (0.11.4) Koala Kong's own place ----
// The multiplayer characters' table (D_800AB57C, 8 bytes each: +0 a scale, +4 a height, +5 the model, +6 the character the match makes)
// has 22 entries (0 to 21; the teddy 3, model B1, character 0x26), with other data right after it. Kong is character 22, his entry ours
// (kong_entry, in high memory: the teddy's with model BA), at the only two places the game reads the table: the select screens' model
// (func_151E7F60, 0x151E7FE8) and the match's start (func_151E2834, 0x151E2CF8, the character made: the teddy's, 0x26). The select
// screens go by a mask of the characters a mode offers, bit 22 his wherever the teddy is (func_151E0B70, 0x151E0D80), and turn through
// 0 to 21 (0x151E1060, 0x151E10D4: up to 22 instead; the wrap from 0 to the last, 0x151E1094 and 0x151E10C0: 22). The match makes the
// teddy for him; as each of its characters is made (func_15082A44: $a0 what it's made from, +4 the model; $a1 its number) and whenever
// a model is put on one (func_150837D4: $a0 the character, $a1 the model), the teddy's model is his for the characters who picked him.
// All of it from the game's own state, the same on every machine.
namespace {
    constexpr uint32_t kong_entry = 0x80FE0000u;
    constexpr int kong_character = 22;
    constexpr int kMaxObjects = 64;
    bool kong_picked[kMaxObjects] = {};
    // (the second look: with his own model number in the game's hands, the select screen's Kong fell through the floor and in a match his
    // model came out as garbage: the game has its own tables and checks by model number, the teddy's B1 among them, and BA isn't the
    // teddy's anywhere in them) So every character is the teddy, B1, for the game, and drawn as Kong (his model, BA) where kong_drawn:
    // func_1502DF38, which draws a character (and works its bones out from its model's skeleton), has BA at +4 from its start to its end.
    bool select_kong[kMaxObjects] = {};
    bool make_kong = false;
    // (the added characters, extras[]: by character, whether it's that one's)
    bool extra_picked[kExtras][kMaxObjects] = {};
    bool select_extra[kExtras][kMaxObjects] = {};
    bool extra_drawing[kExtras][kMaxObjects] = {};
    bool extra_updating[kExtras][kMaxObjects] = {};
    int select_putting_up_extra = -1;
    int8_t make_extra_stack[16] = {};   // (which one the make at each depth is, plus one; 0 none)
    void extra_entry_write(uint8_t* rdram, const Extra& x) {
        MEM_W(0, (gpr)(int32_t)x.entry) = (int32_t)0x3D8F5C29u;   // (3B's scale, 0.07: its own skeleton gives its own height)
        MEM_B(4, (gpr)(int32_t)x.entry) = 0;
        MEM_B(5, (gpr)(int32_t)x.entry) = (int8_t)x.base_model;   // (3B for the game: with Berri's own model number the select screen froze)
        MEM_B(6, (gpr)(int32_t)x.entry) = (int8_t)x.base_character;
        MEM_B(7, (gpr)(int32_t)x.entry) = 0;
    }
    int last_character() {
        int last = kong_character;
        for (const Extra& x : extras) {
            if (x.loaded && x.character > last) {
                last = x.character;
            }
        }
        return last;
    }
    int extra_of(int k);
}
int conker_extra_of_character(int k);
float conker_extra_camera_scale(int k);
namespace {
    // which added character a character is (picked or put up on a select screen), -1 none
    int extra_of(int k) {
        if (k < 0 || k >= kMaxObjects) {
            return -1;
        }
        for (int i = 0; i < kExtras; i++) {
            if (extras[i].loaded && (extra_picked[i][k] || select_extra[i][k])) {
                return i;
            }
        }
        return -1;
    }
    bool make_stack[16] = {};
    int make_stack_depth = 0;
    bool kong_drawn[kMaxObjects] = {};   // (as last drawn, for the camera's hooks)
    bool kong_drawing[kMaxObjects] = {};
    bool select_putting_up = false, select_putting_up_kong = false;

    void kong_entry_write(uint8_t* rdram) {
        MEM_W(0, (gpr)(int32_t)kong_entry) = (int32_t)0x3DA3D70A;   // (the teddy's scale)
        MEM_B(4, (gpr)(int32_t)kong_entry) = 0;
        MEM_B(5, (gpr)(int32_t)kong_entry) = (int8_t)teddy_model;   // (the teddy for the game: only drawn as Kong, see kong_drawn)
        MEM_B(6, (gpr)(int32_t)kong_entry) = 0x26;                 // (the teddy's character)
        MEM_B(7, (gpr)(int32_t)kong_entry) = 0;
    }
}

int conker_extra_of_character(int k) {
    return extra_of(k);
}

float conker_extra_camera_scale(int k) {
    const int i = extra_of(k);
    return i >= 0 ? extras[i].camera_scale : 1.0f;
}

void conker::mp_characters::kong_set_picked(int character, bool picked) {
    if (character >= 0 && character < 16) {
        kong_picked[character] = picked;
    }
}

bool conker::mp_characters::kong_pick(int pick) {
    return kong_loaded && pick == kong_character;
}

// func_151E7F60 at 0x151E7FE8: $t6 the character put up, $t0 its entry
extern "C" void conker_kong_select_entry(uint8_t* rdram, recomp_context* ctx) {
    kong_load();
    extras_load();
    select_putting_up = true;
    select_putting_up_kong = kong_loaded && (int32_t)ctx->r14 == kong_character;
    select_putting_up_extra = -1;
    for (int i = 0; i < kExtras; i++) {
        if (extras[i].loaded && (int32_t)ctx->r14 == extras[i].character) {
            select_putting_up_extra = i;
        }
    }
    if (select_putting_up_extra >= 0) {
        Extra& x = extras[select_putting_up_extra];
        x.on_select = true;
        extra_entry_write(rdram, x);
        ctx->r8 = (gpr)(int32_t)x.entry;
        return;
    }
    {
        static uint32_t told = 0;
        if (told < 30) {
            told++;
            char line[120];
            std::snprintf(line, sizeof(line), "[kong] select: character %d put up (Kong %d, his model loaded %d)", (int)(int32_t)ctx->r14,
                (int)select_putting_up_kong, (int)kong_loaded);
            survey(line);
        }
    }
    if (!select_putting_up_kong) {
        return;
    }
    kong_entry_write(rdram);
    ctx->r8 = (gpr)(int32_t)kong_entry;
}

// func_151E7F60's start ($a0 the slot, $a1 the character): the slot's character drawn as Kong or not from now, whether or not the screen
// makes the character over (the teddy and Kong are both the teddy's model to it: put up after Kong, the teddy was kept, and drawn as Kong)
extern "C" void conker_kong_select_slot(int slot, int character) {
    if (slot >= 0 && slot < 16) {
        kong_drawn[slot] = kong_loaded && character == kong_character;
    }
}

// func_151E2834 at 0x151E2CF8: $t9 the k-th player's pick times 8, $t6 its entry, $v0 the pick's place (D_800E0B90 + k)
// (0.11.4) The bots' characters, from the chat's /bot command: "/bot 2 banjo" makes Bot 2 (the match's character 2, its number in the
// kill feed) Banjo from the next match on; "/bot 2 off" the game's own pick again; "/bot all fox" every bot. Sent in the chat, so every
// machine reads the same line and the bots are alike online too.
namespace {
    int bot_override[16] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
    const char* const original_names[] = { "conker", "", "teddy", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "", "" };
}
std::string conker::mp_characters::bot_command(const std::string& text) {
    std::string t = text;
    std::transform(t.begin(), t.end(), t.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
    if (t.rfind("/bot", 0) != 0) {
        return "";
    }
    char who[16] = {}, what[32] = {};
    if (std::sscanf(t.c_str(), "/bot %15s %31s", who, what) != 2) {
        std::string names = "kong";
        for (const Extra& x : extras) {
            names += ", ";
            names += x.tag;
        }
        return "/bot <number|all> <character>: a character's number (0 to 22) or name (" + names + "), or off";
    }
    int c = -2;
    if (std::string(what) == "off") c = -1;
    else if (std::string(what) == "kong") c = kong_character;
    else if (std::isdigit((unsigned char)what[0])) c = std::atoi(what);
    else {
        for (const Extra& x : extras) {
            if (std::string(what) == x.tag) c = x.character;
        }
        for (int i = 0; i < 22; i++) {
            if (original_names[i][0] && std::string(what) == original_names[i]) c = i;
        }
    }
    int last = kong_character;
    for (const Extra& x : extras) last = std::max(last, x.character);
    if (c == -2 || c > last) {
        return std::string("/bot: no character \"") + what + "\"";
    }
    const bool all = std::string(who) == "all";
    const int n = all ? 0 : std::atoi(who);
    if (!all && (n < 1 || n > 16)) {
        return "/bot: no bot " + std::string(who);
    }
    for (int k = 0; k < 16; k++) {
        if (all || k == n - 1) {
            bot_override[k] = c;
        }
    }
    return std::string(all ? "Every bot" : ("Bot " + std::to_string(n)).c_str()) + (c < 0 ? ": the game's own pick" : " will be " + std::string(what)) + " from the next match";
}

extern "C" void conker_kong_match_entry(uint8_t* rdram, recomp_context* ctx) {
    const int k = (int)((uint32_t)ctx->r2 - 0x800E0B90u);
    // (a bot's character from /bot: its pick, the pick's entry and the place it's kept, before the rest below go by them)
    if (k >= 0 && k < 16 && bot_override[k] >= 0 && k > (int)(int32_t)MEM_W(0, (gpr)(int32_t)0x80082FA0u)) {
        const int c = bot_override[k];
        ctx->r25 = (gpr)(int32_t)(c * 8);
        ctx->r14 = (gpr)(int32_t)(0x800AB57Cu + (uint32_t)c * 8u);
        MEM_B(0, ctx->r2) = (int8_t)c;
        char line[80];
        std::snprintf(line, sizeof(line), "[bots] bot %d made character %d (/bot)", k + 1, c);
        survey(line);
    }
    // (0.11.4, for the cave's own place on the map select) the modes' table (D_800AB68C, 10 bytes a mode: +2 the scene the match is
    // played in, +4 its rules) and the mode picked (gGameState's +0x42), logged at each match's start; the table once
    if (k == 0) {
        static bool table_told = false;
        const uint32_t gs = (uint32_t)MEM_W(0, (gpr)(int32_t)0x8008FDD4u);
        const int mode = (gs >> 24) == 0x80 ? (int)MEM_BU(0x42, (gpr)(int32_t)gs) : -1;
        char line[160];
        if (!table_told) {
            table_told = true;
            survey("the modes' table (D_800AB68C, 10 bytes each):");
            for (int m = 0; m < 48; m++) {
                std::string row;
                for (uint32_t b = 0; b < 10; b++) {
                    char piece[4];
                    std::snprintf(piece, sizeof(piece), "%02X", (unsigned)MEM_BU((uint32_t)(m * 10) + b, (gpr)(int32_t)0x800AB68Cu));
                    row += piece;
                }
                std::snprintf(line, sizeof(line), "  mode %2d: %s (scene %02X)", m, row.c_str(), (unsigned)MEM_BU((uint32_t)(m * 10 + 3), (gpr)(int32_t)0x800AB68Cu));
                survey(line);
            }
        }
        std::snprintf(line, sizeof(line), "[match] mode %d picked: scene %02X", mode, mode >= 0 ? (unsigned)MEM_BU((uint32_t)(mode * 10 + 3), (gpr)(int32_t)0x800AB68Cu) : 0u);
        survey(line);
    }
    const bool his = kong_loaded && (int32_t)ctx->r25 == kong_character * 8;
    for (bool& b : select_kong) {
        b = false;   // (a match: the select screen's characters are gone)
    }
    for (int i = 0; i < kExtras; i++) {
        Extra& x = extras[i];
        for (bool& b : select_extra[i]) {
            b = false;
        }
        const bool theirs = x.loaded && (int32_t)ctx->r25 == x.character * 8;
        if (k >= 0 && k < 16) {
            extra_picked[i][k] = theirs;
        }
        if (theirs) {
            extra_entry_write(rdram, x);
            ctx->r14 = (gpr)(int32_t)x.entry;
            char line[80];
            std::snprintf(line, sizeof(line), "[%s] player %d picked it: made as 3B, its model put on", x.tag, k + 1);
            survey(line);
        }
    }
    if (k >= 0 && k < 16) {
        kong_picked[k] = his;
    }
    if (his) {
        kong_entry_write(rdram);
        ctx->r14 = (gpr)(int32_t)kong_entry;
        char line[80];
        std::snprintf(line, sizeof(line), "[kong] player %d picked him: made as the teddy, his model put on", k + 1);
        survey(line);
    }
}

// func_151E0B70 at 0x151E0D80: $t5 the mode's characters
extern "C" void conker_kong_select_mask(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    kong_load();
    if (kong_loaded && (ctx->r13 & 0x8)) {
        ctx->r13 = (gpr)(int32_t)((uint32_t)ctx->r13 | (1u << kong_character));
    }
    // (the added characters wherever 3B, character 4, is)
    for (Extra& x : extras) {
        extra_load(x);
        if (x.loaded && (ctx->r13 & x.offered_with)) {
            ctx->r13 = (gpr)(int32_t)((uint32_t)ctx->r13 | (1u << (x.character & 31)));   // (the game tests (mask >> character) & 1 by a 32-bit shift: 32 on are 0 on, alike)
        }
    }
}

// func_151E0B70 at 0x151E1060 and 0x151E10D4: $at whether the character ($a1) is under the count
extern "C" void conker_kong_select_count(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (kong_loaded || any_extra_loaded()) {
        ctx->r1 = (int32_t)ctx->r5 < last_character() + 1 ? 1 : 0;
    }
}

// func_151E0B70 at 0x151E1094 and 0x151E10C0: $t1 the last character (the wrap from 0)
extern "C" void conker_kong_select_last(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (kong_loaded || any_extra_loaded()) {
        ctx->r9 = last_character();
    }
}

// func_151E7F60's start: a select screen putting characters up again, the picks of the last match over
extern "C" void conker_kong_select_start() {
    for (bool& b : kong_picked) {
        b = false;
    }
    for (auto& row : extra_picked) {
        for (bool& b : row) {
            b = false;
        }
    }
}

// func_15082A44's start: $a0 what the character is made from (+4 its model), $a1 its number
extern "C" void conker_kong_make(uint8_t* rdram, recomp_context* ctx) {
    // (Kong's head shot off, the first step) the head the game throws (model 6E) as it's made: what it's made from, by whom (the
    // return address), and each character's model and whether its +0x1CA..., to tie the head to the character it came off
    {
        const uint32_t from = (uint32_t)ctx->r4;
        if ((from >> 24) == 0x80 && MEM_BU(4, (gpr)(int32_t)from) == 0x6E) {
            static uint32_t told = 0;
            if (told < 12) {
                told++;
                std::string words;
                for (uint32_t w = 0; w < 0x20; w += 4) {
                    char piece[12];
                    std::snprintf(piece, sizeof(piece), " %08X", (unsigned)(uint32_t)MEM_W(w, (gpr)(int32_t)from));
                    words += piece;
                }
                char line[300];
                std::snprintf(line, sizeof(line), "[head] f=%u a head made (model 6E) by %08X: $a1 %d $a2 %08X $a3 %08X, its 5th %08X; from:%s",
                    (unsigned)conker::netsession::frame(), (unsigned)(uint32_t)ctx->r31, (int)(int32_t)ctx->r5, (unsigned)(uint32_t)ctx->r6,
                    (unsigned)(uint32_t)ctx->r7, (unsigned)(uint32_t)MEM_W(0x10, ctx->r29), words.c_str());
                survey(line);
                for (int k = 0; k < 6; k++) {
                    const gpr o = (gpr)(int32_t)(0x800CC2D0u + (uint32_t)k * 0x32Cu);
                    float pos[3];
                    for (int i = 0; i < 3; i++) {
                        const uint32_t b = (uint32_t)MEM_W(0x14 + 4 * i, o);
                        std::memcpy(&pos[i], &b, 4);
                    }
                    std::snprintf(line, sizeof(line), "[head]   character %d: model %02X, +5 %02X +6 %02X, at %.0f %.0f %.0f, Kong %d", k, (unsigned)MEM_BU(4, o),
                        (unsigned)MEM_BU(5, o), (unsigned)MEM_BU(6, o), pos[0], pos[1], pos[2], (int)kong_picked[k]);
                    survey(line);
                }
            }
        }
    }
    // (whether the character being made is the select screen's Kong: func_151E7F60 put him up just now; its number comes at the end)
    // (a character being made makes others inside it, its gun and the rest, each through here: one flag was cleared by those before
    // the select screen's character came out, so each make's own is kept, a stack, and taken back at its end)
    if (select_putting_up) {
        static uint32_t told = 0;
        if (told < 30) {
            told++;
            char line[120];
            std::snprintf(line, sizeof(line), "[kong] select: a character made (depth %d), Kong %d", make_stack_depth, (int)select_putting_up_kong);
            survey(line);
        }
    }
    if (make_stack_depth < (int)(sizeof(make_stack) / sizeof(make_stack[0]))) {
        make_stack[make_stack_depth] = select_putting_up && select_putting_up_kong;
        make_extra_stack[make_stack_depth] = (int8_t)(select_putting_up ? select_putting_up_extra + 1 : 0);
    }
    make_stack_depth++;
    select_putting_up = false;
}

// func_15082A44 at 0x15083298, its end: $v0 the character made (0: none)
extern "C" void conker_kong_made(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (make_stack_depth > 0) {
        make_stack_depth--;
    }
    make_kong = make_stack_depth < (int)(sizeof(make_stack) / sizeof(make_stack[0])) ? make_stack[make_stack_depth] : false;
    const int make_extra = make_stack_depth < (int)(sizeof(make_stack) / sizeof(make_stack[0])) ? make_extra_stack[make_stack_depth] - 1 : -1;
    // ($v0 is the character's number plus one: func_151E7F60 takes the character at 0x800CC2D0 + ($v0 - 1) x 0x32C. Kong was kept as
    // the next character's, so the select screen's never was him)
    const int k = (int)(int32_t)ctx->r2 - 1;
    if (make_kong) {
        static uint32_t told_made = 0;
        if (told_made < 30) {
            told_made++;
            char line[120];
            std::snprintf(line, sizeof(line), "[kong] select: made as him, $v0 %d (depth now %d)", (int)(int32_t)ctx->r2, make_stack_depth);
            survey(line);
        }
    }
    if (k >= 0 && k < kMaxObjects) {
        select_kong[k] = make_kong;
        for (int i = 0; i < kExtras; i++) {
            select_extra[i][k] = make_extra == i;
        }
        static uint32_t told = 0;
        if (make_kong && told < 12) {
            told++;
            char line[80];
            std::snprintf(line, sizeof(line), "[kong] the select screen's character %d made for him", k);
            survey(line);
        }
    }
    make_kong = false;
}

namespace {
    // Drawn as Kong: in a match (an arena, or the co-op stage) the characters of the players who picked him (character k is player k's),
    // elsewhere (the select screens) the ones made as him
    bool drawn_as_kong(uint8_t* rdram, int k) {
        if (k < 0 || k >= kMaxObjects) {
            return false;
        }
#if defined(CONKER_RT64)
        const bool coop = conker::netmenu::coop_running(rdram);
#else
        const bool coop = false;
#endif
        const bool match = MEM_BU(0, (gpr)(int32_t)0x800BE616) != 0 || coop;
        // (the select screens are with D_800BE616 set too: the multiplayer's flag, not the match's. So both: the players who picked him
        // (set as a match starts, cleared as the select screen puts a character up) and the select screen's characters made as him
        // (cleared as a match starts))
        (void)match;
        return kong_picked[k] || select_kong[k];
    }
}

// (the fourth look) func_1502DF38, a character's update, works its bones out from its model's skeleton (+4's): her model there too, as
// in the drawing (conker_kong_draw_begin), so her bones are her own skeleton's (her own proportions), not 3B's. $a0 the character; at
// its end (0x1502E460) the character at 0xC8 of the stack.
extern "C" void conker_berri_update_begin(uint8_t* rdram, recomp_context* ctx) {
    const int k = (int)(int32_t)ctx->r4;
    if (gold_teddy_now(rdram, k)) {
        const gpr ob = (gpr)(int32_t)(0x800CC2D0u + (uint32_t)k * 0x32Cu);
        teddy_own_model[k] = MEM_BU(4, ob);
        teddy_group_of = teddy_own_model[k];
        MEM_B(4, ob) = (int8_t)small_teddy_model;
        teddy_updating[k] = true;
        return;
    }
    const int i = extra_of(k);
    if (i < 0) {
        return;
    }
    Extra& x = extras[i];
    const gpr ob = (gpr)(int32_t)(0x800CC2D0u + (uint32_t)k * 0x32Cu);
    if (MEM_BU(4, ob) == x.base_model) {
        MEM_B(4, ob) = (int8_t)x.model;
        extra_updating[i][k] = true;
        x.draw_now++;
    }
}

extern "C" void conker_berri_update_end(uint8_t* rdram, recomp_context* ctx) {
    const int k = (int)(int32_t)MEM_W(0xC8, ctx->r29);
    if (k < 0 || k >= kMaxObjects) {
        return;
    }
    if (k < 64 && teddy_updating[k]) {
        MEM_B(4, (gpr)(int32_t)(0x800CC2D0u + (uint32_t)k * 0x32Cu)) = (int8_t)teddy_own_model[k];
        teddy_updating[k] = false;
        teddy_group_of = -1;
        return;
    }
    for (int i = 0; i < kExtras; i++) {
        if (!extra_updating[i][k]) {
            continue;
        }
        Extra& x = extras[i];
        MEM_B(4, (gpr)(int32_t)(0x800CC2D0u + (uint32_t)k * 0x32Cu)) = (int8_t)x.base_model;
        extra_updating[i][k] = false;
        x.draw_now = x.draw_now > 0 ? x.draw_now - 1 : 0;
    }
}

// func_1502C974's start ($a1 the character, drawn in a view) and its end (0x1502CC24, the character at 0x5C of the stack): drawn as
// Kong. (The first look did it around func_1502DF38, the character's update, which works its bones out: but the textures are put on in
// the drawing, and a Kong drawn there came out the teddy's colours and torn. His model is built on the teddy's skeleton now, so his
// bones are the teddy's whichever model the game works them out from.)
extern "C" void conker_kong_draw_begin(uint8_t* rdram, recomp_context* ctx) {
    const int k = (int)(int32_t)ctx->r5;
    if (k < 0 || k >= kMaxObjects) {
        return;
    }
    // (a gold weapon's holder on their own screen: the small teddy; offline only in their own view, $a2 the view)
    if (gold_teddy_now(rdram, k) && (conker::netsession::active() || (int32_t)ctx->r6 == k)) {
        const gpr ob = (gpr)(int32_t)(0x800CC2D0u + (uint32_t)k * 0x32Cu);
        teddy_own_model[k] = MEM_BU(4, ob);
        teddy_group_of = teddy_own_model[k];
        MEM_B(4, ob) = (int8_t)small_teddy_model;
        teddy_drawing[k] = true;
        kong_drawn[k] = false;
        return;
    }
    kong_drawn[k] = kong_loaded && drawn_as_kong(rdram, k);
    {
        // (which characters this draws outside a match, the select screen's among them, a few times)
        static uint32_t told_select = 0;
        if (MEM_BU(0, (gpr)(int32_t)0x800BE616) == 0 && told_select < 16) {
            told_select++;
            char line[120];
            std::snprintf(line, sizeof(line), "[kong] outside a match: character %d drawn (model %02X), Kong's %d", k,
                (unsigned)MEM_BU(4, (gpr)(int32_t)(0x800CC2D0u + (uint32_t)k * 0x32Cu)), (int)kong_drawn[k]);
            survey(line);
        }
    }
    if (!kong_drawn[k]) {
        // (an added character: 3B for the game, drawn with its own model)
        const int i = extra_of(k);
        if (i >= 0) {
            Extra& x = extras[i];
            const gpr ob = (gpr)(int32_t)(0x800CC2D0u + (uint32_t)k * 0x32Cu);
            if (MEM_BU(4, ob) == x.base_model) {
                MEM_B(4, ob) = (int8_t)x.model;
                extra_drawing[i][k] = true;
                x.draw_now++;
                static uint32_t told_b = 0;
                if (told_b < 12) {
                    told_b++;
                    char line[80];
                    std::snprintf(line, sizeof(line), "[%s] character %d drawn as it", x.tag, k);
                    survey(line);
                }
            }
        }
        return;
    }
    static uint32_t told = 0;
    if (told < 6) {
        told++;
        char line[80];
        std::snprintf(line, sizeof(line), "[kong] character %d drawn as him", k);
        survey(line);
    }
    const gpr o = (gpr)(int32_t)(0x800CC2D0u + (uint32_t)k * 0x32Cu);
    if (MEM_BU(4, o) == teddy_model) {
        MEM_B(4, o) = (int8_t)kong_model;
        kong_drawing[k] = true;
    }
}

// (0.11.4, a tool for rigging the characters made from meshes) Pose capture: with a file named pose_capture.txt beside the game, every
// 2nd drawing of character 0 (player 1's) in a match writes its 28 bones' matrices as the game made them (segment 3: bone n at n x 0x40, the
// Nintendo 64's fixed-point layout, the integer halves then the fractions) to pose_capture.bin, with its model (+4) and its animation
// (+0x1E8): 1200 of them (40 seconds), 4 + 448 words each, big-endian as in RDRAM. Conker's own animations as the game poses his
// skeleton, so tools/objchar's tests can put each rig in the poses it will really be in. Nothing without the file; never sent online.
namespace {
    void pose_capture(uint8_t* rdram, recomp_context* ctx, int k) {
        static int state = -1;
        static std::FILE* out = nullptr;
        static uint32_t draws = 0, written = 0;
        if (state < 0) {
            std::ifstream flag("pose_capture.txt");
            state = flag.good() ? 1 : 0;
            if (state == 1) {
                out = std::fopen("pose_capture.bin", "wb");
                if (out == nullptr) {
                    state = 0;
                }
            }
        }
        if (state != 1 || k != 0 || written >= 1200 || MEM_BU(0, (gpr)(int32_t)0x800BE616) == 0 || ((++draws) & 1u) != 0) {
            return;   // (in a match only)
        }
        const uint32_t from = (uint32_t)MEM_W(0x58, ctx->r29), to = (uint32_t)ctx->r2;
        uint32_t seg3 = 0;
        if ((from >> 24) == 0x80 && (to >> 24) == 0x80 && to > from && to - from < 0x10000u) {
            for (uint32_t a = from; a < to; a += 8) {
                if ((uint32_t)MEM_W(0, (gpr)(int32_t)a) == 0xDB06000Cu) {
                    seg3 = (uint32_t)MEM_W(4, (gpr)(int32_t)a);
                }
            }
            for (uint32_t a = from; seg3 == 0 && a > from - 0x8000u && a - 8 >= 0x80000000u;) {
                a -= 8;
                if ((uint32_t)MEM_W(0, (gpr)(int32_t)a) == 0xDB06000Cu) {
                    seg3 = (uint32_t)MEM_W(4, (gpr)(int32_t)a);
                }
            }
        }
        if (seg3 == 0 || ((seg3 & 0xFF000000u) != 0x80000000u && (seg3 & 0xFF000000u) != 0)) {
            return;
        }
        const uint32_t m = 0x80000000u | (seg3 & 0x00FFFFFFu);
        const gpr o = (gpr)(int32_t)0x800CC2D0u;
        uint8_t rec[4 * (4 + 28 * 16)];
        const auto put = [&](int at, uint32_t v) {
            rec[at * 4] = (uint8_t)(v >> 24); rec[at * 4 + 1] = (uint8_t)(v >> 16); rec[at * 4 + 2] = (uint8_t)(v >> 8); rec[at * 4 + 3] = (uint8_t)v;
        };
        put(0, 0x504F5345u);   // "POSE"
        put(1, written);
        put(2, (uint32_t)MEM_BU(4, o));
        put(3, (uint32_t)MEM_W(0x1E8, o));
        for (int b = 0; b < 28; b++) {
            for (int w = 0; w < 16; w++) {
                put(4 + b * 16 + w, (uint32_t)MEM_W(b * 0x40 + w * 4, (gpr)(int32_t)m));
            }
        }
        std::fwrite(rec, sizeof(rec), 1, out);
        written++;
        if (written % 60 == 0 || written == 1200) {
            std::fflush(out);
        }
        if (written == 1200) {
            std::fclose(out);
            out = nullptr;
            survey("[pose capture] 1200 poses written to pose_capture.bin");
        }
    }
}

extern "C" void conker_kong_draw_end(uint8_t* rdram, recomp_context* ctx) {
    const int k = (int)(int32_t)MEM_W(0x5C, ctx->r29);
    pose_capture(rdram, ctx, k);
    if (k >= 0 && k < 64 && teddy_drawing[k]) {
        MEM_B(4, (gpr)(int32_t)(0x800CC2D0u + (uint32_t)k * 0x32Cu)) = (int8_t)teddy_own_model[k];
        teddy_drawing[k] = false;
        teddy_group_of = -1;
        return;
    }
    // (0.11.4) where an added character's feet are drawn against where it stands, a few of its drawings: its bones' matrices (segment 3,
    // bone n at n x 0x40: their heights, the 4th row's y, s15.16 split in halves) and the character's own height (+0x18), to set how far
    // its rig is lowered (its feet stood off the floor)
    for (int i = 0; i < kExtras && k >= 0 && k < kMaxObjects; i++) {
        static uint32_t feet_told[8] = {};
        static uint32_t feet_draws[8] = {};
        if (!extra_drawing[i][k] || i >= 8 || feet_told[i] >= 10 || (++feet_draws[i] % 90) != 30) {
            continue;
        }
        const uint32_t from = (uint32_t)MEM_W(0x58, ctx->r29), to = (uint32_t)ctx->r2;
        uint32_t seg3 = 0;
        if ((from >> 24) == 0x80 && (to >> 24) == 0x80 && to > from && to - from < 0x10000u) {
            for (uint32_t a = from; a < to; a += 8) {
                if ((uint32_t)MEM_W(0, (gpr)(int32_t)a) == 0xDB06000Cu) {
                    seg3 = (uint32_t)MEM_W(4, (gpr)(int32_t)a);
                }
            }
            for (uint32_t a = from; seg3 == 0 && a > from - 0x8000u && a - 8 >= 0x80000000u;) {
                a -= 8;
                if ((uint32_t)MEM_W(0, (gpr)(int32_t)a) == 0xDB06000Cu) {
                    seg3 = (uint32_t)MEM_W(4, (gpr)(int32_t)a);
                }
            }
        }
        if (seg3 == 0) {
            continue;
        }
        feet_told[i]++;
        const uint32_t m = 0x80000000u | (seg3 & 0x00FFFFFFu);
        const auto height_of = [&](int b) {
            const int16_t whole = (int16_t)((uint32_t)MEM_W(b * 0x40 + 6 * 4, (gpr)(int32_t)m) & 0xFFFFu);
            const uint16_t part = (uint16_t)((uint32_t)MEM_W(b * 0x40 + 14 * 4, (gpr)(int32_t)m) & 0xFFFFu);
            return (float)whole + (float)part / 65536.0f;
        };
        const gpr ob = (gpr)(int32_t)(0x800CC2D0u + (uint32_t)k * 0x32Cu);
        const uint32_t ybits = (uint32_t)MEM_W(0x18, ob);
        float y;
        std::memcpy(&y, &ybits, 4);
        char line[200];
        std::snprintf(line, sizeof(line), "[%s] feet: character %d stands at %.1f; hips %.1f, ankles %.1f %.1f, toes %.1f %.1f (animation %08X)",
            extras[i].tag, k, y, height_of(0), height_of(18), height_of(22), height_of(19), height_of(23), (unsigned)(uint32_t)MEM_W(0x1E8, ob));
        survey(line);
    }
    for (int i = 0; i < kExtras && k >= 0 && k < kMaxObjects; i++) {
        if (extra_drawing[i][k]) {
            Extra& x = extras[i];
            MEM_B(4, (gpr)(int32_t)(0x800CC2D0u + (uint32_t)k * 0x32Cu)) = (int8_t)x.base_model;
            extra_drawing[i][k] = false;
            x.draw_now = x.draw_now > 0 ? x.draw_now - 1 : 0;
        }
    }
    if (k < 0 || k >= kMaxObjects || !kong_drawing[k]) {
        return;
    }
    // (0.11.4, measuring Kong's look) his pose as the game draws it, some of his draws: the commands this drawing wrote (from the list
    // it was given, 0x58 of the stack, to the one it returns, $v0), the segments they set, and his 28 bones' matrices (segment 3, which
    // his model's G_MTX commands load: bone n at n x 0x40), to put his model in the teddy's poses here and see what the game does to him
    {
        static uint32_t draws = 0;
        static const uint32_t when[] = { 1, 20, 60, 120, 200, 300, 450, 600, 800, 1000, 1300, 1600 };
        draws++;
        bool now = false;
        for (uint32_t w : when) {
            now |= draws == w;
        }
        const uint32_t from = (uint32_t)MEM_W(0x58, ctx->r29), to = (uint32_t)ctx->r2;
        if (now && (from >> 24) == 0x80 && (to >> 24) == 0x80 && to > from && to - from < 0x10000u) {
            uint32_t seg3 = 0;
            int commands = 0;
            std::string segs;
            for (uint32_t a = from; a < to; a += 8) {
                const uint32_t w0 = (uint32_t)MEM_W(0, (gpr)(int32_t)a), w1 = (uint32_t)MEM_W(4, (gpr)(int32_t)a);
                commands++;
                if ((w0 & 0xFFFF0000u) == 0xDB060000u) {
                    char piece[32];
                    std::snprintf(piece, sizeof(piece), " %u=%08X", (unsigned)((w0 & 0xFFFFu) / 4u), w1);
                    segs += piece;
                    if ((w0 & 0xFFFFu) == 0x0Cu) {
                        seg3 = w1;
                    }
                }
            }
            char line[300];
            std::snprintf(line, sizeof(line), "[kong pose] draw %u, character %d: commands %08X..%08X (%d), segments set:%s", (unsigned)draws, k, from, to, commands,
                segs.empty() ? " none" : segs.c_str());
            survey(line);
            // (not set in it: the last setting before it, in the frame's list as far back as 32 KB)
            for (uint32_t a = from; seg3 == 0 && a > from - 0x8000u && a - 8 >= 0x80000000u;) {
                a -= 8;
                if ((uint32_t)MEM_W(0, (gpr)(int32_t)a) == 0xDB06000Cu) {
                    seg3 = (uint32_t)MEM_W(4, (gpr)(int32_t)a);
                    std::snprintf(line, sizeof(line), "[kong pose]   segment 3 set before it, at %08X: %08X", a, seg3);
                    survey(line);
                }
            }
            if (seg3 != 0 && ((seg3 & 0xFF000000u) == 0x80000000u || (seg3 & 0xFF000000u) == 0x00000000u)) {
                const uint32_t m = 0x80000000u | (seg3 & 0x00FFFFFFu);
                for (int b = 0; b < 28; b++) {
                    std::string text;
                    for (int w = 0; w < 16; w++) {
                        char piece[12];
                        std::snprintf(piece, sizeof(piece), " %08X", (uint32_t)MEM_W(b * 0x40 + w * 4, (gpr)(int32_t)m));
                        text += piece;
                    }
                    std::snprintf(line, sizeof(line), "[kong pose]   bone %2d:%s", b, text.c_str());
                    survey(line);
                }
            }
        }
    }
    const gpr o = (gpr)(int32_t)(0x800CC2D0u + (uint32_t)k * 0x32Cu);
    MEM_B(4, o) = (int8_t)teddy_model;
    kong_drawing[k] = false;
}

bool conker::mp_characters::kong_drawn_as(int character) {
    return character >= 0 && character < kMaxObjects && kong_drawn[character];
}

// func_150837D4's start: $a0 the character, $a1 the model put on it
extern "C" void conker_kong_model_on(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    (void)ctx;
}

// func_1502B350 at 0x1502B3DC: the entry has been read into its buffer (0x28 of the stack, 0x20 its size; 0x34 the entry's size and flags)
extern "C" void conker_kong_entry_read(uint8_t* rdram, recomp_context* ctx) {
    // each level's entry as read: its packed size (the room) and its size unpacked (the first word), to see which is the race's
    if (path_a == 4) {
        static uint32_t told = 0;
        const gpr sp0 = ctx->r29;
        const uint32_t buffer0 = (uint32_t)MEM_W(0x28, sp0);
        if (told < 40 && (buffer0 >> 24) == 0x80) {
            told++;
            char line[140];
            std::snprintf(line, sizeof(line), "[levels] bank 4's %02X read: %u bytes packed, %u unpacked, flags %08X", (unsigned)path_b,
                (unsigned)(uint32_t)MEM_W(0x20, sp0), (unsigned)(uint32_t)MEM_W(0, (gpr)(int32_t)buffer0), (unsigned)(uint32_t)MEM_W(0x34, sp0));
            survey(line);
        }
    }
    // (the race's course's other files, read after it in the same load: every word, for its start places and checkpoints)
    if (path_a == 4 && (path_b == 0x32 || path_b == 0x33) && swapping == Swap::None) {
        static uint32_t dumped = 0;
        const gpr sp1 = ctx->r29;
        const uint32_t buffer1 = (uint32_t)MEM_W(0x28, sp1), size1 = (uint32_t)MEM_W(0x20, sp1);
        if (dumped < 4 && (buffer1 >> 24) == 0x80 && size1 <= 0x400u) {
            dumped++;
            char head[96];
            std::snprintf(head, sizeof(head), "[race] the course's file of %u bytes (flags %08X), as read:", (unsigned)size1, (unsigned)(uint32_t)MEM_W(0x34, sp1));
            survey(head);
            for (uint32_t at = 0; at < size1; at += 32) {
                std::string row = "  ";
                for (uint32_t w = at; w < at + 32 && w + 4 <= size1; w += 4) {
                    char piece[12];
                    std::snprintf(piece, sizeof(piece), "%08X ", (unsigned)(uint32_t)MEM_W(w, (gpr)(int32_t)buffer1));
                    row += piece;
                }
                survey(row.c_str());
            }
        }
    }
    // (an added character bigger than the entry it's drawn from: the entry's unpacked size, its first word, raised to ours before the
    // game makes room for it from that word (as Kong's is); its own data still unpacks into the front, then ours goes over it)
    if ((swapping == Swap::Extra && swapping_extra >= 0 && swapping_extra < kExtras) || (swapping == Swap::Weapon && swapping_weapon >= 0)) {
        const gpr spx = ctx->r29;
        const uint32_t bufx = (uint32_t)MEM_W(0x28, spx);
        const uint32_t ours_size = swapping == Swap::Weapon ? (uint32_t)sword_bins[swapping_weapon].size() : (uint32_t)extras[swapping_extra].bin.size();
        if ((bufx >> 24) == 0x80 && ours_size > (uint32_t)MEM_W(0, (gpr)(int32_t)bufx)) {
            char line[120];
            std::snprintf(line, sizeof(line), "[%s] its entry's room raised from %u to %u bytes", swapping == Swap::Weapon ? "sword" : extras[swapping_extra].tag,
                (unsigned)(uint32_t)MEM_W(0, (gpr)(int32_t)bufx), (unsigned)ours_size);
            kong_say(line);
            MEM_W(0, (gpr)(int32_t)bufx) = (int32_t)((ours_size + 15u) & ~15u);
        }
    }
    if (swapping == Swap::None || swapping == Swap::Extra || swapping == Swap::Weapon) {
        return;
    }
    const std::vector<uint8_t>& ours = swapping == Swap::Kong ? kong_rzip : swapping == Swap::Cave ? cave_rzip : lagoon_rzip;
    const gpr sp = ctx->r29;
    const uint32_t buffer = (uint32_t)MEM_W(0x28, sp);
    const uint32_t room = (uint32_t)MEM_W(0x20, sp);
    const uint32_t flags = (uint32_t)MEM_W(0x34, sp);
    // (ours unpacked no bigger than the entry's own: the game makes room for that much)
    const uint32_t ours_unpacked = ours.size() >= 4 ? ((uint32_t)ours[0] << 24) | ((uint32_t)ours[1] << 16) | ((uint32_t)ours[2] << 8) | ours[3] : 0u;
    const uint32_t theirs_unpacked = (buffer >> 24) == 0x80 ? (uint32_t)MEM_W(0, (gpr)(int32_t)buffer) : 0u;
    if ((buffer >> 24) != 0x80 || (flags & 0x70000000u) != 0x10000000u || ours.size() > room || ((swapping == Swap::Lagoon || swapping == Swap::Cave) && ours_unpacked > theirs_unpacked)) {
        char line[160];
        std::snprintf(line, sizeof(line), "[%s] not put in: buffer %08X, room %u, ours %u (%u unpacked, theirs %u), flags %08X",
            swapping == Swap::Kong ? "kong" : "lagoon", (unsigned)buffer, (unsigned)room, (unsigned)ours.size(), (unsigned)ours_unpacked,
            (unsigned)theirs_unpacked, (unsigned)flags);
        kong_say(line);
        return;
    }
    if (swapping == Swap::Kong) {
        teddy_survey(rdram, buffer, room);
    }
    // (Pirate Lagoon) built for this course: as many pieces as its own entry has, Lagoon's track the first, the others empty. With
    // Lagoon's one piece alone the race crashed as it set up (func_150039E0, 0x15004094: the course's other pieces, which its own data
    // goes by, weren't there: a null read)
    std::vector<uint8_t> built;
    if (swapping == Swap::Lagoon || swapping == Swap::Cave) {
        built = lagoon_entry_for(rdram, buffer, room, theirs_unpacked, swapping == Swap::Cave ? cave_rzip : lagoon_rzip, swapping == Swap::Cave);
        if (built.empty()) {
            return;
        }
    }
    const std::vector<uint8_t>& put = (swapping == Swap::Lagoon || swapping == Swap::Cave) ? built : ours;
    for (size_t i = 0; i < put.size(); i++) {
        MEM_B((uint32_t)i, (gpr)(int32_t)buffer) = (int8_t)put[i];
    }
    const bool was_kong = swapping == Swap::Kong;
    swapping = Swap::None;   // (the level's other files that follow in the same load are its own)
    char line[180];
    std::snprintf(line, sizeof(line), "[%s] entry %u %02X replaced (%u of %u bytes; %u unpacked, theirs %u)", was_kong ? "kong" : "lagoon",
        (unsigned)path_a, (unsigned)path_b, (unsigned)put.size(), (unsigned)room, (unsigned)ours_unpacked, (unsigned)theirs_unpacked);
    kong_say(line);
}

// (0.11.4) func_1510CE60 before 0x1510CF74 ($s0 the texture id of a G_SETTIMG it loaded, $t9 whether it lists the model's textures):
// the loader marks each id in a bitmap on its own stack, 0x3CC bytes, the game's ids (below 0x1E60), to list the textures for the model
// to let go of later. Ours (0x1F00 on) were marked past its end, in its callers' frames: the held sword's ids (0x23C0 on) set bits
// in func_1502FFD8's saved $s0, the weapon's instance, which came back as 0xFF1FC801, and the game crashed in func_15031A50. Ours
// are handed out from our own memory and never let go of (conker_kong_texture_release): they're left out of the list.
extern "C" void conker_texture_mark_guard(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if ((uint32_t)ctx->r16 >= 0x1E60u) {
        ctx->r25 = 0;
    }
}

// func_1510D0EC's start ($a0 the texture's id): his, handed out from ours (true: the function returns at once, $v0 the texture)
extern "C" int conker_kong_texture(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t id = (uint32_t)ctx->r4;
    if (id < 0x1F00u || id > 0x252Au) {
        return 0;
    }
    {
        // (the added characters made from meshes: their own textures, 0x2140 to 0x21FF, from their folders, handed out from 0x80A80000)
        static bool scanned = false;
        static std::vector<LagoonTexture> own;
        if (!scanned) {
            scanned = true;
            uint32_t at = 0x80A80000u;
            std::vector<std::string> folders;
            for (const Extra& x : extras) {
                folders.push_back(std::string(x.asset).substr(0, std::string(x.asset).find('/')));
            }
            folders.push_back("sword");
            for (const std::string& folder : folders) {
                for (uint32_t tid = 0x2140; tid <= 0x23FF; tid++) {
                    if (tid == 0x2200) {
                        tid = 0x2300;   // (0x2200 to 0x22FF the custom maps')
                    }
                    char name[64];
                    std::snprintf(name, sizeof(name), "%s/tex_%04X.bin", folder.c_str(), (unsigned)tid);
                    LagoonTexture t{ tid, read_asset(name), 0 };
                    if (t.data.empty() || at + t.data.size() > 0x80B00000u) {
                        continue;
                    }
                    t.at = at;
                    at = (at + (uint32_t)t.data.size() + 15u) & ~15u;
                    own.push_back(std::move(t));
                }
            }
            char line[96];
            std::snprintf(line, sizeof(line), "[extra] %u textures of the added characters' own", (unsigned)own.size());
            kong_say(line);
        }
        for (const LagoonTexture& t : own) {
            if (t.id == id) {
                for (size_t i = 0; i < t.data.size(); i++) {
                    MEM_B((uint32_t)i, (gpr)(int32_t)t.at) = (int8_t)t.data[i];
                }
                ctx->r2 = (gpr)(int32_t)t.at;
                const uint32_t size_out = (uint32_t)ctx->r5;
                if ((size_out >> 24) == 0x80 && (size_out & 3u) == 0) {
                    MEM_W(0, (gpr)(int32_t)size_out) = (int32_t)t.data.size();
                }
                return 1;
            }
        }
    }
    cave_load();
    for (const LagoonTexture& t : cave_textures) {
        if (t.id == id) {
            for (size_t i = 0; i < t.data.size(); i++) {
                MEM_B((uint32_t)i, (gpr)(int32_t)t.at) = (int8_t)t.data[i];
            }
            ctx->r2 = (gpr)(int32_t)t.at;
            const uint32_t size_out = (uint32_t)ctx->r5;
            if ((size_out >> 24) == 0x80 && (size_out & 3u) == 0) {
                MEM_W(0, (gpr)(int32_t)size_out) = (int32_t)t.data.size();
            }
            return 1;
        }
    }
    lagoon_load();
    for (const LagoonTexture& t : lagoon_textures) {
        if (t.id == id) {
            for (size_t i = 0; i < t.data.size(); i++) {
                MEM_B((uint32_t)i, (gpr)(int32_t)t.at) = (int8_t)t.data[i];
            }
            ctx->r2 = (gpr)(int32_t)t.at;
            const uint32_t size_out = (uint32_t)ctx->r5;
            if ((size_out >> 24) == 0x80 && (size_out & 3u) == 0) {
                MEM_W(0, (gpr)(int32_t)size_out) = (int32_t)t.data.size();
            }
            static std::set<uint32_t> lagoon_told;
            if (lagoon_told.insert(id).second) {
                char line[96];
                std::snprintf(line, sizeof(line), "[lagoon] texture %04X handed out at %08X", (unsigned)id, (unsigned)t.at);
                kong_say(line);
            }
            return 1;
        }
    }
    kong_load();
    for (const KongTexture& t : kong_textures) {
        if (t.id == id) {
            for (size_t i = 0; i < t.data.size(); i++) {
                MEM_B((uint32_t)i, (gpr)(int32_t)t.at) = (int8_t)t.data[i];
            }
            ctx->r2 = (gpr)(int32_t)t.at;
            // and its size where $a1 points: func_1510CE60 finds the palette by it (the texture's address + its size - 0x200, the last
            // 512 bytes), so without it the colours were read from wherever: the first look's Kong came out black, his ears and mouth
            // in random colours
            const uint32_t size_out = (uint32_t)ctx->r5;
            if ((size_out >> 24) == 0x80 && (size_out & 3u) == 0) {
                MEM_W(0, (gpr)(int32_t)size_out) = (int32_t)t.data.size();
            }
            static std::set<uint32_t> handed_told;
            if (handed_told.insert(id).second) {
                char line[96];
                std::snprintf(line, sizeof(line), "[kong] texture %04X handed out at %08X", (unsigned)id, (unsigned)t.at);
                kong_say(line);
            }
            return 1;
        }
    }
    ctx->r2 = (gpr)(int32_t)0x80000000u;   // (none: as the game's for a texture it doesn't have)
    return 1;
}

// func_1510D7AC's start ($a0 the texture's id): his never let go of (true: the function returns at once)
extern "C" int conker_kong_texture_release(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    const uint32_t id = (uint32_t)ctx->r4;
    return (id >= 0x1F00u && id <= 0x252Au) ? 1 : 0;
}

// ---- (0.11.4, for Kong's own slot) the multiplayer characters' table ----
// func_151E7F60 (a select screen's slot in $a0, its character in $a1) puts a character up: its model is the byte at +5 of
// D_800AB57C's 8-byte entry for it (+4 a height). Logged: the table (its first 34 entries, up to the modes' table at 0x800AB68C), the
// select screen's characters by mode (D_8008FEA4, a mask of them each) and each slot's character as it's put up.
extern "C" void conker_kong_select_start();
extern "C" void conker_kong_select_slot(int slot, int character);
extern "C" void conker_mp_character_up(uint8_t* rdram, recomp_context* ctx) {
    conker_kong_select_start();
    conker_kong_select_slot((int)(int32_t)ctx->r4, (int)(int32_t)ctx->r5);
    static bool table_told = false;
    if (!table_told) {
        table_told = true;
        survey("the multiplayer characters (D_800AB57C, 8 bytes each: the words, then +4 height, +5 model):");
        for (uint32_t c = 0; c < 34; c++) {
            const gpr e = (gpr)(int32_t)(0x800AB57Cu + 8u * c);
            char line[96];
            std::snprintf(line, sizeof(line), "  %2u: %08X %08X (model %02X)", (unsigned)c, (unsigned)MEM_W(0, e), (unsigned)MEM_W(4, e), (unsigned)MEM_BU(5, e));
            survey(line);
        }
        std::string masks = "the select screen's masks (D_8008FEA4):";
        for (uint32_t m = 0; m < 8; m++) {
            char piece[12];
            std::snprintf(piece, sizeof(piece), " %08X", (unsigned)MEM_W(4u * m, (gpr)(int32_t)0x8008FEA4));
            masks += piece;
        }
        survey(masks.c_str());
    }
    static uint32_t told = 0;
    if (told < 80) {
        told++;
        const uint32_t c = (uint32_t)ctx->r5;
        char line[96];
        std::snprintf(line, sizeof(line), "[select] slot %d put up as character %u (model %02X)", (int)(int32_t)ctx->r4, (unsigned)c,
            c < 64 ? (unsigned)MEM_BU(5, (gpr)(int32_t)(0x800AB57Cu + 8u * c)) : 0u);
        survey(line);
    }
}

// ---- (0.11.4) An added character's sounds ----
// The game plays a character's sounds (its grunts as it swings, its voice, its footsteps) through func_10010154 ($a0 the sound, $a1 the
// object) and func_10010FFC ($a1 the sound, its 6th argument the object). An added character is 3B for the game, so it makes 3B's. Each
// one's assets/<name>/sounds.txt (lines "from to", hexadecimal sound numbers, # a comment) swaps a sound for another as it plays, for
// that character only. Logged to characters.log: every sound an added character plays (its first 200), and every sound a character
// with an added character's own model plays (the story's: the ogre's own grunts, to find them), so the lists can be filled in.
namespace {
    struct SoundMap { bool tried = false; std::map<uint32_t, std::vector<uint32_t>> swap; std::map<uint32_t, uint32_t> next; };
    std::map<int, SoundMap> sound_maps;
    const SoundMap& sound_map_of(int i) {
        SoundMap& m = sound_maps[i];
        if (!m.tried) {
            m.tried = true;
            const std::string name = std::string(extras[i].tag) + "/sounds.txt";
            const std::vector<uint8_t> text = read_asset(name.c_str());
            std::string line;
            for (size_t at = 0; at <= text.size(); at++) {
                const char ch = at < text.size() ? (char)text[at] : '\n';
                if (ch != '\n') {
                    line += ch;
                    continue;
                }
                const size_t hash = line.find('#');
                if (hash != std::string::npos) {
                    line.resize(hash);
                }
                // (from, then one or more to take in turn)
                std::vector<uint32_t> words;
                size_t p = 0;
                while (p < line.size()) {
                    while (p < line.size() && (line[p] == ' ' || line[p] == '\t' || line[p] == '\r' || line[p] == ',')) {
                        p++;
                    }
                    size_t q = p;
                    while (q < line.size() && std::isxdigit((unsigned char)line[q])) {
                        q++;
                    }
                    if (q == p) {
                        break;
                    }
                    words.push_back((uint32_t)std::strtoul(line.substr(p, q - p).c_str(), nullptr, 16));
                    p = q;
                }
                if (words.size() >= 2) {
                    m.swap[words[0]].assign(words.begin() + 1, words.end());
                }
                line.clear();
            }
            char said[120];
            std::snprintf(said, sizeof(said), "[%s] %zu sounds swapped (%s)", extras[i].tag, m.swap.size(), name.c_str());
            survey(said);
        }
        return m;
    }
}

extern "C" void conker_extra_sound(uint8_t* rdram, recomp_context* ctx, int which) {
    const uint32_t object = which == 0 ? (uint32_t)ctx->r5 : (uint32_t)MEM_W(0x14, ctx->r29);
    constexpr uint32_t characters = 0x800CC2D0u, character_size = 0x32Cu;
    if (object < characters || (object - characters) % character_size != 0 || (object - characters) / character_size >= (uint32_t)kMaxObjects) {
        return;
    }
    const int k = (int)((object - characters) / character_size);
    const uint32_t sound = (which == 0 ? (uint32_t)ctx->r4 : (uint32_t)ctx->r5) & 0xFFFFu;
    const int i = extra_of(k);
    if (i < 0) {
        // (the story's: a character with an added character's own model)
        const int model = (int)MEM_BU(4, (gpr)(int32_t)object);
        for (const Extra& x : extras) {
            if (model == x.model) {
                static uint32_t told = 0;
                if (told < 300) {
                    told++;
                    char line[120];
                    std::snprintf(line, sizeof(line), "[sound] the story's %s (character %d) plays sound %04X (%s)", x.tag, k, (unsigned)sound,
                        which == 0 ? "func_10010154" : "func_10010FFC");
                    survey(line);
                }
            }
        }
        return;
    }
    SoundMap& m = sound_maps[i];
    sound_map_of(i);
    const auto it = m.swap.find(sound);
    static uint32_t told = 0;
    if (told < 200) {
        told++;
        char line[120];
        std::snprintf(line, sizeof(line), "[sound] %s (character %d) plays sound %04X (%s)%s", extras[i].tag, k, (unsigned)sound,
            which == 0 ? "func_10010154" : "func_10010FFC", it != m.swap.end() ? ", swapped" : "");
        survey(line);
    }
    if (it == m.swap.end()) {
        return;
    }
    // (each in turn: the game's calls are the same on every machine, so the turn is too)
    const uint32_t pick = it->second[m.next[sound]++ % it->second.size()];
    const uint32_t to = (which == 0 ? ((uint32_t)ctx->r4 & ~0xFFFFu) : ((uint32_t)ctx->r5 & ~0xFFFFu)) | (pick & 0xFFFFu);
    if (which == 0) {
        ctx->r4 = (gpr)(int32_t)to;
    } else {
        ctx->r5 = (gpr)(int32_t)to;
    }
}
