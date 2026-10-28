// Graphics: Conker's Detail, Conker kept at his most detailed model (called from a hook in conker.toml).
//
// Each character has a few models, from the most detailed down, and every game frame
// func_1502C6E8 picks one for each object by how far it is from the camera: under 500 units
// the first (level 0), then further levels at the distances in D_80096DE0 and D_80096DE4, and
// past 2000 the last. Nearly still (xz speed, +0x3C, under 3) a level from 2 on is moved one
// closer. Two flags override it: D_800C35EA set to 1 makes every object level 0, and in the
// multiplayer arenas (D_800BE616, set by func_15015F40 for scenes 26, 36, 43, 45, 48, 51, 52 and
// 63, with the player count) a character whose first model is 0, Conker, is kept at level 1.
// The level is clamped to the object's models (+0x2C8 is their count), and when it changed the
// new model is loaded (func_150837D4, with the model's number from the list at +0x2C4; the
// loaded one is then at +0x4) and the level stored at +0x1C8. An object whose level the game
// sets itself (+0x1C9, func_1502FBE8) isn't picked for.
//
// Conker is object 0, his models 0 to 4. Measured (each level picked logged as he walked about
// the first room of a save): he flipped between level 0 and 1 as the camera's distance crossed
// 500, and between 1 and 2 each time he started or stopped walking, from the nearly-still rule.
// With Always Highest he stays at level 0 in single player; multiplayer keeps the original's
// level 1, which the game chose for its split screens.

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>

#include "recomp.h"

#include "conker.hpp"

namespace {
    // Set in the multiplayer arenas (see the top): Conker is kept at level 1 there.
    constexpr uint32_t multiplayer_detail_flag = 0x800BE616;
}

// func_1502C6E8 at 0x1502C92C, with the level picked: $v1 is the level, $a3 the object
// (gObjects, 0x32C bytes each) and 0x50($sp) the object's index. The level is compared with the
// object's last after this, so a changed one loads the model as the game's own would. Level 0
// is always one of the object's models (+0x2C8, their count, is at least 1 for an object that
// gets here: the game clamps the level to count - 1 and stops if that's below 0).
// (possess68) A measurement of what the setting costs in the game's memory: conker_detail.log, next to the game. For Conker (object 0)
// each model change is logged (the level the game picked, the one used, the new model's size: the heap block it was loaded into),
// with the game's heap before and after (func_10003C6C's heap: its blocks from D_800380B4, +0 next, +8 owner << 24 | size, owner 0
// free); and every 30 seconds or so the model in use and the heap.
namespace {
    FILE* detail_log = nullptr;

    void dlog(const char* format, ...) {
        if (detail_log == nullptr) {
            detail_log = std::fopen("conker_detail.log", "w");
            if (detail_log == nullptr) {
                return;
            }
        }
        va_list args;
        va_start(args, format);
        std::vfprintf(detail_log, format, args);
        va_end(args);
        std::fputc('\n', detail_log);
        std::fflush(detail_log);
    }

    uint32_t mem_word(uint8_t* rdram, uint32_t address) {
        return (uint32_t)MEM_W(0, (gpr)(int32_t)address);
    }

    bool in_ram(uint32_t address) {
        return (address >> 24) == 0x80 && (address & 0x00FFFFFFu) < 0x00800000u;
    }

    struct HeapInfo {
        uint32_t used = 0, free_bytes = 0, largest = 0, blocks = 0;
    };

    // the heap's blocks walked; and the size of the one holding `inside` (from its start to the next block's), if asked
    HeapInfo walk_heap(uint8_t* rdram, uint32_t inside = 0, uint32_t* holder_size = nullptr) {
        HeapInfo info;
        uint32_t block = mem_word(rdram, 0x800380B4);
        for (int n = 0; n < 60000 && in_ram(block); n++) {
            const uint32_t header = mem_word(rdram, block + 8);
            const uint32_t size = header & 0x00FFFFFFu;
            const uint32_t next = mem_word(rdram, block);
            if ((header >> 24) == 0) {
                info.free_bytes += size;
                info.largest = std::max(info.largest, size);
            } else {
                info.used += size;
            }
            info.blocks++;
            if (holder_size != nullptr && inside >= block && in_ram(next) && inside < next) {
                *holder_size = next - block;
            }
            if (!in_ram(next) || next <= block) {
                break;
            }
            block = next;
        }
        return info;
    }

    struct Pending {
        bool on = false;
        uint32_t call = 0;
        int from = 0, to = 0, picked = 0;
        HeapInfo before;
    } pending;
    uint32_t calls = 0;

    void measure(uint8_t* rdram, gpr object, int picked, int used) {
        calls++;
        const int current = (int)(int8_t)MEM_B(0x1C8, object);
        const bool highest = conker::model_detail::always_highest() && MEM_BU(0, (gpr)(int32_t)multiplayer_detail_flag) == 0;
        if (pending.on && current == pending.to) {
            uint32_t model_size = 0;
            const uint32_t model = mem_word(rdram, (uint32_t)object + 4);
            const HeapInfo after = walk_heap(rdram, model, &model_size);
            dlog("call %u: Conker's model level %d -> %d loaded (the game picked %d; setting %s): the model %08X, its heap block %u bytes; "
                "heap free %u -> %u (%+d), in use %u -> %u (%+d), largest free %u",
                calls, pending.from, pending.to, pending.picked, highest ? "Always Highest" : "Original", model, model_size,
                pending.before.free_bytes, after.free_bytes, (int)after.free_bytes - (int)pending.before.free_bytes,
                pending.before.used, after.used, (int)after.used - (int)pending.before.used, after.largest);
            pending.on = false;
        }
        if (used != current && !pending.on) {
            pending.on = true;
            pending.call = calls;
            pending.from = current;
            pending.to = used;
            pending.picked = picked;
            pending.before = walk_heap(rdram);
        }
        if (calls % 900 == 1) {
            uint32_t model_size = 0;
            const uint32_t model = mem_word(rdram, (uint32_t)object + 4);
            const HeapInfo now = walk_heap(rdram, model, &model_size);
            dlog("call %u: setting %s; Conker on model level %d (the game picks %d), the model's heap block %u bytes; heap in use %u, "
                "free %u (largest %u), %u blocks", calls, highest ? "Always Highest" : "Original", current, picked, model_size, now.used,
                now.free_bytes, now.largest, now.blocks);
        }
    }
}

extern "C" void conker_model_detail(uint8_t* rdram, recomp_context* ctx) {
    {
        // (possess68) the measurement, for Conker (the player's object) offline
        const uint32_t index = (uint32_t)MEM_W(0x50, ctx->r29);
        if (index == 0 && !conker::netsession::active()) {
            const int picked = (int)(int32_t)ctx->r3;
            const bool highest = conker::model_detail::always_highest() && MEM_BU(0, (gpr)(int32_t)multiplayer_detail_flag) == 0;
            measure(rdram, ctx->r7, picked, highest ? 0 : picked);
        }
    }
    if (!conker::model_detail::always_highest() || MEM_BU(0, (gpr)(int32_t)multiplayer_detail_flag) != 0) {
        return;
    }
    const gpr object = ctx->r7;
    const uint32_t index = (uint32_t)MEM_W(0x50, ctx->r29);
    const gpr models = (gpr)MEM_W(0x2C4, object);
    // Conker: the player's object, or a character whose first model is Conker's (the game's own
    // test, for its multiplayer rule).
    if (index == 0 || MEM_BU(0, models) == 0) {
        ctx->r3 = 0;
    }
}
