// Gold weapons (possess116, stage 1 of 3): three extra weapons for Total War, a sniper rifle, a revolver and the knives, lying in the
// underground room where the poison is taken for the objective. Their own colour (gold) and their own aim come in later stages; here
// they are spawned, picked up and come back as the game's own weapons do.
//
// A weapon lying about is a prop (D_800DBEF4's records of 0xA0, D_800DBEF0 of them), made as the level loads by func_150039E0 from the
// level's two lists of setup records (0x44 each, read by func_1502B7F0 into the stack's +0xA0 and +0x9C; the first list's count kept at
// D_800DBEF0 and, as a byte, D_800DBF00): its place (+0x0, +0x2, +0x4, halfwords), its kind (+0x10, the prop's +0x54), its behaviour
// (+0x14, an index into D_80088C90's table: a weapon's is func_1511DF6C) and its id (+0x33, the prop's +0x72). Once both lists are read
// (0x15003AB0, $v0 the second's size), conker_gold_props copies the list holding a weapon of each kind to ours with a copy of that record
// added for each gold weapon: its place the room's, its id one no record has. The match's pickups (D_800D23B0's block: +0xEA4 how many,
// at most 32, each one's id at +0xEF0 (its low byte), kind at +0x1174, state, holder and the rest in arrays of 32 words) are what the
// match's script (func_1508DC24, once a frame) gives and brings back by id: conker_gold_pickups adds one for each gold weapon, a copy of
// a pickup of its kind, as the script runs. Every machine alike: the level's data and the game's memory, at the same frame.

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "recomp.h"

#include "conker.hpp"

namespace {
    constexpr uint32_t scene_now = 0x800BE9F0;          // D_800BE9F0: the scene being played
    constexpr int32_t total_war = 0x2D;
    constexpr uint32_t first_count = 0x800DBEF0;        // D_800DBEF0: the first list's records (then all the props)
    constexpr uint32_t first_count_byte = 0x800DBF00;   // D_800DBF00: the same, a byte
    constexpr uint32_t behaviours = 0x80088C90;         // D_80088C90: 0xC a behaviour, its function first
    constexpr uint32_t weapon_behaviour = 0x1511DF6C;
    constexpr uint32_t record_size = 0x44;
    constexpr uint32_t ours = 0x80FE8000;               // ours: the lists with the gold weapons' records (up to 0x80FF0000)
    constexpr uint32_t ours_size = 0x8000;
    constexpr uint32_t pickups_block = 0x800D23B0;
    constexpr uint32_t pickup_arrays[] = { 0xEF0, 0xF70, 0xFF0, 0x1070, 0x10F0, 0x1174, 0x11F4, 0x1274, 0x12F4, 0x1374, 0x13F4, 0x1474 };
    constexpr int pickups_most = 32;

    struct Gold {
        const char* name;
        uint32_t kind;     // the pickup's kind (the match's pickups' +0x1174)
        uint32_t prop;     // the prop's (its setup record's +0x10: not the pickup's)
        int16_t x, y, z;
        uint8_t id;      // given as the level loads (0: none yet)
        int pickup;      // (possess127) its entry in the match's pickups (-1: none yet)
    };
    // Where player 1 held R-Look in the room (the possess115 test's [pos] lines). (possess118) A weapon's prop has a kind of its own, not
    // its pickup's: the sniper rifle's 0x56 (ids 11 and 12, the pickups of kind 24), the revolver's 0x53 (7, 19: 23), the knives' 0x3C
    // (5, 17: 14), the possess116 test's [weapons] lines; possess116 looked for the pickups' kinds among the props and found none.
    Gold golds[3] = {
        { "sniper rifle", 0x24, 0x56, -63, -1408, 219, 0, -1 },
        { "revolver", 0x23, 0x53, -422, -1408, 30, 0, -1 },
        { "knives", 0x14, 0x3C, 96, -1408, -148, 0, -1 },
    };
    bool golds_made = false;
    std::vector<uint32_t> textures[3];   // (possess128) the textures each weapon's model loads, as the frame's lists are walked
    // (possess131) each vertex load's source (a physical address) in the characters' lists, and the weapons held by the characters whose
    // lists loaded it: a bit a weapon (0 the sniper rifle, 1 the revolver, 2 the knives, 3 another or none). A weapon's part of a
    // character's model is what only its holders load. And whether that's been seen to hold for each weapon (a holder's list sharing most
    // of its loads with the others').
    // (possess135) counts, not bits: how many times each weapon's holders (and the others) loaded it. possess131-133 kept bits, so one
    // frame's list put to the wrong character (players close together, the nearest character to its first matrix) marked a weapon's
    // vertices as everyone's for good, and the gold went back to silver (the possess133 test: the revolver whole until the four of them
    // stood together, never after). A source is a weapon's while its holders' loads are at least 10 times all the others'.
    struct Holders { uint32_t n[4] = {}; };
    std::unordered_map<uint32_t, Holders> vertex_holders;
    // (possess141) the lists found to be a weapon's whole (every load its alone), kept for the level: a frame whose counts dipped drew the
    // weapon in the hand silver again (the possess140 test)
    std::unordered_map<uint32_t, int> whole_lists;
    bool weapons_own(const Holders& h, int g) {
        const uint32_t others = h.n[0] + h.n[1] + h.n[2] + h.n[3] - h.n[g];
        return h.n[g] >= 4 && others * 10u <= h.n[g];
    }
    bool shared_by_others(const Holders& h, int g) {
        const uint32_t others = h.n[0] + h.n[1] + h.n[2] + h.n[3] - h.n[g];
        return others * 10u > h.n[g];
    }
    bool vertex_baseline[3] = {};

    bool kseg0(uint32_t a) { return (a >> 24) == 0x80 && (a & 0x00FFFFFFu) < 0x00FF0000u; }

    void say(const char* format, ...) {
        char line[2400];
        va_list args;
        va_start(args, format);
        std::vsnprintf(line, sizeof(line), format, args);
        va_end(args);
        conker::netsession::log(line);
    }

    bool is_weapon(uint8_t* rdram, uint32_t record) {
        const uint32_t index = (uint32_t)MEM_W(0x14, (gpr)(int32_t)record);
        return index < 0x400u && (uint32_t)MEM_W(0, (gpr)(int32_t)(behaviours + index * 0xCu)) == weapon_behaviour &&
            MEM_W(0xC, (gpr)(int32_t)record) == 0;
    }
}

bool conker::gold_weapons::is_gold_id(int id) {
    if (!golds_made || id <= 0) {
        return false;
    }
    for (const Gold& g : golds) {
        if (g.id == (uint8_t)id) {
            return true;
        }
    }
    return false;
}

// func_150039E0, 0x15003AB0: both setup lists read ($v0 the second's size, the stack's +0xA0 and +0x9C the lists).
extern "C" void conker_gold_props(uint8_t* rdram, recomp_context* ctx) {
    golds_made = false;
    for (auto& t : textures) {
        t.clear();
    }
    vertex_holders.clear();
    whole_lists.clear();
    for (bool& b : vertex_baseline) {
        b = false;
    }
    for (Gold& g : golds) {
        g.id = 0;
        g.pickup = -1;
    }
    if ((int32_t)MEM_W(0, (gpr)(int32_t)scene_now) != total_war) {
        return;
    }
    const gpr sp = ctx->r29;
    uint32_t list[2] = { (uint32_t)MEM_W(0xA0, sp), (uint32_t)MEM_W(0x9C, sp) };
    uint32_t count[2] = { (uint32_t)MEM_W(0, (gpr)(int32_t)first_count), (uint32_t)ctx->r2 / record_size };
    for (int l = 0; l < 2; l++) {
        if (count[l] > 0 && (!kseg0(list[l]) || !kseg0(list[l] + count[l] * record_size))) {
            say("[gold] the level's lists aren't where they should be (%08X x%u, %08X x%u): no gold weapons", (unsigned)list[0],
                (unsigned)count[0], (unsigned)list[1], (unsigned)count[1]);
            return;
        }
    }
    // each gold weapon's template: the first weapon of its kind, and its list
    int from_list[3] = { -1, -1, -1 };
    uint32_t from_record[3] = {};
    bool used[256] = {};
    for (int l = 0; l < 2; l++) {
        for (uint32_t i = 0; i < count[l]; i++) {
            const uint32_t r = list[l] + i * record_size;
            used[MEM_BU(0x33, (gpr)(int32_t)r)] = true;
            if (!is_weapon(rdram, r)) {
                continue;
            }
            for (int g = 0; g < 3; g++) {
                if (from_list[g] < 0 && (uint32_t)MEM_W(0x10, (gpr)(int32_t)r) == golds[g].prop) {
                    from_list[g] = l;
                    from_record[g] = r;
                }
            }
        }
    }
    // the ids: the first ones past 25 (the match's pickups' 1 to 25) no record has
    uint32_t next = 26;
    for (int g = 0; g < 3; g++) {
        if (from_list[g] < 0) {
            say("[gold] no %s among the level's weapons (its prop's kind %02X): left out", golds[g].name, (unsigned)golds[g].prop);
            continue;
        }
        while (next < 200 && used[next]) {
            next++;
        }
        golds[g].id = (uint8_t)next;
        used[next] = true;
    }
    // the lists again, in ours, each with its gold weapons' records at its end
    uint32_t at = ours;
    for (int l = 0; l < 2; l++) {
        int added = 0;
        for (int g = 0; g < 3; g++) {
            added += (golds[g].id != 0 && from_list[g] == l) ? 1 : 0;
        }
        if (added == 0) {
            continue;
        }
        const uint32_t size = (count[l] + (uint32_t)added) * record_size;
        if (at + size > ours + ours_size) {
            say("[gold] no room in ours for the list (%u records)", (unsigned)(count[l] + (uint32_t)added));
            for (Gold& g : golds) {
                g.id = 0;
            }
            return;
        }
        std::memcpy(rdram + (at - 0x80000000u), rdram + (list[l] - 0x80000000u), count[l] * record_size);
        uint32_t r = at + count[l] * record_size;
        for (int g = 0; g < 3; g++) {
            if (golds[g].id == 0 || from_list[g] != l) {
                continue;
            }
            std::memcpy(rdram + (r - 0x80000000u), rdram + (from_record[g] - 0x80000000u), record_size);
            MEM_H(0x0, (gpr)(int32_t)r) = golds[g].x;
            MEM_H(0x2, (gpr)(int32_t)r) = golds[g].y;
            MEM_H(0x4, (gpr)(int32_t)r) = golds[g].z;
            MEM_B(0x33, (gpr)(int32_t)r) = (int8_t)golds[g].id;
            say("[gold] the %s (kind %02X) put in list %d as record %u: id %u at %d %d %d, copied from the one at %d %d %d (id %u)",
                golds[g].name, (unsigned)golds[g].kind, l + 1, (unsigned)((r - at) / record_size), (unsigned)golds[g].id, golds[g].x, golds[g].y,
                golds[g].z, (int)MEM_H(0x0, (gpr)(int32_t)from_record[g]), (int)MEM_H(0x2, (gpr)(int32_t)from_record[g]),
                (int)MEM_H(0x4, (gpr)(int32_t)from_record[g]), (unsigned)MEM_BU(0x33, (gpr)(int32_t)from_record[g]));
            r += record_size;
        }
        if (l == 0) {
            MEM_W(0xA0, sp) = (int32_t)at;
            MEM_W(0, (gpr)(int32_t)first_count) = (int32_t)(count[0] + (uint32_t)added);
            MEM_B(0, (gpr)(int32_t)first_count_byte) = (int8_t)(uint8_t)(count[0] + (uint32_t)added);
        } else {
            MEM_W(0x9C, sp) = (int32_t)at;
            ctx->r2 = (gpr)(int32_t)((count[1] + (uint32_t)added) * record_size);
        }
        at = (at + size + 15u) & ~15u;
    }
    golds_made = golds[0].id != 0 || golds[1].id != 0 || golds[2].id != 0;
}

// func_1508DC24's start (the match's script, once a frame): a pickup for each gold weapon, should the match have none for it.
extern "C" void conker_gold_pickups(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    if (!golds_made || (int32_t)MEM_W(0, (gpr)(int32_t)scene_now) != total_war) {
        return;
    }
    const uint32_t block = (uint32_t)MEM_W(0, (gpr)(int32_t)pickups_block);
    if (!kseg0(block)) {
        return;
    }
    const gpr b = (gpr)(int32_t)block;
    int32_t count = MEM_W(0xEA4, b);
    if (count <= 0 || count > pickups_most) {
        return;
    }
    for (Gold& g : golds) {
        if (g.id == 0) {
            continue;
        }
        int have = -1, like = -1;
        for (int32_t i = 0; i < count; i++) {
            if (MEM_BU(0xEF0 + 4 * (uint32_t)i + 3, b) == g.id) {
                have = i;
            }
            if (like < 0 && (uint32_t)MEM_W(0x1174 + 4 * (uint32_t)i, b) == g.kind) {
                like = i;
            }
        }
        if (have >= 0) {
            g.pickup = have;
            continue;
        }
        if (like < 0 || count >= pickups_most) {
            static uint32_t told = 0;
            if (told++ < 6) {
                say("[gold] no pickup for the %s: %s", g.name, like < 0 ? "the match has none of its kind to copy" : "the match's pickups are full");
            }
            continue;
        }
        for (uint32_t a : pickup_arrays) {
            MEM_W(a + 4 * (uint32_t)count, b) = MEM_W(a + 4 * (uint32_t)like, b);
        }
        MEM_B(0xEF0 + 4 * (uint32_t)count + 3, b) = (int8_t)g.id;
        say("[gold] the %s's pickup made: %d (id %u, kind %02X), a copy of pickup %d (id %u): state %d, flags %X", g.name, (int)count,
            (unsigned)g.id, (unsigned)g.kind, like, (unsigned)MEM_BU(0xEF0 + 4 * (uint32_t)like + 3, b), (int)MEM_W(0x1070 + 4 * (uint32_t)count, b),
            (unsigned)MEM_W(0x11F4 + 4 * (uint32_t)count, b));
        g.pickup = count;
        count++;
        MEM_W(0xEA4, b) = count;
    }
}

