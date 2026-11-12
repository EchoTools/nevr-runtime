/* The shader-set resource's table grows by 1, not 32, when the factory constructs it (docs/quest/combat/shaderstep.md), in a combat match only.
 *
 * What the game does. CFactory<CGShaderSetResource>::ConstructInstance (ELF 0x1906f4c, 304 bytes) builds a shader-set resource in place and
 * stores the table's growth step at object + 0x98 (`mov w8,#0x20` at ELF 0x1907000, `str x8,[x19,#0x98]`). The table is the CTableXT at
 * object + 0x78: its CMemBlock is first, the step at table + 0x20.
 *
 * What this does. A thunk on the factory's vtable word for ConstructInstance, ABS64 0x365ddc8 (shaderstep_targets.h, a class C word of
 * ADR 0046), installed by the hook manager. The handler always runs the game's function; in a combat match (table_step.h `Scope::kCombat`) it
 * then writes 1 to the step of the object it returned, after reading that the game wrote 0x20 (table_step.h ApplyShaderSetStep). In every
 * other scope the object is stock. tablestep.h does the same for the factory's other constructor, CreateInstance.
 *
 * Arena (docs/adr/0046). Arena and combat run on the same APK. What changes for an arena match: nothing the player sees or the game
 * computes; the object is the game's. What it costs there: each ConstructInstance asks the loaded levels once (`table_step::CurrentScope`, a
 * seven process_vm_readv calls with one gamespace loaded, four for the two tables and three for each gamespace, plus two for the in-flight table and three for each fiber in it) and runs the game's function; it runs once per shader set the game builds, on a loading thread, never
 * per frame. The constructions an arena level costs are counted per level change (`combat_shaderstep_counts`);
 * not yet read on a headset.
 *
 * When. The normal step (combat_install.h), after the plugin pass. A resource the game builds before the step is stock; the step cannot
 * correct it. Whether shader sets are built before the normal step has not been measured on a headset.
 *
 * Which APKs. The combat variants (combat::Active()); every other APK keeps the game's function.
 *
 * Switch. `combat.shaderstep` (default true) in config.yaml (docs/adr/0046 rule 4). The `shaderstep.off` file is not read; one found in
 * the data directory is a Warning naming the key.
 *
 * Frequency (ADR 0038). Once per shader set the game constructs through this slot; not on a per-frame path. The log is written after the work:
 * one line for the first object set, and a Warning for each of the first five objects whose step was not the game's 0x20.
 *
 * Owner (ADR 0039). This file owns the growth step ConstructInstance leaves. tablestep.h owns the one CreateInstance leaves; the two
 * functions are separate constructors of the same class and a resource is built by one of them.
 *
 * Origin. The game's own code, read in the disassembly (docs/quest/combat/shaderstep.md), and the reference build's upstream patch: `tools/re3/res_heap_patch.py`
 * with `--table-step-1` (RE-10's padding finding, wave 8; commit 35bba9f of kotorvr/echoquest-combat) writes `mov wN,#1` over the `mov wN,#0x20` that gives the CTableXT growth step
 * its 32 in the CGShaderSetResource, CGInstancedModelResource and CGMeshListResource constructors, at these addresses less 0x100000. This module does
 * the same change as a hook on the constructor's slot, in a combat match only (docs/quest/combat/heaps.md section 3 cites the patch). It came with the combat
 * plugin (#1594), whose host-test harness and checked-pin style it follows.
 *
 * Lines (sentinel log):
 *   install   {"event":"combat_shaderstep_install","result":"installed|refused|off","reason":"ok|off_config|<token>","status":"<GotStatus>|..",
 *              "slot_vaddr":"0x..","consequence":".."}   Error for a refusal, Info otherwise
 *   first     {"event":"combat_shaderstep_first","obj":"0x..","step":1,"msg":".."}   Info, once
 *   not_stock {"event":"combat_shaderstep_not_stock","found":"0x..","expected":"0x20","msg":".."}   Warning, the first five
 *   stock     {"event":"combat_shaderstep_stock","scope":"other|unknown","msg":".."}   Info, once: the game's object was left as it is
 *   counts   {"event":"combat_shaderstep_counts","level":"0x..","scope":"combat|other|unknown","left_alone":<n>,"changed":<n>,"msg":".."}   Info, one per level
 *            change when there is anything to report: the objects built under the level that ended (table_step.h RegisterCounts)
 *   legacy    {"event":"combat_shaderstep_legacy_switch","file":"shaderstep.off","replaced_by":"combat.shaderstep: false","msg":".."}   Warning
 *
 * Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/combat/combat_install.h"
#include "quest/combat/shaderstep_targets.h"
#include "quest/combat/table_step.h"
#include "quest/sentinel/got_hook.h"

namespace nevr_quest::combat::shaderstep {

using table_step::ApplyShaderSetStep;
using table_step::StepFix;

// The normal step: `on` is `combat.shaderstep`. Fixed status tokens: "installed", "off", "already_ran", "module_not_loaded", "no_build_id",
// "build_id_mismatch", or a GotStatus name.
ModuleResult Install(bool on) noexcept;
// The same on the image `lookup` finds, with `pins` (kPins in production).
ModuleResult InstallAt(sentinel::ImageLookup lookup, const Pins& pins, bool on) noexcept;

struct Stats {
  std::uint32_t constructed;  // objects the handler saw built in a combat match
  std::uint32_t stepped;      // of those, the step written to 1
  std::uint32_t notStock;     // of those, the word was not the game's 0x20: left alone
  std::uint32_t stock;        // objects built outside a combat match
};
Stats CurrentStats() noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

// The image lookup Install uses; nullptr restores sentinel::FindLoadedImage.
void SetImageLookupForTest(sentinel::ImageLookup lookup) noexcept;
// Pins Install uses; nullptr restores production.
void SetPinsForTest(const Pins* pins) noexcept;
// Forgets the hook, the thunk and the counters.
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::shaderstep
