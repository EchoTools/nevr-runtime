/* The static BVH (#2182, docs/adr/0046; docs/quest/combat/bvh.md): the Quest's CBVHTree is stubbed, so no cast against the level's static geometry
 * ever hits. This module gives it a real static raycast tree, built at run time from the game's own CBVHResource (the SBVHTreeData that
 * CStaticRaycastCS passes to CBVHTree::Initialize), so walls block line of sight, bullets and AI. No game-derived bytes are stored anywhere.
 *
 * Six hooks (bvh_targets.h), each a CallbackThunk installed by the hook manager; no code page is written:
 *   Initialize  CBVHTree::Initialize(this, SBVHTreeData*)       the game's function runs, then the data is parsed and a Tree* kept in the 8-byte CBVHTree
 *   Shutdown    CBVHTree::Shutdown(this)                         the game's function runs, then the Tree* is cleared
 *   Intersect   CBVHTree::Intersect(this, CRay*, SBVHHit*)       nearest hit, for the casts the gate allows
 *   Occluded    CBVHTree::Occluded(this, C3Vector*, C3Vector*)   any hit on the segment
 *   Gate        CncaGameSpace::StaticRaycast(this, CRay*, SBVHHit*, CncaGameSpaceMT**)   the callers' gate: marks the thread while a cast the tree answers runs
 *   Touch       CR15TouchInteractQuery::RayQueryStatic(...)      answers a miss, as the stock Quest does
 * The four tree hooks are installed together or not at all, and the gate with them unless it is switched off; the touch hook is separate (its
 * refusal is an Error and the module goes on).
 *
 * Switches (docs/adr/0046 C2): `combat.bvh` (default true, as the reference build has it: afk_bvh.cpp "ON BY DEFAULT"; the answers are scoped to combat maps),
 * `combat.bvh_gate` (true: only the three listed callers' casts use the tree) and `combat.bvh_touch` (true: touch rays are answered with a miss).
 * The plugin's kill files `bvh.off`, `bvhgate.off`, `bvhtouch.keep` are no longer read: one found in the plugin's data directory is a Warning
 * (`combat_bvh_legacy_switch`) that names the key that replaces it.
 *
 * Owner (ADR 0039). This file owns the six slots. The q15t plugin no longer installs any of them.
 *
 * Which APKs (ADR 0046 rule 5). The q15t and combat variants only (the normal step of combat_install.h).
 *
 * Arena (same APK). Arena and combat run on the same APK, so `combat::Active()` does not say which one a player is in. Every query hook answers
 * the game's own function while no combat map is loaded: `InCombatMap()` below is a value the game-thread tick refreshes (4 times a second, from
 * combat_level.h `Scan`), so the arena path of a query is one relaxed load, a test and a tail call of the game's function. Trees are built at load
 * whatever the level (the build is a parse of the game's own data and a table entry, no copy; the arena levels ship empty resources, which log
 * `combat_bvh_tree` `none`), so this module does not depend on the level being in combat_level's tables while it loads; only the answers are scoped.
 * KNOWN-WRONG #2246: the scope here is this module's own cached answer. combat_scope.h (F19) is the shared one; this module switches to it when
 * it lands, and the value then also fails closed when the tick stops (this one does not: with no frames there are no casts).
 *
 * Reads. A reference argument the game's caller formed (the ray, the segment's two points, the hit, this) is read with plain loads: the
 * caller dereferences the same objects, and a system call (process_vm_readv) per cast would cost more than the traversal. The game's resource
 * (the header and both blocks, which nothing else reads before us) is probed once, when the tree is built, with process_vm_readv (safe_read.h).
 *
 * Frequency (ADR 0038). Initialize and Shutdown run once per level load (a handful in a session). Intersect and Occluded run once per cast the
 * game makes against its static level: by the AI's patrol update and by bullets (the three callers in bvh_targets.h) and, with the gate off, by every
 * cast. Gate runs once per StaticRaycast, Touch once per touch-interaction ray (measured on a headset: about 40 to 120 a second in the logged
 * combat session, all answered as misses). Measured per call on the host: docs/quest/combat/bvh.md "Cost".
 *
 * Origin. The reference build's runtime/native/afkxr/afk_bvh.cpp (slot table :93-99, allowed call sites :103-110, gate :38-57, tree hooks) and
 * afk_bvh_core.h (the format, bvh_core.h), reconstructed on the manager; the commit is in docs/audits/2026-11-05-q15t-origin-citations.md. The
 * module is Ducky's: every address here is in upstream's afk_bvh.cpp.
 *
 * Lines (sentinel log):
 *   switch    one {"event":"combat_config_switch","module":"bvh|bvh_gate|bvh_touch","key":"combat.bvh|..","on":0|1,"source":"default|config"} per key, generated from the
 *             registration table by the configuration step (combat_install.h); the module writes none
 *   install   {"event":"combat_bvh_install","result":"installed|off|refused","reason":"..","hooks":<n>,"gate":0|1,"touch":0|1,"allowed_casts":<n>,"msg":".."}
 *             Info for installed and off (`reason` `off_config`), Error for a refusal (`reason` is the cause, `hook` the one that stopped it).
 *   tree      {"event":"combat_bvh_tree","result":"built|none|refused","why":"..","data":"0x..","triangle4":<n>,"nodes":<n>,"root_ref":"0x.."}
 *   hit       {"event":"combat_bvh_first_hit",...} and {"event":"combat_bvh_first_block",...}  once each: the first answered hit and blocked segment
 *   summary   {"event":"combat_bvh_summary","window_s":10,"intersect":..,"occluded":..,..,"us_per_cast_avg":"..","us_per_cast_max":".."}  every 10 s a counter moved
 *   scope     {"event":"combat_bvh_scope","from":0|1,"to":0|1,"level":"0x.."}  Info, one per change of the answer
 *   legacy    {"event":"combat_bvh_legacy_switch","file":"..","replaced_by":".."}  Warning
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "quest/combat/bvh_targets.h"
#include "quest/combat/combat_install.h"
#include "quest/sentinel/got_hook.h"

namespace nevr_quest::combat::bvh {

namespace detail {
// Whether a combat map is loaded and no Echo Arena level is, as of the last look. Written by the tick's callback, read by any thread.
inline std::atomic<bool> g_inCombatMap{false};
}  // namespace detail

// True while a combat map is loaded and no Echo Arena level is. One relaxed load.
inline bool InCombatMap() noexcept { return detail::g_inCombatMap.load(std::memory_order_relaxed); }

// The `combat:` keys the step reads.
struct Switches {
  bool bvh = true;  // combat.bvh
  bool gate = true;  // combat.bvh_gate
  bool touch = true;  // combat.bvh_touch
};

// The step on the loaded libr15. Fixed status tokens: "installed", "off_config", "already_ran", "module_not_loaded", "no_thread_key",
// "tick_table_full", or the manager's status name of the hook that was refused (the tree is then not installed).
ModuleResult Install(const Switches& switches) noexcept;
// The same on the image `lookup` finds, with `pins` (kPins in production).
ModuleResult InstallAt(sentinel::ImageLookup lookup, const Pins& pins, const Switches& switches) noexcept;

// True once the four tree hooks are installed: what wallfire depends on.
bool TreeInstalled() noexcept;

struct Counters {
  std::uint64_t treesBuilt;     // Initialize calls that produced a tree
  std::uint64_t treesRefused;   // Initialize calls that did not (no geometry, bad sizes, unreadable, table full)
  std::uint64_t intersect;      // casts answered from a tree
  std::uint64_t intersectHit;
  std::uint64_t occluded;
  std::uint64_t occludedBlocked;
  std::uint64_t gated;          // casts kept off the tree (the stock miss)
  std::uint64_t touch;          // touch rays answered with a miss
  std::uint64_t stock;          // calls that went to the game's function with a combat map loaded (no tree for the object)
};
Counters GetCounters() noexcept;
int TreeCount() noexcept;  // entries in use in the tree table (cap 64)

// The combat tick's callback (combat_tick.h): refreshes `InCombatMap()` and writes the 10 s summary. One clock compare per frame otherwise.
void Tick(std::uintptr_t game) noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

// The image lookup Install uses; nullptr restores sentinel::FindLoadedImage.
void SetImageLookupForTest(sentinel::ImageLookup lookup) noexcept;
// The pins Install uses; nullptr restores kPins.
void SetPinsForTest(const Pins* pins) noexcept;
// The clock the tick reads, in seconds; nullptr restores CLOCK_MONOTONIC.
void SetClockForTest(double (*clock)() noexcept) noexcept;
// The return address (an ELF vaddr) of the last StaticRaycast the gate saw while a combat map was loaded.
std::uintptr_t LastGateReturnForTest() noexcept;
// The switches the last Install was given (the registration table's row reads them from the configuration).
Switches LastSwitchesForTest() noexcept;
// Forgets the hooks, the thunks, the trees, the flags and the counters.
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::bvh