// (possess120) func_15113E54 runs the props each frame: those of the level's first list one by one (0 to D_800DBEF0 less D_800DBF98, the
// second list's count), and the second list's only through the groups of D_800D23C0 (the level's own, by index). The gold weapons are
// at the second list's end, in no group, so they were never run (the possess119 test: lying on the floor, not spinning, never taken;
// their +0x7C, the behaviour's own block, never made). Here, at 0x15113F00 (the first loop done), each is run as that loop runs a
// prop: in sight of a view the frame before (+0x6F's 0x40) and not hidden (+0x6E), its behaviour (+0x38, the weapons' func_1511DF6C).
extern "C" void func_1511DF6C(uint8_t* rdram, recomp_context* ctx);

// (possess121, possess122) Gold, lying about. A weapon's model (its prop's +0xC, shared by every prop of its kind) is its vertices (+0x28,
// +0x16 of them, 0x10 each: x, y, z, flags, s, t, then r g b a at +0xC) and right after them its display list (+0x1C, ended by a
// G_ENDDL), which loads those vertices by address. possess121 coloured a copy of the vertices alone, which only the prop's collisions
// read (func_150A44F0: their places into the props' collision buffers), so nothing changed on screen (its test: 98, 190 and 105
// vertices copied, the weapons as ever). Now each gold weapon gets a copy of its whole model in ours, vertices and display list, the
// list's addresses inside the model (its vertices, its own lists, textures and lights) moved to the copy's, the vertices' colours made
// gold (as bright as they were, in the Rare logo's gold); its +0x1C is the copy's list. The level's model and every other weapon as
// they are. [gold] lines: what the copy was, and whether the list lights its vertices (then the colours aren't what's drawn).
namespace {
    constexpr uint32_t models_ours = 0x80FF0000;   // ours: the gold weapons' models (0x5000 each, up to 0x80FFF000)
    struct GoldModel { uint32_t prop = 0, dl = 0, from_dl = 0, from_vertices = 0, vertices = 0, ours = 0; };
    GoldModel gold_models[3];
    constexpr uint32_t model_room = 0x5000;   // each gold weapon's own room in ours

    uint32_t to_kseg0(uint32_t a) {
        if ((a >> 24) == 0x80) {
            return a;
        }
        if (a < 0x00800000u) {
            return a | 0x80000000u;
        }
        return 0;   // (a segment's address: not ours to move)
    }

    void gold_colours(uint8_t* rdram, uint32_t at, uint32_t count, int g) {
        for (uint32_t v = 0; v < count; v++) {
            const gpr c = (gpr)(int32_t)(at + v * 0x10u + 0xCu);
            const uint32_t r = MEM_BU(0, c), gr = MEM_BU(1, c), b = MEM_BU(2, c);
            const uint32_t l = (r * 30u + gr * 59u + b * 11u) / 100u;
            // (possess127) brighter: the Rare logo's gold from a deep amber in the shadows to a pale yellow at the light, which the list's
            // colour combiner adds over the texture (gold_combiner): possess123's (60 + 0.9 l, 40 + 0.7 l, 0.15 l) times the dark texture
            // came out a dull olive (the possess125 test)
            // (possess128) the sniper rifle's texture is the lightest of the three: added over it, that gold came out too bright and lemon
            // yellow (the possess127 test; the revolver and the knives right as they are), so its gold is darker and deeper
            if (g == 0) {
                MEM_B(0, c) = (int8_t)(uint8_t)std::min(255u, 92u + l * 30u / 100u);
                MEM_B(1, c) = (int8_t)(uint8_t)std::min(255u, 52u + l * 24u / 100u);
                MEM_B(2, c) = (int8_t)(uint8_t)std::min(255u, 4u + l * 5u / 100u);
                continue;
            }
            MEM_B(0, c) = (int8_t)(uint8_t)std::min(255u, 150u + l * 45u / 100u);
            MEM_B(1, c) = (int8_t)(uint8_t)std::min(255u, 95u + l * 50u / 100u);
            MEM_B(2, c) = (int8_t)(uint8_t)std::min(255u, 8u + l * 12u / 100u);
        }
    }

    // (possess127) A G_SETCOMBINE (0xFC) of the copied list made shiny gold: the colour of each cycle (a - b) * c + d as TEXEL0 * SHADE +
    // SHADE, the texture's detail over the vertices' gold and brighter for it, where the weapon's own was TEXEL0 * SHADE (the vertices' colour
    // times the texture: the dark gun metal kept it dull). The second cycle too when it's the first's (one cycle's lists set both alike);
    // its alpha as it was. Returns whether it changed.
    bool gold_combiner(uint8_t* rdram, uint32_t c) {
        const gpr at = (gpr)(int32_t)c;
        uint32_t w0 = (uint32_t)MEM_W(0, at), w1 = (uint32_t)MEM_W(4, at);
        const uint32_t a0 = (w0 >> 20) & 0xF, c0 = (w0 >> 15) & 0x1F, a1 = (w0 >> 5) & 0xF, c1 = w0 & 0x1F;
        const uint32_t b0 = (w1 >> 28) & 0xF, b1 = (w1 >> 24) & 0xF, d0 = (w1 >> 15) & 0x7, d1 = (w1 >> 6) & 0x7;
        const bool texture_shade = a0 == 1 && c0 == 4;   // (TEXEL0 times SHADE, as a weapon's own)
        if (!texture_shade) {
            return false;
        }
        const bool second_alike = a1 == a0 && c1 == c0 && b1 == b0 && d1 == d0;
        w0 = (w0 & ~((0xFu << 20) | (0x1Fu << 15))) | (1u << 20) | (4u << 15);
        w1 = (w1 & ~((0xFu << 28) | (0x7u << 15))) | (15u << 28) | (4u << 15);
        if (second_alike) {
            w0 = (w0 & ~((0xFu << 5) | 0x1Fu)) | (1u << 5) | 4u;
            w1 = (w1 & ~((0xFu << 24) | (0x7u << 6))) | (15u << 24) | (4u << 6);
        }
        MEM_W(0, at) = (int32_t)w0;
        MEM_W(4, at) = (int32_t)w1;
        return true;
    }

    // the gold copy of a prop's model: its list's address in ours, or 0
    uint32_t gold_model(uint8_t* rdram, uint32_t prop, int g) {
        const gpr p = (gpr)(int32_t)prop;
        const uint32_t from = (uint32_t)MEM_W(0x28, p), dl = (uint32_t)MEM_W(0x1C, p), n = (uint32_t)MEM_HU(0x16, p);
        if (!kseg0(from) || !kseg0(dl) || dl < from || dl - from != n * 0x10u || n == 0 || n > 0x800) {
            say("[gold] the %s's model isn't as expected (vertices %08X x%u, list %08X): not made gold", golds[g].name, (unsigned)from, (unsigned)n,
                (unsigned)dl);
            return 0;
        }
        // the list's end (its G_ENDDL)
        uint32_t end = 0, lit = 0;
        std::string ops;
        for (uint32_t c = dl; c < dl + 0x8000u && kseg0(c + 8); c += 8) {
            const uint32_t w0 = (uint32_t)MEM_W(0, (gpr)(int32_t)c), w1 = (uint32_t)MEM_W(4, (gpr)(int32_t)c);
            const uint32_t op = w0 >> 24;
            if (ops.size() < 24 * 3) {
                char piece[8];
                std::snprintf(piece, sizeof(piece), " %02X", (unsigned)op);
                ops += piece;
            }
            if (op == 0xD9 && (w1 & 0x00200000u) != 0) {
                lit = 1;
            }
            if (op == 0xDF) {
                end = c + 8;
                break;
            }
        }
        const uint32_t size = end > from ? end - from : 0;
        if (end == 0 || size > model_room) {
            say("[gold] the %s's model: %s (%u bytes): not made gold", golds[g].name, end == 0 ? "no end to its list" : "no room in ours",
                (unsigned)size);
            return 0;
        }
        const uint32_t ours = models_ours + (uint32_t)g * model_room;
        std::memcpy(rdram + (ours - 0x80000000u), rdram + (from - 0x80000000u), size);
        uint32_t moved = 0;
        for (uint32_t c = ours + n * 0x10u; c < ours + size; c += 8) {
            const uint32_t op = (uint32_t)MEM_W(0, (gpr)(int32_t)c) >> 24;
            if (op != 0x01 && op != 0xDE && op != 0xFD && op != 0xDC && op != 0xDB) {
                continue;
            }
            const uint32_t w1 = (uint32_t)MEM_W(4, (gpr)(int32_t)c);
            const uint32_t a = to_kseg0(w1);
            if (a >= from && a < end) {
                const uint32_t to = ours + (a - from);
                MEM_W(4, (gpr)(int32_t)c) = (int32_t)((w1 >> 24) == 0x80 ? to : (to & 0x1FFFFFFFu));
                moved++;
            }
        }
        // (possess125) the vertex loads (G_VTX, 0x01) by a segment's address: the game sets the segment to the model's vertices before it
        // draws the list, so the loads' offsets, from the lowest, are the vertices' places in the model; they're made our copy's own
        // (physical) addresses, so the copy draws its own gold vertices. Only when every load fits inside the model's vertices.
        uint32_t seg_low = 0xFFFFFFFFu, seg_high = 0, seg_loads = 0, seg = 0xFF;
        std::string loads;
        for (uint32_t c = ours + n * 0x10u; c < ours + size; c += 8) {
            const uint32_t w0 = (uint32_t)MEM_W(0, (gpr)(int32_t)c), w1 = (uint32_t)MEM_W(4, (gpr)(int32_t)c);
            if ((w0 >> 24) != 0x01) {
                continue;
            }
            if (loads.size() < 6 * 10) {
                char piece[12];
                std::snprintf(piece, sizeof(piece), " %08X", (unsigned)w1);
                loads += piece;
            }
            const uint32_t s_id = w1 >> 24;
            if (s_id == 0 || s_id > 0x0F || (seg != 0xFF && seg != s_id)) {
                seg_loads = 0xFFFFFFFFu;   // (not all by one segment: left alone)
                break;
            }
            seg = s_id;
            const uint32_t off = w1 & 0x00FFFFFFu, count = (w0 >> 12) & 0xFFu;
            seg_low = std::min(seg_low, off);
            seg_high = std::max(seg_high, off + count * 0x10u);
            seg_loads++;
        }
        uint32_t seg_moved = 0;
        if (seg_loads != 0 && seg_loads != 0xFFFFFFFFu && seg_high - seg_low <= n * 0x10u) {
            for (uint32_t c = ours + n * 0x10u; c < ours + size; c += 8) {
                const uint32_t w0 = (uint32_t)MEM_W(0, (gpr)(int32_t)c), w1 = (uint32_t)MEM_W(4, (gpr)(int32_t)c);
                if ((w0 >> 24) == 0x01) {
                    MEM_W(4, (gpr)(int32_t)c) = (int32_t)((ours + ((w1 & 0x00FFFFFFu) - seg_low)) & 0x1FFFFFFFu);
                    seg_moved++;
                }
            }
        }
        gold_colours(rdram, ours, n, g);
        uint32_t combiners = 0, shiny = 0;
        std::string combines;
        for (uint32_t c = ours + n * 0x10u; c < ours + size; c += 8) {
            if (((uint32_t)MEM_W(0, (gpr)(int32_t)c) >> 24) != 0xFC) {
                continue;
            }
            if (combines.size() < 3 * 18) {
                char piece[24];
                std::snprintf(piece, sizeof(piece), " %06X:%08X", (unsigned)((uint32_t)MEM_W(0, (gpr)(int32_t)c) & 0xFFFFFFu),
                    (unsigned)MEM_W(4, (gpr)(int32_t)c));
                combines += piece;
            }
            combiners++;
            shiny += gold_combiner(rdram, c) ? 1u : 0u;
        }
        say("[gold] the %s's colour combiners: %u in its list, %u made shiny gold (TEXEL0 x SHADE + SHADE); they were:%s", golds[g].name,
            (unsigned)combiners, (unsigned)shiny, combines.c_str());
        gold_models[g].from_vertices = from;
        gold_models[g].vertices = n;
        gold_models[g].ours = ours;
        say("[gold] the %s made gold: its model (%u vertices at %08X, its list at %08X, %u bytes in all) copied to %08X, %u addresses moved, "
            "%u vertex loads by segment %X made ours (offsets %X to %X); the list %s its vertices; its first commands:%s; its first loads:%s",
            golds[g].name, (unsigned)n, (unsigned)from, (unsigned)dl, (unsigned)size, (unsigned)ours, (unsigned)moved, (unsigned)seg_moved,
            (unsigned)(seg == 0xFF ? 0 : seg), (unsigned)(seg_low == 0xFFFFFFFFu ? 0 : seg_low), (unsigned)seg_high,
            lit ? "LIGHTS" : "doesn't light", ops.c_str(), loads.c_str());
        return ours + n * 0x10u;
    }

    void gold_paint(uint8_t* rdram, uint32_t list, uint32_t first, uint32_t count) {
        for (uint32_t i = first; i < count; i++) {
            const uint32_t prop = list + i * 0xA0u;
            const gpr p = (gpr)(int32_t)prop;
            int g = -1;
            for (int k = 0; k < 3; k++) {
                g = (golds[k].id != 0 && MEM_BU(0x72, p) == golds[k].id) ? k : g;
            }
            if (g < 0) {
                continue;
            }
            GoldModel& m = gold_models[g];
            const uint32_t now = (uint32_t)MEM_W(0x1C, p);
            if (now == m.dl || (m.prop == prop && m.dl == 1 && now == m.from_dl)) {
                continue;   // (gold already, or tried and failed for this model)
            }
            // (a new level, or the first time: made again from the model as it is now)
            m.prop = prop;
            m.from_dl = now;
            m.dl = gold_model(rdram, prop, g);
            if (m.dl == 0) {
                m.dl = 1;
                continue;
            }
            MEM_W(0x1C, p) = (int32_t)m.dl;
        }
    }
}

