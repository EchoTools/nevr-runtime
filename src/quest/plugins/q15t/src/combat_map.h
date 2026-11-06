// Whether one of the game's gamespaces holds a combat map: the detector `aibots` and `music` read (aibots.cpp, music.cpp).
//
// KNOWN-WRONG #2168: the level list below is a second copy of the one the runtime's refresh-rate rule keeps
// (src/quest/combat/refresh_rate.cpp, which owns the refresh rate and replaced this plugin's rule, #2172). It stays until the two modules
// that read it move into the runtime (`music`, `aibots`: F17, F18), which then ask the runtime's detector; this file goes with them.
#pragma once

#include <cstdint>

namespace q15t {

// True when one of the game's gamespaces holds a combat map; `level` gets the deciding level's CSymbol64 (0 when none).
bool CombatMapLoaded(std::uintptr_t game, std::uint64_t* level);

}  // namespace q15t
