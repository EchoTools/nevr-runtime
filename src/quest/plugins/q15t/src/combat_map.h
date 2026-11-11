// Whether one of the game's gamespaces holds a combat map.
//
// KNOWN-WRONG #2168: unused: no module of the plugin reads this (the last readers moved into the runtime). It is removed when the fold ends. Its level list is a
// second copy of the one the runtime keeps (src/quest/combat/combat_level.h).
#pragma once

#include <cstdint>

namespace q15t {

// True when one of the game's gamespaces holds a combat map; `level` gets the deciding level's CSymbol64 (0 when none).
bool CombatMapLoaded(std::uintptr_t game, std::uint64_t* level);

}  // namespace q15t
