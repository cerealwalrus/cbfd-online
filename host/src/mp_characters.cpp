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
