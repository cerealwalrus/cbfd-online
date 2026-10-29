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
    };
    // Where player 1 held R-Look in the room (the possess115 test's [pos] lines). (possess118) A weapon's prop has a kind of its own, not
    // its pickup's: the sniper rifle's 0x56 (ids 11 and 12, the pickups of kind 24), the revolver's 0x53 (7, 19: 23), the knives' 0x3C
    // (5, 17: 14), the possess116 test's [weapons] lines; possess116 looked for the pickups' kinds among the props and found none.
    Gold golds[3] = {
        { "sniper rifle", 0x24, 0x56, -63, -1448, 219, 0 },
        { "revolver", 0x23, 0x53, -422, -1448, 30, 0 },
        { "knives", 0x14, 0x3C, 96, -1448, -148, 0 },
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
    for (const Gold& g : golds) {
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

// (possess121) Gold, lying about. A prop's model is its vertices (+0x28, +0x16 of them, 0x10 each: x, y, z, flags, s, t, then its
// colour, r g b a at +0xC) and the lists drawing them; a prop that moves (+0x70's 0x1, the weapons, spinning) has its vertices put
// where it is each frame (func_150A44F0: their places alone, x y z, into its buffer in D_800DBEF8's table, or +0x20 and +0x24, copies
// made as the level loads), the rest of each vertex as it was copied. The gold weapons share their model with the ones they were copied
// from, so each gets vertices of its own in ours, their colours made gold (as bright as they were, in the Rare logo's gold), and its
// buffers' colours made the same, each frame. Only the gold weapons' own copies: the level's model, and every other weapon, as they are.
namespace {
    constexpr uint32_t vertices_ours = 0x80FF0000;   // ours: the gold weapons' vertices (up to 0x80FF8000)
    constexpr uint32_t vertices_ours_size = 0x8000;
    constexpr uint32_t moved_vertices = 0x800DBEF8;  // D_800DBEF8: each prop's vertex buffer, put where it is each frame
    struct GoldModel { uint32_t prop = 0, from = 0, ours = 0, count = 0; };
    GoldModel gold_models[3];
    uint32_t vertices_used = 0;
    uint32_t gold_level = 0xFFFFFFFFu;   // the props list the models above were made for

    void gold_colours(uint8_t* rdram, uint32_t at, uint32_t count) {
        if (!kseg0(at) || !kseg0(at + count * 0x10u)) {
            return;
        }
        for (uint32_t v = 0; v < count; v++) {
            const gpr c = (gpr)(int32_t)(at + v * 0x10u + 0xCu);
            const uint32_t r = MEM_BU(0, c), gr = MEM_BU(1, c), b = MEM_BU(2, c);
            const uint32_t l = (r * 30u + gr * 59u + b * 11u) / 100u;
            MEM_B(0, c) = (int8_t)(uint8_t)std::min(255u, 60u + l * 90u / 100u);
            MEM_B(1, c) = (int8_t)(uint8_t)std::min(255u, 40u + l * 70u / 100u);
            MEM_B(2, c) = (int8_t)(uint8_t)std::min(255u, l * 15u / 100u);
        }
    }

    // the colours of our gold vertices into a buffer of the same model's (each frame: the same colours, whatever was there)
    void copy_colours(uint8_t* rdram, uint32_t to, uint32_t from, uint32_t count) {
        if (!kseg0(to) || !kseg0(to + count * 0x10u) || to == from) {
            return;
        }
        for (uint32_t v = 0; v < count; v++) {
            MEM_W(v * 0x10u + 0xCu, (gpr)(int32_t)to) = MEM_W(v * 0x10u + 0xCu, (gpr)(int32_t)from);
        }
    }

    void gold_paint(uint8_t* rdram, uint32_t list, uint32_t first, uint32_t count) {
        if (gold_level != list) {
            gold_level = list;
            vertices_used = 0;
            for (GoldModel& m : gold_models) {
                m = GoldModel{};
            }
        }
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
            const uint32_t n = (uint32_t)MEM_HU(0x16, p);
            if (m.prop != prop) {
                const uint32_t from = (uint32_t)MEM_W(0x28, p);
                if (n == 0 || n > 0x800 || !kseg0(from) || vertices_used + n * 0x10u > vertices_ours_size) {
                    continue;
                }
                m.prop = prop;
                m.from = from;
                m.count = n;
                m.ours = vertices_ours + vertices_used;
                vertices_used += (n * 0x10u + 15u) & ~15u;
                std::memcpy(rdram + (m.ours - 0x80000000u), rdram + (from - 0x80000000u), n * 0x10u);
                gold_colours(rdram, m.ours, n);
                MEM_W(0x28, p) = (int32_t)m.ours;
                for (uint32_t b : { 0x20u, 0x24u }) {
                    if ((uint32_t)MEM_W(b, p) == from) {
                        MEM_W(b, p) = (int32_t)m.ours;
                    }
                }
                say("[gold] the %s made gold: its %u vertices (from %08X) copied to %08X; its buffers %08X %08X", golds[g].name, (unsigned)n,
                    (unsigned)from, (unsigned)m.ours, (unsigned)MEM_W(0x20, p), (unsigned)MEM_W(0x24, p));
            }
            // its own buffers (never the level's model)
            for (uint32_t b : { 0x20u, 0x24u }) {
                const uint32_t at = (uint32_t)MEM_W(b, p);
                if (at != m.from) {
                    copy_colours(rdram, at, m.ours, std::min(n, m.count));
                }
            }
            const uint32_t table = (uint32_t)MEM_W(0, (gpr)(int32_t)moved_vertices);
            if (kseg0(table)) {
                const uint32_t at = (uint32_t)MEM_W(4 * i, (gpr)(int32_t)table);
                if (at != m.from) {
                    copy_colours(rdram, at, m.ours, std::min(n, m.count));
                }
            }
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
