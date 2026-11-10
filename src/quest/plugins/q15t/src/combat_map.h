// Whether one of the game's gamespaces holds a combat map.
//
// KNOWN-WRONG #2168: the level list below is a second copy of the one the runtime keeps (src/quest/combat/combat_level.h, which owns the refresh rate and
// replaced this plugin's rule, #2172). No module of the plugin reads it any longer: music and aibots, its last readers, moved into the runtime; this file goes
// with the plugin's gamespace lookup when the fold ends.
#pragma once

#include <cstdint>

namespace q15t {

// True when one of the game's gamespaces holds a combat map; `level` gets the deciding level's CSymbol64 (0 when none).
bool CombatMapLoaded(std::uintptr_t game, std::uint64_t* level);

}  // namespace q15t
