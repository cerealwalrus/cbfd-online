// Online: the game's memory heap, kept the same on every machine and kept from running out.
//
// The game's heap (func_10003C6C allocates, func_10004074 frees): every block on one list in address order from D_800380B4 (+0 next,
// +4 previous, +8 owner << 24 | size; owner 0 is free), the free ones also on the free list from D_800380B8 (+0xC next free). The
// owners 1-4 are a level's data by age (func_10004250 frees owner 2 and ages 3 and 4, func_10004308 frees 1-4), 255 is kept.
//
// Two things in it ran at the pace of each machine's own audio thread, not the game's:
//   - a cache that func_10009CBC fills (allocating at 0x10009ED8, freeing at 0x1000A280), used from the audio thread too;
//   - the streamed sounds' buffers (voices: func_151F2960 allocates four when one starts), which the audio thread frees when the
//     sound has played (func_151F39E4 from func_151F2E88).
// So the two machines' heaps were laid out differently within seconds, and the audio thread changed the heap's lists while the game
// thread did (the game's interrupt masking doesn't stop a thread). Those blocks, kept between matches, also ended up anywhere in the
// heap, cutting up the free memory: after long sessions a match's load found no free block big enough and the game stopped itself
// (func_150AD770's syscall, called at 0x10003DD8), on one machine or both.
//
// Online now:
//   - those allocations, and any allocation from a thread that isn't the game's, come from a pool of their own in the memory the
//     game leaves unused (it uses the first 4 MB: 0x80400000-0x80600000), so the game's heap is only the game thread's, the same on every machine;
//   - an allocation the heap can't give (where the game would stop) comes from a second pool (0x80600000-0x807F0000), only the
//     game thread's;
//   - the pools' blocks follow func_10004250 and func_10004308 (a level's data freed and aged by owner) like the heap's;
//   - a heap block freed from another thread is freed by the game thread at its next frame.
// A pool's block looks like the heap's to the game (a header before it with the owner and size: func_100043B4 changes the owner
// there). Both pools are within the first 8 MB: the audio library stops the game for sample data past it (func_100214F0 at
// 0x100215BC, 0.9.36's pools at 0x80C00000 had it stop as the first music played).
//
// The heap is also walked every 15 seconds online and logged (what is in use and free, the largest free block, each owner's bytes
// that changed), with the pools'.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <algorithm>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "recomp.h"

#include "conker.hpp"

extern "C" void func_10004074(uint8_t* rdram, recomp_context* ctx);

namespace {
    constexpr uint32_t first_block = 0x800380B4;
    constexpr uint32_t first_free = 0x800380B8;
    constexpr uint32_t request = 0x800380C0;   // the allocator's last request: size, owner, mode, end, flag

    uint32_t word(uint8_t* rdram, uint32_t address) {
        return (uint32_t)MEM_W(0, (gpr)(int32_t)address);
    }

    void put_word(uint8_t* rdram, uint32_t address, uint32_t value) {
        MEM_W(0, (gpr)(int32_t)address) = (int32_t)value;
    }

    bool ram_pointer(uint32_t address) {
        return (address >> 24) == 0x80 && (address & 0x7FFFFFu) < 0x7FFFF0u && (address & 3u) == 0;
    }

    std::thread::id game_thread;
    std::atomic<bool> game_thread_known{ false };

    bool on_game_thread() {
        return game_thread_known.load() && std::this_thread::get_id() == game_thread;
    }

    bool on_other_thread() {
        return game_thread_known.load() && std::this_thread::get_id() != game_thread;
    }

    // A pool: first fit by address in [start, end), kept entirely in the game's memory, so that a resync (the host's memory copied
    // to this machine) takes the pool's state with its contents. A block is 16 bytes of header and its memory, the pointer
    // 16-aligned: +0 'USED' or 'FREE', +4 the next block (0: the last), +8 the previous one, +0xC owner << 24 | size (the heap's
    // header layout before the pointer: next, previous, owner and size; func_100043B4 changes the owner there).
    struct Pool {
        uint32_t start;
        uint32_t end;
        const char* name;
        uint32_t peak = 0;

