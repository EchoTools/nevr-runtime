/* What level the game has loaded: the shared answer the combat modules ask (#2172, docs/adr/0046), so no module reads the game's gamespace tables
 * for itself. Arena and combat run on the same APK, so `combat::Active()` does not say which one a player is in; this does.
 *
 * What it reads. CR15Game's two gamespace tables ({sym, gs*} rows of 16 bytes; counts at 0x1f78 and 0x1fb8; offsets 0x1f48/0x1f88) and, in each
 * gamespace of the game (gs+0x8 is the game), its level CSymbol64 at gs+0x58. The pinned build's offsets; the q15t plugin's `refresh_rate.cpp`
 * carried them. Reads use process_vm_readv (safe_read.h): an unreadable game is no level (fail closed).
 *
 * The three classes: a combat map (`mpl_combat_dyson`, `_combustion`, `_fission`, `_gauss`), an Echo Arena level (`mpl_arena_a`,
 * `_a_lowspec`, `_b`, `mpl_tutorial_a`), and a social-lobby level (`mpl_lobby_b`, `_b2`, `_b2_lowspec`, `_b_combat`, `_b_arena`, `_b_social`). An Echo Arena
 * level that is loaded wins over any other level still loaded during a transition. Each id is `EchoVR::CSymbol64Hash` of its name
 * (combat_level_test checks it).
 *
 * Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include <cstdint>

namespace nevr_quest::combat::level {

// What the loaded gamespaces decide: a combat map (or, with `lobby`, a social-lobby level) is kCombatOrLobby; with `lobby`, an Echo Arena level
// is kEchoArena and wins; anything else is kNone.
enum class Kind { kNone, kCombatOrLobby, kEchoArena };

bool IsCombatMap(std::uint64_t level) noexcept;
bool IsLobbyLevel(std::uint64_t level) noexcept;
bool IsEchoArenaLevel(std::uint64_t level) noexcept;

// What the loaded levels of `game` decide; `level` gets the deciding level's CSymbol64 (0 when none).
Kind Classify(std::uintptr_t game, bool lobby, std::uint64_t* level) noexcept;

// True when one of the game's gamespaces holds a combat map (never a lobby or arena level); `level` gets it (0 when none).
bool CombatMapLoaded(std::uintptr_t game, std::uint64_t* level) noexcept;

}  // namespace nevr_quest::combat::level
