/* The game words custctx reads and hooks (#2178), as ELF vaddrs of the store libr15 (build ID b243509c...; the disassembly lists each
 * 0x100000 higher). got_pinned_test.cpp checks every line below against the store file: the three relocations, each symbol's definition, the
 * Resize export and each code word.
 *
 *   R31  CR15NetCustomizationRenderer::LoadMeshData(ctx, model)   defined 0x1247d90, JUMP_SLOT 0x36cf6f0
 *   R32  CR15NetCustomizationRenderer::DestroyContext(ctx)        defined 0x12492fc, JUMP_SLOT 0x36c22f8
 *   R33  CTableXT<SR15NetCustomDrawData>::Grow()                  defined 0x124893c, JUMP_SLOT 0x36ed7f0
 *        CMemBlock::Resize(u64 bytes, u32 keep)                   defined 0x11dc30c (an export; Grow's own resize, called by the guard)
 *        the render-target layout pool pointer                    a GOT word at 0x379b2c0 (RegisterRTLayout 0x18829c0..0x18829d4: the pool at
 *                                                                 +0x84 capacity (u32) and +0x30 free count (u64))
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/combat/pinned_words.h"
#include "quest/sentinel/callback_thunk.h"
#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/pinned_targets.h"

namespace nevr_quest::combat::custctx {

inline constexpr const char* kLoadSymbol =
    "_ZN10NRadEngine8NRadGame28CR15NetCustomizationRenderer12LoadMeshDataERNS0_27SR15NetCustomizationContextERNS_16CComponentHandleINS_8CModelCSEEE";
inline constexpr const char* kDestroySymbol =
    "_ZN10NRadEngine8NRadGame28CR15NetCustomizationRenderer14DestroyContextEPNS0_27SR15NetCustomizationContextE";
inline constexpr const char* kGrowSymbol = "_ZN10NRadEngine8CTableXTINS_8NRadGame21SR15NetCustomDrawDataEE4GrowEv";
inline constexpr const char* kResizeSymbol = "_ZN10NRadEngine9CMemBlock6ResizeEyj";

// The slots, the real functions, Resize and the pool pointer: the host test supplies stand-ins' offsets.
struct Pins {
  std::uint64_t slotLoad, fnLoad, slotDestroy, fnDestroy, slotGrow, fnGrow, fnResize, rtPool;
};
inline constexpr Pins kPins = {0x36cf6f0ULL, 0x1247d90ULL, 0x36c22f8ULL, 0x12492fcULL, 0x36ed7f0ULL, 0x124893cULL, 0x11dc30cULL, 0x379b2c0ULL};

// CTableXT<SR15NetCustomDrawData> as Grow reads it (CMemBlock first): `ldp x9,x8,[x0,#0x28]` (capacity, count) at 0x1248948, `ldr x8,[x19,#0x20]` (step)
// at 0x1248958. One element is 0x9718 bytes (`mov w9,#0x9718` at 0x1248970).
struct DrawTable {
  std::uint8_t* data;          // +0x00
  std::uint64_t size;          // +0x08
  void* allocator;             // +0x10
  std::uint32_t align, flags;  // +0x18
  std::uint64_t step;          // +0x20
  std::uint64_t capacity;      // +0x28
  std::uint64_t count;         // +0x30
};
static_assert(offsetof(DrawTable, step) == 0x20 && offsetof(DrawTable, capacity) == 0x28 && offsetof(DrawTable, count) == 0x30, "CTableXT layout");
inline constexpr std::uint64_t kDrawSize = 0x9718;

// The layout pool, as RegisterRTLayout reads it: capacity (u32) at +0x84 (0x18829c8), free count (u64) at +0x30 (0x18829cc). A full pool is a
// deliberate trap (brk) in CGRenderTargetState::Initialize, so a load that would take one slot per new mesh entry needs this many to spare.
inline constexpr std::uintptr_t kPoolCapacityOffset = 0x84, kPoolFreeOffset = 0x30;
inline constexpr std::uint64_t kLayoutMargin = 64;

// std::uint64_t LoadMeshData(void* self, void* ctx, void* model) (the stock failure value is 1), likewise DestroyContext(self, ctx) and
// void* Grow(DrawTable*).
using LoadSig = std::uint64_t(void* self, void* ctx, void* model);
using DestroySig = std::uint64_t(void* self, void* ctx);
using GrowSig = void*(DrawTable* table);
struct LoadTag {};
struct DestroyTag {};
struct GrowTag {};
using LoadThunk = sentinel::CallbackThunk<LoadTag, LoadSig>;
using DestroyThunk = sentinel::CallbackThunk<DestroyTag, DestroySig>;
using GrowThunk = sentinel::CallbackThunk<GrowTag, GrowSig>;

// The code words the three guards were derived from (34 words).
inline constexpr PinnedWord kWords[] = {
    {0x1247d90, 0x6db82beb}, {0x1247d94, 0x6d0123e9}, {0x12492fc, 0xb40008a1}, {0x1249300, 0xf81b0ffa}, {0x1249348, 0xf9405668},
    {0x12493f0, 0x912fe2a9}, {0x1249634, 0xaa1503e1}, {0x1249644, 0x97f29a73}, {0x1249650, 0x340000c0}, {0x124965c, 0x97f23071},
    {0xef0010, 0xf0013ef0},  {0xef0014, 0xf9437a11},  {0xed5820, 0xb0013f70},  {0xed5824, 0xf9417e11},  {0x124893c, 0xf81d0ff6},
    {0x1248948, 0xa942a009}, {0x1248950, 0xeb09011f}, {0x1248954, 0x540001a3}, {0x1248958, 0xf9401268}, {0x1248968, 0x9a883134},
    {0x1248970, 0x5292e309}, {0x1248978, 0x97f3d6a2}, {0x1248984, 0xf9001669}, {0x1248998, 0xf9001a68}, {0x12489c8, 0x97f27a0a},
    {0x11dc30c, 0xf81d0ff6}, {0x11dc310, 0xa90153f5}, {0x1248910, 0x2a1f03e0}, {0x18829c0, 0xb000f8d3}, {0x18829c4, 0xf9416268},
    {0x18829c8, 0xb9408503}, {0x18829cc, 0xf9401909}, {0x18829d0, 0xcb09006a}, {0x18829d4, 0xeb03015f},
};
inline constexpr std::size_t kWordCount = sizeof(kWords) / sizeof(kWords[0]);

// Each slot as a manager target on a libr15 loaded at `base`: it must hold the function itself (base + its address), so a word something else
// already replaced is refused, not chained over.
inline sentinel::GotTarget Target(const char* symbol, std::uint64_t slot, std::uint64_t fn, std::uintptr_t base) {
  sentinel::GotTarget target(sentinel::pinned::kLibR15, symbol, sentinel::RelocKind::kJumpSlot, sentinel::pinned::LibR15PatchedBuildId(), slot);
  target.expectedOriginal = reinterpret_cast<const void*>(base + fn);
  return target.Owned("combat/custctx");
}
inline sentinel::GotTarget LoadTarget(std::uintptr_t base, const Pins& pins = kPins) { return Target(kLoadSymbol, pins.slotLoad, pins.fnLoad, base); }
inline sentinel::GotTarget DestroyTarget(std::uintptr_t base, const Pins& pins = kPins) {
  return Target(kDestroySymbol, pins.slotDestroy, pins.fnDestroy, base);
}
inline sentinel::GotTarget GrowTarget(std::uintptr_t base, const Pins& pins = kPins) { return Target(kGrowSymbol, pins.slotGrow, pins.fnGrow, base); }

}  // namespace nevr_quest::combat::custctx
