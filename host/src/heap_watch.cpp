// Online (a test, 0.9.34): the game's memory heap, watched. A session halted when a new match was being loaded after a long
// one: func_10003C6C (the game's allocator) found no free block big enough and called the game's own stop (func_150AD770, a
// syscall). To find what fills the heap, it is walked every 15 seconds online and when an allocation fails:
//   - every block is on one list in address order from D_800380B4 (+0 next, +4 previous, +8 owner << 24 | size), and the free
//     ones are also on the free list from D_800380B8 (+0xC next free);
//   - each owner's bytes in use are logged when they changed since the last walk.
//   - (0.9.35) every allocation and free online is logged (the first 30000), with its caller and thread, to line the two
//     machines' logs up: their heaps were laid out differently from the first seconds;
//   - the heap's block map (address, size, owner; owner 0 is free) when an allocation fails and after a match's memory is
//     given back.
// Nothing is changed in the game.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_set>

#include "recomp.h"

#include "conker.hpp"

namespace {
    constexpr uint32_t first_block = 0x800380B4;
    constexpr uint32_t first_free = 0x800380B8;
    constexpr uint32_t request = 0x800380C0;   // the allocator's last request: size, owner, mode, end, flag

    uint32_t word(uint8_t* rdram, uint32_t address) {
        return (uint32_t)MEM_W(0, (gpr)(int32_t)address);
    }

    bool ram_pointer(uint32_t address) {
        return (address >> 24) == 0x80 && (address & 0x7FFFFFu) < 0x7FFFF0u && (address & 3u) == 0;
    }

    std::thread::id game_thread;
    bool game_thread_known = false;
    int trace_lines = 0;
    uint32_t last_used = 0;

    const char* thread_name() {
        return game_thread_known && std::this_thread::get_id() == game_thread ? "game" : "other";
    }

    void block_map(uint8_t* rdram, const char* why) {
        std::string text;
        uint32_t b = word(rdram, first_block);
        int n = 0;
        char head[96];
        std::snprintf(head, sizeof(head), "[heap] f=%u block map (%s), address:size:owner", (unsigned)conker::netsession::frame(), why);
        conker::netsession::log(head);
        for (; b != 0 && ram_pointer(b) && n < 4000; n++) {
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

    uint32_t last_bytes[256] = {};
    uint32_t last_count[256] = {};
    bool walked_once = false;

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
        char line[300];
        std::snprintf(line, sizeof(line), "[heap] f=%u %s: %u blocks%s; in use %u bytes in %u, free %u bytes in %u (largest %u); last request %u bytes for owner %u, mode %u",
                      (unsigned)conker::netsession::frame(), why, (unsigned)blocks, broken ? " (the list is broken)" : "", (unsigned)used,
                      (unsigned)used_blocks, (unsigned)free_bytes, (unsigned)free_count, (unsigned)largest, (unsigned)word(rdram, request),
                      (unsigned)word(rdram, request + 4), (unsigned)word(rdram, request + 8));
        conker::netsession::log(line);
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
        if (last_used > used + 300000u) {
            block_map(rdram, "after memory was given back");
        }
        last_used = used;
        std::memcpy(last_bytes, bytes, sizeof(bytes));
        std::memcpy(last_count, count, sizeof(count));
        walked_once = true;
    }
}

// Every frame (func_10011BB8): a walk every 900 frames online.
extern "C" void conker_heap_watch(uint8_t* rdram) {
    if (!conker::netsession::active()) {
        return;
    }
    if (!game_thread_known) {
        game_thread = std::this_thread::get_id();
        game_thread_known = true;
    }
    static uint32_t last = 0xFFFFFFFFu;
    const uint32_t now = (uint32_t)conker::netsession::frame();
    if (now / 900u != last) {
        last = now / 900u;
        walk(rdram, "every 15 s");
    }
}

// func_10003C6C about to stop the game: no free block for the request.
extern "C" void conker_heap_failed(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    walk(rdram, "NO MEMORY: an allocation failed");
    block_map(rdram, "the allocation failed");
}

// func_10003C6C (allocate: $a0 size, $a1 owner, $a2 mode, $a3 from the end, 0x10($sp) flag) begins; called from
// func_10003C40 its caller is the one in func_10003C40's frame.
namespace {
    uint32_t alloc_size = 0, alloc_owner = 0, alloc_mode = 0, alloc_end = 0, alloc_from = 0;
}

extern "C" void conker_heap_alloc_begin(uint8_t* rdram, recomp_context* ctx) {
    alloc_size = (uint32_t)ctx->r4;
    alloc_owner = (uint32_t)ctx->r5 & 0xFFu;
    alloc_mode = (uint32_t)ctx->r6;
    alloc_end = (uint32_t)ctx->r7;
    alloc_from = (uint32_t)ctx->r31;
    if (alloc_from == 0x10003C5Cu) {
        alloc_from = word(rdram, (uint32_t)ctx->r29 + 0x1C);
    }
}

// func_10003C6C returns ($v0: the memory, 0 if none).
extern "C" void conker_heap_alloc_end(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (!conker::netsession::active() || trace_lines >= 30000) {
        return;
    }
    trace_lines++;
    char line[160];
    std::snprintf(line, sizeof(line), "[alloc] f=%u %u bytes owner %u mode %u%s -> %06X from %08X (%s)", (unsigned)conker::netsession::frame(),
                  (unsigned)alloc_size, (unsigned)alloc_owner, (unsigned)alloc_mode, alloc_end != 0 ? " end" : "",
                  (unsigned)((uint32_t)ctx->r2 & 0xFFFFFFu), (unsigned)alloc_from, thread_name());
    conker::netsession::log(line);
}

// func_10004074 (free: $a0 the memory) begins.
extern "C" void conker_heap_free(uint8_t* rdram, recomp_context* ctx) {
    if (!conker::netsession::active() || trace_lines >= 30000 || (uint32_t)ctx->r4 == 0) {
        return;
    }
    trace_lines++;
    const uint32_t block = (uint32_t)ctx->r4 - 0xCu;
    const uint32_t header = ram_pointer(block) ? word(rdram, block + 8) : 0;
    char line[160];
    std::snprintf(line, sizeof(line), "[alloc] f=%u free %06X (%u bytes owner %u) from %08X (%s)", (unsigned)conker::netsession::frame(),
                  (unsigned)((uint32_t)ctx->r4 & 0xFFFFFFu), (unsigned)(header & 0xFFFFFFu), (unsigned)(header >> 24), (unsigned)(uint32_t)ctx->r31,
                  thread_name());
    conker::netsession::log(line);
}
