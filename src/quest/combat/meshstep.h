/* The mesh-list resource's tables grow by 1, not 32 (docs/quest/combat/meshstep.md), in a combat match only.
 *
 * What the game does. The constructor of NRadEngine::CGMeshListResource (ELF 0x18677cc, 516 bytes) gives each of its eleven CTableXT members
 * a growth step of 32: one `mov w20,#0x20` (ELF 0x186784c) is the source of the eleven stores at block + 0x20. Its callers reach it through
 * a PLT stub that loads the constructor from the JUMP_SLOT at ELF 0x36e5548.
 *
 * What this does. A thunk on that JUMP_SLOT (meshstep_targets.h), installed by the hook manager. While a combat map is loaded (table_step.h
 * `Scope::kCombat`) the handler builds the object itself, a C++ reconstruction of the constructor that stores 1 where the game stores 32: the
 * same stores and the same four helper calls in the stock order, with the two vtable words read from the game's data pages at install time
 * the way the stock code reads them on each call. In every other scope (an Echo Arena level, a lobby, no game verified yet) it calls the
 * game's own constructor and the object is stock. The reconstruction never calls the game's constructor, so a hook older on the slot is not
 * reached from a combat build; nothing is older than this one.
 *
 * Arena (docs/adr/0046). Arena and combat run on the same APK. What changes for an arena match: nothing the player sees or the game
 * computes; the constructor is the game's own. What it costs there: each construction of a mesh list asks the loaded levels once
 * (`table_step::CurrentScope`, seven process_vm_readv calls with one gamespace loaded, four for the two tables and three for each gamespace, plus two for the in-flight table and three for each fiber in it) and calls the game's constructor; it runs once per mesh list the game
 * builds, on a loading thread, never per frame. The constructions an arena level costs are counted per level
 * change (`combat_meshstep_counts`); not yet read on a headset.
 *
 * When. The normal step (combat_install.h), after the plugin pass. The JUMP_SLOT of a BIND_NOW image is filled before any constructor runs and
 * must hold base + the constructor; a slot another hook took first joins the manager's chain (got_hook.h).
 *
 * Which APKs. The combat variants (combat::Active()); every other APK keeps the game's constructor.
 *
 * Switch. `combat.meshstep` (default true, as upstream's shipped build turns `--resource-table-step-1` on; this replacement of the constructor has not run on a headset, the plugin's copy having always refused) in config.yaml (docs/adr/0046 rule 4). The plugin's `meshstep.off` is not read; one found in the
 * plugin's data directory is a Warning naming the key.
 *
 * Pinned code. Before the thunk goes in, the 33 code words of meshstep_targets.h are read and compared; a difference refuses the module with
 * nothing hooked.
 *
 * Frequency (ADR 0038). Once per CGMeshListResource the game constructs. The hook is not on a per-frame path. A construction costs the scope read,
 * eleven CMemBlock constructions the stock constructor makes as well, and two counter updates; the log is written after the work, twice per
 * process.
 *
 * Owner (ADR 0039). This file owns the growth step of the mesh-list tables. The reference build writes the same constant as a code-word row
 * (docs/quest/combat/heaps.md section 3); that row is not in this repository's APKs.
 *
 * Origin. The game's own code, read in the disassembly (docs/quest/combat/meshstep.md), and the reference build's upstream patch: `tools/re3/res_heap_patch.py`
 * with `--table-step-1` (RE-10's padding finding, wave 8; commit 35bba9f of kotorvr/echoquest-combat) writes `mov wN,#1` over the `mov wN,#0x20` that gives the CTableXT growth step
 * its 32 in the CGShaderSetResource, CGInstancedModelResource and CGMeshListResource constructors, at these addresses less 0x100000. This module does
 * the same change as a hook on the constructor's slot, in a combat match only (docs/quest/combat/heaps.md section 3 cites the patch). It came with the combat
 * plugin (#1595), whose host-test harness and checked-pin style it follows.
 *
 * Lines (sentinel log):
 *   install  {"event":"combat_meshstep_install","result":"installed|refused|off","reason":"ok|off_config|<token>","status":"<GotStatus>|..",
 *             "slot_vaddr":"0x..","consequence":".."}   Error for a refusal, Info otherwise. A pinned word that differs is a Warning
 *             `combat_meshstep_pin` with `vaddr`, `have` and `want` first.
 *   built    {"event":"combat_meshstep_built","scope":"combat","growth_step":1,"stock_growth_step":32,"msg":".."}   Info, once
 *   stock    {"event":"combat_meshstep_stock","scope":"other|unknown","msg":".."}   Info, once: the game's own constructor ran
 *   counts   {"event":"combat_meshstep_counts","level":"0x..","scope":"combat|other|unknown","left_alone":<n>,"changed":<n>,"msg":".."}   Info, one per level
 *            change when there is anything to report: the objects built under the level that ended (table_step.h RegisterCounts)
 *   legacy   {"event":"combat_meshstep_legacy_switch","file":"meshstep.off","replaced_by":"combat.meshstep: false","msg":".."}   Warning
 *
 * Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/combat/combat_install.h"
#include "quest/combat/meshstep_targets.h"
#include "quest/sentinel/got_hook.h"

namespace nevr_quest::combat::meshstep {

// The helpers the stock constructor calls (thiscall shapes are plain functions on AArch64).
struct Fns {
  std::uint64_t (*current)();                                                        // CMemoryContext::Current()
  void* (*currentPtr)();                                                             // CMemoryContext::CurrentPtr()
  void (*listDataCtor)(void* self);                                                  // CGMeshListData::CGMeshListData
  void (*memblockCtor)(void* self, std::uint64_t a, std::uint64_t b, void* alloc);   // CMemBlock::CMemBlock
};

// The normal step: `on` is `combat.meshstep`. Fixed status tokens: "installed", "off", "already_ran", "module_not_loaded", "no_build_id",
// "build_id_mismatch", "pinned_word_mismatch", "vtable_unreadable", or a GotStatus name.
ModuleResult Install(bool on) noexcept;
// The same on the image `lookup` finds, with `pins` and the helpers given (production: kPins and the game's own, from base + pins).
ModuleResult InstallAt(sentinel::ImageLookup lookup, const Pins& pins, const Fns* fns, bool on) noexcept;

struct Stats {
  std::uint32_t built;  // constructions made by the reconstruction
  std::uint32_t stock;  // constructions left to the game's constructor
};
Stats CurrentStats() noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

// The image lookup Install uses; nullptr restores sentinel::FindLoadedImage.
void SetImageLookupForTest(sentinel::ImageLookup lookup) noexcept;
// Pins and helpers Install uses; nullptr restores production.
void SetPinsForTest(const Pins* pins) noexcept;
void SetFnsForTest(const Fns* fns) noexcept;
// Forgets the hook, the thunk, the published words and the counters.
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::meshstep
