/* The pinned slot, function and code words of the combat music module (music.h).
 *
 * Every address is an ELF virtual address in the store libr15 (build ID in pinned_targets.h, `LibR15PatchedBuildId`); the disassembly's address is the ELF
 * address plus 0x100000. src/quest/tests/got_pinned_test.cpp checks the relocation and every code word against the real file
 * (`just test-quest-hooks-pinned`, which needs the store APK).
 *
 *   CWwiseEngine::PostSoundEvent  JUMP_SLOT 0x36d76e8 -> 0x2549264  (`readelf -rW`)
 *
 * The same slot and function are the ones kotorvr's echoquest-combat music module hooks (runtime/native/afkxr/afk_music.cpp:51-52, build 4987566).
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/sentinel/callback_thunk.h"
#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/pinned_targets.h"

namespace nevr_quest::combat::music::targets {

inline constexpr const char* kSymbol = "_ZN10NRadEngine12CWwiseEngine14PostSoundEventENS_12CSoundSymbolEjNS_7CFlagsTIjLy1EEEPFviPvES4_jRj";
inline constexpr std::uint64_t kSlotVaddr = 0x36d76e8ULL;   // the JUMP_SLOT libr15 calls it through
inline constexpr std::uint64_t kStockVaddr = 0x2549264ULL;  // the game's function: what a slot nobody hooked holds

// The slot and the function it holds, as one value a host test can replace (its stand-in for the game's function is not at the pinned address).
struct Pins {
  std::uint64_t slotVaddr, stockVaddr;
};
inline constexpr Pins kPins = {kSlotVaddr, kStockVaddr};

// PostSoundEvent(self x0, event w1, object w2, &flags x3, callback x4, cookie x5, seek ms w6, &playing id x7) -> x0. The flags are a
// CFlags<unsigned> passed by reference (`ldr w8,[x3]`); the result is handed back as it came (its low word is the post's status).
struct Tag {};
using Sig = std::uint64_t(void* self, std::uint32_t event, std::uint32_t object, std::uint32_t* flags, void* callback, void* cookie,
                          std::uint32_t seekMs, std::uint32_t* playingId);
using Thunk = sentinel::CallbackThunk<Tag, Sig>;

// The first 29 words of PostSoundEvent: the prologue and everything up to the seek test that fixes the argument layout the extra Stop posts use.
// Read before the slot is changed.
struct Word {
  std::uint64_t vaddr;
  std::uint32_t word;
};
inline constexpr std::size_t kWordCount = 29;
inline constexpr Word kWords[kWordCount] = {
    {0x2549264, 0xd10183ff},  // sub sp, sp, #0x60
    {0x2549268, 0xa9016bfb},  // stp x27, x26, [sp, #0x10]
    {0x254926c, 0xa90263f9},  // stp x25, x24, [sp, #0x20]
    {0x2549270, 0xa9035bf7},  // stp x23, x22, [sp, #0x30]
    {0x2549274, 0xa90453f5},  // stp x21, x20, [sp, #0x40]
    {0x2549278, 0xa9057bf3},  // stp x19, x30, [sp, #0x50]
    {0x254927c, 0xb9400068},  // ldr w8, [x3]            flags = *x3
    {0x2549280, 0x2a0203e9},  // mov w9, w2              object
    {0x2549284, 0x3100045f},  // cmn w2, #0x1
    {0x2549288, 0xaa0103f4},  // mov x20, x1             event
    {0x254928c, 0xda9f1135},  // csinv x21, x9, xzr, ne  object, or all ones for 0xffffffff
    {0x2549290, 0xaa0703f6},  // mov x22, x7             &playing id
    {0x2549294, 0x2a0603f3},  // mov w19, w6             seek ms
    {0x2549298, 0xaa0503f7},  // mov x23, x5             cookie
    {0x254929c, 0xaa0403f8},  // mov x24, x4             callback
    {0x25492a0, 0xaa0303f9},  // mov x25, x3
    {0x25492a4, 0xaa0003fa},  // mov x26, x0             self
    {0x25492a8, 0x2a1403e0},  // mov w0, w20
    {0x25492ac, 0xaa1503e1},  // mov x1, x21
    {0x25492b0, 0x2a0803e2},  // mov w2, w8
    {0x25492b4, 0xaa0403e3},  // mov x3, x4
    {0x25492b8, 0xaa0503e4},  // mov x4, x5
    {0x25492bc, 0x2a1f03e5},  // mov w5, wzr
    {0x25492c0, 0xaa1f03e6},  // mov x6, xzr
    {0x25492c4, 0x2a1f03e7},  // mov w7, wzr
    {0x25492c8, 0x9410088e},  // bl 0x02a4b500
    {0x25492cc, 0xb90002c0},  // str w0, [x22]           *playing id = result
    {0x25492d0, 0x340003c0},  // cbz w0, 0x02649348
    {0x25492d4, 0x34000b73},  // cbz w19, 0x02649440     seek 0: no seek call
};

// The slot as a manager target on a libr15 loaded at `base`: it must hold the game's function (base + the pins' function), or the newest hook the
// manager already has on the slot (got_hook.h, "Several handlers may hold one slot").
inline sentinel::GotTarget Target(std::uintptr_t base, const Pins& pins = kPins) {
  sentinel::GotTarget target(sentinel::pinned::kLibR15, kSymbol, sentinel::RelocKind::kJumpSlot, sentinel::pinned::LibR15PatchedBuildId(),
                             pins.slotVaddr);
  target.expectedOriginal = reinterpret_cast<const void*>(base + pins.stockVaddr);
  return target.Owned("combat_music");
}

}  // namespace nevr_quest::combat::music::targets