extern "C" void conker_gold_run(uint8_t* rdram, recomp_context* ctx) {
    if (!golds_made || (int32_t)MEM_W(0, (gpr)(int32_t)scene_now) != total_war) {
        return;
    }
    const uint32_t list = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800DBEF4);
    const uint32_t count = (uint32_t)MEM_W(0, (gpr)(int32_t)first_count);
    if (!kseg0(list) || count == 0 || count > 1024 || !kseg0(list + count * 0xA0u)) {
        return;
    }
    const uint32_t first = count - std::min(count, (uint32_t)MEM_W(0, (gpr)(int32_t)0x800DBF98));
    gold_paint(rdram, list, first, count);
    for (uint32_t i = first; i < count; i++) {
        const uint32_t prop = list + i * 0xA0u;
        const gpr p = (gpr)(int32_t)prop;
        bool gold = false;
        for (const Gold& g : golds) {
            gold = gold || (g.id != 0 && MEM_BU(0x72, p) == g.id);
        }
        if (!gold || (uint32_t)MEM_W(0x38, p) != weapon_behaviour || (MEM_BU(0x6F, p) & 0x40u) == 0 || MEM_BU(0x6E, p) != 0) {
            continue;
        }
        const recomp_context saved = *ctx;
        ctx->r4 = p;
        func_1511DF6C(rdram, ctx);
        *ctx = saved;
    }
}

// ---- (possess127) A gold weapon's own aim: the 5th player's view, scoped ----
// The 5th player's view through a scope (the sniper rifle's, the revolver's, the knives') shows his own gun close up in front of him, where
// players 1 to 4 see past nothing: func_1502E4C4 readies each character's drawing for the frame and, while its character is scoped (its
// +0x31C's +0x197) or in first person (its camera's +0x2C 0x10000), marks it not drawn in view k (+0x74, a bit a view it isn't drawn in),
// k its own index (0x1502E95C to 0x1502E9A0): a player's own view. The 5th player's character is the match's 5th, its bit 4, past the
// views, and its view is drawn as view 1 (index 0), so it was never hidden there: his own body and gun drawn from inside the scope. A
// player holding a gold weapon (the pickup it holds, the match's +0xEB0 a character, one of the gold weapons'), scoped, keeps that bit
// clear: drawn in their own view as the 5th player is. From the game's memory, the same on every machine.
namespace {
    bool kept_projection_have(uint32_t view);
    void put_kept_projection(uint8_t* rdram, uint32_t view, uint32_t record);
    int gold_scoped_view(uint8_t* rdram, uint32_t view);
}
namespace {
    constexpr uint32_t characters = 0x800CC2D0;   // gObjects: 0x32C a character
    constexpr uint32_t character_size = 0x32C;

    int gold_held(uint8_t* rdram, int character) {
        if (!golds_made || character < 0 || character >= 25) {
            return -1;
        }
        const uint32_t block = (uint32_t)MEM_W(0, (gpr)(int32_t)pickups_block);
        if (!kseg0(block)) {
            return -1;
        }
        const int32_t held = MEM_W(0xEB0 + 4 * (uint32_t)character, (gpr)(int32_t)block);
        for (int g = 0; g < 3; g++) {
            if (golds[g].id != 0 && golds[g].pickup >= 0 && held == golds[g].pickup) {
                return g;
            }
        }
        return -1;
    }
    uint32_t scope_told = 0;
    int scope_last[25];
    bool scope_last_set = false;
}

// func_1502E4C4 at 0x1502E9A0: $t7 the character's +0x74 with its own view's bit set ($t4 as it was), $s1 the character.
extern "C" void conker_gold_scope_body(uint8_t* rdram, recomp_context* ctx) {
    if (!golds_made || (int32_t)MEM_W(0, (gpr)(int32_t)scene_now) != total_war) {
        return;
    }
    const uint32_t c = (uint32_t)ctx->r17;
    if (c < characters || (c - characters) % character_size != 0 || (c - characters) / character_size >= 25) {
        return;
    }
    const int k = (int)((c - characters) / character_size);
    const gpr aim = (gpr)(int32_t)MEM_W(0x31C, (gpr)(int32_t)c);
    if (!kseg0((uint32_t)aim) || MEM_BU(0x197, aim) == 0) {
        return;
    }
    const int g = gold_held(rdram, k);
    if (!scope_last_set) {
        scope_last_set = true;
        for (int& v : scope_last) {
            v = -2;
        }
    }
    if (g != scope_last[k] && scope_told < 40) {
        scope_told++;
        say("[gold] character %d scoped with %s: %s", k, g >= 0 ? golds[g].name : "a weapon not gold",
            g >= 0 ? "drawn in its own view (the 5th player's aim)" : "hidden from its own view, as the game has it");
    }
    scope_last[k] = g;
    if (g >= 0) {
        ctx->r15 = ctx->r12;
        // (possess128) and seen in that view: scoped, the view's eye is inside his head, so the loop above hid him there too (func_150A6360's
        // check found him at the eye: func_1506160C's 10, his hidden bits for the view, +0xF, and his fade in it, +0xB a view, to 0), and
        // the characters' drawing (func_15030E08) leaves out one faded to nothing: possess127 cleared the bit alone and nothing changed (the
        // possess127 test). His fade in his own view full and its hidden bits clear, as the 5th player's are in his.
        if (k < 4) {
            const gpr ch = (gpr)(int32_t)c;
            MEM_B(0xF, ch) = (int8_t)(MEM_BU(0xF, ch) & ~((1u << k) | (0x10u << k)));
            MEM_B(0xB + (uint32_t)k, ch) = (int8_t)0xFF;
        }
    }
}

// (possess129) func_1502E4C4 at 0x1502E9C8, its end: $t2 the character's +0x74 with its +0x2FC ORed in (the views it's to be left out of
// besides, which the camera sets in first person and scoped: possess128's clearing at 0x1502E9A0 was put back here, the possess128 test's
// player 1 never seen in his own scope). For a gold weapon's holder, scoped, its own view's bit clear again. [gold] lines of the bits.
extern "C" void conker_gold_scope_end(uint8_t* rdram, recomp_context* ctx) {
    if (!golds_made || (int32_t)MEM_W(0, (gpr)(int32_t)scene_now) != total_war) {
        return;
    }
    const uint32_t c = (uint32_t)ctx->r17;
    if (c < characters || (c - characters) % character_size != 0 || (c - characters) / character_size >= 4) {
        return;
    }
    const int k = (int)((c - characters) / character_size);
    const gpr aim = (gpr)(int32_t)MEM_W(0x31C, (gpr)(int32_t)c);
    if (!kseg0((uint32_t)aim) || MEM_BU(0x197, aim) == 0 || gold_held(rdram, k) < 0) {
        return;
    }
    static uint32_t told = 0;
    const uint32_t before = (uint32_t)ctx->r10 & 0xFFu;
    if (told < 20 && (before & (1u << k)) != 0) {
        told++;
        say("[gold] character %d scoped with a gold weapon: its +0x2FC %02X put it out of its own view again (+0x74 %02X): cleared; its hidden bits %02X, "
            "its fades %u %u %u %u", k, (unsigned)MEM_BU(0x2FC, (gpr)(int32_t)c), (unsigned)before, (unsigned)MEM_BU(0xF, (gpr)(int32_t)c),
            (unsigned)MEM_BU(0xB, (gpr)(int32_t)c), (unsigned)MEM_BU(0xC, (gpr)(int32_t)c), (unsigned)MEM_BU(0xD, (gpr)(int32_t)c),
            (unsigned)MEM_BU(0xE, (gpr)(int32_t)c));
    }
    ctx->r10 = (gpr)(int32_t)(before & ~(1u << k));
}

// ---- (possess127) Where a weapon in a hand is drawn: the frame's list walked ----
// A gold weapon on the ground draws from its own gold copy (gold_model); in a hand it's the game's own again (the possess125 test). How the
// hand's weapon is drawn isn't known yet: whether from the same model (its list called, its vertices loaded) or another. At the end of the
// frame's list (func_1501878C, 0x15018C8C, from possess_test.cpp's hook), every 30 frames, the list is walked through the lists it calls
// (segments as it sets them) and each call of a weapon's model (the sniper rifle's, the revolver's, the knives': the props' own, and our
// gold copies) and each load of its vertices counted, with where from: [gold] [hand] lines, for the next step.
namespace {
    // a texture only weapon g's model loads (none of the other two's)
    bool own_texture(int g, uint32_t t) {
        if (std::find(textures[g].begin(), textures[g].end(), t) == textures[g].end()) {
            return false;
        }
        for (int o = 0; o < 3; o++) {
            if (o != g && std::find(textures[o].begin(), textures[o].end(), t) != textures[o].end()) {
                return false;
            }
        }
        return true;
    }

    // (possess129) Gold in the hand: a copy of the character's list (and of any list it calls on the way) whose weapon's part, from a load
    // of the weapon's own texture until a texture not the weapon's, loads gold copies of its vertices (gold_colours, ours) and has its colour
    // combiners shiny gold (gold_combiner); the frame's call of the character's list is pointed at the copy, for that frame alone. Ours:
    // 0x80F90000 to 0x80FA0000 (net_view.cpp's ring of the HUD's lists ends at 0x80F90000, net_menu.cpp's mirror starts at 0x80FA0000), a
    // half each frame, the frame before's left for RT64 as it draws it.
    struct Clone {
        uint8_t* rdram;
        uint32_t at, end;
        int g;
        bool in_part = false;
        uint32_t vertices = 0, combiners = 0, lists = 0;
        bool full = false;
        // (possess131) the weapon's part by its vertices (vertex_holders) once that's been seen for the weapon, else by its textures; and,
        // for the local player scoped with it, the body's triangles left out (the view's eye is inside him: only the gun drawn)
        bool by_vertices = false, hide_body = false, last_weapon = false;
        // (possess132) the whole list the weapon's: every vertex load in it only that weapon's holders' (the hand's weapon is a list of its
        // own: the revolver's 13 loads at 265490, the sniper rifle's 5 at 264D90, the possess131 test), so all of it gold, its combiners too
        bool whole = false;
        uint32_t hidden = 0, hidden_vertices = 0;
        bool weapon_vertex(uint32_t a) const {
            auto it = vertex_holders.find(a);
            return it != vertex_holders.end() && weapons_own(it->second, g);
        }

        uint32_t take(uint32_t bytes) {
            bytes = (bytes + 15u) & ~15u;
            if (at + bytes > end) {
                full = true;
                return 0;
            }
            const uint32_t a = at;
            at += bytes;
            return a;
        }
        static bool phys_ok(uint32_t a) { return a < 0x00800000u && (a & 7u) == 0; }
        static uint32_t resolve(const uint32_t* seg, uint32_t a) {
            if ((a >> 24) == 0x80) {
                return a & 0x1FFFFFFFu;
            }
            const uint32_t s = (a >> 24) & 0x0Fu;
            return (s == 0 ? 0u : seg[s]) + (a & 0x00FFFFFFu);
        }
        // the list at src (physical) copied, its weapon's part made gold: the copy's physical address, or 0 (nothing in it to change)
        uint32_t list(uint32_t src, uint32_t* seg, int depth) {
            if (!phys_ok(src) || depth > 8 || full) {
                return 0;
            }
            uint32_t length = 0;
            for (uint32_t c = src; phys_ok(c) && c - src < 0x4000u; c += 8) {
                length += 8;
                if (((uint32_t)MEM_W(0, (gpr)(int32_t)(c | 0x80000000u)) >> 24) == 0xDF) {
                    break;
                }
            }
            const uint32_t mark = at;
            const uint32_t copy = take(length);
            if (copy == 0) {
                return 0;
            }
            std::memcpy(rdram + (copy - 0x80000000u), rdram + src, length);
            bool changed = false;
            for (uint32_t c = copy; c < copy + length; c += 8) {
                const gpr cmd = (gpr)(int32_t)c;
                const uint32_t w0 = (uint32_t)MEM_W(0, cmd), w1 = (uint32_t)MEM_W(4, cmd);
                const uint32_t op = w0 >> 24;
                if (op == 0xDF) {
                    break;
                }
                if (op == 0xDB && ((w0 >> 16) & 0xFFu) == 0x06 && (w0 & 0xFFFFu) < 64u) {
                    seg[(w0 & 0xFFFFu) / 4u] = w1 & 0x00FFFFFFu;
                } else if (op == 0xFD) {
                    const uint32_t t = resolve(seg, w1);
                    if (own_texture(g, t)) {
                        in_part = true;
                    } else if (std::find(textures[g].begin(), textures[g].end(), t) == textures[g].end()) {
                        in_part = false;
                    }
                } else if (op == 0xFC && (in_part || whole)) {
                    if (gold_combiner(rdram, c)) {
                        combiners++;
                        changed = true;
                    }
                } else if ((op == 0x05 || op == 0x06 || op == 0x07) && hide_body && !last_weapon) {
                    MEM_W(0, cmd) = 0;
                    MEM_W(4, cmd) = 0;
                    hidden++;
                    changed = true;
                } else if (op == 0x01 && !whole && !(by_vertices ? weapon_vertex(resolve(seg, w1)) : in_part)) {
                    last_weapon = false;
                    // (possess135) the body left out by its vertices: a copy of them all at one place (its first's), so whatever draws its
                    // triangles draws nothing (possess131-133 cleared the triangle commands alone, 0x05 to 0x07, and the body stayed)
                    const uint32_t n = (w0 >> 12) & 0xFFu;
                    const uint32_t from = resolve(seg, w1);
                    if (hide_body && n != 0 && phys_ok(from) && from + n * 0x10u <= 0x00800000u) {
                        const uint32_t v = take(n * 0x10u);
                        if (v != 0) {
                            std::memcpy(rdram + (v - 0x80000000u), rdram + from, n * 0x10u);
                            const uint32_t first = (uint32_t)MEM_W(0, (gpr)(int32_t)v), second = (uint32_t)MEM_W(4, (gpr)(int32_t)v) & 0xFFFF0000u;
                            for (uint32_t k = 1; k < n; k++) {
                                const gpr at = (gpr)(int32_t)(v + k * 0x10u);
                                MEM_W(0, at) = (int32_t)first;
                                MEM_W(4, at) = (int32_t)(second | ((uint32_t)MEM_W(4, at) & 0xFFFFu));
                            }
                            MEM_W(4, cmd) = (int32_t)(v & 0x1FFFFFFFu);
                            hidden_vertices += n;
                            changed = true;
                        }
                    }
                } else if (op == 0x01) {
                    last_weapon = true;
                    const uint32_t n = (w0 >> 12) & 0xFFu;
                    const uint32_t from = resolve(seg, w1);
                    if (n != 0 && phys_ok(from) && from + n * 0x10u <= 0x00800000u) {
                        const uint32_t v = take(n * 0x10u);
                        if (v != 0) {
                            std::memcpy(rdram + (v - 0x80000000u), rdram + from, n * 0x10u);
                            gold_colours(rdram, v, n, g);
                            MEM_W(4, cmd) = (int32_t)(v & 0x1FFFFFFFu);
                            vertices += n;
                            changed = true;
                        }
                    }
                } else if (op == 0xDE) {
                    const uint32_t to = resolve(seg, w1);
                    const uint32_t child = list(to, seg, depth + 1);
                    if (child != 0) {
                        MEM_W(4, cmd) = (int32_t)child;
                        changed = true;
                    }
                    if (((w0 >> 16) & 0xFFu) == 1) {
                        break;
                    }
                }
            }
            if (!changed) {
                at = mark;
                return 0;
            }
            lists++;
            return copy & 0x1FFFFFFFu;
        }
    };

