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
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

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

    bool kseg0(uint32_t a) { return (a >> 24) == 0x80 && (a & 0x00FFFFFFu) < 0x00FF0000u; }

    void say(const char* format, ...) {
        char line[512];
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

    void gold_colours(uint8_t* rdram, uint32_t at, uint32_t count) {
        for (uint32_t v = 0; v < count; v++) {
            const gpr c = (gpr)(int32_t)(at + v * 0x10u + 0xCu);
            const uint32_t r = MEM_BU(0, c), gr = MEM_BU(1, c), b = MEM_BU(2, c);
            const uint32_t l = (r * 30u + gr * 59u + b * 11u) / 100u;
            // (possess127) brighter: the Rare logo's gold from a deep amber in the shadows to a pale yellow at the light, which the list's
            // colour combiner adds over the texture (gold_combiner): possess123's (60 + 0.9 l, 40 + 0.7 l, 0.15 l) times the dark texture
            // came out a dull olive (the possess125 test)
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
        gold_colours(rdram, ours, n);
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
    }
}

// ---- (possess127) Where a weapon in a hand is drawn: the frame's list walked ----
// A gold weapon on the ground draws from its own gold copy (gold_model); in a hand it's the game's own again (the possess125 test). How the
// hand's weapon is drawn isn't known yet: whether from the same model (its list called, its vertices loaded) or another. At the end of the
// frame's list (func_1501878C, 0x15018C8C, from possess_test.cpp's hook), every 30 frames, the list is walked through the lists it calls
// (segments as it sets them) and each call of a weapon's model (the sniper rifle's, the revolver's, the knives': the props' own, and our
// gold copies) and each load of its vertices counted, with where from: [gold] [hand] lines, for the next step.
namespace {
    struct Walk {
        uint8_t* rdram;
        uint32_t seg[16] = {};
        uint32_t steps = 0;
        uint32_t calls[3][2] = {}, loads[3][2] = {};
        uint32_t matrix = 0;
        std::string samples[3];
        uint32_t sampled[3] = {};

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
        void run(uint32_t list, int depth, uint32_t end) {
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
                if (op == 0xDA) {
                    matrix = resolve(w1);
                    continue;
                }
                if (op == 0x01) {
                    const uint32_t a = resolve(w1);
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
                        run(to, depth + 1, 0);
                    }
                }
            }
        }
    };
    uint32_t hand_told = 0, hand_frames = 0;
}

void conker::gold_weapons::frame_end(uint8_t* rdram, uint32_t start, uint32_t end) {
    if (!golds_made || (int32_t)MEM_W(0, (gpr)(int32_t)scene_now) != total_war || hand_told >= 120 || (++hand_frames % 30u) != 0) {
        return;
    }
    if (!kseg0(start) || !kseg0(end) || end <= start || end - start > 0x100000u) {
        return;
    }
    Walk w{ rdram };
    w.run(start & 0x1FFFFFFFu, 0, end & 0x1FFFFFFFu);
    // who holds a gold weapon, and who holds which pickup
    std::string held;
    const uint32_t block = (uint32_t)MEM_W(0, (gpr)(int32_t)pickups_block);
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
    }
}
