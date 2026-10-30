// custctx (source build RE-33, the source build's custctx v1-v4; rows R31, R32, R33; default ON, option Q15T_CUSTCTX): three guards on the
// lobby cosmetics code (CR15NetCustomizationRenderer), all GOT wrappers on libr15's JUMP_SLOTs, installed all or none.
//
//  R31 LoadMeshData (slot ELF 0x36cf6f0 -> fn 0x1247d90): marks the context live and "load in flight" (under one mutex) before
//      the real function runs. v4: refuses a load (returns 1, the stock failure value; CreateContext then destroys the empty
//      context and returns NULL) when the render-target layout pool (pointer at ELF 0x379b2c0: +0x84 capacity, +0x30 free
//      count) has fewer than 64 free slots, because a full pool is a deliberate trap (brk) in CGRenderTargetState::Initialize.
//  R32 DestroyContext (slot 0x36c22f8 -> fn 0x12492fc): runs the real function only for a live context with no load in flight
//      and no destroy already running; every other destroy is dropped (one INFO line, capped): the destroy of a context being
//      loaded, being destroyed on another thread, or at an address no live context holds (a stale second destroy).
//  R33 CTableXT<SR15NetCustomDrawData>::Grow (slot 0x36ed7f0 -> fn 0x124893c): does Grow's resize itself (CMemBlock::Resize, fn
//      0x11dc30c) and checks it; a table that would be written through NULL (resize failed, add 0, or room left but data NULL)
//      is made a consistent empty one and Grow gets a spare element that is in no table (that player's cosmetics are not drawn).
// Data-page hooks only; no code word is written. Details, evidence and prediction: docs/quest/q15t/modules/custctx.md.
#pragma once

#include <cstddef>
#include <cstdint>

#include "import_hook.h"
#include "word_pins.h"

namespace q15t {

struct CustCtxAddrs {
  std::uintptr_t got_load, fn_load, got_destroy, fn_destroy, got_grow, fn_grow, fn_resize, rt_pool;  // ELF vaddrs
};
inline constexpr CustCtxAddrs kCustCtxPinned = {0x36cf6f0, 0x1247d90, 0x36c22f8, 0x12492fc, 0x36ed7f0, 0x124893c, 0x11dc30c, 0x379b2c0};

struct CustCtxFns {
  void (*resize)(void* block, std::uint64_t bytes, std::uint32_t keep);  // CMemBlock::Resize(u64, u32)
};

// The Resize export of the loaded game library; false unless it is exactly at base + addrs.fn_resize.
bool ResolveCustCtxFns(std::uintptr_t base, const CustCtxAddrs& addrs, CustCtxFns* out);
// The address check ResolveCustCtxFns makes on what dlsym returned (a seam for the host test).
bool CustCtxResizeMatches(std::uintptr_t base, const CustCtxAddrs& addrs, const void* resize, CustCtxFns* out);

// `<q15t data dir>/custctx.off` present: refused (reason "off_file", one INFO line). Anything else that stops it: one ERROR
// line and the first failure's InstallResult. In every refusal no slot is left swapped.
inline constexpr WordPin kCustCtxWordPins[] = {
    {0x1247d90, 0x6db82beb},
    {0x1247d94, 0x6d0123e9},
    {0x12492fc, 0xb40008a1},
    {0x1249300, 0xf81b0ffa},
    {0x1249348, 0xf9405668},
    {0x12493f0, 0x912fe2a9},
    {0x1249634, 0xaa1503e1},
    {0x1249644, 0x97f29a73},
    {0x1249650, 0x340000c0},
    {0x124965c, 0x97f23071},
    {0xef0010, 0xf0013ef0},
    {0xef0014, 0xf9437a11},
    {0xed5820, 0xb0013f70},
    {0xed5824, 0xf9417e11},
    {0x124893c, 0xf81d0ff6},
    {0x1248948, 0xa942a009},
    {0x1248950, 0xeb09011f},
    {0x1248954, 0x540001a3},
    {0x1248958, 0xf9401268},
    {0x1248968, 0x9a883134},
    {0x1248970, 0x5292e309},
    {0x1248978, 0x97f3d6a2},
    {0x1248984, 0xf9001669},
    {0x1248998, 0xf9001a68},
    {0x12489c8, 0x97f27a0a},
    {0x11dc30c, 0xf81d0ff6},
    {0x11dc310, 0xa90153f5},
    {0x1248910, 0x2a1f03e0},
    {0x18829c0, 0xb000f8d3},
    {0x18829c4, 0xf9416268},
    {0x18829c8, 0xb9408503},
    {0x18829cc, 0xf9401909},
    {0x18829d0, 0xcb09006a},
    {0x18829d4, 0xeb03015f},
};
inline constexpr std::size_t kCustCtxWordPinCount = sizeof kCustCtxWordPins / sizeof kCustCtxWordPins[0];
// True when every pin matches at base + its vaddr (one ERROR line naming the first that does not). No MD5 or other file-hash gate.
bool CustCtxWordPinsMatch(std::uintptr_t base);

InstallResult InstallCustCtx(std::uintptr_t base);
InstallResult InstallCustCtxAt(std::uintptr_t base, const CustCtxAddrs& addrs, const CustCtxFns& fns);

struct CustCtxStats {
  unsigned destroys_dropped, grow_failures, loads_refused;
};
CustCtxStats GetCustCtxStats();

// Test seams: the kill-file directory; counters and tracking back to zero; the allocator the spare elements come from.
void SetCustCtxDirForTest(const char* dir);
void ResetCustCtxForTest();
void SetCustCtxMemalignForTest(int (*memalign)(void**, std::size_t, std::size_t));  // nullptr = posix_memalign

}  // namespace q15t