        static constexpr uint32_t used_mark = 0x55534544u;   // "USED"
        static constexpr uint32_t free_mark = 0x46524545u;   // "FREE"

        void init(uint8_t* rdram) const {
            const uint32_t mark = word(rdram, start);
            if (mark != used_mark && mark != free_mark) {
                put_word(rdram, start, free_mark);
                put_word(rdram, start + 4, 0);
                put_word(rdram, start + 8, 0);
                put_word(rdram, start + 0xC, (end - start - 0x10u) & 0xFFFFFFu);
            }
        }

        bool owns(uint32_t pointer) const {
            return pointer >= start + 0x10u && pointer < end;
        }

        static uint32_t size_of(uint8_t* rdram, uint32_t block) {
            return word(rdram, block + 0xC) & 0xFFFFFFu;
        }

        // Calls fn(block, used) for every block, in address order.
        template <typename F>
        void each(uint8_t* rdram, F&& fn) const {
            init(rdram);
            uint32_t block = start;
            for (int n = 0; block >= start && block < end && n < 100000; n++) {
                const uint32_t mark = word(rdram, block);
                if (mark != used_mark && mark != free_mark) {
                    break;
                }
                fn(block, mark == used_mark);
                block = word(rdram, block + 4);
                if (block == 0) {
                    break;
                }
            }
        }

        uint32_t allocate(uint8_t* rdram, uint32_t size, uint32_t owner) {
            init(rdram);
            const uint32_t need = (size + 15u) & ~15u;
            uint32_t found = 0;
            each(rdram, [&](uint32_t block, bool used) {
                if (found == 0 && !used && size_of(rdram, block) >= need) {
                    found = block;
                }
            });
            if (found == 0) {
                return 0;
            }
            const uint32_t have = size_of(rdram, found);
            if (have - need >= 0x20u) {
                // the rest stays free, as a block of its own after this one
                const uint32_t rest = found + 0x10u + need, next = word(rdram, found + 4);
                put_word(rdram, rest, free_mark);
                put_word(rdram, rest + 4, next);
                put_word(rdram, rest + 8, found);
                put_word(rdram, rest + 0xC, (have - need - 0x10u) & 0xFFFFFFu);
                if (next != 0) {
                    put_word(rdram, next + 8, rest);
                }
                put_word(rdram, found + 4, rest);
                put_word(rdram, found + 0xC, need);
            }
            put_word(rdram, found, used_mark);
            put_word(rdram, found + 0xC, (owner << 24) | size_of(rdram, found));
            uint32_t in_use = 0;
            each(rdram, [&](uint32_t block, bool used) {
                if (used) {
                    in_use += 0x10u + size_of(rdram, block);
                }
            });
            peak = in_use > peak ? in_use : peak;
            return found + 0x10u;
        }

        bool release(uint8_t* rdram, uint32_t pointer) {
            uint32_t block = pointer - 0x10u;
            if (!owns(pointer) || word(rdram, block) != used_mark) {
                return false;
            }
            put_word(rdram, block, free_mark);
            put_word(rdram, block + 0xC, size_of(rdram, block));   // owner 0
            // joined with the next block and the one before when they are free
            const uint32_t next = word(rdram, block + 4);
            if (next != 0 && word(rdram, next) == free_mark) {
                const uint32_t after = word(rdram, next + 4);
                put_word(rdram, block + 0xC, size_of(rdram, block) + 0x10u + size_of(rdram, next));
                put_word(rdram, block + 4, after);
                if (after != 0) {
                    put_word(rdram, after + 8, block);
                }
            }
            const uint32_t previous = word(rdram, block + 8);
            if (previous != 0 && word(rdram, previous) == free_mark) {
                const uint32_t after = word(rdram, block + 4);
                put_word(rdram, previous + 0xC, size_of(rdram, previous) + 0x10u + size_of(rdram, block));
                put_word(rdram, previous + 4, after);
                if (after != 0) {
                    put_word(rdram, after + 8, previous);
                }
            }
            return true;
        }

