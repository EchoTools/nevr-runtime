/* The instanced-model resource's tables grow by 1, not 32 (docs/quest/combat/instmodel.md), in a combat match only.
 *
 * What the game does. The constructor of NRadEngine::CGInstancedModelResource (ELF 0x19078dc, 376 bytes) gives each of its eight CTableXT
 * members a growth step of 32: one `mov w20,#0x20` (ELF 0x1907958) is the source of the eight stores at member + 0x20. Its callers reach it
 * through a PLT stub that loads the constructor from the JUMP_SLOT at ELF 0x36c5c68.
 *
 * What this does. A thunk on that JUMP_SLOT (instmodel_targets.h), installed by the hook manager. The handler always runs the game's
 * constructor. While a combat map is loaded (table_step.h `Scope::kCombat`) it then sets the eight steps to 1, each only where the stock value
 * 0x20 is still there; a step that holds another value is left as it is. In every other scope the object is stock. Nothing between the stock
 * constructor's stores reads a step, so the object after the handler is the object a constructor with `mov w20,#1` would build.
 *
 * Arena (docs/adr/0046). Arena and combat run on the same APK. What changes for an arena match: nothing the player sees or the game
 * computes; the object is the game's. What it costs there: each construction asks the loaded levels once (`table_step::CurrentScope`, seven process_vm_readv calls
 * with one gamespace loaded: four for the two tables, three for each gamespace) and then runs the game's constructor; it runs once per instanced-model resource the game builds, on a loading
 * thread, never per frame. The constructions an arena level costs are counted per level change
 * (`combat_instmodel_counts`); not yet read on a headset.
 *
 * When. The normal step (combat_install.h), after the plugin pass. The slot must hold base + the constructor; a slot another hook took first
 * joins the manager's chain (got_hook.h), and the handler calls it through its original.
 *
 * Which APKs. The q15t and combat variants (combat::Active()); every other APK keeps the game's constructor.
 *
 * Switch. `combat.instmodel` (default true) in config.yaml (docs/adr/0046 rule 4). The plugin's `instmodel.off` is not read; one found in the
 * plugin's data directory is a Warning naming the key.
 *
 * Pinned code. Before the thunk goes in, the constructor's first word and the step constant (instmodel_targets.h) are read and compared; a
 * difference refuses the module with nothing hooked.
 *
 * Frequency (ADR 0038). Once per CGInstancedModelResource the game constructs; not on a per-frame path. The log is written after the work:
 * one `combat_instmodel_steps` line for each of the first 16 combat constructions, then one Warning that the cap was reached.
 *
 * Owner (ADR 0039). This file owns the growth step of the instanced-model tables. The reference build writes the same constant as a code-word
 * row (docs/quest/combat/heaps.md section 3); that row is not in this repository's APKs.
 *
 * Origin. The game's own constructor, read in the disassembly (docs/quest/combat/instmodel.md). The record of #2167
 * (docs/audits/2026-11-05-q15t-origin-citations.md) holds no reference-build (`afk_*`) or EchoCombat citation for this module; the reference
 * build's `res_heap_patch.py` writes the immediate (heaps.md cites it), and this module is a hook-level replacement of that row. It came with
 * the q15t plugin (#1592), whose host-test harness and checked-pin style it follows.
 *
 * Lines (sentinel log):
 *   install  {"event":"combat_instmodel_install","result":"installed|refused|off","reason":"ok|off_config|<token>","status":"<GotStatus>|..",
 *             "slot_vaddr":"0x..","consequence":".."}   Error for a refusal, Info otherwise. A pinned word that differs is a Warning
 *             `combat_instmodel_pin` with `vaddr`, `have` and `want` first.
 *   steps    {"event":"combat_instmodel_steps","steps_set":<n>,"steps_mismatch":<n>,"step":1,"msg":".."}   Debug, the first 16
 *   mismatch {"event":"combat_instmodel_mismatch","offset":"0x..","found":"0x..","expected":"0x20","msg":".."}   Warning, the first 16
 *   cap      {"event":"combat_instmodel_log_cap","cap":16,"msg":".."}   Info, once
 *   stock    {"event":"combat_instmodel_stock","scope":"other|unknown","msg":".."}   Info, once: the game's object was left as it is
 *   counts   {"event":"combat_instmodel_counts","level":"0x..","scope":"combat|other|unknown","left_alone":<n>,"changed":<n>,"msg":".."}   Info, one per level
 *            change when there is anything to report: the objects built under the level that ended (table_step.h RegisterCounts)
 *   legacy   {"event":"combat_instmodel_legacy_switch","file":"instmodel.off","replaced_by":"combat.instmodel: false","msg":".."}   Warning
 *
 * Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/combat/combat_install.h"
#include "quest/combat/instmodel_targets.h"
#include "quest/sentinel/got_hook.h"

namespace nevr_quest::combat::instmodel {

// The normal step: `on` is `combat.instmodel`. Fixed status tokens: "installed", "off", "already_ran", "module_not_loaded", "no_build_id",
// "build_id_mismatch", "pinned_word_mismatch", or a GotStatus name.
ModuleResult Install(bool on) noexcept;
// The same on the image `lookup` finds, with `pins` (kPins in production).
ModuleResult InstallAt(sentinel::ImageLookup lookup, const Pins& pins, bool on) noexcept;

struct Stats {
  std::uint32_t objects;   // constructions made in a combat match
  std::uint32_t stepsSet;  // growth steps set to 1
  std::uint32_t mismatch;  // growth steps that did not hold the stock value and were left alone
  std::uint32_t stock;     // constructions outside a combat match, left as the game made them
};
Stats CurrentStats() noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

// The image lookup Install uses; nullptr restores sentinel::FindLoadedImage.
void SetImageLookupForTest(sentinel::ImageLookup lookup) noexcept;
// Pins Install uses; nullptr restores production.
void SetPinsForTest(const Pins* pins) noexcept;
// Forgets the hook, the thunk and the counters.
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::instmodel
