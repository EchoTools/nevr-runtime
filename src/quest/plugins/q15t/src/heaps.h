// Module `heaps` (rows R06 R07 R08 R09; docs/quest/q15t/modules/heaps.md): raise the engine's four memory-heap sizes without writing a
// code or rodata word.
//
// The game reads each size as one u64 from .rodata THROUGH a GLOB_DAT slot (a RELRO data word): `ldr x8,[GOT]; ldr x1,[x8]`.
// Swapping the slot (got_swap.h, data page only) to point at a plugin-owned u64 changes what every reader of that slot sees;
// the rodata page is never touched. The source build did the same swap for the System heap (the source build's memsys M1) and patched
// the three other words in the libr15 file (the source build's patch table P11/P12, res_heap_patch.py).
//
//   row  symbol (NRadEngine::)         GLOB_DAT slot  rodata u64  stock   ->  value
//   R06  kResourceMemoryAndroid        0x370b720      0x2b9caa0   206 MiB     320 MiB
//   R07  kSystemMemoryAndroid          0x371d020      0x2b9ca90   400 MiB     640 MiB (512 MiB on a Quest 2, "hollywood")
//   R08  kGpuMemoryAndroid             0x3710188      0x2b9ca98   160 MiB     512 MiB
//   R09  kR15GameSpaceMemoryQuest      0x3715198      0x2b9e1c0    40 MiB     128 MiB
//
// TIMING: R06-R08 are read once by android_main, before CSysOS::Init, so the swap has to be done before android_main runs.
// The plugin loader calls InitEx at that point only for a plugin listed `early: true` (docs/quest/q15t/modules/heaps.md, "Timing").
#pragma once

#include <cstdint>

#include "import_hook.h"

namespace q15t {

// One row's pins, ELF vaddrs of the pinned build (readelf -rW; disassembly VA = ELF vaddr + 0x100000).
struct HeapRowPin {
  const char* id;           // "R06"
  const char* symbol;       // the symbol the GLOB_DAT relocation names
  std::uintptr_t got;       // the GLOB_DAT slot
  std::uintptr_t ro;        // the rodata u64 (the relocation's symbol value): the slot must hold base + ro
  std::uintptr_t reader;    // the instruction that loads the u64 through the slot
  std::uint32_t reader_word;  // its stock word (ldr xN,[x8])
  std::uint64_t stock;      // the rodata u64's stock value
  std::uint64_t target;     // what the plugin's u64 holds (R07: the non-Quest-2 value)
};

// The four rows, in R06 R07 R08 R09 order.
const HeapRowPin* HeapRowPins(int* count);

// Swaps every row that passes its checks (each row on its own: one refusal does not stop the others) and logs one line per
// row. The returned status is kOk when every row was swapped, else the first refusal's status (`reason` names the row).
// Does nothing, with one line, when `heaps.off` is in the plugin's data directory; `memsys.off` leaves R07 alone (the source
// build's one switch covered the System heap and the layout pool).
InstallResult InstallHeaps(std::uintptr_t base);

struct HeapsStats {
  int rows;       // rows attempted
  int swapped;    // rows whose slot now points at the plugin's u64 and reads back the new value
  int refused;    // rows refused (a check failed, nothing was written)
};
HeapsStats GetHeapsStats();

// Test seams: the value of `ro.product.device` (null: the real property on Android, "" on a host), and a state reset.
void SetHeapsDeviceForTest(const char* device);
void ResetHeapsForTest();

}  // namespace q15t
