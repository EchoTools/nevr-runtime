/* The engine's four memory-heap sizes are raised before the game allocates them (docs/quest/combat/heaps.md).
 *
 * What the game does. android_main reads the resource, System and GPU heap sizes once, each as one u64 from .rodata through a GLOB_DAT
 * word (`ldr x8,[slot]; ldr x1,[x8]`), and hands them to the allocator constructor; the game-space size is read by
 * GameSpaceAllocatorSize. The stock sizes (206, 400, 160 and 40 MiB) run dry in a combat match: the System arena, the spawn path and the
 * game-space allocation all fail on the stock heaps (the reference build's combat patches raise them for that reason).
 *
 * What this does. Each of the four slots is pointed at a u64 of ours that holds the new size (heaps_targets.h): 320 MiB for the
 * resource heap, 640 MiB for the System heap (512 MiB on a Quest 2, the headset whose device name is `hollywood`), 512 MiB for the GPU
 * heap and 128 MiB for the game space. The write goes through the hook manager's checked data slot (sentinel::GotDataSlot, ADR 0046
 * rule 2): it stores only over the exact word the loader put there, refuses a slot that holds code or sits in an executable mapping, and
 * touches no .rodata or code word. Before a row is written its reader word and its .rodata size are read and compared with the stock
 * ones; a row that differs is refused alone and the other rows go on.
 *
 * When. This is an early module (combat_install.h, `InstallEarly`): a constructor step of the sentinel, after the build-time libr15 patch
 * is verified and before `android_main` runs, so every size is ours before the first allocation it governs. The sentinel's constructor
 * runs while the dynamic linker finishes loading libr15's dependency closure, after the relocations of the whole set were applied and
 * before the activity's entry; a slot the loader has not relocated holds 0 and is refused (the write's expected word is base + .rodata
 * address), so a step that ran too early says so and writes nothing. memsys.h's witness line says on a headset whether the sizes
 * were in place when android_main started.
 *
 * Which APKs. The q15t and combat variants (combat::Active()); every other APK keeps the game's sizes.
 *
 * Switches (docs/adr/0046 rule 4, `combat:` in config.yaml): `combat.heaps` (default true) turns the module off; `combat.memsys` false
 * leaves the System heap (R07) as the game has it, because the System heap and the render-target pool are one memory change
 * (memsys.h) and one switch covers both.
 *
 * The headset. The System heap's size follows the headset, read once from the device name system property (`ro.product.device`). The
 * identity step of ADR 0031 does not exist in the sentinel yet (#2037 moves this read there).
 *
 * Frequency (ADR 0038). One step per process; no hook, so nothing here runs afterwards.
 *
 * Owner (ADR 0039). This file owns the four sizes. The reference runtime's own heap rows are not in this repository's APKs.
 *
 * Lines (sentinel log):
 *   row      {"event":"combat_heaps_row","row":"R06","status":"swapped|refused|left","why":"..","slot_vaddr":"0x..","value_mib":<n>,
 *             "observed_mib":<n>,"device":".."}   Debug for a swapped row, Error for a refused write, Info for a row left alone
 *            a pinned word that differs (why `reader_word_not_stock` or `rodata_not_stock`) is a Warning with `have` (the word read, or
 *            "unreadable"), `want` and no `observed_mib`
 *   install  {"event":"combat_heaps_install","result":"installed|partial|refused|off","reason":"ok|row_refused|<status token>|combat.heaps","swapped":<n>,"refused":<n>,"left":<n>,
 *             "resource_mib":<n>,"system_mib":<n>,"gpu_mib":<n>,"game_space_mib":<n>,"device":"..","consequence":".."}
 *            `*_mib` is the size the game reads through the slot after the step (0 when it could not be read).
 *
 * Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/combat/combat_install.h"
#include "quest/combat/heaps_targets.h"
#include "quest/sentinel/got_hook.h"

namespace nevr_quest::combat::heaps {

// The step on the loaded libr15: `heapsOn` is `combat.heaps`, `systemOn` is `combat.memsys`. installed is true when all four rows (three
// when the System heap is left alone) were swapped. Fixed status tokens: "installed", "partial", "refused", "off", "already_ran",
// "module_not_loaded", "no_build_id", "build_id_mismatch".
ModuleResult Install(bool heapsOn, bool systemOn) noexcept;

// The same on the image `lookup` finds, for the headset `device` (the `ro.product.device` value; "" for none).
ModuleResult InstallAt(sentinel::ImageLookup lookup, const char* device, bool heapsOn, bool systemOn) noexcept;

// The size the System heap row writes for `device`.
std::uint64_t SystemBytesFor(const char* device) noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

// The device name Install reads; nullptr restores the system property (empty off Android).
void SetDeviceForTest(const char* device) noexcept;
// The image lookup Install uses; nullptr restores sentinel::FindLoadedImage.
void SetImageLookupForTest(sentinel::ImageLookup lookup) noexcept;
// Gives the four slots back (a restore, which refuses a slot someone else changed) and forgets that the step ran.
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::heaps