        uint32_t owner_of(uint8_t* rdram, uint32_t pointer) const {
            return word(rdram, pointer - 0x4u) >> 24;
        }

        void stats(uint8_t* rdram, uint32_t* in_use, uint32_t* blocks) const {
            *in_use = 0;
            *blocks = 0;
            each(rdram, [&](uint32_t block, bool used) {
                if (used) {
                    *in_use += 0x10u + size_of(rdram, block);
                    (*blocks)++;
                }
            });
        }
    };

    std::mutex pool_mutex;
    Pool audio_pool{ 0x80400000u, 0x80600000u, "audio" };
    Pool spare_pool{ 0x80600000u, 0x807F0000u, "spare" };
    std::vector<uint32_t> deferred_frees;   // heap blocks freed from another thread, for the game thread
    unsigned spare_used = 0;

    // 0.10.27, measuring only (nothing in the game's memory changes): every heap block the game thread is given, kept here by its
    // pointer with its size, its owner, who asked (the caller) and the code addresses found on the stack above (the callers' callers),
    // so that the 15-second walk can say what the heap holds by who asked for it (to compare two players with four).
    struct Asked {
        uint32_t size = 0, owner = 0, from = 0, frame = 0;
        uint32_t up[4] = {};
    };
    std::unordered_map<uint32_t, Asked> asked_blocks;
    Asked pending_ask;
    bool pending_valid = false;

    bool code_address(uint32_t value) {
        return (value >= 0x10000000u && value < 0x10100000u) || (value >= 0x15000000u && value < 0x16100000u);
    }

    uint32_t last_bytes[256] = {};
    uint32_t last_count[256] = {};
    bool walked_once = false;

    void block_map(uint8_t* rdram, const char* why) {
        std::string text;
        uint32_t b = word(rdram, first_block);
        char head[96];
        std::snprintf(head, sizeof(head), "[heap] f=%u block map (%s), address:size:owner", (unsigned)conker::netsession::frame(), why);
        conker::netsession::log(head);
        for (int n = 0; b != 0 && ram_pointer(b) && n < 4000; n++) {
            const uint32_t header = word(rdram, b + 8);
            char part[40];
            std::snprintf(part, sizeof(part), " %06X:%u:%u", (unsigned)(b & 0xFFFFFFu), (unsigned)(header & 0xFFFFFFu), (unsigned)(header >> 24));
            text += part;
            if (text.size() > 900) {
                conker::netsession::log(("[heap]  " + text).c_str());
                text.clear();
            }
            b = word(rdram, b);
        }
        if (!text.empty()) {
            conker::netsession::log(("[heap]  " + text).c_str());
        }
    }

