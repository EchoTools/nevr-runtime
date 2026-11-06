/* The pinned slots of the heap-size module (heaps.h): the four GLOB_DAT words libr15 reads a heap size through.
 *
 * Every address is an ELF virtual address in the store libr15 (build ID b243509c...; the patched file the sentinel runs on has the same
 * layout at these words, ADR 0021). The disassembly's address is the ELF address plus 0x100000. src/quest/tests/got_pinned_test.cpp
 * checks each row against the real file: the GLOB_DAT relocation names the symbol at the slot, the symbol is a data object whose
 * address is `ro`, the word at `ro` is the stock size, and the word at `reader` is the load that reads it.
 *
 *   row  symbol (NRadEngine::)        GLOB_DAT slot   .rodata u64   reader (the load)               stock     set to
 *   R06  kResourceMemoryAndroid       0x370b720       0x2b9caa0     0xf78590  ldr x1,[x8]  android_main  206 MiB   320 MiB
 *   R07  kSystemMemoryAndroid         0x371d020       0x2b9ca90     0xf78524  ldr x1,[x8]  android_main  400 MiB   640 MiB (512 MiB on a Quest 2)
 *   R08  kGpuMemoryAndroid            0x3710188       0x2b9ca98     0xf78614  ldr x1,[x8]  android_main  160 MiB   512 MiB
 *   R09  kR15GameSpaceMemoryQuest     0x3715198       0x2b9e1c0     0x11ffdec ldr x0,[x8]  GameSpaceAllocatorSize  40 MiB  128 MiB
 *
 * Each reader is `ldr x8,[x8,#<slot>]` (the GLOB_DAT word) then the load above: the stock value lives in .rodata and is reached
 * through the slot, so pointing the slot at a value of ours changes what the reader sees and writes nothing in a code or rodata page.
 *
 * Origins (docs/audits/2026-11-05-q15t-origin-citations.md, rows for `heaps` and `memsys`): the sizes 320 MiB (R06), 512 MiB (R08)
 * and 128 MiB (R09) and the System heap's 640 MiB with 512 MiB on a Quest 2 come from the reference build's heap patches
 * (`runtime/native/afkxr/afk_memsys.cpp`:253 for the System heap, `tools/re3/res_heap_patch.py`:7-12,163-170 and
 * `tools/qcp_patches.py`:62-71 for the others, pinned in that record). The slot, reader and rodata addresses are measured in the
 * store libr15 (`readelf -rW`, and the disassembly for the readers).
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/pinned_targets.h"

namespace nevr_quest::combat::heaps {

inline constexpr std::uint64_t kMiB = 1ULL << 20;
inline constexpr std::size_t kRowCount = 4;
inline constexpr std::size_t kSystemRow = 1;  // R07, the row `combat.memsys: false` leaves alone
inline constexpr std::uint64_t kQuest2SystemBytes = 512 * kMiB;

// `ldr x1,[x8]` and `ldr x0,[x8]`: the loads that read a size through its slot.
inline constexpr std::uint32_t kLdrX1FromX8 = 0xf9400101;
inline constexpr std::uint32_t kLdrX0FromX8 = 0xf9400100;

struct RowPin {
  const char* id;            // "R06"
  const char* symbol;        // the symbol the GLOB_DAT relocation names
  std::uint64_t slot;        // the GLOB_DAT slot
  std::uint64_t ro;          // the .rodata u64 the slot points at: the slot holds base + ro
  std::uint64_t reader;      // the load that reads the u64 through the slot
  std::uint32_t readerWord;  // its stock instruction word
  std::uint64_t stock;       // the u64's stock value
  std::uint64_t target;      // what the slot's new u64 holds (R07: the value for any headset but a Quest 2)
};

inline constexpr RowPin kRows[kRowCount] = {
    {"R06", "_ZN10NRadEngine22kResourceMemoryAndroidE", 0x370b720ULL, 0x2b9caa0ULL, 0xf78590ULL, kLdrX1FromX8, 206 * kMiB, 320 * kMiB},
    {"R07", "_ZN10NRadEngine20kSystemMemoryAndroidE", 0x371d020ULL, 0x2b9ca90ULL, 0xf78524ULL, kLdrX1FromX8, 400 * kMiB, 640 * kMiB},
    {"R08", "_ZN10NRadEngine17kGpuMemoryAndroidE", 0x3710188ULL, 0x2b9ca98ULL, 0xf78614ULL, kLdrX1FromX8, 160 * kMiB, 512 * kMiB},
    {"R09", "_ZN10NRadEngine24kR15GameSpaceMemoryQuestE", 0x3715198ULL, 0x2b9e1c0ULL, 0x11ffdecULL, kLdrX0FromX8, 40 * kMiB, 128 * kMiB},
};

// The row's GLOB_DAT slot as a manager target: the patched libr15's build ID and the pinned slot. `expectedOriginal` is not read by a
// data write (GotDataSlot::Write takes the expected word itself).
inline sentinel::GotTarget RowTarget(const RowPin& row) {
  return sentinel::GotTarget(sentinel::pinned::kLibR15, row.symbol, sentinel::RelocKind::kGlobDat,
                             sentinel::pinned::LibR15PatchedBuildId(), row.slot)
      .Owned("combat_heaps");
}

}  // namespace nevr_quest::combat::heaps