    struct Walk {
        uint8_t* rdram;
        uint32_t seg[16] = {};
        uint32_t steps = 0;
        uint32_t calls[3][2] = {}, loads[3][2] = {};
        uint32_t matrix = 0;
        std::string samples[3];
        uint32_t sampled[3] = {};
        // (possess128) the textures each weapon's model loads (G_SETTIMG inside its list), and where else they're loaded from: the hand's
        // weapon is drawn from a model of its own (the possess127 test: none of the props' models called for it)
        std::string elsewhere[3];
        uint32_t elsewhere_n[3] = {}, elsewhere_told[3] = {};
        // (possess129) where a weapon's own texture (its model's alone, not one the others' share) is loaded outside its model: the hand's
        // weapon, part of the character's model (the possess128 test: the revolver's 1F54C0 loaded 0x2D8 into the list at 264D90, called from
        // the frame's list, with a matrix at player 1's place as he held it). Each: the frame's command calling that list, its list, the
        // segments as they were there, the matrix at the load and the weapon.
        struct Site { uint32_t call, to, matrix; int g; uint32_t seg[16]; };
        std::vector<Site> sites;
        // (possess131) every call of a list from the frame's list: its first matrix inside (a character's is at the character), its segments,
        // its vertex loads' sources, and a short listing of it (for the [hand] lines)
        struct Call { uint32_t call, to, matrix; uint32_t seg[16]; std::vector<uint32_t> vertices; std::string listing; uint32_t listed;
            uint32_t fc0 = 0, fc1 = 0; };   // (possess136) the colour combiner set when it was called
        uint32_t last_fc0 = 0, last_fc1 = 0;
        std::vector<Call> all;
        int cur = -1;
        uint32_t top_call = 0, top_to = 0;
        uint32_t top_seg[16] = {};
        static float fixed(uint8_t* rdram, uint32_t m, int r, int c) {
            const gpr at = (gpr)(int32_t)(m | 0x80000000u);
            const int32_t i = MEM_H((uint32_t)((r * 4 + c) * 2), at);
            const uint32_t f = (uint16_t)MEM_HU((uint32_t)(32 + (r * 4 + c) * 2), at);
            return (float)i + (float)f / 65536.0f;
        }

        static bool phys_ok(uint32_t a) { return a < 0x00800000u && (a & 7u) == 0; }
        uint32_t resolve(uint32_t a) const {
            if ((a >> 24) == 0x80) {
                return a & 0x1FFFFFFFu;
            }
            const uint32_t s = (a >> 24) & 0x0Fu;
            return (s == 0 ? 0u : seg[s]) + (a & 0x00FFFFFFu);
        }
        uint32_t word(uint32_t phys) const { return (uint32_t)MEM_W(0, (gpr)(int32_t)(phys | 0x80000000u)); }
        void note(int g, int which, bool call, uint32_t at, uint32_t list, int depth) {
            (call ? calls : loads)[g][which]++;
            if (sampled[g] < 4) {
                sampled[g]++;
                char piece[120];
                std::snprintf(piece, sizeof(piece), " | %s %s at %06X in the list at %06X (depth %d, matrix %06X)", call ? "called" : "vertices",
                    which == 0 ? "the game's" : "our gold", (unsigned)at, (unsigned)list, depth, (unsigned)matrix);
                samples[g] += piece;
            }
        }
        void run(uint32_t list, int depth, uint32_t end, int inside = -1) {
            for (uint32_t c = list; phys_ok(c) && steps < 400000u; c += 8) {
                if (end != 0 && c >= end) {
                    return;
                }
                steps++;
                const uint32_t w0 = word(c), w1 = word(c + 4);
                const uint32_t op = w0 >> 24;
                if (op == 0xDF) {
                    return;
                }
                if (op == 0xDB && ((w0 >> 16) & 0xFFu) == 0x06 && (w0 & 0xFFFFu) < 64u) {
                    seg[(w0 & 0xFFFFu) / 4u] = w1 & 0x00FFFFFFu;
                    continue;
                }
                if (cur >= 0 && depth >= 1 && all[(size_t)cur].listed < 90 && (op == 0xDA || op == 0xFD || op == 0x01 || op == 0xDE || op == 0xFC || op == 0xDB)) {
                    Call& k = all[(size_t)cur];
                    k.listed++;
                    char piece[32];
                    if (op == 0x01) {
                        std::snprintf(piece, sizeof(piece), " 01:%u@%06X", (unsigned)((w0 >> 12) & 0xFFu), (unsigned)resolve(w1));
                    } else {
                        std::snprintf(piece, sizeof(piece), " %02X:%06X", (unsigned)op, (unsigned)(op == 0xFC ? (w1 & 0xFFFFFFu) : resolve(w1)));
                    }
                    if (k.listing.size() < 1400) {
                        k.listing += piece;
                    }
                }
                if (op == 0xFC) {
                    last_fc0 = w0;
                    last_fc1 = w1;
                }
                if (op == 0xDA) {
                    matrix = resolve(w1);
                    if (cur >= 0 && depth >= 1 && all[(size_t)cur].matrix == 0) {
                        all[(size_t)cur].matrix = matrix;
                    }
                    continue;
                }
                if (op == 0xFD) {
                    const uint32_t t = resolve(w1);
                    for (int g = 0; g < 3; g++) {
                        auto& known = textures[g];
                        const bool is_known = std::find(known.begin(), known.end(), t) != known.end();
                        if (inside == g) {
                            if (!is_known && known.size() < 8) {
                                known.push_back(t);
                            }
                        } else if (inside < 0 && is_known) {
                            if (depth >= 1 && top_call != 0 && own_texture(g, t) && phys_ok(matrix)) {
                                bool have = false;
                                for (const Site& site : sites) {
                                    have = have || site.call == top_call;
                                }
                                if (!have && sites.size() < 32) {
                                    Site site{ top_call, top_to, matrix, g, {} };
                                    std::memcpy(site.seg, top_seg, sizeof(site.seg));
                                    sites.push_back(site);
                                }
                            }
                            elsewhere_n[g]++;
                            if (elsewhere_told[g] < 3 && phys_ok(matrix)) {
                                elsewhere_told[g]++;
                                char piece[200];
                                std::snprintf(piece, sizeof(piece), " | its texture %06X loaded at %06X in the list at %06X (depth %d), matrix %06X at %.0f %.0f %.0f",
                                    (unsigned)t, (unsigned)c, (unsigned)list, depth, (unsigned)matrix, fixed(rdram, matrix, 3, 0), fixed(rdram, matrix, 3, 1),
                                    fixed(rdram, matrix, 3, 2));
                                elsewhere[g] += piece;
                            }
                        }
                    }
                    continue;
                }
                if (op == 0x01) {
                    const uint32_t a = resolve(w1);
                    if (cur >= 0 && all[(size_t)cur].vertices.size() < 600) {
                        all[(size_t)cur].vertices.push_back(a);
                    }
                    for (int g = 0; g < 3; g++) {
                        const GoldModel& m = gold_models[g];
                        const uint32_t game_v = m.from_vertices & 0x1FFFFFFFu, our_v = m.ours & 0x1FFFFFFFu;
                        if (m.vertices != 0 && a >= game_v && a < game_v + m.vertices * 0x10u) {
                            note(g, 0, false, c, list, depth);
                        } else if (m.ours != 0 && a >= our_v && a < our_v + m.vertices * 0x10u) {
                            note(g, 1, false, c, list, depth);
                        }
                    }
                    continue;
                }
                if (op == 0xDE) {
                    const uint32_t to = resolve(w1);
                    int into = inside;
                    for (int g = 0; g < 3; g++) {
                        const GoldModel& m = gold_models[g];
                        if ((m.from_dl != 0 && to == (m.from_dl & 0x1FFFFFFFu)) || (m.dl > 1 && to == (m.dl & 0x1FFFFFFFu))) {
                            into = g;
                        }
                    }
                    for (int g = 0; g < 3; g++) {
                        const GoldModel& m = gold_models[g];
                        if (m.from_dl != 0 && to == (m.from_dl & 0x1FFFFFFFu)) {
                            note(g, 0, true, c, list, depth);
                        } else if (m.dl > 1 && to == (m.dl & 0x1FFFFFFFu)) {
                            note(g, 1, true, c, list, depth);
                        }
                    }
                    if (!phys_ok(to)) {
                        continue;
                    }
                    if (((w0 >> 16) & 0xFFu) == 1) {   // (a branch: no return)
                        c = to - 8;
                        list = to;
                        continue;
                    }
                    if (depth < 14) {
                        if (depth == 0) {
                            top_call = c;
                            top_to = to;
                            std::memcpy(top_seg, seg, sizeof(top_seg));
                            if (all.size() < 400) {
                                Call k{ c, to, 0, {}, {}, {}, 0 };
                                std::memcpy(k.seg, seg, sizeof(k.seg));
                                k.fc0 = last_fc0;
                                k.fc1 = last_fc1;
                                all.push_back(std::move(k));
                                cur = (int)all.size() - 1;
                            }
                        }
                        run(to, depth + 1, 0, into);
                        if (depth == 0) {
                            top_call = 0;
                            cur = -1;
                        }
                    }
                }
            }
        }
    };
    uint32_t hand_told = 0, hand_frames = 0;
}