    void walk(uint8_t* rdram, const char* why) {
        std::unordered_set<uint32_t> free_blocks;
        uint32_t f = word(rdram, first_free);
        for (int n = 0; ram_pointer(f) && n < 40000; n++) {
            if (!free_blocks.insert(f).second) {
                break;
            }
            f = word(rdram, f + 0xC);
        }
        uint32_t bytes[256] = {}, count[256] = {};
        uint32_t blocks = 0, used = 0, used_blocks = 0, free_bytes = 0, free_count = 0, largest = 0;
        uint32_t b = word(rdram, first_block);
        bool broken = false;
        for (int n = 0; b != 0; n++) {
            if (!ram_pointer(b) || n >= 60000) {
                broken = true;
                break;
            }
            const uint32_t header = word(rdram, b + 8);
            const uint32_t size = header & 0xFFFFFFu, owner = header >> 24;
            blocks++;
            if (free_blocks.count(b) != 0) {
                free_bytes += size;
                free_count++;
                largest = size > largest ? size : largest;
            } else {
                used += size;
                used_blocks++;
                bytes[owner] += size;
                count[owner]++;
            }
            b = word(rdram, b);
        }
        char line[400];
        {
            std::lock_guard lock{ pool_mutex };
            uint32_t audio_in_use = 0, audio_blocks = 0, spare_in_use = 0, spare_blocks = 0;
            audio_pool.stats(rdram, &audio_in_use, &audio_blocks);
            spare_pool.stats(rdram, &spare_in_use, &spare_blocks);
            std::snprintf(line, sizeof(line),
                          "[heap] f=%u %s: %u blocks%s; in use %u bytes in %u, free %u bytes in %u (largest %u); last request %u bytes for owner %u, mode %u; "
                          "audio pool %u bytes in %u blocks (peak %u), spare pool %u bytes in %u blocks (used %u times)",
                          (unsigned)conker::netsession::frame(), why, (unsigned)blocks, broken ? " (the list is broken)" : "", (unsigned)used,
                          (unsigned)used_blocks, (unsigned)free_bytes, (unsigned)free_count, (unsigned)largest, (unsigned)word(rdram, request),
                          (unsigned)word(rdram, request + 4), (unsigned)word(rdram, request + 8), (unsigned)audio_in_use,
                          (unsigned)audio_blocks, (unsigned)audio_pool.peak, (unsigned)spare_in_use, (unsigned)spare_blocks,
                          spare_used);
        }
        conker::netsession::log(line);
        {
            std::lock_guard lock{ pool_mutex };
            uint32_t pool_bytes[256] = {}, pool_count[256] = {};
            audio_pool.each(rdram, [&](uint32_t block, bool used) {
                if (used) {
                    const uint32_t owner = audio_pool.owner_of(rdram, block + 0x10u);
                    pool_bytes[owner] += 0x10u + Pool::size_of(rdram, block);
                    pool_count[owner]++;
                }
            });
            std::string owners;
            for (int owner = 0; owner < 256; owner++) {
                if (pool_count[owner] != 0) {
                    char part[48];
                    std::snprintf(part, sizeof(part), " %d:%u/%u", owner, (unsigned)pool_bytes[owner], (unsigned)pool_count[owner]);
                    owners += part;
                }
            }
            if (!owners.empty()) {
                conker::netsession::log(("[heap]   audio pool by owner:bytes/blocks" + owners).c_str());
            }
        }
        std::string changes;
        for (int owner = 0; owner < 256; owner++) {
            if (bytes[owner] == last_bytes[owner] && count[owner] == last_count[owner] && walked_once) {
                continue;
            }
            if (bytes[owner] == 0 && last_bytes[owner] == 0) {
                continue;
            }
            char part[64];
            std::snprintf(part, sizeof(part), " %d:%u/%u(%+d)", owner, (unsigned)bytes[owner], (unsigned)count[owner],
                          (int)bytes[owner] - (int)last_bytes[owner]);
            changes += part;
            if (changes.size() > 900) {
                conker::netsession::log(("[heap]   owner:bytes/blocks(change)" + changes).c_str());
                changes.clear();
            }
        }
        if (!changes.empty()) {
            conker::netsession::log(("[heap]   owner:bytes/blocks(change)" + changes).c_str());
        }
        std::memcpy(last_bytes, bytes, sizeof(bytes));
        std::memcpy(last_count, count, sizeof(count));
        walked_once = true;
        // (0.10.27) what is in use, by who asked for it: caller/owner, bytes in blocks, the largest, the callers' callers of the largest
        {
            struct Sum { uint32_t bytes = 0, blocks = 0, largest = 0; uint32_t up[4] = {}; };
            std::map<std::pair<uint32_t, uint32_t>, Sum> sums;
            std::lock_guard lock{ pool_mutex };
            uint32_t walk_b = word(rdram, first_block);
            for (int n = 0; walk_b != 0 && ram_pointer(walk_b) && n < 60000; n++) {
                const uint32_t header = word(rdram, walk_b + 8);
                const uint32_t size = header & 0xFFFFFFu, owner = header >> 24;
                if (free_blocks.count(walk_b) == 0) {
                    const auto found = asked_blocks.find(walk_b + 0xCu);
                    const uint32_t from = found != asked_blocks.end() ? found->second.from : 0u;
                    Sum& sum = sums[{ from, owner }];
                    sum.bytes += size;
                    sum.blocks++;
                    if (size > sum.largest) {
                        sum.largest = size;
                        if (found != asked_blocks.end()) {
                            std::memcpy(sum.up, found->second.up, sizeof(sum.up));
                        }
                    }
                }
                walk_b = word(rdram, walk_b);
            }
            std::vector<std::pair<std::pair<uint32_t, uint32_t>, Sum>> list(sums.begin(), sums.end());
            std::sort(list.begin(), list.end(), [](const auto& x, const auto& y) { return x.second.bytes > y.second.bytes; });
            std::string text;
            int shown = 0;
            for (const auto& item : list) {
                if (shown >= 40 || item.second.bytes < 2048) {
                    break;
                }
                char part[120];
                std::snprintf(part, sizeof(part), " %08X/%u:%u/%u(max %u; %08X %08X %08X)", (unsigned)item.first.first, (unsigned)item.first.second,
                    (unsigned)item.second.bytes, (unsigned)item.second.blocks, (unsigned)item.second.largest, (unsigned)item.second.up[0],
                    (unsigned)item.second.up[1], (unsigned)item.second.up[2]);
                text += part;
                shown++;
                if (text.size() > 800) {
                    conker::netsession::log(("[heap]   in use by caller/owner:bytes/blocks" + text).c_str());
                    text.clear();
                }
            }
            if (!text.empty()) {
                conker::netsession::log(("[heap]   in use by caller/owner:bytes/blocks" + text).c_str());
            }
        }
    }

