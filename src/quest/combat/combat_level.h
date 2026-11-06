/* What level the game has loaded: the shared answer the combat modules ask (#2172, docs/adr/0046), so no module reads the game's gamespace tables
 * for itself. Arena and combat run on the same APK, so `combat::Active()` does not say which one a player is in; this does.
 *
 * What it reads. CR15Game's two gamespace tables ({sym, gs*} rows of 16 bytes; counts at 0x1f78 and 0x1fb8; offsets 0x1f48/0x1f88) and, in each
 * gamespace of the game (gs+0x8 is the game), its level CSymbol64 at gs+0x58. The pinned build's offsets; the q15t plugin's `refresh_rate.cpp`
 * carried them (got_pinned_test.cpp checks them against the store libr15, and combat_level_targets.h ties each offset to its pinned word at compile time). Reads use process_vm_readv (safe_read.h): an unreadable
 * game is no level (fail closed).
 *
 * The three classes. A combat map: `mpl_combat_dyson`, `_combustion`, `_fission`, `_gauss`. The combat lobby: `mpl_lobby_b_combat` only. The other
 * lobby levels (`mpl_lobby_b`, `_b2`, `_b2_lowspec`, `_b_arena`, `_b_social`) are the lobbies arena players use and are NOT a class here: nothing
 * about them is combat's. An Echo Arena level: `mpl_arena_a`, `_a_lowspec`, `_b`, `mpl_tutorial_a`. An Echo Arena level that is loaded wins over any
 * other level still loaded during a transition. Each id is `EchoVR::CSymbol64Hash` of its name (combat_level_test checks it).
 *
 * Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include <cstdint>

namespace nevr_quest::combat::level {

// Every class the loaded gamespaces hold, at once, with the first level found of each (0 when none). One scan of the tables.
struct Loaded {
  bool combatMap = false;
  bool combatLobby = false;
  bool echoArena = false;
  std::uint64_t combatMapLevel = 0;
  std::uint64_t combatLobbyLevel = 0;
  std::uint64_t echoArenaLevel = 0;
};
//
// `inFlight` also reads the gamespaces still being loaded: CncaGame's init-fiber table (rows game+0x1ed8, count +0x1f08, the fiber's gamespace at
// +0x30). A gamespace enters the two tables above only after its level's resources are built, but the level is stored in it before, so a caller that
// acts while resources are built (the table-step modules) needs this source; the same rules apply to a level found there, an Echo Arena level
// winning wherever it is found. Callers that decide per frame on a level that is up (refresh_rate, pfx) leave it false and see what they always did.
Loaded Scan(std::uintptr_t game, bool inFlight = false) noexcept;

// What the loaded gamespaces decide: an Echo Arena level is kEchoArena and wins always; else a combat map (or, with `lobby`, the combat lobby)
// is kCombatOrLobby; anything else, every other lobby level included, is kNone.
enum class Kind { kNone, kCombatOrLobby, kEchoArena };

bool IsCombatMap(std::uint64_t level) noexcept;
bool IsCombatLobby(std::uint64_t level) noexcept;
bool IsEchoArenaLevel(std::uint64_t level) noexcept;

// What the loaded levels of `game` decide; `level` gets the deciding level's CSymbol64 (0 when none).
Kind Classify(std::uintptr_t game, bool lobby, std::uint64_t* level, bool inFlight = false) noexcept;

// The name of a level this file knows (a combat map, the combat lobby, an Echo Arena level, the other lobbies), or nullptr.
const char* LevelName(std::uint64_t level) noexcept;

// True when one of the game's gamespaces holds a combat map (never a lobby or arena level); `level` gets it (0 when none).
bool CombatMapLoaded(std::uintptr_t game, std::uint64_t* level) noexcept;

}  // namespace nevr_quest::combat::level