void conker::gold_weapons::frame_end(uint8_t* rdram, uint32_t start, uint32_t end) {
    if (!golds_made || (int32_t)MEM_W(0, (gpr)(int32_t)scene_now) != total_war) {
        return;
    }
    if (!kseg0(start) || !kseg0(end) || end <= start || end - start > 0x100000u) {
        return;
    }
    hand_frames++;
    bool anyone = false;
    for (int k = 0; k < 5; k++) {
        anyone = anyone || gold_held(rdram, k) >= 0;
    }
    const bool tell = hand_told < 300 && (hand_frames % 60u) == 0;
    // (possess142) and, as the frame's list ends, each gold weapon's scoped holder's view's projection (both buffers) as its unscoped one,
    // whatever made it since (func_1510B5F8 puts it back as it's made: conker_gold_projection_made)
    if (anyone) {
        const uint32_t records = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800BE628);
        for (uint32_t v = 0; v < 4 && kseg0(records); v++) {
            if (!kept_projection_have(v) || gold_scoped_view(rdram, v) < 0) {
                continue;
            }
            const uint32_t record = records + v * 0x180u;
            put_kept_projection(rdram, v, record);
        }
    }
    if (!anyone && !tell && (hand_frames % 30u) != 0) {
        return;   // (each half second still, so the weapons' textures are learnt from the ones lying about)
    }
    Walk w{ rdram };
    w.run(start & 0x1FFFFFFFu, 0, end & 0x1FFFFFFFu);
    // (possess131) each call from the frame's list whose first matrix is at a character (within 250 across): the weapon that character holds
    // (its pickup's kind: one of the three, or 3 for another or none) noted for each vertex source it loads; then each call of a gold
    // weapon's holder pointed at a gold copy (Clone): its weapon's part by the vertices only that weapon's holders load, once the holder's
    // list has been seen sharing most of its loads with the others' (the same model), else by the weapon's own textures (possess129).
    const uint32_t block = (uint32_t)MEM_W(0, (gpr)(int32_t)pickups_block);
    auto weapon_of = [&](int k) -> int {
        if (!kseg0(block)) {
            return 3;
        }
        const int32_t p = MEM_W(0xEB0 + 4 * (uint32_t)k, (gpr)(int32_t)block);
        if (p < 0 || p >= pickups_most) {
            return 3;
        }
        const uint32_t kind = (uint32_t)MEM_W(0x1174 + 4 * (uint32_t)p, (gpr)(int32_t)block);
        for (int g = 0; g < 3; g++) {
            if (golds[g].kind == kind) {
                return g;
            }
        }
        return 3;
    };
    auto character_at = [&](uint32_t matrix) -> int {
        if (!Walk::phys_ok(matrix) || matrix == 0) {
            return -1;
        }
        const float mx = Walk::fixed(rdram, matrix, 3, 0), my = Walk::fixed(rdram, matrix, 3, 1), mz = Walk::fixed(rdram, matrix, 3, 2);
        int best = -1;
        float best_d = 250.0f * 250.0f;
        for (int k = 0; k < 5; k++) {
            const gpr ch = (gpr)(int32_t)(characters + (uint32_t)k * character_size);
            auto f = [&](uint32_t off) { uint32_t bits = (uint32_t)MEM_W(off, ch); float v; std::memcpy(&v, &bits, 4); return v; };
            const float dx = f(0x14) - mx, dy = f(0x18) - my, dz = f(0x1C) - mz;
            const float d = dx * dx + dy * dy + dz * dz;
            if (d < best_d) {
                best_d = d;
                best = k;
            }
        }
        return best;
    };
    std::vector<int> owner(w.all.size(), -1);
    for (size_t i = 0; i < w.all.size(); i++) {
        owner[i] = character_at(w.all[i].matrix);
        if (owner[i] < 0) {
            continue;
        }
        const int held = weapon_of(owner[i]);
        for (uint32_t a : w.all[i].vertices) {
            Holders& h = vertex_holders[a];
            h.n[held]++;
            if (h.n[held] > 100000u) {
                for (uint32_t& c : h.n) {
                    c /= 2u;
                }
            }
        }
    }
    uint32_t shared_n[3] = {}, total_n[3] = {}, own_n[3] = {};
    for (size_t i = 0; i < w.all.size(); i++) {
        if (owner[i] < 0) {
            continue;
        }
        const int g = weapon_of(owner[i]);
        if (g > 2) {
            continue;
        }
        for (uint32_t a : w.all[i].vertices) {
            const Holders& h = vertex_holders[a];
            total_n[g]++;
            shared_n[g] += shared_by_others(h, g) ? 1u : 0u;
            own_n[g] += weapons_own(h, g) ? 1u : 0u;
        }
    }
    for (int g = 0; g < 3; g++) {
        if (total_n[g] >= 8 && shared_n[g] * 10u >= total_n[g] * 3u && own_n[g] > 0) {
            vertex_baseline[g] = true;
        }
    }
    // (possess132) four slots of 16 KB, one a frame (possess129-131's two halves: the revolver flashed in the hand, RT64 still drawing a
    // frame from the half being written again)
    static uint32_t pool_half = 0;
    pool_half = (pool_half + 1u) & 3u;
    Clone clone{ rdram, 0x80F90000u + pool_half * 0x4000u, 0x80F90000u + pool_half * 0x4000u + 0x4000u, 0 };
    // (possess132) this machine's own character: its camera's (the one whose +0x23D is this machine's player), its +0x3D0. possess131 took
    // the player's number for the character's: player 1's machine (camera 1) left none out, the host's the wrong one
    int local_character = -1;
    {
        const int me = conker::netsession::active() ? conker::netsession::local_player() : 0;
        const uint32_t looks = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800DBFF0);
        for (uint32_t i = 0; kseg0(looks) && i < 4; i++) {
            const gpr camera = (gpr)(int32_t)(looks + i * 0x9A0u);
            const uint32_t ch = (uint32_t)MEM_W(0x3D0, camera);
            if ((int)MEM_BU(0x23D, camera) == me && ch >= characters && (ch - characters) % character_size == 0 && (ch - characters) / character_size < 25) {
                local_character = (int)((ch - characters) / character_size);
            }
        }
    }
    std::string made;
    static bool listed[3] = {};
    for (size_t i = 0; i < w.all.size(); i++) {
        const int k = owner[i];
        const int g = k >= 0 ? gold_held(rdram, k) : -1;
        if (g < 0) {
            continue;
        }
        const gpr aim = (gpr)(int32_t)MEM_W(0x31C, (gpr)(int32_t)(characters + (uint32_t)k * character_size));
        const bool scoped = kseg0((uint32_t)aim) && MEM_BU(0x197, aim) != 0;
        uint32_t seg[16];
        std::memcpy(seg, w.all[i].seg, sizeof(seg));
        clone.g = g;
        clone.in_part = false;
        clone.last_weapon = false;
        clone.by_vertices = vertex_baseline[g];
        clone.whole = false;
        if (clone.by_vertices && !w.all[i].vertices.empty()) {
            bool all_own = true;
            for (uint32_t a : w.all[i].vertices) {
                auto it = vertex_holders.find(a);
                all_own = all_own && it != vertex_holders.end() && weapons_own(it->second, g);
            }
            clone.whole = all_own;
        }
        {
            auto known = whole_lists.find(w.all[i].to);
            if (clone.whole) {
                whole_lists[w.all[i].to] = g;
            } else if (known != whole_lists.end() && known->second == g) {
                clone.whole = true;
            }
        }
        clone.hide_body = scoped && k == local_character;
        const uint32_t before_vertices = clone.vertices, before_hidden = clone.hidden, before_hidden_v = clone.hidden_vertices;
        const uint32_t before_combiners = clone.combiners;
        uint32_t copy = clone.list(w.all[i].to, seg, 0);
        // (possess136) a weapon's list with no colour combiner of its own (the sniper rifle's in the hand: the possess135 test's 0 combiners)
        // draws by the one set before it, the texture times the vertices' colours, so its gold came out dark: called through a list of ours,
        // the combiner before it made shiny gold, the copy, and that combiner put back for what follows
        if (copy != 0 && clone.whole && clone.combiners == before_combiners && (w.all[i].fc0 >> 24) == 0xFC) {
            const uint32_t mini = clone.take(32);
            if (mini != 0) {
                const gpr m = (gpr)(int32_t)mini;
                MEM_W(0, m) = (int32_t)w.all[i].fc0;
                MEM_W(4, m) = (int32_t)w.all[i].fc1;
                const bool shiny = gold_combiner(rdram, mini);
                MEM_W(8, m) = (int32_t)0xDE000000u;
                MEM_W(12, m) = (int32_t)copy;
                MEM_W(16, m) = (int32_t)w.all[i].fc0;
                MEM_W(20, m) = (int32_t)w.all[i].fc1;
                MEM_W(24, m) = (int32_t)0xDF000000u;
                MEM_W(28, m) = 0;
                if (shiny) {
                    clone.combiners++;
                    copy = mini & 0x1FFFFFFFu;
                }
            }
        }
        if (copy != 0) {
            MEM_W(4, (gpr)(int32_t)(w.all[i].call | 0x80000000u)) = (int32_t)copy;
        }
        if (tell && made.size() < 1500) {
            char piece[240];
            std::snprintf(piece, sizeof(piece), " | character %d's %s (the call at %06X of the list at %06X, %u loads): %s by its %s, %u vertices gold%s", k,
                golds[g].name, (unsigned)w.all[i].call, (unsigned)w.all[i].to, (unsigned)w.all[i].vertices.size(),
                copy != 0 ? "a gold copy drawn" : (clone.full ? "no room in ours" : "nothing in it made gold"), clone.whole ? "whole list" : (clone.by_vertices ? "vertices" : "textures"),
                (unsigned)(clone.vertices - before_vertices), clone.hide_body ? (clone.hidden != before_hidden || clone.hidden_vertices != before_hidden_v ? ", the body left out (scoped)" : ", scoped, nothing left out") : "");
            made += piece;
        }
        if (tell && !listed[g]) {
            listed[g] = true;
            say("[gold] [hand] the list character %d holds the %s with (at %06X):%s", k, golds[g].name, (unsigned)w.all[i].to, w.all[i].listing.c_str());
        }
    }
    if (tell) {
        say("[gold] [hand] f=%u the weapons' parts by their vertices: the sniper rifle %s (%u of %u loads its own, %u shared), the revolver %s (%u/%u, %u), "
            "the knives %s (%u/%u, %u); %u sources known", (unsigned)hand_frames, vertex_baseline[0] ? "found" : "not yet", (unsigned)own_n[0],
            (unsigned)total_n[0], (unsigned)shared_n[0], vertex_baseline[1] ? "found" : "not yet", (unsigned)own_n[1], (unsigned)total_n[1],
            (unsigned)shared_n[1], vertex_baseline[2] ? "found" : "not yet", (unsigned)own_n[2], (unsigned)total_n[2], (unsigned)shared_n[2],
            (unsigned)vertex_holders.size());
    }
    if (!tell) {
        return;
    }
    if (!made.empty()) {
        say("[gold] [hand] f=%u the weapons in the hands:%s (ours used: %u bytes, %u lists, %u combiners, %u triangles and %u body vertices left out)", (unsigned)hand_frames,
            made.c_str(), (unsigned)(clone.at - (0x80F90000u + pool_half * 0x4000u)), (unsigned)clone.lists, (unsigned)clone.combiners, (unsigned)clone.hidden, (unsigned)clone.hidden_vertices);
    }
    // who holds a gold weapon, and who holds which pickup
    std::string held;
    if (kseg0(block)) {
        for (int k = 0; k < 5; k++) {
            const int32_t p = MEM_W(0xEB0 + 4 * (uint32_t)k, (gpr)(int32_t)block);
            const gpr aim = (gpr)(int32_t)MEM_W(0x31C, (gpr)(int32_t)(characters + (uint32_t)k * character_size));
            char piece[64];
            std::snprintf(piece, sizeof(piece), " %d:%d(%02X%s)", k, (int)p, kseg0((uint32_t)aim) ? (unsigned)MEM_BU(0x78, aim) : 0u,
                gold_held(rdram, k) >= 0 ? " gold" : "");
            held += piece;
        }
    }
    hand_told++;
    say("[gold] [hand] f=%u the frame's list walked (%u commands): the characters' pickups (weapon out):%s", (unsigned)hand_frames, (unsigned)w.steps,
        held.c_str());
    for (int g = 0; g < 3; g++) {
        say("[gold] [hand]   the %s's model: called %u times (the game's), %u (our gold); its vertices loaded %u (the game's), %u (ours)%s", golds[g].name,
            (unsigned)w.calls[g][0], (unsigned)w.calls[g][1], (unsigned)w.loads[g][0], (unsigned)w.loads[g][1], w.samples[g].c_str());
        std::string tex;
        for (uint32_t t : textures[g]) {
            char piece[16];
            std::snprintf(piece, sizeof(piece), " %06X", (unsigned)t);
            tex += piece;
        }
        say("[gold] [hand]   the %s's textures:%s; loaded outside its model %u times%s", golds[g].name, tex.empty() ? " (none seen yet)" : tex.c_str(),
            (unsigned)w.elsewhere_n[g], w.elsewhere[g].c_str());
    }
    // where each of the first 5 characters is, for the matrices above
    std::string where;
    for (int k = 0; k < 5; k++) {
        const gpr ch = (gpr)(int32_t)(characters + (uint32_t)k * character_size);
        char piece[64];
        auto f = [&](uint32_t off) { uint32_t b = (uint32_t)MEM_W(off, ch); float v; std::memcpy(&v, &b, 4); return v; };
        std::snprintf(piece, sizeof(piece), " %d at %.0f %.0f %.0f;", k, f(0x14), f(0x18), f(0x1C));
        where += piece;
    }
    say("[gold] [hand]   the characters:%s", where.c_str());
}


// ---- (possess136) A gold weapon's own zoom: the 5th player's ----
// Scoped with the 5th player's sniper rifle, pushing the stick up and down doesn't zoom his view, it only changes how fast the stick turns
// the aim (the possess135 test's p5.mp4), which is what the gold weapons are to do. func_1510B128(view, the zoom across, the zoom up and
// down, ...) puts a view's field of view (its record's +0x74 and +0x78, D_800BE628's 0x180 a view) at its unzoomed one (+0x6C, +0x70)
// with the zoom, then works its frustum and projection out from it; the aim's turning (the game's, and the mouse's here) goes by that
// field of view. The 5th player's zoom goes into his own view's record, the extra camera's, but his view is drawn by view 1's: never zoomed.
// For a gold weapon's holder, scoped, the view is made with no zoom (its frustum and projection), and its field of view then put back
// as zoomed for what goes by it. Every view on every machine alike, from the game's memory.
namespace {
    struct FovKeep { bool on = false; uint32_t view = 0, across = 0, up = 0; };
    FovKeep fov_keep;
    uint32_t fov_told = 0;
    // (possess144) each view's zoom as the game last gave it (func_1510B128's $a1, degrees across from its unzoomed field of view), for the
    // gold knives' zoom bar
    float zoom_given[5] = {};

    // the character a view is drawn for (its camera's, the camera's +0x23D the view), scoped with a gold weapon: the weapon, or -1
    int gold_scoped_view(uint8_t* rdram, uint32_t view) {
        if (!golds_made || (int32_t)MEM_W(0, (gpr)(int32_t)scene_now) != total_war || view > 3) {
            return -1;
        }
        const uint32_t looks = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800DBFF0);
        for (uint32_t i = 0; kseg0(looks) && i < 4; i++) {
            const gpr camera = (gpr)(int32_t)(looks + i * 0x9A0u);
            if ((uint32_t)MEM_BU(0x23D, camera) != view) {
                continue;
            }
            const uint32_t ch = (uint32_t)MEM_W(0x3D0, camera);
            if (ch < characters || (ch - characters) % character_size != 0 || (ch - characters) / character_size >= 25) {
                return -1;
            }
            const gpr aim = (gpr)(int32_t)MEM_W(0x31C, (gpr)(int32_t)ch);
            if (!kseg0((uint32_t)aim) || MEM_BU(0x197, aim) == 0) {
                return -1;
            }
            return gold_held(rdram, (int)((ch - characters) / character_size));
        }
        return -1;
    }
}

// func_1510B128's start ($a0 the view, $a1 and $a2 the zoom, floats)
extern "C" void conker_gold_fov_start(uint8_t* rdram, recomp_context* ctx) {
    fov_keep.on = false;
    const uint32_t view = (uint32_t)ctx->r4;
    if (view < 5) {
        const uint32_t bits = (uint32_t)ctx->r5;
        float across;
        std::memcpy(&across, &bits, 4);
        zoom_given[view] = std::isfinite(across) ? std::fabs(across) : 0.0f;
    }
    if (gold_scoped_view(rdram, view) < 0 || ((uint32_t)ctx->r5 == 0 && (uint32_t)ctx->r6 == 0)) {
        return;
    }
    fov_keep = FovKeep{ true, view, (uint32_t)ctx->r5, (uint32_t)ctx->r6 };
    ctx->r5 = 0;
    ctx->r6 = 0;
}

