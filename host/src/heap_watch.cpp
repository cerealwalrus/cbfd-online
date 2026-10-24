// Online (a test, 0.9.34): the game's memory heap, watched. A session halted when a new match was being loaded after a long
// one: func_10003C6C (the game's allocator) found no free block big enough and called the game's own stop (func_150AD770, a
// syscall). To find what fills the heap, it is walked every 15 seconds online and when an allocation fails:
//   - every block is on one list in address order from D_800380B4 (+0 next, +4 previous, +8 owner << 24 | size), and the free
//     ones are also on the free list from D_800380B8 (+0xC next free);
//   - each owner's bytes in use are logged when they changed since the last walk.
// Nothing is changed in the game.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
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
}
