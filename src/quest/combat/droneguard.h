/* The drone's follow update is skipped while its target is null, instead of crashing the game (#2178; the q15t plugin's droneguard, moved here).
 *
 * What the game does. CR15NetFollowPlayerCS::Update calls Follow(SCompactPoolHandle) once per enabled component, through its PLT stub (the one
 * caller). Follow loads [cs+0x140], the CR15PlayerNavCS system the component follows, and passes it as `this` to CR15PlayerNavCS::FinalTransform
 * with no null check; the drone's actor is enabled before any orbit target exists, so the first updates can fault (droneguard_targets.h).
 *
 * What this does. Stage 0 (the null half): the handler returns while [cs+0x140] is null and calls the game's Follow with the same two arguments
 * otherwise; plus a read-only `combat_droneguard_state` line on a change of the followed player's cache (at most 16). Stage 1 (the idx guard, off
 * unless `combat.droneguard_stage1` is true): before Follow, the engine's own CComponentHandle<CR15PlayerNavCS>::RefreshCache(cs+0x148), then the
 * update is skipped when the cached index is unresolved (u16 [cs+0x190] == 0xffff), the cached system [cs+0x188] is 0, or
 * CR15PlayerNavCS::IsComponentValid(target, idx) refuses the cached index. A read that fails is doubt: Follow runs. There is no access-violation
 * handler: a non-null but stale pointer still faults, as in the reference build.
 *
 * Arena (same APK). This is a crash-only guard and it acts in every level on purpose; it asks nothing of the combat level answer (combat_scope.h),
 * so it is not a reader of the scope row. The fault happens when the drone's actor is enabled, which can be in the first frames of a level load, before a
 * level look that is up to a quarter of a second old would see the level. What it changes: only a call whose target is null (stage 0), or whose cache is stale (stage 1), is skipped for that component that frame,
 * where the stock call would dereference null; every other call reaches the game's Follow with the same two arguments. Whether the arena
 * has CR15NetFollowPlayerCS components at all is not established; `combat_droneguard_stats` gives the call count on a headset.
 *
 * Which APKs (docs/adr/0046 rule 5). It runs where combat::Active() is true: the q15t and combat variants. Its switches are `combat.droneguard` (default
 * true) and `combat.droneguard_stage1` (default false; read only when the first is on) in config.yaml. The plugin had no switch file, and its build option
 * for the second stage is gone.
 *
 * Install. The patched build ID, then the stage-0 code words (droneguard_targets.h), then the JUMP_SLOT, which must hold Follow itself (a word something
 * else already replaced is refused, not chained over). Stage 1 is tried after that: its own code words and the two engine functions (resolved by dlsym on
 * the loaded game library, each at exactly its pinned address) must all hold, else the null half stays alone and one Error `combat_droneguard_stage1`
 * line names why. It registers in combat::InstallNormal()'s table at the position the plugin installed it (after custctx, before peerclamp).
 *
 * Frequency (ADR 0038). Follow runs once per enabled follow component per frame. Every call pays the thunk's counter, four plain loads from the component (Follow loads [cs+0xc8] as its first data access, so the
 * game dereferences the same object on every call; a null component goes to the game's function) a compare of the state key and, for a call with a target, one relaxed load of the open-run count (0 unless a run is open: the run tables are scanned only while it is above 0, which is while a component is skipped and for at most kRunStaleSeconds after a run whose component died, because the scan releases stale slots); stage 1 adds a RefreshCache call, a
 * never-faulting read of the refreshed cache and, for a resolved player, an IsComponentValid call. Host numbers are
 * in docs/quest/combat/droneguard.md; not measured on a headset.
 *
 * Owner (ADR 0039). This file owns whether Follow runs for a component whose target is null (stage 0) or whose cache is stale (stage 1).
 *
 * Origin. The q15t plugin's `droneguard.cpp` (imported in #1591), written on the reference build's `afk_droneguard.cpp` (Ducky's echoquest-combat;
 * docs/audits/2026-11-05-q15t-origin-citations.md rows 34-36 and 149-150: afk_droneguard.cpp:1,42-48,43-44,46-60,57-58,66-80,76-77,86-99,108-124,126-146).
 *
 * Lines (sentinel log):
 *   install   {"event":"combat_droneguard_install","result":"installed|refused|off","reason":"ok|off_config|<token>","hook":<GotStatus>,
 *              "stage1":"off|on|null_half_only","consequence":".."}  Info for installed and off, Error for refused (+ pinned_word_mismatch: "pin","found","expected")
 *   stage1    {"event":"combat_droneguard_stage1","result":"on|null_half_only","reason":"ok|pinned_word_mismatch|engine_functions_missing",...}  Info / Error
 *   state     {"event":"combat_droneguard_state","target":"set|0","handle_sys":"set|0","idx_unresolved":"yes|no","raw190":"%08x","flags16c":"%02x","cs":"0x.."}
 *             Info, on the first call and on every change of (index word, flags byte, target null, handle system null), at most kStateLines
 *   skip      {"event":"combat_droneguard_skip","cs":"0x..",...}  Info, the first skip of a component's run of skips (a run belongs to the component; a run holds one of kRunSlots
 *             slots from its first skip until its target arrives, or until a later run needs a slot while it is older than kRunStaleSeconds: the game gives no signal
 *             that a component died, so such a run is dropped and counted in runs_stale, and a table full of fresh runs writes one Warning
 *             combat_droneguard_run_table_full and counts each refused run in run_table_full), at most kRunLines in a process; {"event":"combat_droneguard_skips","skips":<n>}  Info, every
 *             kTotalEvery skips, at most kTotalLines; {"event":"combat_droneguard_target_set","skips":<n>}  Info, when a component's target arrives after skips, at most kRunLines in a process
 *   idx skip  {"event":"combat_droneguard_idx_skip","why":"idx ffff|handle-sys 0|refused but cached","cs":"0x.."}, {"event":"combat_droneguard_idx_skips","idx_skips":<n>},
 *             {"event":"combat_droneguard_idx_resolved","idx_skips":<n>}  Info, as above
 *   cap       {"event":"combat_droneguard_log_cap","lines":"state|skip|skips|target_set|idx_skip|idx_skips|idx_resolved","cap":<n>,"msg":".."}  Warning, once per
 *             kind, written by the first line over its cap (line_budget.h); the counts go on in the stats line
 *   stats     {"event":"combat_droneguard_stats","calls":<n>,"skips":<n>,"idx_skips":<n>,"runs":<n>,"target_set":<n>,"runs_stale":<n>,"run_table_full":<n>,"final":0|1,"consequence":".."}  Debug every kStatsEvery calls, Info once at
 *             the end of the process when the guard is installed
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/combat/combat_install.h"
#include "quest/combat/droneguard_targets.h"
#include "quest/sentinel/got_hook.h"

namespace nevr_quest::combat::droneguard {

inline constexpr unsigned kStatsEvery = 16384;  // a Debug stats line every this many calls (a power of two)
inline constexpr unsigned kTotalEvery = 600;    // a skip total every this many skips
inline constexpr unsigned kTotalLines = 8;      // at most this many totals of each kind
inline constexpr unsigned kStateLines = 16;     // at most this many state lines
inline constexpr unsigned kRunSlots = 16;       // components with a run open at once, per table (null-target runs, stage-1 runs)
inline constexpr unsigned kRunStaleSeconds = 30;  // a run older than this is stale: its slot may be reclaimed and its target-set line is not written
inline constexpr unsigned kRunLines = 8;        // at most this many lines of each run kind (skip, target_set, idx_skip, idx_resolved) in a process

// The two words the install pins: the slot and the function it must hold. The host test supplies a stand-in's address.
struct Pins {
  std::uint64_t slot, fn;
};
inline constexpr Pins kPins = {kFollowSlotVaddr, kFollowVaddr};

// True when `refresh` and `valid` (what dlsym answered) are exactly at base + their pinned vaddrs; then `*out` holds them.
bool EngineExportsMatch(std::uintptr_t base, const void* refresh, const void* valid, EngineFns* out) noexcept;
// dlsym on the loaded game library (RTLD_NOLOAD), then EngineExportsMatch. False when the library or a symbol is missing or at another address.
bool ResolveEngineFns(std::uintptr_t base, EngineFns* out) noexcept;

// Installs on the libr15 `lookup` finds. `on` is combat.droneguard, `stage1` combat.droneguard_stage1 (tried only when `on`); `fns` are the two engine
// functions stage 1 calls (nullptr: stage 1 stays off with the reason engine_functions_missing). Logs one combat_droneguard_install line. A module that is
// off by its switch answers `off` (status "off_config"). installed with stage 1 asked and not available answers status "installed_null_half".
ModuleResult InstallAt(sentinel::ImageLookup lookup, const Pins& pins, bool on, bool stage1, const EngineFns* fns) noexcept;

// The table's row: InstallAt on the loaded libr15 with the production pins and, for stage 1, the engine functions dlsym finds.
ModuleResult Install(bool on, bool stage1) noexcept;

struct Stats {
  std::uint64_t calls;      // every Follow call that reached the handler (the thunk's counter)
  std::uint64_t skips;      // skipped because the target was null
  std::uint64_t idxSkips;   // skipped by stage 1
  std::uint64_t runs;       // null-target runs begun (a component's first skip after a call with a target)
  std::uint64_t targetSets; // runs ended by the target arriving inside the stale window (each is a target_set event)
  std::uint64_t runsStale;  // runs older than kRunStaleSeconds, dropped without a line (either table)
  std::uint64_t runTableFull;  // runs that found no slot and no stale one to take (either table)
};
Stats CurrentStats() noexcept;

// The table's end-of-process step: writes the final combat_droneguard_stats line when the guard is installed, nothing otherwise.
void NoteEnd() noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

// How many times a call with a target scanned a run table (0 while no run is open).
unsigned RunScansForTest() noexcept;
// The clock the stale rule reads, in milliseconds (production: CLOCK_MONOTONIC). nullptr restores production; ResetForTest also does.
void SetClockForTest(std::int64_t (*now)() noexcept) noexcept;

// How the install finds libr15 (production: the loaded image). nullptr restores production.
void SetImageLookupForTest(sentinel::ImageLookup lookup) noexcept;
// Forgets the hook, clears the thunk, the counters, the logged-once state and stage 1's functions.
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::droneguard