// func_1510B128 at 0x1510B314 (its projection made, about to return)
extern "C" void conker_gold_fov_end(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    if (!fov_keep.on) {
        return;
    }
    fov_keep.on = false;
    const uint32_t records = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800BE628);
    if (!kseg0(records)) {
        return;
    }
    const gpr record = (gpr)(int32_t)(records + fov_keep.view * 0x180u);
    auto f = [&](uint32_t bits) { float v; std::memcpy(&v, &bits, 4); return v; };
    auto get = [&](gpr at, uint32_t off) { return f((uint32_t)MEM_W(off, at)); };
    auto put = [&](gpr at, uint32_t off, float v) { uint32_t b; std::memcpy(&b, &v, 4); MEM_W(off, at) = (int32_t)b; };
    const float extra = get((gpr)(int32_t)0x800C3648, 0);
    const float across = get(record, 0x6C) + f(fov_keep.across) + extra;
    const float up = get(record, 0x70) + f(fov_keep.up) + extra;
    if (fov_told < 10) {
        fov_told++;
        say("[gold] view %u scoped with a gold weapon: drawn unzoomed (%.1f), its field of view kept zoomed for the aim (%.1f)", (unsigned)fov_keep.view,
            get(record, 0x74), across);
    }
    put(record, 0x74, across);
    put(record, 0x78, up);
}

// ---- (possess136, possess137) No reticle over a gold weapon's scope, its zoom bar kept ----
// What's drawn over a view for it alone (func_151674F8's objects of layer 4, $a1 4, $a2 the view, $a3 the part: $a0 the list it adds to,
// $v0 its end as it returns, 0x15167A38) has the scope's HUD among it: the reticle (its circle and cross, at the view's middle) and the
// zoom bar beside it, both from the reticle's object (possess136 emptied that object's types: both went, the possess136 test). Now it's
// drawn, and of what it added for a gold weapon's scoped holder's own view, the rectangles around the view's middle (within a quarter of
// its height: the reticle; the bar sits a third of it off to the side) are taken out (G_NOOPs). [gold] lines of what was there.
namespace {
    struct ReticleKeep { bool on = false; uint32_t view = 0, start = 0, part = 0; int character = -1, gold = -1; int32_t kind = -1; };
    ReticleKeep reticle_keep;
    uint32_t reticle_told = 0;

    // (possess143) the character a view's HUD is drawn for, scoped (any weapon): its index, or -1. Views 0 to 3, and the 5th player's (4:
    // possess_test.cpp's fifth_hud_pass draws his reticle's objects with view 4, the 5th look camera's +0x23D). Not view 1 on the 5th
    // player's machine: his view is drawn in its part there, and player 1's isn't seen.
    int scoped_hud_character(uint8_t* rdram, uint32_t view) {
        if (!golds_made || (int32_t)MEM_W(0, (gpr)(int32_t)scene_now) != total_war) {
            return -1;
        }
        const bool fifth = conker::possess_test::online_extra_camera();
        if (view > 4 || (view == 4 && !fifth) || (view == 0 && conker::possess_test::fifth_split_view(rdram))) {
            return -1;
        }
        const uint32_t looks = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800DBFF0);
        for (uint32_t i = 0; kseg0(looks) && i < (fifth ? 5u : 4u); i++) {
            const gpr camera = (gpr)(int32_t)(looks + i * 0x9A0u);
            if ((uint32_t)MEM_BU(0x23D, camera) != view) {
                continue;
            }
            const uint32_t ch = (uint32_t)MEM_W(0x3D0, camera);
            if (ch < characters || (ch - characters) % character_size != 0 || (ch - characters) / character_size >= 25) {
                return -1;
            }
            const gpr aim = (gpr)(int32_t)MEM_W(0x31C, (gpr)(int32_t)ch);
            if (!kseg0((uint32_t)aim) || MEM_BU(0x197, aim) == 0) {
                return -1;
            }
            return (int)((ch - characters) / character_size);
        }
        return -1;
    }

    // (possess143) The zoom bar for the gold knives' scope, which has none of its own: the bar (and its marker) as the scope of a sniper
    // rifle or a revolver draws it (gold or not, any view's: every machine draws the 4 views), kept for each part (func_151674F8's $a3):
    // that part's commands that set the RDP's state (in order, so the state at each rectangle is as it was), the reticle's object's own
    // setup and end lists (DE D_8008AC70, D_8008AC98), and the bar's rectangles, placed as the gold weapons' are (at the view's left edge)
    // from the view's top left. Until one's been seen, a bar of our own: a green see-through rectangle where the gold weapons' sits.
    struct BarCopy {
        bool have = false;
        std::vector<uint32_t> words;     // pairs
        std::vector<size_t> rects;       // where each rectangle's first word is in words
        std::vector<int32_t> places;     // 4 a rectangle: its two corners' x and y (quarter pixels) from the view's top left
        uint32_t from_view = 0;
        int from_character = -1;
        bool from_gold = false;
        int32_t first_high = 0;          // (possess144) its first rectangle's height (quarter pixels): the marker's, in part 1
    };
    BarCopy bar_copy[2];
    uint32_t bar_told = 0;
    // (possess144) the most each view's been zoomed with the gold knives (degrees; 10 to begin with): the marker's full height
    float knives_zoom_most[5] = { 10.0f, 10.0f, 10.0f, 10.0f, 10.0f };

    // the kind of the pickup character k holds (the match's pickups' +0x1174: the sniper rifle's 0x24, the revolver's 0x23), or -1
    int32_t held_kind(uint8_t* rdram, int k) {
        const uint32_t block = (uint32_t)MEM_W(0, (gpr)(int32_t)pickups_block);
        if (!kseg0(block) || k < 0 || k >= 25) {
            return -1;
        }
        const int32_t p = MEM_W(0xEB0 + 4 * (uint32_t)k, (gpr)(int32_t)block);
        const int32_t count = MEM_W(0xEA4, (gpr)(int32_t)block);
        if (p < 0 || p >= count || p >= pickups_most) {
            return -1;
        }
        return MEM_W(0x1174 + 4 * (uint32_t)p, (gpr)(int32_t)block);
    }

    bool state_command(uint32_t op) {
        switch (op) {
        case 0xE2: case 0xE3: case 0xE6: case 0xE7: case 0xE8: case 0xEF: case 0xF0: case 0xF2: case 0xF3: case 0xF4: case 0xF5:
        case 0xF7: case 0xF8: case 0xF9: case 0xFA: case 0xFB: case 0xFC: case 0xFD:
            return true;
        default:
            return false;
        }
    }

    uint32_t clamp_field(int32_t v) {
        return (uint32_t)std::min(std::max(v, 0), 0xFFF);
    }

    // the kept bar written at `at` for the view whose top left is left, top: the list's new end
    uint32_t put_bar(uint8_t* rdram, uint32_t at, const BarCopy& b, float left, float top) {
        const int32_t l4 = (int32_t)std::lround(left * 4.0f), t4 = (int32_t)std::lround(top * 4.0f);
        size_t r = 0;
        for (size_t i = 0; i + 1 < b.words.size(); i += 2) {
            uint32_t w0 = b.words[i], w1 = b.words[i + 1];
            if (r < b.rects.size() && b.rects[r] == i) {
                const int32_t* p = &b.places[r * 4];
                w0 = (w0 & 0xFF000000u) | (clamp_field(p[0] + l4) << 12) | clamp_field(p[1] + t4);
                w1 = (w1 & 0xFF000000u) | (clamp_field(p[2] + l4) << 12) | clamp_field(p[3] + t4);
                r++;
            }
            MEM_W(0, (gpr)(int32_t)at) = (int32_t)w0;
            MEM_W(4, (gpr)(int32_t)at) = (int32_t)w1;
            at += 8;
        }
        return at;
    }

    // a filled rectangle of our own (no scope's bar seen yet): 1-cycle, the primitive colour (G_CC_PRIMITIVE), see-through; its corners
    // in quarter pixels on the screen (whole pixels)
    uint32_t put_fill(uint8_t* rdram, uint32_t at, int32_t x0q, int32_t y0q, int32_t x1q, int32_t y1q, uint32_t rgba) {
        const uint32_t x0 = clamp_field(x0q) & ~3u, y0 = clamp_field(y0q) & ~3u, x1 = clamp_field(x1q) & ~3u, y1 = clamp_field(y1q) & ~3u;
        const uint32_t words[] = {
            0xE7000000u, 0,
            0xFCFFFFFFu, 0xFFFDF6FBu,
            0xEF000CFFu, 0x00504340u,
            0xFA000000u, rgba,
            0xF6000000u | (x1 << 12) | y1, (x0 << 12) | y0,
            0xE7000000u, 0,
        };
        for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i += 2) {
            MEM_W(0, (gpr)(int32_t)at) = (int32_t)words[i];
            MEM_W(4, (gpr)(int32_t)at) = (int32_t)words[i + 1];
            at += 8;
        }
        return at;
    }

    // our own bar's place (no scope's bar seen yet), quarter pixels from the view's top left: 4 pixels wide at its left edge, 6 in, over
    // its height as the gold weapons' sits
    void own_bar_place(float high, int32_t out[4]) {
        out[0] = 24;
        out[1] = (int32_t)std::lround(high * 0.10f * 4.0f);
        out[2] = 40;
        out[3] = (int32_t)std::lround(high * 0.38f * 4.0f);
    }
}

extern "C" void conker_gold_reticle_start(uint8_t* rdram, recomp_context* ctx) {
    reticle_keep.on = false;
    if ((uint32_t)ctx->r5 != 4 || (uint32_t)ctx->r7 > 1) {
        return;
    }
    const uint32_t view = (uint32_t)(int32_t)(int16_t)(uint16_t)(uint32_t)ctx->r6;
    const int character = scoped_hud_character(rdram, view);
    if (character < 0 || !kseg0((uint32_t)ctx->r4)) {
        return;
    }
    reticle_keep = ReticleKeep{ true, view, (uint32_t)ctx->r4, (uint32_t)ctx->r7, character, gold_held(rdram, character), held_kind(rdram, character) };
}

