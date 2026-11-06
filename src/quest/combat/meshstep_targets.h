/* The pinned slot, helpers and code words of the mesh-list table-step module (meshstep.h).
 *
 * Every address is an ELF virtual address in the store libr15 (build ID b243509c...); the disassembly's address is the ELF address plus
 * 0x100000. src/quest/tests/got_pinned_test.cpp checks the relocation, the function and every word against the real file.
 *
 *   slot    CGMeshListResource::CGMeshListResource(CPresenceInit const&)   JUMP_SLOT 0x36e5548 -> 0x18677cc   (`readelf -rW`)
 *   thunk   the PLT stub the constructor's callers reach it through: `adrp x16,0x36e5000; ldr x17,[x16,#0x548]; br x17` at 0xf1bcc0
 *   vtables the two GLOB_DAT words the constructor loads its vtables from: 0x371f860 (CPresence) and 0x370c7d8 (CGMeshListResource), each
 *           `adrp xN,<page>; ldr xN,[xN,#<offset>]` in the constructor (0x18677d4/0x18677d8 and 0x1867800/0x1867808)
 *
 * The constructor's body is reproduced in meshstep.cpp from its disassembly: the eleven CMemBlock constructions, the growth step stored at
 * block + 0x20 by one `mov w20,#0x20` (0x186784c), the two vtable stores and the helpers it calls, in the stock order. kWords pins the
 * instructions that reading rests on: the thunk, the two adrp/ldr pairs that load the vtables, the `ldr q1` of the {7, 0} member constant,
 * the `mov w20,#0x20` and the `bl` of every helper call, so a libr15 that differs refuses the module whole.
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/combat/table_step.h"
#include "quest/sentinel/callback_thunk.h"
#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/pinned_targets.h"

namespace nevr_quest::combat::meshstep {

inline constexpr const char* kSymbol = "_ZN10NRadEngine18CGMeshListResourceC2ERKNS_13CPresenceInitE";

struct Pins {
  std::uint64_t ctor;           // CGMeshListResource::CGMeshListResource(CPresenceInit const&)
  std::uint64_t slot;           // the JUMP_SLOT the thunk calls through
  std::uint64_t current;        // CMemoryContext::Current (the stock call at ctor + 0x30)
  std::uint64_t listDataCtor;   // CGMeshListData::CGMeshListData
  std::uint64_t currentPtr;     // CMemoryContext::CurrentPtr
  std::uint64_t memblockCtor;   // CMemBlock::CMemBlock(CMemBlock*, u64, u32, CAllocator*)
  std::uint64_t vtblPresence;   // GLOB_DAT word of _ZTVN10NRadEngine9CPresenceE; + 0x10 is the vtable the stock constructor stores first
  std::uint64_t vtblMesh;       // GLOB_DAT word of _ZTVN10NRadEngine18CGMeshListResourceE; + 0x10 is the vtable it stores last
};
inline constexpr const char* kVtblPresenceSymbol = "_ZTVN10NRadEngine9CPresenceE";
inline constexpr const char* kVtblMeshSymbol = "_ZTVN10NRadEngine18CGMeshListResourceE";
inline constexpr Pins kPins = {0x18677cc, 0x36e5548, 0xf28430, 0xef9c70, 0xefbc50, 0xeedec0, 0x371f860, 0x370c7d8};

inline constexpr table_step::PinWord kWords[] = {
    {0xf1bcc0, 0xd0013e50}, {0xf1bcc4, 0xf942a611}, {0x18677d4, 0x9000f5c8},
    {0x18677d8, 0xf9443108}, {0x1867800, 0xb000f52a}, {0x1867804, 0xd0009e69},
    {0x1867808, 0xf943ed4a}, {0x186780c, 0x3dc37d21}, {0x186784c, 0x52800414},
    {0x18677fc, 0x97db030d}, {0x1867830, 0x97da4910}, {0x1867834, 0x97da5107},
    {0x1867848, 0x97da199e}, {0x186785c, 0x97da50fd}, {0x1867870, 0x97da1994},
    {0x1867880, 0x97da50f4}, {0x1867894, 0x97da198b}, {0x18678a4, 0x97da50eb},
    {0x18678b8, 0x97da1982}, {0x18678c8, 0x97da50e2}, {0x18678dc, 0x97da1979},
    {0x18678ec, 0x97da50d9}, {0x1867900, 0x97da1970}, {0x1867910, 0x97da50d0},
    {0x1867924, 0x97da1967}, {0x1867934, 0x97da50c7}, {0x1867948, 0x97da195e},
    {0x1867958, 0x97da50be}, {0x186796c, 0x97da1955}, {0x186797c, 0x97da50b5},
    {0x1867990, 0x97da194c}, {0x18679a0, 0x97da50ac}, {0x18679b4, 0x97da1943},
};
inline constexpr std::size_t kWordCount = sizeof kWords / sizeof kWords[0];

using Sig = void(void* self, const void* init);
struct Tag {};
using Thunk = sentinel::CallbackThunk<Tag, Sig>;

// The slot as a manager target on a libr15 loaded at `base`: the slot must hold the constructor itself (base + ctor).
inline sentinel::GotTarget Target(std::uintptr_t base, const Pins& pins) {
  sentinel::GotTarget target(sentinel::pinned::kLibR15, kSymbol, sentinel::RelocKind::kJumpSlot, sentinel::pinned::LibR15PatchedBuildId(),
                             pins.slot);
  target.expectedOriginal = reinterpret_cast<const void*>(base + pins.ctor);
  return target.Owned("combat_meshstep");
}

}  // namespace nevr_quest::combat::meshstep