    // Is a streamed sound being started (func_151F2960 on the game thread's stack)?
    bool starting_stream(uint8_t* rdram, recomp_context* ctx, uint32_t from) {
        if (from >= 0x151F2960u && from < 0x151F2BA8u) {
            return true;
        }
        const uint32_t sp = (uint32_t)ctx->r29;
        if (!ram_pointer(sp)) {
            return false;
        }
        for (uint32_t offset = 0; offset < 0x400u; offset += 4) {
            const uint32_t value = word(rdram, sp + offset);
            if (value >= 0x151F2960u && value < 0x151F2BA8u) {
                return true;
            }
        }
        return false;
    }

    // Calls func_10004074 (free) from the game thread, its stack below the game's.
    void free_on_game_thread(uint8_t* rdram, recomp_context* ctx, uint32_t pointer) {
        const recomp_context saved = *ctx;
        ctx->r4 = (gpr)(int32_t)pointer;
        ctx->r29 = (gpr)(int32_t)(((uint32_t)saved.r29 - 0x200u) & ~7u);
        func_10004074(rdram, ctx);
        *ctx = saved;
    }
}

// Every frame (func_10011BB8, the game thread): the game thread is noted; online, the frees left by other threads are done, and the
// heap is walked every 900 frames.
extern "C" void conker_heap_watch(uint8_t* rdram, recomp_context* ctx) {
    if (!game_thread_known.load()) {
        game_thread = std::this_thread::get_id();
        game_thread_known.store(true);
    }
    if (!conker::netsession::active()) {
        return;
    }
    std::vector<uint32_t> frees;
    {
        std::lock_guard lock{ pool_mutex };
        frees.swap(deferred_frees);
    }
    for (uint32_t pointer : frees) {
        free_on_game_thread(rdram, ctx, pointer);
    }
    static uint32_t last = 0xFFFFFFFFu;
    const uint32_t now = (uint32_t)conker::netsession::frame();
    if (now / 900u != last) {
        last = now / 900u;
        walk(rdram, "every 15 s");
    }
}