extern "C" void conker_gold_reticle_end(uint8_t* rdram, recomp_context* ctx) {
    if (!reticle_keep.on) {
        return;
    }
    reticle_keep.on = false;
    const uint32_t start = reticle_keep.start, end = (uint32_t)ctx->r2;
    const uint32_t records = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800BE628);
    if (!kseg0(end) || end <= start || end - start > 0x8000u || !kseg0(records)) {
        return;
    }
    const int g = reticle_keep.gold;
    const bool gold = g >= 0;
    const uint32_t part = reticle_keep.part & 1u;
    const gpr record = (gpr)(int32_t)(records + reticle_keep.view * 0x180u);
    auto f = [&](uint32_t off) { uint32_t b = (uint32_t)MEM_W(off, record); float v; std::memcpy(&v, &b, 4); return v; };
    const float top = f(0x24), bottom = f(0x28), left = f(0x2C), right = f(0x30);
    const float cx = (left + right) * 0.5f, cy = (top + bottom) * 0.5f;
    // (possess139) the zoom bar: a tall thin rectangle (3 times as tall as wide at least); kept, with whatever sits across it (its marker).
    // Every other rectangle of the scope's HUD taken out: possess138 took out those within a quarter of the view's height of its middle,
    // and the reticle's dots further out stayed (the possess138 test's screenshots).
    // (possess143) where it was found kept for each view (possess139's one for all: the views not gold are looked through now too)
    static float bars_l[5] = { -1.0f, -1.0f, -1.0f, -1.0f, -1.0f }, bars_r[5] = { -1.0f, -1.0f, -1.0f, -1.0f, -1.0f };
    float& bar_l = bars_l[reticle_keep.view];
    float& bar_r = bars_r[reticle_keep.view];
    bool bar_here = false;
    for (uint32_t c = start; c + 8 <= end; c += 8) {
        const uint32_t w0 = (uint32_t)MEM_W(0, (gpr)(int32_t)c), w1 = (uint32_t)MEM_W(4, (gpr)(int32_t)c);
        const uint32_t op = w0 >> 24;
        if (op != 0xE4 && op != 0xE5 && op != 0xF6) {
            continue;
        }
        const float xl = (float)((w0 >> 12) & 0xFFFu) * 0.25f, yl = (float)(w0 & 0xFFFu) * 0.25f;
        const float xh = (float)((w1 >> 12) & 0xFFFu) * 0.25f, yh = (float)(w1 & 0xFFFu) * 0.25f;
        const float wide = std::fabs(xl - xh), high = std::fabs(yl - yh);
        // (not the cross's own upright, at the view's middle: the bar sits well off to the side)
        if (high >= 3.0f * wide && high >= 8.0f && std::fabs((xl + xh) * 0.5f - cx) > (right - left) * 0.12f) {
            bar_l = std::min(xl, xh) - 2.0f;
            bar_r = std::max(xl, xh) + 2.0f;
            bar_here = true;
        }
    }
    // (possess143) a sniper rifle's or revolver's scope's bar copied for the gold knives' (the knives' own, gold or not, has none)
    const bool copy_it = (reticle_keep.kind == 0x24 || reticle_keep.kind == 0x23) && g != 2 && bar_l >= 0.0f && (bar_here || part == 1);
    BarCopy copy;
    copy.from_view = reticle_keep.view;
    copy.from_character = reticle_keep.character;
    copy.from_gold = gold;
    const int32_t l4 = (int32_t)std::lround(left * 4.0f), t4 = (int32_t)std::lround(top * 4.0f);
    uint32_t removed = 0, kept = 0;
    std::string ops;
    for (uint32_t c = start; c + 8 <= end; c += 8) {
        const gpr at = (gpr)(int32_t)c;
        const uint32_t w0 = (uint32_t)MEM_W(0, at), w1 = (uint32_t)MEM_W(4, at);
        const uint32_t op = w0 >> 24;
        if (gold && ops.size() < 3 * 60) {
            char piece[8];
            std::snprintf(piece, sizeof(piece), " %02X", (unsigned)op);
            ops += piece;
        }
        if (op != 0xE4 && op != 0xE5 && op != 0xF6) {
            if (copy_it && (state_command(op) || (op == 0xDE && ((uint32_t)w1 == 0x8008AC70u || (uint32_t)w1 == 0x8008AC98u)))) {
                copy.words.push_back(w0);
                copy.words.push_back(w1);
            }
            continue;
        }
        const uint32_t words = (op == 0xF6) ? 1u : 3u;   // (a texture rectangle's two G_RDPHALFs after it)
        const float xl = (float)((w0 >> 12) & 0xFFFu) * 0.25f;
        const float xh = (float)((w1 >> 12) & 0xFFFu) * 0.25f;
        const float rx = (xl + xh) * 0.5f;
        const bool on_bar = bar_l >= 0.0f && rx >= bar_l && rx <= bar_r;
        if (on_bar) {
            // (possess141) the zoom bar (and its marker) moved to the view's left edge, 6 pixels in
            const int32_t shift = (int32_t)std::lround((left + 6.0f - (bar_l + 2.0f)) * 4.0f);
            int32_t nxl = (int32_t)((w0 >> 12) & 0xFFFu), nxh = (int32_t)((w1 >> 12) & 0xFFFu);
            if (shift < 0 && nxl + shift >= 0 && nxh + shift >= 0) {
                nxl += shift;
                nxh += shift;
                if (gold) {
                    MEM_W(0, at) = (int32_t)((w0 & ~(0xFFFu << 12)) | ((uint32_t)nxl << 12));
                    MEM_W(4, at) = (int32_t)((w1 & ~(0xFFFu << 12)) | ((uint32_t)nxh << 12));
                }
            }
            if (copy_it && c + words * 8 <= end) {
                copy.rects.push_back(copy.words.size());
                copy.places.push_back(nxl - l4);
                copy.places.push_back((int32_t)(w0 & 0xFFFu) - t4);
                copy.places.push_back(nxh - l4);
                copy.places.push_back((int32_t)(w1 & 0xFFFu) - t4);
                for (uint32_t k = 0; k < words; k++) {
                    copy.words.push_back((uint32_t)MEM_W(k * 8, at));
                    copy.words.push_back((uint32_t)MEM_W(k * 8 + 4, at));
                }
            }
            kept++;
            c += (words - 1) * 8;
            continue;
        }
        if (!gold) {
            c += (words - 1) * 8;
            continue;
        }
        removed++;
        for (uint32_t k = 0; k < words && c + k * 8 + 8 <= end; k++) {
            MEM_W(k * 8, at) = 0;
            MEM_W(k * 8 + 4, at) = 0;
        }
        c += (words - 1) * 8;
    }
    // (a gold scope's kept over one not gold's: its marker as the gold weapons' own scope shows it. possess144: and of part 1's, the one
    // least zoomed, its marker the shortest: its colour the marker's unzoomed one, the gold knives' marker's height their own zoom's)
    if (!copy.rects.empty()) {
        copy.first_high = std::abs(copy.places[1] - copy.places[3]);
    }
    const BarCopy& had = bar_copy[part];
    const bool better = !had.have || (gold && !had.from_gold) || (gold == had.from_gold && (part == 0 || copy.first_high <= had.first_high));
    if (copy_it && !copy.rects.empty() && copy.words.size() <= 2u * 96u && better) {
        const bool first = !bar_copy[part].have;
        copy.have = true;
        bar_copy[part] = std::move(copy);
        if (first) {
            say("[gold] the zoom bar's part %u copied for the gold knives' scope, from view %u's (character %d's scope%s): %u commands, %u of "
                "them its rectangles", (unsigned)part, (unsigned)bar_copy[part].from_view, bar_copy[part].from_character, gold ? ", gold" : "",
                (unsigned)(bar_copy[part].words.size() / 2), (unsigned)bar_copy[part].rects.size());
        }
    }
    // (possess143) the gold knives' scope: the zoom bar put after what was drawn, as the gold sniper rifle's and revolver's show it
    // (possess144) and its marker (part 1's) as high as the knives are zoomed: from nothing unzoomed to the bar's whole height at the most
    // they've been zoomed in this view (10 degrees to begin with), across the bar's middle, as the sniper rifle's grows with its zoom
    uint32_t list_end = end;
    if (g == 2) {
        const uint32_t v = reticle_keep.view;
        // (possess145) the zoom from the view's look camera itself (+0x19C across, +0x1A0 up and down: what func_1510B128 is handed, and
        // what the sniper rifle's marker goes by, func_151CC1D4): the possess144 test's knives had func_1510B128's last zoom all through
        // (27.8, the sniper rifle's), the knives' own zoom never handed to it
        float cam_across = 0.0f, cam_up = 0.0f;
        bool cam_found = false;
        {
            const uint32_t looks = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800DBFF0);
            const uint32_t records = conker::possess_test::online_extra_camera() ? 5u : 4u;
            for (uint32_t i = 0; kseg0(looks) && i < records; i++) {
                const gpr camera = (gpr)(int32_t)(looks + i * 0x9A0u);
                if ((uint32_t)MEM_BU(0x23D, camera) != v) {
                    continue;
                }
                const uint32_t ab = (uint32_t)MEM_W(0x19C, camera), ub = (uint32_t)MEM_W(0x1A0, camera);
                std::memcpy(&cam_across, &ab, 4);
                std::memcpy(&cam_up, &ub, 4);
                cam_found = std::isfinite(cam_across) && std::isfinite(cam_up);
                break;
            }
        }
        const float zoom = cam_found ? std::fabs(cam_across) : zoom_given[v];
        knives_zoom_most[v] = std::max(knives_zoom_most[v], zoom);
        const float frac = std::min(1.0f, std::max(0.0f, zoom / knives_zoom_most[v]));
        int32_t bar[4];
        if (bar_copy[0].have && bar_copy[0].places.size() >= 4) {
            for (int i = 0; i < 4; i++) {
                bar[i] = bar_copy[0].places[(size_t)i];
            }
        } else {
            own_bar_place(bottom - top, bar);
        }
        const int32_t bar_top = std::min(bar[1], bar[3]), bar_bottom = std::max(bar[1], bar[3]);
        const int32_t middle = (bar_top + bar_bottom) / 2;
        const int32_t half = std::max(4, (int32_t)std::lround((float)((bar_bottom - bar_top) / 2 - 4) * frac));
        const bool have = bar_copy[part].have;
        if (part == 0) {
            if (have) {
                list_end = put_bar(rdram, end, bar_copy[0], left, top);
            } else {
                list_end = put_fill(rdram, end, l4 + bar[0], t4 + bar[1], l4 + bar[2], t4 + bar[3], 0x30C830A0u);
            }
        } else if (have && bar_copy[1].places.size() >= 4) {
            BarCopy marker = bar_copy[1];
            marker.places[1] = middle + half;
            marker.places[3] = middle - half;
            list_end = put_bar(rdram, end, marker, left, top);
        } else {
            const int32_t bx0 = std::min(bar[0], bar[2]), bx1 = std::max(bar[0], bar[2]);
            list_end = put_fill(rdram, end, l4 + bx0 + 4, t4 + middle - half, l4 + bx1 - 4, t4 + middle + half, 0x70FF70E0u);
        }
        ctx->r2 = (gpr)(int32_t)list_end;
        // (logged as the zoom changes, half a degree at a time)
        static int told_zoom[5] = { -1000, -1000, -1000, -1000, -1000 };
        const int zoom_step = (int)std::lround(zoom * 2.0f);
        if (part == 1 && bar_told < 40 && zoom_step != told_zoom[v]) {
            bar_told++;
            told_zoom[v] = zoom_step;
            say("[gold] view %u scoped with the gold knives: the zoom bar added at the view's left edge (%s); zoomed %.1f degrees (its look "
                "camera's %.1f %.1f%s, func_1510B128's last %.1f; the most %.1f), the marker %.0f%% of the bar", (unsigned)v,
                have ? "the copy of a scope's" : "our own, no scope's bar seen yet", zoom, cam_across, cam_up, cam_found ? "" : ", none found",
                zoom_given[v], knives_zoom_most[v], frac * 100.0f);
        }
    }
    if (gold && reticle_told < 12) {
        reticle_told++;
        say("[gold] view %u scoped with a gold weapon: of its scope's HUD (%u commands, the view's middle %.0f,%.0f), %u  rectangles taken "
            "out (the reticle), %u kept (the zoom bar, %.0f to %.0f across); its commands:%s", (unsigned)reticle_keep.view, (unsigned)((end - start) / 8), cx, cy,
            (unsigned)removed, (unsigned)kept, bar_l, bar_r, ops.c_str());
    }
}

// (possess137) func_1510B5F8(view, its matrix, its field of view up and down, across, ...) makes a view's projection; besides func_1510B128
// (conker_gold_fov_start), func_1510B51C calls it again as the frame is drawn (func_1501878C), from the view's field of view as it is (the
// zoomed one, put back for the aim): the possess136 test's views still zoomed. For a gold weapon's scoped holder's view, made unzoomed.
// (possess142) The 5th player's view is drawn with view 1's projection, never his own (possess136's notes): whatever his scope does to his
// own view's record, what's drawn has player 1's unscoped one. For a gold weapon's scoped holder the same: each view's projection as made
// while its field of view is its own unzoomed one exactly (its record's +0x74 and +0x78 at +0x6C and +0x70 with D_800C3648's), the
// fixed point one at +0x100 (a frame's buffer each, D_800BE9C0) and the floats at +0xBC, is kept; and whenever one is made for the view while
// its character is scoped with a gold weapon, that kept one is put over it as func_1510B5F8 returns (0x1510B680, after guMtxF2L). possess136
// to 141 changed the field of view handed to it instead, and the view still narrowed (its projection 1.80 to 1.89 across against 1.71,
// the possess138 to 141 tests). [gold] lines of what was made, by whom and what was put over it.
namespace {
    struct KeptProjection { bool have = false; uint8_t fixed[0x40] = {}, floats[0x40] = {}; float across = 0, up = 0; };
    KeptProjection kept_projection[4];
    uint32_t projection_caller[4] = {};
    uint32_t projection_told = 0;
    float record_float(gpr record, uint8_t* rdram, uint32_t off) {
        uint32_t b = (uint32_t)MEM_W(off, record);
        float v;
        std::memcpy(&v, &b, 4);
        return v;
    }
    float fixed_element(uint8_t* rdram, uint32_t m, int i) {   // (row 0 column 0: i 0; row 1 column 1: i 5)
        const gpr at = (gpr)(int32_t)m;
        const int32_t whole = MEM_H((uint32_t)(i * 2), at);
        const uint32_t part = (uint16_t)MEM_HU((uint32_t)(32 + i * 2), at);
        return (float)whole + (float)part / 65536.0f;
    }
}

// func_1510B5F8's start: who asked (for the [gold] lines)
extern "C" void conker_gold_projection(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    const uint32_t view = (uint32_t)ctx->r4;
    if (view < 4) {
        projection_caller[view] = (uint32_t)ctx->r31;
    }
}

// func_1510B5F8 at 0x1510B680: the view (its stack's +0x30) has its projection made
extern "C" void conker_gold_projection_made(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t view = (uint32_t)MEM_W(0x30, ctx->r29);
    const uint32_t records = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800BE628);
    if (view > 3 || !kseg0(records) || !golds_made || (int32_t)MEM_W(0, (gpr)(int32_t)scene_now) != total_war) {
        return;
    }
    const uint32_t record = records + view * 0x180u;
    const gpr r = (gpr)(int32_t)record;
    const uint32_t fixed = record + 0x100u + (uint32_t)(MEM_BU(0, (gpr)(int32_t)0x800BE9C0) & 1u) * 0x40u;
    const uint32_t floats = record + 0xBCu;
    const float extra = record_float((gpr)(int32_t)0x800C3648, rdram, 0);
    const float across = record_float(r, rdram, 0x74), up = record_float(r, rdram, 0x78);
    const bool unzoomed = std::fabs(across - (record_float(r, rdram, 0x6C) + extra)) < 0.01f && std::fabs(up - (record_float(r, rdram, 0x70) + extra)) < 0.01f;
    KeptProjection& kp = kept_projection[view];
    if (gold_scoped_view(rdram, view) < 0) {
        if (unzoomed) {
            std::memcpy(kp.fixed, rdram + (fixed - 0x80000000u), 0x40);
            std::memcpy(kp.floats, rdram + (floats - 0x80000000u), 0x40);
            kp.across = across;
            kp.up = up;
            kp.have = true;
        }
        return;
    }
    if (!kp.have) {
        return;
    }
    const float made_x = fixed_element(rdram, fixed, 0), made_y = fixed_element(rdram, fixed, 5);
    std::memcpy(rdram + (fixed - 0x80000000u), kp.fixed, 0x40);
    std::memcpy(rdram + (floats - 0x80000000u), kp.floats, 0x40);
    if (projection_told < 40) {
        projection_told++;
        say("[gold] view %u scoped with a gold weapon: its projection (made by %08X at %.1f by %.1f degrees, its scale %.3f %.3f) put back to its "
            "unscoped one (%.1f by %.1f, %.3f %.3f)", (unsigned)view, (unsigned)projection_caller[view], across, up, made_x, made_y, kp.across, kp.up,
            fixed_element(rdram, fixed, 0), fixed_element(rdram, fixed, 5));
    }
}


// ---- (possess142) A gold weapon's laser seen by its holder, as the 5th player sees his ----
// func_151D75C4 decides whether what's attached to a character is drawn (its laser among it): not while the character is scoped (its
// +0x31C's +0x197) and has a camera of its own (+0x318). The 5th player's character has none (its camera is the extra one, the game
// doesn't know it), so his laser shows as he aims through the scope, the players' never. For a gold weapon's holder the scope is read as
// clear there (0x151D7630, $t3 the +0x197, $v0 the character): their laser shows as his does.
extern "C" void conker_gold_laser(uint8_t* rdram, recomp_context* ctx) {
    if (!golds_made || (int32_t)MEM_W(0, (gpr)(int32_t)scene_now) != total_war) {
        return;
    }
    const uint32_t c = (uint32_t)ctx->r2;
    if (c < characters || (c - characters) % character_size != 0 || (c - characters) / character_size >= 25) {
        return;
    }
    if (gold_held(rdram, (int)((c - characters) / character_size)) >= 0) {
        ctx->r11 = 0;
    }
}

