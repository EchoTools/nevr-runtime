/* The render-target layout pool is made with 1024 slots, and a witness line says whether android_main started after the sizes were set
 * (docs/quest/combat/memsys.md).
 *
 * What the game does. CGRawInterface::InitCommon (0x1881890..) makes the pool of render-target layouts with
 * CSimplePool<SGRenderTargetLayout>::Alloc(pool, 0x200, allocator) (`mov w1,#0x200` at 0x18818b8, the call at 0x18818c8, which returns to
 * 0x18818cc) and then fills the pool's free stack inline with 511..0 and adds 512 to the free count. A combat match needs more than 512
 * layouts and the pool runs dry.
 *
 * R37, the pool. A wrapper on Alloc's JUMP_SLOT (0x36e9b48) acts only on the call that returns to base + 0x18818cc with capacity 0x200. It
 * calls Alloc with 0x400, checks the layout it got (a free-stack pointer that is non-null and 4-aligned, a stack capacity of at least
 * 0x400, a capacity word of 0x400, a free count of 0, and the whole stack readable and writable), writes 1023..512 under where the inline
 * fill will write 511..0, and sets the count to 512; the inline fill then completes 0..1023 and the count reads 1024. Any other call,
 * any other capacity and any layout it does not recognise goes to the game's function untouched, with one Warning, and the pool is the
 * stock 512.
 *
 * R38, the witness. android_main's first call is CSysJava::Init (0x10784e4, return address 0x10784e8), before it reads any heap size. A
 * wrapper on Init's JUMP_SLOT (0x36de5c0) writes one line on the call that returns to base + 0xf784e8, with the System heap size the game
 * is about to read: the raised size when heaps.h's step ran first, the stock 400 MiB when it did not. No line means android_main passed
 * Init before this step installed. It then calls the game's function with the same argument.
 *
 * When. This is an early module (combat_install.h): after heaps.h's step and before android_main runs, on the sentinel's constructor
 * thread. Both slots are libr15's own JUMP_SLOTs of a BIND_NOW image, filled before any constructor runs, and each must hold
 * base + the function (the target's expected original); a slot another hook took first is refused and left alone.
 *
 * Which APKs. The q15t and combat variants (combat::Active()); every other APK keeps the game's pool.
 *
 * Switch. `combat.memsys` (default true) in config.yaml (docs/adr/0046 rule 4): false installs nothing here and also leaves the System heap
 * (heaps.h, R07) alone, because the heap and the pool are one memory change.
 *
 * Pinned code. Before a row installs, the 21 code words its reasoning relies on (memsys_targets.h) are read and compared; a difference
 * refuses that row only.
 *
 * Frequency (ADR 0038). The Alloc wrapper runs once per pool made (InitCommon makes the pool once per process; the wrapper's counters
 * count every call) and logs only after Alloc returned. The Init wrapper runs once per process. Neither is a hot path.
 *
 * Owner (ADR 0039). This file owns the layout pool's slot count. The reference runtime's pool change is not in this repository's APKs.
 *
 * Lines (sentinel log):
 *   hook     {"event":"combat_memsys_hook","row":"R37|R38","symbol":"..","status":"<GotStatus>|pinned_word_mismatch"}  Info when installed;
 *            a pinned word that differs is a Warning that adds `vaddr`, `have` (the word read, or "unreadable") and `want`
 *   install  {"event":"combat_memsys_install","result":"installed|partial|refused|off","reason":"ok|row_refused|<token>|combat.memsys",
 *             "r37":"..","r38":"..","consequence":".."}
 *   pool     {"event":"combat_memsys_pool","result":"raised|not_raised","slots":1024,"pre_pushed":512,"cap_read_back":<n>}  once, after Alloc
 *            (a layout it does not recognise is one Warning with the words read: stack, stack_cap, cap_read, count, readable)
 *   witness  {"event":"combat_memsys_witness","msg":"android_main reached CSysJava::Init","system_heap_mib_next_read":<n|"unreadable">,
 *             "system_heap":"raised|stock|unreadable","system_slot_vaddr":"0x.."}  once
 *
 * Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/combat/combat_install.h"
#include "quest/combat/memsys_targets.h"
#include "quest/sentinel/got_hook.h"

namespace nevr_quest::combat::memsys {

// The step on the loaded libr15: `on` is `combat.memsys`. Fixed status tokens: "installed", "partial", "refused", "off", "already_ran",
// "module_not_loaded", "no_build_id", "build_id_mismatch".
ModuleResult Install(bool on) noexcept;

// The same on the image `lookup` finds, with `pins` (kPins in production; a host test points the functions at stand-ins).
ModuleResult InstallAt(sentinel::ImageLookup lookup, const Pins& pins, bool on) noexcept;

struct Stats {
  std::uint32_t allocCalls;      // calls through the R37 wrapper
  std::uint32_t poolRaised;      // pools made with 1024 slots
  std::uint32_t poolNotRaised;   // the right call site, but the layout was not recognised (the stock 512 stays)
  std::uint32_t initCalls;       // calls through the R38 wrapper
  std::uint32_t initWitnessed;   // 1 once android_main's first call was seen
};
Stats CurrentStats() noexcept;

// ---- test support ------------------------------------------------------------------------------------------------------

// The image lookup Install uses; nullptr restores sentinel::FindLoadedImage.
void SetImageLookupForTest(sentinel::ImageLookup lookup) noexcept;
// The pins Install uses (a host test points the two functions at stand-ins it can call); nullptr restores kPins. The object must outlive
// the install.
void SetPinsForTest(const Pins* pins) noexcept;
// A test cannot make a call return into libr15, so it reads the return addresses a call from its own function produced and sets those
// as the sites. 0 = base + the pin.
void SetSitesForTest(std::uintptr_t allocReturn, std::uintptr_t initReturn) noexcept;
// The return addresses of the last call through each wrapper.
void LastCallersForTest(std::uintptr_t* allocReturn, std::uintptr_t* initReturn) noexcept;
// Forgets the two hooks, clears the thunks, the counters and the once-only lines.
void ResetForTest() noexcept;

}  // namespace nevr_quest::combat::memsys
