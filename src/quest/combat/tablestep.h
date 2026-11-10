/* The shader-set resource's table grows by 1, not 32, when the factory creates it (docs/quest/combat/tablestep.md), in a combat match only.
 *
 * What the game does. CFactory<CGShaderSetResource>::CreateInstance (ELF 0x190707c, 292 bytes) allocates a 0xb0-byte object, fills its
 * presence and resource words, runs the CMemBlock constructor on the CTableXT at object + 0x78 and stores the table's growth step:
 * `mov w8,#0x20` at ELF 0x1907130, `str x8,[x19,#0x98]` at 0x1907138. The store is the last write to the step before the function returns.
 *
 * What this does. A thunk on the factory's vtable word for CreateInstance, ABS64 0x365ddd0 (tablestep_targets.h, a class C word of ADR 0046),
 * installed by the hook manager. The handler always runs the game's function; in a combat match (table_step.h `Scope::kCombat`) it then writes
 * 1 to the step of the object it returned, after reading that the game wrote 0x20 (table_step.h ApplyShaderSetStep), which is the object a
 * function with `mov w8,#1` would return. In every other scope the object is stock. shaderstep.h does the same for the factory's other
 * constructor, ConstructInstance.
 *
 * Arena (docs/adr/0046). Arena and combat run on the same APK. What changes for an arena match: nothing the player sees or the game
 * computes; the object is the game's. What it costs there: each CreateInstance asks the loaded levels once (`table_step::CurrentScope`, a
 * seven process_vm_readv calls with one gamespace loaded, four for the two tables and three for each gamespace, plus two for the in-flight table and three for each fiber in it) and runs the game's function; it runs once per shader set the game creates, on a loading thread, never
 * per frame. The creations an arena level costs are counted per level change (`combat_tablestep_counts`);
 * not yet read on a headset.
 *
 * When. The normal step (combat_install.h), after the plugin pass. A resource the game creates before the step is stock; the step cannot
 * correct it. Whether shader sets are created before the normal step has not been measured on a headset.
 *
 * Which APKs. The q15t and combat variants (combat::Active()); every other APK keeps the game's function.
 *
 * Switch. `combat.tablestep` (default true) in config.yaml (docs/adr/0046 rule 4). The plugin's `tablestep.off` is not read; one found in the
 * plugin's data directory is a Warning naming the key.
 *
 * Pinned code. Before the thunk goes in, the step constant (tablestep_targets.h) is read and compared; a difference refuses the module with
 * nothing hooked.
 *
 * Frequency (ADR 0038). Once per shader set the game creates through this slot; not on a per-frame path. The log is written after the work:
 * one line for the first object set and one Warning for the first object whose step was not the game's 0x20.
 *
 * Owner (ADR 0039). This file owns the growth step CreateInstance leaves. shaderstep.h owns the one ConstructInstance leaves.
 *
 * Origin. The game's own code, read in the disassembly (docs/quest/combat/tablestep.md), and the reference build's upstream patch: `tools/re3/res_heap_patch.py`
 * with `--table-step-1` (RE-10's padding finding, wave 8; commit 35bba9f of kotorvr/echoquest-combat) writes `mov wN,#1` over the `mov wN,#0x20` that gives the CTableXT growth step
 * its 32 in the CGShaderSetResource, CGInstancedModelResource and CGMeshListResource constructors, at these addresses less 0x100000. This module does
 * the same change as a hook on the constructor's slot, in a combat match only (docs/quest/combat/heaps.md section 3 cites the patch). It came with the q15t
 * plugin (#1593), whose host-test harness and checked-pin style it follows.
 *
 * Lines (sentinel log):
 *   install   {"event":"combat_tablestep_install","result":"installed|refused|off","reason":"ok|off_config|<token>","status":"<GotStatus>|..",
 *              "slot_vaddr":"0x..","consequence":".."}   Error for a refusal, Info otherwise. A pinned word that differs is a Warning
 *              `combat_tablestep_pin` with `vaddr`, `have` and `want` first.
 *   first     {"event":"combat_tablestep_first","from":32,"to":1,"msg":".."}   Info, once
 *   not_stock {"event":"combat_tablestep_not_stock","found":"0x..","expected":"0x20","msg":".."}   Info, once
 *   stock     {"event":"combat_tablestep_stock","scope":"other|unknown","msg":".."}   Info, once: the game's object was left as it is
 *   counts   {"event":"combat_tablestep_counts","level":"0x..","scope":"combat|other|unknown","left_alone":<n>,"changed":<n>,"msg":".."}   Info, one per level
 *            change when there is anything to report: the objects built under the level that ended (table_step.h RegisterCounts)
 *   legacy    {"event":"combat_tablestep_legacy_switch","file":"tablestep.off","replaced_by":"combat.tablestep: false","msg":".."}   Warning
 *
 * Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/combat/combat_install.h"
#include "quest/combat/tablestep_targets.h"
#include "quest/sentinel/got_hook.h"

namespace nevr_quest::combat::tablestep {

// The normal step: `on` is `combat.tablestep`. Fixed status tokens: "installed", "off", "already_ran", "module_not_loaded", "no_build_id",
// "build_id_mismatch", "pinned_word_mismatch", or a GotStatus name.
ModuleResult Install(bool on) noexcept;
// The same on the image `lookup` finds, with `pins` (kPins in production).
ModuleResult InstallAt(sentinel::ImageLookup lookup, const Pins& pins, bool on) noexcept;

struct Stats {
  std::uint32_t rewritten;   // steps set from 32 to 1, in a combat match
  std::uint32_t unexpected;  // objects whose step was not 32 (left alone), in a combat match
  std::uint32_t stock;       // objects created outside a combat match
};
Stats CurrentStats() noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

// The image lookup Install uses; nullptr restores sentinel::FindLoadedImage.
void SetImageLookupForTest(sentinel::ImageLookup lookup) noexcept;
// Pins Install uses; nullptr restores production.
void SetPinsForTest(const Pins* pins) noexcept;
// Forgets the hook, the thunk and the counters.
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::tablestep