// ---- (possess143) A gold weapon's laser beam seen by its holder through the scope, as the 5th player sees his ----
// possess142's func_151D75C4 (what's attached to a character) wasn't the beam: the possess142 test's gold sniper rifle scoped, the fire
// button held, showed the laser's dot alone. The laser sight (func_15101378, each frame: from the gun's bone, +0x110's +0x20, to where the
// aim's ray ends, +0x31C's +0xB8, or 2000 along the aim) has its beam in an object of its own (its +0x30 there), whose check for each view
// it's drawn in (func_151028AC, $a1 the view) leaves it out of its holder's own view (the laser's +0x132) while he's scoped (+0x197). The
// 5th player's laser is his extra camera's view's, 4, never one of the 4 views drawn, so his beam is in his view all along. For a gold
// weapon's holder the scope is read as clear there (0x151028E0, $v1 the holder, $t0 his +0x197): his beam drawn in his own view too.
extern "C" void conker_gold_beam(uint8_t* rdram, recomp_context* ctx) {
    if (!golds_made || (int32_t)MEM_W(0, (gpr)(int32_t)scene_now) != total_war || (uint32_t)ctx->r8 == 0) {
        return;
    }
    const uint32_t c = (uint32_t)ctx->r3;
    if (c < characters || (c - characters) % character_size != 0 || (c - characters) / character_size >= 25) {
        return;
    }
    const int k = (int)((c - characters) / character_size);
    const int g = gold_held(rdram, k);
    if (g < 0) {
        return;
    }
    ctx->r8 = 0;
    static int told_for[25];
    static bool told_set = false;
    static uint32_t told = 0;
    if (!told_set) {
        told_set = true;
        for (int& v : told_for) {
            v = -1;
        }
    }
    if (told_for[k] != g && told < 20) {
        told++;
        told_for[k] = g;
        say("[gold] character %d's laser beam (the %s's) drawn in its own view, scoped (view %d)", k, golds[g].name,
            (int)(int16_t)(uint16_t)(uint32_t)ctx->r5);
    }
}

// ---- (possess144) The beam itself, from the gun to where it points ----
// The possess143 test: the beam's check passed for its holder's own view, and still only the dot showed. func_151408A4 (the beam's
// drawing for a view, $a0 the beam, $a1 the view) doesn't draw a beam along its length at all: it draws a glow facing the camera (a
// quad, its 4 vertices worked out each frame), halfway from the gun to the view's camera (func_15144B34: the look camera of the view's
// index, its +0x2F8), as big and bright as the laser points at that camera (the beam's direction against the way to the camera), and
// none when it points away from it (0x15140BF0: $f0 0, left out unless the beam's +0x58 has 0x10000). The 5th player's view is drawn as
// view 1, so his glow faces player 1's camera, wherever that is, and is seen from the side, stretched out from his gun: the green beam
// of the possess142 test. For a gold weapon's scoped holder's own view (the laser's +0x132 the view): drawn (+0x58's 0x10000 for the
// call), and its 4 vertices put along the laser instead, a strip from the gun (the beam's +0x34) to where the laser ends (the laser
// sight's +0x40: the aim's ray's end), turned to face the view's eye, the texture's middle along its length (its edges soft across it),
// its alpha (+0x5C) set. [gold] lines of each one drawn, or left out.
namespace {
    struct BeamKeep { bool on = false; uint32_t beam = 0, laser = 0, flags = 0; int view = -1, character = -1, stage = 0; bool moved = false; };
    BeamKeep beam_keep;
    uint32_t beam_told = 0, beam_left_told = 0;
    constexpr float beam_half_width = 1.5f;
    constexpr uint32_t beam_alpha = 0x80;

    float float_at(uint8_t* rdram, uint32_t at) {
        const uint32_t b = (uint32_t)MEM_W(0, (gpr)(int32_t)at);
        float v;
        std::memcpy(&v, &b, 4);
        return v;
    }
    int16_t short_of(float v) {
        return (int16_t)std::lround(std::min(32767.0f, std::max(-32768.0f, v)));
    }
}

// func_151408A4's start
extern "C" void conker_gold_beam_start(uint8_t* rdram, recomp_context* ctx) {
    beam_keep.on = false;
    if (!golds_made || (int32_t)MEM_W(0, (gpr)(int32_t)scene_now) != total_war) {
        return;
    }
    const uint32_t beam = (uint32_t)ctx->r4;
    const int view = (int)(int16_t)(uint16_t)(uint32_t)ctx->r5;
    if (!kseg0(beam) || view < 0 || view > 3) {
        return;
    }
    const uint32_t laser = (uint32_t)MEM_W(0x170, (gpr)(int32_t)beam);
    if (!kseg0(laser) || (int)MEM_BU(0x132, (gpr)(int32_t)laser) != view) {
        return;
    }
    const uint32_t c = (uint32_t)MEM_W(0x110, (gpr)(int32_t)laser);
    if (c < characters || (c - characters) % character_size != 0 || (c - characters) / character_size >= 25) {
        return;
    }
    const int k = (int)((c - characters) / character_size);
    const gpr aim = (gpr)(int32_t)MEM_W(0x31C, (gpr)(int32_t)c);
    if (!kseg0((uint32_t)aim) || MEM_BU(0x197, aim) == 0 || gold_held(rdram, k) < 0) {
        return;
    }
    const uint32_t flags = (uint32_t)MEM_W(0x58, (gpr)(int32_t)beam);
    beam_keep = BeamKeep{ true, beam, laser, flags, view, k, 0, false };
    MEM_W(0x58, (gpr)(int32_t)beam) = (int32_t)(flags | 0x10000u);
}

// (possess145) func_151408A4 at 0x15140954, func_15144CEC returned ($v0: the beam's start in front of the view's eye; +0x98, +0x9C of
// $sp its place on the screen): the possess144 test's beam was left out every frame (its start, at the gun, close by the eye). For a gold
// weapon's holder's own view, its start taken as on the screen (the view's middle), so it goes on to be drawn: what's drawn is put along
// the laser as it returns.
extern "C" void conker_gold_beam_placed(uint8_t* rdram, recomp_context* ctx) {
    if (!beam_keep.on || (uint32_t)ctx->r16 != beam_keep.beam) {
        return;
    }
    beam_keep.stage = 1;
    const uint32_t records = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800BE628);
    if (!kseg0(records)) {
        return;
    }
    const uint32_t record = records + (uint32_t)beam_keep.view * 0x180u;
    const float top = float_at(rdram, record + 0x24), bottom = float_at(rdram, record + 0x28), left = float_at(rdram, record + 0x2C),
        right = float_at(rdram, record + 0x30);
    const uint32_t sp = (uint32_t)ctx->r29;
    const float x = float_at(rdram, sp + 0x98), y = float_at(rdram, sp + 0x9C);
    const bool on_screen = (uint32_t)ctx->r2 != 0 && std::isfinite(x) && std::isfinite(y) && x >= left && x <= right && y >= top && y <= bottom;
    if (on_screen) {
        return;
    }
    auto put = [&](uint32_t at, float v) { uint32_t b; std::memcpy(&b, &v, 4); MEM_W(0, (gpr)(int32_t)at) = (int32_t)b; };
    if ((uint32_t)ctx->r2 == 0) {
        put(sp + 0x8C, 1.0f);
        put(sp + 0x94, 0.0f);
    }
    put(sp + 0x98, (left + right) * 0.5f);
    put(sp + 0x9C, (top + bottom) * 0.5f);
    ctx->r2 = 1;
    beam_keep.moved = true;
}

// (possess145) func_151408A4 at 0x15140A0C: func_151D5D60 has given the view its vertices (+0x114 of $sp, 0 if none)
extern "C" void conker_gold_beam_listed(uint8_t* rdram, recomp_context* ctx) {
    if (!beam_keep.on || (uint32_t)ctx->r16 != beam_keep.beam) {
        return;
    }
    beam_keep.stage = MEM_W(0x114, ctx->r29) != 0 ? 3 : 2;
}

// func_151408A4 at 0x15141194, returning ($v0 its list for the view, 0 if left out; $s0 the beam, $sp its frame: +0x114 its 4th vertex)
extern "C" void conker_gold_beam_end(uint8_t* rdram, recomp_context* ctx) {
    if (!beam_keep.on) {
        return;
    }
    beam_keep.on = false;
    const uint32_t beam = beam_keep.beam;
    if ((uint32_t)ctx->r16 != beam) {
        return;
    }
    const gpr b = (gpr)(int32_t)beam;
    MEM_W(0x58, b) = (int32_t)(((uint32_t)MEM_W(0x58, b) & ~0x10000u) | (beam_keep.flags & 0x10000u));
    if ((uint32_t)ctx->r2 == 0) {
        if (beam_left_told < 10) {
            beam_left_told++;
            static const char* const stages[] = { "the beam's +0x58 without its 2 (before its start was placed)", "its start placed, then left out before its vertices",
                "func_151D5D60 gave the view no vertices", "after its vertices (too far from the eye, or pointing away)" };
            say("[gold] character %d's laser beam left out of its own view %d: %s%s; its +0x58 %08X", beam_keep.character, beam_keep.view,
                stages[std::min(std::max(beam_keep.stage, 0), 3)], beam_keep.moved ? " (its start taken as on the screen)" : "",
                (unsigned)beam_keep.flags);
        }
        return;
    }
    const uint32_t vertices = (uint32_t)ctx->r2;   // (+0x110 of $sp: the vertices' start, +0x114 having gone on to the 4th)
    const uint32_t looks = (uint32_t)MEM_W(0, (gpr)(int32_t)0x800DBFF0);
    if (!kseg0(vertices) || !kseg0(looks)) {
        return;
    }
    const uint32_t eye_at = looks + (uint32_t)beam_keep.view * 0x9A0u + 0x2F8u;
    float s[3], e[3], o[3];
    for (int i = 0; i < 3; i++) {
        s[i] = float_at(rdram, beam + 0x34u + 4u * (uint32_t)i);
        e[i] = float_at(rdram, beam_keep.laser + 0x40u + 4u * (uint32_t)i);
        o[i] = float_at(rdram, eye_at + 4u * (uint32_t)i);
    }
    float u[3] = { e[0] - s[0], e[1] - s[1], e[2] - s[2] };
    const float length = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    if (!std::isfinite(length) || length < 1.0f) {
        return;
    }
    for (float& x : u) {
        x /= length;
    }
    const float n[3] = { o[0] - s[0], o[1] - s[1], o[2] - s[2] };
    // (n x u: the strip's corners wound as the game's glow's are, its face away from the eye by their order, as the game's)
    float side[3] = { n[1] * u[2] - n[2] * u[1], n[2] * u[0] - n[0] * u[2], n[0] * u[1] - n[1] * u[0] };
    const float side_length = std::sqrt(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
    if (!std::isfinite(side_length) || side_length < 1e-3f) {
        return;
    }
    for (float& x : side) {
        x *= beam_half_width / side_length;
    }
    // the corners as the game's (+V+H, +V-H, -V-H, -V+H about the middle): the laser's end the +V side
    const float corner[4][3] = {
        { e[0] + side[0], e[1] + side[1], e[2] + side[2] },
        { e[0] - side[0], e[1] - side[1], e[2] - side[2] },
        { s[0] - side[0], s[1] - side[1], s[2] - side[2] },
        { s[0] + side[0], s[1] + side[1], s[2] + side[2] },
    };
    int32_t st[4][2];
    for (int i = 0; i < 4; i++) {
        const gpr v = (gpr)(int32_t)(vertices + 0x10u * (uint32_t)i);
        st[i][0] = MEM_H(8, v);
        st[i][1] = MEM_H(10, v);
    }
    const int32_t mid[2] = { (st[0][0] + st[1][0] + st[2][0] + st[3][0]) / 4, (st[0][1] + st[1][1] + st[2][1] + st[3][1]) / 4 };
    const int32_t across[2] = { (st[0][0] - st[1][0]) / 2, (st[0][1] - st[1][1]) / 2 };
    const int sign[4] = { 1, -1, -1, 1 };
    for (int i = 0; i < 4; i++) {
        const gpr v = (gpr)(int32_t)(vertices + 0x10u * (uint32_t)i);
        MEM_H(0, v) = short_of(corner[i][0]);
        MEM_H(2, v) = short_of(corner[i][1]);
        MEM_H(4, v) = short_of(corner[i][2]);
        MEM_H(8, v) = (int16_t)(mid[0] + sign[i] * across[0]);
        MEM_H(10, v) = (int16_t)(mid[1] + sign[i] * across[1]);
    }
    // (possess145: its alpha whole, not faded with what's in front of its start: the gun itself, there)
    const uint32_t fade = 0xFF;
    MEM_B(0x5C, b) = (int8_t)beam_alpha;
    if (beam_told < 12) {
        beam_told++;
        say("[gold] character %d's laser beam drawn in its own view %d along the laser: from %.0f %.0f %.0f to %.0f %.0f %.0f (%.0f long), "
            "the eye at %.0f %.0f %.0f; its texture %d,%d across %d,%d; alpha %u%s", beam_keep.character, beam_keep.view, s[0], s[1], s[2], e[0],
            e[1], e[2], length, o[0], o[1], o[2], (int)mid[0], (int)mid[1], (int)across[0], (int)across[1], (unsigned)((beam_alpha * fade) >> 8),
            beam_keep.moved ? " (its start taken as on the screen)" : "");
    }
}

namespace {
    bool kept_projection_have(uint32_t view) {
        return view < 4 && kept_projection[view].have;
    }
    void put_kept_projection(uint8_t* rdram, uint32_t view, uint32_t record) {
        const KeptProjection& kp = kept_projection[view];
        for (uint32_t buffer = 0; buffer < 2; buffer++) {
            std::memcpy(rdram + (record + 0x100u + buffer * 0x40u - 0x80000000u), kp.fixed, 0x40);
        }
        std::memcpy(rdram + (record + 0xBCu - 0x80000000u), kp.floats, 0x40);
    }
}
