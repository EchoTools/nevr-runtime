/* Three guards on the lobby cosmetics renderer (CR15NetCustomizationRenderer), installed all or none (#2178; the q15t plugin's custctx, moved here).
 *
 * What the game does and what each guard does (store libr15; custctx_targets.h has the addresses).
 *  R31 LoadMeshData: loads one player's cosmetics into a context. The guard marks the context live and "load in flight" (under one mutex) before the
 *      game's function runs. It refuses a load, answering 1 (the stock failure value: CreateContext then destroys the empty context and answers NULL),
 *      when the render-target layout pool has fewer than kLayoutMargin (64) free slots, because a full pool is a deliberate trap (brk) in
 *      CGRenderTargetState::Initialize on the load's new mesh entries. An unreadable or null pool pointer never refuses.
 *  R32 DestroyContext: the guard runs the game's destroy only for a live context with no load in flight and no destroy already running. Every other
 *      destroy is dropped (answer 0): the destroy of a context being loaded, being destroyed on another thread, or at an address no live context holds
 *      (a stale second destroy). A null context goes to the game's function, which returns at once. More than kMax (256) contexts at once: the
 *      overflow is untracked and gets the game's behaviour, one Warning.
 *  R33 CTableXT<SR15NetCustomDrawData>::Grow: the guard does Grow's resize itself (CMemBlock::Resize, called by its exported address, keep 1) and checks
 *      it. A table that would be written through NULL (the resize failed, an add of 0, or room left but the data NULL) is made a consistent empty one and
 *      Grow is given a spare element that is in no table: that player's cosmetics are not drawn. The spare is a fresh 64-aligned element, or, when that
 *      allocation fails, one of kSpares allocated at install, used round robin, so Grow never gets a NULL element.
 *
 * All or none. The three hooks go in one after the other; when one is refused (the manager has already rolled its own failed install back) the hooks
 * before it are removed with the manager's Remove and every thunk is disarmed, so no slot stays swapped. The install line says which hook was refused.
 *
 * Which APKs (docs/adr/0046 rule 5). It runs where combat::Active() is true: the q15t and combat variants. Its switch is `combat.custctx` in config.yaml
 * (default true). The plugin's `custctx.off` file is no longer read; one found in the files folder's q15t directory is a Warning that names the key.
 *
 * Arena (same APK). It acts in EVERY level, on purpose: it does not use level_scope.h. It guards code arena shares (the lobby, where cosmetics load),
 * and its three faults are crashes (a destroy racing a load, a draw table written through NULL, a trap on a full layout pool); a scope by level would
 * leave the lobby unguarded. It changes arena's result in one place: LoadMeshData is refused, and that player's cosmetics are skipped, when the
 * layout pool has fewer than 64 free slots, up to 63 slots before the game's own function would trap; in arena that happens only in a state where a
 * nearby load would crash the process. Everything else passes the game's own arguments to the game's own function. Cost in arena: per cosmetic load (per
 * player join or lobby change): one mutex lock and unlock pair around a scan of the 256-entry table, three never-faulting reads for the pool state;
 * per destroy: the same lock and scan; per draw-table Grow: one read of the table header. The rates are not known: `combat_custctx_stats` counts them
 * (ADR 0038); host numbers per call are in docs/quest/combat/custctx.md. Not measured on a headset.
 *
 * Install. The patched build ID, then the code words (custctx_targets.h), then the Resize export (looked up with dlsym on the loaded libr15 and accepted
 * only at exactly base + its pinned address), then the install-time spares, then the three JUMP_SLOTs, each of which must hold its function itself.
 * It registers in combat::InstallNormal()'s table at the position the plugin installed it (after voipdrop).
 *
 * Owner (ADR 0039). This file owns whether a destroy, a load and a draw-table growth of the cosmetics renderer reach the game's function.
 *
 * Origin. The q15t plugin's `custctx.cpp`, written here (imported in #1591) on the reference build's `afk_custctx.cpp` (Ducky's echoquest-combat;
 * docs/audits/2026-11-05-q15t-origin-citations.md rows 29-33 and 148: afk_custctx.cpp:1,63-65,73-95,100-101,168-285 and afk_tap.cpp:174). The plugin's
 * v4 layout-pool refusal and its 34 code-word pins are additions to the source's.
 *
 * Lines (sentinel log):
 *   install  {"event":"combat_custctx_install","result":"installed|refused|off","reason":"ok|off_config|<token>","load":..,"destroy":..,"grow":..,
 *             "layout_margin":64,"consequence":".."}  Info for installed and off, Error for refused (+ `which` and the hook's status for slot_refused,
 *             `rollback` the status of the first removal; pinned_word_mismatch: "pin","found","expected")
 *   loads    {"event":"combat_custctx_loads","loads":<n>,"layouts_used":<n>,"layouts_capacity":<n>}  Info at every 32nd load, at most 32 lines
 *   refused  {"event":"combat_custctx_load_refused","layouts_used":..,"layouts_capacity":..,"refused":<n>,"consequence":".."}  Info for the first 16 refusals
 *            and every 50th
 *   dropped  {"event":"combat_custctx_dropped","ctx":"0x..","why":"..","dropped":<n>,"consequence":".."}  Info, the first 16; then
 *            {"event":"combat_custctx_drops_total","dropped":<n>}  Info at every 50th drop, at most 16
 *   draw     {"event":"combat_custctx_draw_failure","entries":..,"bytes":..,"in_use":..,"why":"..","failure":<n>,"consequence":".."}  Warning, the first 16;
 *            then {"event":"combat_custctx_draw_failures_total","failures":<n>} Warning at every 50th, at most 16
 *   full     {"event":"combat_custctx_full","max":256,"consequence":".."}  Warning, once
 *   stats    {"event":"combat_custctx_stats","loads":..,"loads_refused":..,"destroys_dropped":..,"grow_failures":..,"final":0|1,"consequence":".."}
 *            Debug every kStatsEvery loads (a power of two), Info once at the end of the process when the guards are installed
 *   legacy   {"event":"combat_custctx_legacy_switch","key":"combat.custctx","file":"custctx.off","consequence":".."}  Warning
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/combat/combat_install.h"
#include "quest/combat/custctx_targets.h"
#include "quest/sentinel/got_hook.h"

namespace nevr_quest::combat::custctx {

inline constexpr unsigned kStatsEvery = 1024;  // a Debug stats line every this many loads (a power of two)

// The engine function the guard calls: CMemBlock::Resize(block, bytes, keep).
struct Fns {
  void (*resize)(void* block, std::uint64_t bytes, std::uint32_t keep);
};

// True (and `out` filled) only when `resize` is exactly at base + pins.fnResize (the address the pinned word table was derived at).
bool ResizeMatches(std::uintptr_t base, const Pins& pins, const void* resize, Fns* out) noexcept;

// Installs on the libr15 `lookup` finds. `fns` is Resize (nullptr: dlsym on the loaded libr15, checked by ResizeMatches; a Fns with no function is a
// missing export). `on` is combat.custctx; Logs one
// combat_custctx_install line. A module that is off by its switch answers `off` (status "off_config").
ModuleResult InstallAt(sentinel::ImageLookup lookup, const Pins& pins, const Fns* fns, bool on) noexcept;

// The table's row: InstallAt on the loaded libr15 with the production pins, the production Resize lookup and the plugin's data directory.
ModuleResult Install(bool on) noexcept;

struct Stats {
  unsigned loads;             // LoadMeshData calls that reached the handler
  unsigned loadsRefused;      // refused for a nearly full layout pool
  unsigned destroysDropped;   // destroys not passed on
  unsigned growFailures;      // draw tables made empty with a spare element
};
Stats CurrentStats() noexcept;

// The table's end-of-process step: writes the final combat_custctx_stats line when the guards are installed, nothing otherwise.
void NoteEnd() noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

// How Install finds libr15 (production: the loaded image). nullptr restores production.
void SetImageLookupForTest(sentinel::ImageLookup lookup) noexcept;
// What the install's lookup of the Resize export answers (production: dlsym on the loaded libr15). nullptr restores production.
void SetSymbolLookupForTest(const void* (*lookup)(const char* symbol)) noexcept;
// The allocator the spare elements come from (nullptr: posix_memalign).
void SetMemalignForTest(int (*memalign)(void**, std::size_t, std::size_t)) noexcept;
// Forgets the hooks, clears the thunks, the live-context table, the counters and the log budget.
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::custctx
