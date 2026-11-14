// ConfigAlias on Quest (docs/adr/0048 decision 5): a map's arena rules are read under its own key while any of its levels
// is loaded, over the core (core/config_alias.h). The twin of src/runtime/content/config_alias_hook.h.
//
// The lookup is NRadEngine::NRadGame::CR15NetBalanceSettingsCS::Json(CSymbol64) (libr15 0x210f2d0, 30 instructions): a binary
// search of the balance table's 0x18-byte records that returns record+8, else a tail call to CJsonConfigCS::Json(key)
// (0x15927fc), which returns 0 when it holds no record. Every caller (ten, among them CR15NetLobby::RefreshCachedMatchSettings
// and CR15NetGame::SettingsUpdateCB) reaches it through libr15's PLT stub 0xed60d0, so the hook is the JUMP_SLOT at 0x36c2750.
//
// Loaded levels: source A only, both game-space tables of the game object (net game +8), as the PC hook reads them. Quest's
// CncaGame::EnableGameSpace (0x17dc124) moves a space's {level, space} entry from the table at +0x1F88 (count +0x1FB8) to the
// table at +0x1F48 (count +0x1F78); DisableGameSpace (0x17dc424) moves it back. A level is loaded while it is in either.
// Source B (the Enable and Disable detours) and the cached-rules refresh are not built on Quest: the loaded levels come from
// the game-space tables alone, so a level those tables do not show is not seen.
//
// With nothing staged, InstallIfStaged installs nothing, so a run with no map is unchanged.
#pragma once

#include <cstdint>

namespace nevr_quest::config_alias_hook {

// The caller installs the session level (the owner-JSON level, else the main-level symbol). Until one is set, main_level
// reads 0, and only the loaded levels decide.
void SetMainLevelSource(std::uint64_t (*source)() noexcept) noexcept;

// Game thread only: when the main level, the table generation or the game-space tables changed since the last call, reads the
// loaded levels and calls Reconcile. Catches std::exception: on failure the old snapshot stays and `reason=reconcile_failed` is
// logged once. Runs as frame work every tick, and from the lookup on the tick thread.
void ReconcileIfFrameMoved() noexcept;

// The most levels both game-space tables of `game` (a CncaGame) can list: the sum of their counts, or -1 when a table is not
// readable. The reader sizes its buffer with it.
int LoadedLevelUpperBound(const void* game) noexcept;

// The levels in both game-space tables of `game`, into out[cap]. Returns the count, or -1 when a table is not readable (a null
// data pointer with a count, or a count over the table's own capacity word, core/game_space_table.h). Tests drive it with a
// fake game object.
int ReadLoadedLevels(const void* game, std::uint64_t* out, int cap) noexcept;

// The lookup with the alias applied; `original` is the game's function (or a test double).
using LookupFn = void* (*)(std::uint64_t key);
void* RunLookupForTest(LookupFn original, std::uint64_t key) noexcept;

}  // namespace nevr_quest::config_alias_hook