// func_10003C6C (allocate: $a0 size, $a1 owner, $a2 mode, $a3 from the end, 0x10($sp) flag) begins. Returns 1 when the memory was
// given from a pool ($v0), the function then not run. Called from func_10003C40, the caller is the one in func_10003C40's frame.
extern "C" int conker_heap_alloc_begin(uint8_t* rdram, recomp_context* ctx) {
    if (!conker::netsession::active()) {
        return 0;
    }
    uint32_t from = (uint32_t)ctx->r31;
    if (from == 0x10003C5Cu) {
        from = word(rdram, (uint32_t)ctx->r29 + 0x1C);
    }
    const bool other_thread = on_other_thread();
    const bool cache = from == 0x10009ED8u;
    // (0.10.75) the pause screen's allocations with this machine's menu open (its textures, func_1510D0EC's cache: kept loaded):
    // this machine's alone, so from the pool, not the game's heap
    const bool pause = on_game_thread() && conker::netmenu::local_pause_drawing();
    if (!other_thread && !cache && !pause && !starting_stream(rdram, ctx, from)) {
        if (on_game_thread()) {
            // (0.10.27, measuring) noted, for conker_heap_alloc_end
            pending_ask = Asked{};
            pending_ask.size = (uint32_t)ctx->r4;
            pending_ask.owner = (uint32_t)ctx->r5 & 0xFFu;
            pending_ask.from = from;
            pending_ask.frame = (uint32_t)conker::netsession::frame();
            const uint32_t sp = (uint32_t)ctx->r29;
            int found = 0;
            if (ram_pointer(sp)) {
                for (uint32_t offset = 0; offset < 0x600u && found < 4; offset += 4) {
                    const uint32_t value = word(rdram, sp + offset);
                    if (code_address(value) && value != from && value != 0x10003C5Cu) {
                        bool seen = false;
                        for (int k = 0; k < found; k++) {
                            seen = seen || pending_ask.up[k] == value;
                        }
                        if (!seen) {
                            pending_ask.up[found++] = value;
                        }
                    }
                }
            }
            pending_valid = true;
        }
        return 0;
    }
    uint32_t pointer = 0;
    {
        std::lock_guard lock{ pool_mutex };
        pointer = audio_pool.allocate(rdram, (uint32_t)ctx->r4, (uint32_t)ctx->r5 & 0xFFu);
    }
    if (pointer == 0) {
        static int logged = 0;
        if (logged < 20) {
            logged++;
            char line[160];
            std::snprintf(line, sizeof(line), "[heap] f=%u the audio pool is full: %u bytes for %08X from the game's heap", (unsigned)conker::netsession::frame(),
                          (unsigned)(uint32_t)ctx->r4, (unsigned)from);
            conker::netsession::log(line);
        }
        return 0;
    }
    ctx->r2 = (gpr)(int32_t)pointer;
    return 1;
}

// func_10003C6C about to stop the game (0x10003DD8): no free block for the request. Online the spare pool gives it (returns 1, the
// function's frame undone: $s0 and $ra from 0x18 and 0x1C of its frame of 0x70). Otherwise the heap is logged first.
extern "C" int conker_heap_failed(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t size = word(rdram, request), owner = word(rdram, request + 4) & 0xFFu;
    if (conker::netsession::active() && on_game_thread()) {
        uint32_t pointer = 0;
        {
            std::lock_guard lock{ pool_mutex };
            pointer = spare_pool.allocate(rdram, size, owner);
            if (pointer != 0) {
                spare_used++;
            }
        }
        if (pointer != 0) {
            char line[200];
            std::snprintf(line, sizeof(line), "[heap] f=%u the heap had no block of %u bytes (owner %u): given from the spare pool at %08X", (unsigned)conker::netsession::frame(),
                          (unsigned)size, (unsigned)owner, (unsigned)pointer);
            conker::netsession::log(line);
            static int maps = 0;
            if (maps < 3) {
                maps++;
                walk(rdram, "the heap had no block big enough");
                block_map(rdram, "the heap had no block big enough");
            }
            const uint32_t sp = (uint32_t)ctx->r29;
            ctx->r2 = (gpr)(int32_t)pointer;
            ctx->r16 = (gpr)(int32_t)word(rdram, sp + 0x18);
            ctx->r31 = (gpr)(int32_t)word(rdram, sp + 0x1C);
            ctx->r29 = (gpr)(int32_t)(sp + 0x70u);
            return 1;
        }
    }
    walk(rdram, "NO MEMORY: an allocation failed");
    block_map(rdram, "the allocation failed");
    return 0;
}

