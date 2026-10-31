// meshstep (issue #1555): CTableXT growth step 1 for the mesh-list tables. Hook-only, no code word is written.
//
// The stock constructor of NRadEngine::CGMeshListResource (ELF 0x18677cc) gives each of its eleven CTableXT members a growth step
// of 32: one `mov w20,#0x20` (ELF 0x186784c) is stored at +0x2d0, +0x308, +0x340, ... +0x500 (the member block base + 0x20). Its only
// caller is a GOT thunk (ELF 0x1bcc0) that loads the constructor's address from the data-page word at ELF 0x36e5548 and branches.
// This module swaps that word for a C++ reconstruction of the constructor that stores 1 instead of 32, and calls the same helpers
// the stock code calls, in the same order. The reconstruction reads its two vtable words from the game's data pages at install
// time, the way the stock code reads them on each call. Nothing from the game's code is copied; the words the reconstruction was
// derived from are pinned (kMeshStepWordPins) and a mismatch refuses the module with nothing swapped.
//
// Refused (one ERROR line, nothing swapped) when a pin does not read back, a helper or vtable word is missing, or the slot does not
// hold base + the constructor. `meshstep.off` in the plugin data directory turns it off (reason "off_file", one INFO line).
#pragma once

#include <cstddef>
#include <cstdint>

#include "import_hook.h"
#include "word_pins.h"

namespace q15t {

// ELF vaddrs in the pinned build (disassembly VA - 0x100000).
struct MeshStepAddrs {
  std::uintptr_t ctor;             // CGMeshListResource::CGMeshListResource(CPresenceInit const&)
  std::uintptr_t got_slot;         // the GOT word thunk_FUN_019677cc calls through
  std::uintptr_t current_thunk;    // CMemoryContext::Current (call site ctor+0x2b)
  std::uintptr_t list_data_ctor;   // CGMeshListData::CGMeshListData
  std::uintptr_t current_ptr_thunk;  // CMemoryContext::CurrentPtr
  std::uintptr_t memblock_ctor;    // CMemBlock::CMemBlock(CMemBlock*, u64, u32, CAllocator*)
  std::uintptr_t vtbl_presence;    // data word; + 0x10 is the NRadEngine::CPresence vtable the stock constructor stores first
  std::uintptr_t vtbl_mesh;        // data word; + 0x10 is the CGMeshListResource vtable
};
inline constexpr MeshStepAddrs kMeshStepPinned = {0x18677cc, 0x36e5548, 0xf28430, 0xef9c70, 0xefbc50, 0xeedec0, 0x371f860, 0x36fc7d8};

// The instruction words the reconstruction was derived from: the thunk's adrp/ldr, the vtable adrp/ldr pairs, the `mov w20,#0x20`
// the patched word replaces, the `ldr q1` of the {7, 0} member constant, and every `bl` of the stock constructor.
inline constexpr WordPin kMeshStepWordPins[] = {
    {0x1bcc0, 0xd0013e50},   {0x1bcc4, 0xf942a611},   {0x18677d4, 0x9000f5c8}, {0x18677d8, 0xf9443108}, {0x1867800, 0xb000f52a},
    {0x1867804, 0xd0009e69}, {0x1867808, 0xf943ed4a}, {0x186780c, 0x3dc37d21}, {0x186784c, 0x52800414}, {0x18677fc, 0x97db030d},
    {0x1867830, 0x97da4910}, {0x1867834, 0x97da5107}, {0x1867848, 0x97da199e}, {0x186785c, 0x97da50fd}, {0x1867870, 0x97da1994},
    {0x1867880, 0x97da50f4}, {0x1867894, 0x97da198b}, {0x18678a4, 0x97da50eb}, {0x18678b8, 0x97da1982}, {0x18678c8, 0x97da50e2},
    {0x18678dc, 0x97da1979}, {0x18678ec, 0x97da50d9}, {0x1867900, 0x97da1970}, {0x1867910, 0x97da50d0}, {0x1867924, 0x97da1967},
    {0x1867934, 0x97da50c7}, {0x1867948, 0x97da195e}, {0x1867958, 0x97da50be}, {0x186796c, 0x97da1955}, {0x186797c, 0x97da50b5},
    {0x1867990, 0x97da194c}, {0x18679a0, 0x97da50ac}, {0x18679b4, 0x97da1943},
};
inline constexpr std::size_t kMeshStepWordPinCount = sizeof kMeshStepWordPins / sizeof kMeshStepWordPins[0];

// The helpers, as the stock constructor calls them (thiscall shapes are plain functions on AArch64).
struct MeshStepFns {
  std::uint64_t (*current)();                                       // CMemoryContext::Current()
  void* (*current_ptr)();                                           // CMemoryContext::CurrentPtr()
  void (*list_data_ctor)(void* self);                               // CGMeshListData::CGMeshListData
  void (*memblock_ctor)(void* self, std::uint64_t a, std::uint64_t b, void* alloc);  // CMemBlock::CMemBlock
};

// Installs the reconstruction at the pinned addresses of the game library at `base`.
InstallResult InstallMeshStep(std::uintptr_t base);
// The same with the addresses and helpers given (the host test's seam).
InstallResult InstallMeshStepAt(std::uintptr_t base, const MeshStepAddrs& addrs, const MeshStepFns& fns);

struct MeshStepStats {
  unsigned builds;  // constructions made by the reconstruction since the last reset
};
MeshStepStats GetMeshStepStats();
void ResetMeshStepForTest();

}  // namespace q15t
