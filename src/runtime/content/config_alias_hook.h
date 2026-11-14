#pragma once
// The runtime half of ConfigAlias (docs/adr/0048 decision 5): the detour on the balance-config lookup
// 0x140D0B170, the loaded levels read from game state, the reconcile that publishes core/config_alias.h's snapshot,
// and the optional refresh of the rule values the game caches. The hooks are installed only when a map staged an
// alias (nevr_config_alias::InstallIfStaged), so a run with no map is unchanged.
//
// Loaded-level sources:
// - A, primary: the game object's two level tables, the loaded-but-disabled one (+0x1F90) and the enabled one
//   (+0x1F50). EnableGameSpace moves a level from the first to the second, so a level is loaded at all while it is in
//   either.
// - B: detours on CGameLevelResource::Enable/Disable keep a set of enabled level resources (LevelInstances). Each
//   reconcile revalidates it. While A can be read, an entry survives only while the enabled table still lists that
//   resource under that level and the resource reads enabled, so B never adds a level A does not show. When A cannot
//   be read (no game object, or a table that does not parse), B is the source: an entry survives while its memory is
//   readable, still names its level and reads enabled.

#include <cstdint>

namespace nevr_config_alias_hook {

// A caller installs the main-level source. Until one is set, main_level reads 0 and only the loaded levels decide.
void SetMainLevelSource(std::uint64_t (*source)());

// When on, a change of mp_arena_rules' winner makes the next frame's reconcile call
// the game's own balance-settings update (0x1401B2A90) with the net game, so the zone values the game copied into its
// globals follow the live alias. Off by default: the lookup-site answer is what the game reads.
void SetCachedRulesRefresh(bool enabled);

// The per-frame tick's call (game thread only; elsewhere it returns at once). Reads the main level and the loaded
// levels, and calls Reconcile when they, the session, B's events or the armed maps changed since the last reconcile.
// It runs a refresh that is due. It may allocate and catches std::exception: on failure the old snapshot stays and
// `config_alias reason=reconcile_failed` is logged once.
void ReconcileIfFrameMoved() noexcept;

// The detour itself, exposed for the test: an allowed key is answered by its alias's record while one is live, and
// by the stock key's record when the alias's is absent. Every other key goes to the game's lookup untouched. On the
// game thread it reconciles first, so a lookup in the frame a level loads sees it; it never calls the refresh (the
// refresh reads through this lookup), it only marks one due for the next tick. While the state stamp is unchanged that
// costs a lock-free stamp check; only a moved stamp (a level load, rare) takes the lock and allocates.
using LookupFn = void* (*)(std::uint64_t key);
void* LookupDetour(std::uint64_t key);

// Source B's detours: the game's result is returned unchanged; a zero result records the resource as enabled or
// drops it.
using LevelResourceFn = std::int64_t (*)(void* level_resource);
std::int64_t EnableDetour(void* level_resource);
std::int64_t DisableDetour(void* level_resource);

using RefreshFn = std::int64_t (*)(void* net_game);

#ifdef NEVR_CONFIG_ALIAS_HOOK_TESTING
void SetOriginalForTesting(LookupFn original);
void SetLevelResourceOriginalsForTesting(LevelResourceFn enable, LevelResourceFn disable);
void SetRefreshForTesting(RefreshFn refresh);
void ResetForTesting();
#endif

}  // namespace nevr_config_alias_hook