// func_10004074 (free: $a0 the memory) begins. Returns 1 when it was a pool's block (freed there) or a heap block freed from another
// thread (left for the game thread), the function then not run.
extern "C" int conker_heap_free(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t pointer = (uint32_t)ctx->r4;
    if (pointer == 0) {
        return 0;
    }
    std::lock_guard lock{ pool_mutex };
    if (on_game_thread()) {
        asked_blocks.erase(pointer);   // (0.10.27, measuring)
    }
    if (audio_pool.owns(pointer)) {
        audio_pool.release(rdram, pointer);
        return 1;
    }
    if (spare_pool.owns(pointer)) {
        spare_pool.release(rdram, pointer);
        return 1;
    }
    if (conker::netsession::active() && on_other_thread() && ram_pointer(pointer)) {
        deferred_frees.push_back(pointer);
        return 1;
    }
    return 0;
}

// func_10004250 begins (frees owner 2, ages owners 3 and 4 by one) and func_10004308 (frees owners 1-4): the same for the pools'
// blocks, as they would have been in the heap (the cache's and the voices' are 255, kept).
extern "C" void conker_heap_age(uint8_t* rdram, recomp_context* ctx, int all) {
    (void)ctx;
    std::lock_guard lock{ pool_mutex };
    for (Pool* pool : { &audio_pool, &spare_pool }) {
        std::vector<uint32_t> pointers;
        pool->each(rdram, [&](uint32_t block, bool used) {
            if (used) {
                pointers.push_back(block + 0x10u);
            }
        });
        for (uint32_t pointer : pointers) {
            const uint32_t owner = pool->owner_of(rdram, pointer);
            if (all != 0) {
                if (owner >= 1 && owner <= 4) {
                    pool->release(rdram, pointer);
                }
            } else if (owner == 2) {
                pool->release(rdram, pointer);
            } else if (owner == 3 || owner == 4) {
                const uint32_t header = word(rdram, pointer - 0x4u);
                put_word(rdram, pointer - 0x4u, ((owner - 1u) << 24) | (header & 0xFFFFFFu));
            }
        }
    }
}

// 0.10.27, measuring only: func_10003C6C returns ($v0 the memory, at 0x10004060 on every path that gives a heap block): the block is
// kept with who asked (conker_heap_alloc_begin), and one of 8 KB or more is logged as it is given.
extern "C" void conker_heap_alloc_end(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (!pending_valid || !conker::netsession::active() || !on_game_thread()) {
        pending_valid = false;
        return;
    }
    pending_valid = false;
    const uint32_t pointer = (uint32_t)ctx->r2;
    if (!ram_pointer(pointer)) {
        return;
    }
    {
        std::lock_guard lock{ pool_mutex };
        if (asked_blocks.size() < 200000) {
            asked_blocks[pointer] = pending_ask;
        }
    }
    static int logged = 0;
    if (pending_ask.size >= 8192 && logged < 1500) {
        logged++;
        char line[200];
        std::snprintf(line, sizeof(line), "[alloc] f=%u %u bytes owner %u at %08X for %08X (up %08X %08X %08X %08X)", (unsigned)pending_ask.frame,
            (unsigned)pending_ask.size, (unsigned)pending_ask.owner, (unsigned)pointer, (unsigned)pending_ask.from, (unsigned)pending_ask.up[0],
            (unsigned)pending_ask.up[1], (unsigned)pending_ask.up[2], (unsigned)pending_ask.up[3]);
        conker::netsession::log(line);
    }
}
