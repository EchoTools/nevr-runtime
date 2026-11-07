// Whether one of the game's gamespaces holds a combat map: the detector `aibots` reads (aibots.cpp).
//
// KNOWN-WRONG #2168: the level list below is a second copy of the one the runtime's refresh-rate rule keeps
// (src/quest/combat/refresh_rate.cpp, which owns the refresh rate and replaced this plugin's rule, #2172). It stays until the module
// that reads it moves into the runtime (`aibots`: F18, #2185), which then asks the runtime's detector; this file goes with it.
#pragma once

#include <cstdint>

namespace q15t {

// True when one of the game's gamespaces holds a combat map; `level` gets the deciding level's CSymbol64 (0 when none).
bool CombatMapLoaded(std::uintptr_t game, std::uint64_t* level);

}  // namespace q15t
