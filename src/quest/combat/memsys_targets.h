/* The pinned slots and code words of the render-target pool module (memsys.h).
 *
 * Every address is an ELF virtual address in the store libr15 (build ID b243509c...); the disassembly's address is the ELF address plus
 * 0x100000. src/quest/tests/got_pinned_test.cpp checks the two JUMP_SLOT relocations and every code word against the real file.
 *
 *   R37  CSimplePool<SGRenderTargetLayout,uint>::Alloc   JUMP_SLOT 0x36e9b48 -> 0x1901b5c   (`readelf -rW` line 127997)
 *   R38  CSysJava::Init(android_app*)                    JUMP_SLOT 0x36de5c0 -> 0xf7e8e8    (`readelf -rW` line 122188)
 *
 * Both functions are libr15's own, so a slot holds `base + the function` in this BIND_NOW image (got_hook.h), and the target says so.
 *
 * Origins (docs/audits/2026-11-05-q15t-origin-citations.md): the layout-pool raise and the 21 pinned words are the reference build's
 * `runtime/native/afkxr/afk_memsys.cpp`:11-17,65-77,96-105 (pinned in that record); the layouts and addresses are re-read from the
 * disassembly.
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/pinned_targets.h"

namespace nevr_quest::combat::memsys {

inline constexpr const char* kAllocSymbol = "_ZN10NRadEngine11CSimplePoolINS_20SGRenderTargetLayoutEjE5AllocEjPNS_10CAllocatorE";
inline constexpr const char* kInitSymbol = "_ZN10NRadEngine8CSysJava4InitEP11android_app";

// The slots, the real functions and the two return addresses the wrappers act on (the instruction after the game's call).
struct Pins {
  std::uint64_t allocSlot, allocFn, allocSite;  // R37: the slot, the real function, the return address of InitCommon's call
  std::uint64_t initSlot, initFn, initSite;     // R38: the slot, the real function, the return address of android_main's first call
  std::uint64_t systemSlot;                     // the GLOB_DAT of kSystemMemoryAndroid (the witness reads what it points at)
};
inline constexpr Pins kPins = {0x36e9b48ULL, 0x1901b5cULL, 0x18818ccULL, 0x36de5c0ULL, 0xf7e8e8ULL, 0xf784e8ULL, 0x371d020ULL};

// One pinned instruction word and the row refused when it differs.
struct Word {
  std::uint64_t vaddr;
  std::uint32_t word;
  int row;  // 37 or 38
};
inline constexpr std::size_t kWordCount = 21;
inline constexpr Word kWords[kWordCount] = {
    {0x18818b8, 0x52804001, 37},  // InitCommon: mov w1,#0x200
    {0x18818c8, 0x97da8bfe, 37},  // bl <PLT Alloc>
    {0x18818cc, 0xa942ae6a, 37},  // ldp x10,x11,[x19,#0x28]  (the return address of that call)
    {0x18818d8, 0x52803f6c, 37},  // mov w12,#0x1fb
    {0x18818e0, 0x4f00c420, 37},  // movi v0.4s,#0x1,msl #8 (0x1ff)
    {0x18818e8, 0x91080169, 37},  // add x9,x11,#0x200 (the free count the inline fill stores)
    {0x1881904, 0xf120011f, 37},  // cmp x8,#0x800 (512 x 4 B)
    {0x1881914, 0xf9001a69, 37},  // str x9,[x19,#0x30]
    {0xf248c0, 0xb0013e30, 37},   // PLT stub Alloc: adrp x16,0x36e9000
    {0xf248c4, 0xf945a611, 37},   // ldr x17,[x16,#0xb48]
    {0x1901b5c, 0xd10143ff, 37},  // Alloc: sub sp,sp,#0x50
    {0x1901c24, 0xad0002c0, 37},  // stp q0,q0,[x22]  (count 0)
    {0x1901c30, 0xf9001e75, 37},  // str x21,[x19,#0x38]  (stack cap)
    {0x1901c4c, 0xf9001660, 37},  // str x0,[x19,#0x28]  (stack)
    {0xf784e4, 0x97fe5633, 38},   // android_main: bl <PLT Init> (first call)
    {0xf784e8, 0x97fdb586, 38},   // bl <PLT CSymbol64::Initialize> (the return address of that call)
    {0xf0ddb0, 0xb0013e90, 38},   // PLT stub Init: adrp x16,0x36de000
    {0xf0ddb4, 0xf942e211, 38},   // ldr x17,[x16,#0x5c0]
    {0xf78510, 0xb0013d28, 38},   // android_main: adrp x8,0x371d000
    {0xf78514, 0xf9401108, 38},   // ldr x8,[x8,#0x20]  (the System GLOB_DAT word)
    {0xf78524, 0xf9400101, 38},   // ldr x1,[x8]  (the size, once)
};

// CSimplePool<SGRenderTargetLayout> layout, read from Alloc's body (0x1901b5c..0x1901c80): +0x28 free-stack pointer (str x0,[x19,#0x28]
// at 0x1901c4c), +0x30 free count (zeroed by stp q0,q0,[x22] at 0x1901c24, x22 = pool+0x20), +0x38 free-stack capacity in entries
// (str x21,[x19,#0x38] at 0x1901c30) and +0x84 capacity (str w21,[x19,#0x84] at 0x1901bbc).
inline constexpr std::uintptr_t kOffStack = 0x28, kOffCount = 0x30, kOffStackCap = 0x38, kOffCap = 0x84;
inline constexpr std::uint32_t kCapStock = 0x200, kCapNew = 0x400;

using AllocSig = std::uint64_t(void* pool, std::uint32_t capacity, void* allocator);
using InitSig = std::uint64_t(void* app);
struct AllocTag {};
struct InitTag {};
// kCaller: the handler is told the instruction after the game's call, which is how it tells InitCommon's call from any other.
using AllocThunk = sentinel::CallbackThunk<AllocTag, AllocSig, /*kCaller=*/true>;
using InitThunk = sentinel::CallbackThunk<InitTag, InitSig, /*kCaller=*/true>;

// The slot as a manager target on a libr15 loaded at `base`: the slot must hold the real function (base + fn).
inline sentinel::GotTarget AllocTarget(std::uintptr_t base, const Pins& pins) {
  sentinel::GotTarget target(sentinel::pinned::kLibR15, kAllocSymbol, sentinel::RelocKind::kJumpSlot,
                             sentinel::pinned::LibR15PatchedBuildId(), pins.allocSlot);
  target.expectedOriginal = reinterpret_cast<const void*>(base + pins.allocFn);
  return target.Owned("combat_memsys");
}
inline sentinel::GotTarget InitTarget(std::uintptr_t base, const Pins& pins) {
  sentinel::GotTarget target(sentinel::pinned::kLibR15, kInitSymbol, sentinel::RelocKind::kJumpSlot,
                             sentinel::pinned::LibR15PatchedBuildId(), pins.initSlot);
  target.expectedOriginal = reinterpret_cast<const void*>(base + pins.initFn);
  return target.Owned("combat_memsys");
}

}  // namespace nevr_quest::combat::memsys
