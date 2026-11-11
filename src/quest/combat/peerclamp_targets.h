/* The game words peerclamp reads and hooks (#2178), as ELF vaddrs of the store libr15 (build ID b243509c...; the disassembly lists each
 * 0x100000 higher). got_pinned_test.cpp checks every line below against the store file: the relocation, the symbol's definition, the
 * invalid-address word and each code word.
 *
 *   SBroadcasterData::CleanupPeers()   defined at 0x24ef9b0 (1260 bytes), JUMP_SLOT 0x36efad8 (the one slot that names it; its one caller is
 *   CBroadcaster::Receive(uint) at 0x24eeec8, `ldr x0,[x19]; bl <PLT CleanupPeers>`, x0 the SBroadcasterData).
 *   CSysNet::SAddress::kInvalid        reached through the GOT word at 0x371d538 (`adrp x23,0x381d000; ldr x23,[x23,#0x538]` at 0x24efa14/18).
 *
 * What CleanupPeers reads (the disassembly): the head index `ldr w27,[x0,#0x554]` (0x24ef9d8), entries per chunk `ldr w8,[x19,#0x560]` (0x24efa68),
 * the chunk pointer table `ldr x9,[x19,#0x518]` (0x24efa6c), "now" `ldr x24,[x19,#0x478]` (0x24efa74); per entry (stride `mov w28,#0x948`, 0x24efa30)
 * the next index `ldr w27,[x8,#8]` (0x24efa90), the last-received stamp `ldr x25,[x8,#0x228]` (0x24efa94) and the key at +0x18 (`add x22,x8,#0x18`,
 * 0x24efa98) compared with kInvalid over 0x80 bytes (`mov w2,#0x80`, 0x24efa84; `cbz x0` at 0x24efaa4); then the unsigned age `sub x25,x24,x25`
 * (0x24efae4), with no clamp.
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

namespace nevr_quest::combat::peerclamp {

inline constexpr const char* kCleanupSymbol = "_ZN10NRadEngine16SBroadcasterData12CleanupPeersEv";
inline constexpr std::uint64_t kCleanupVaddr = 0x24ef9b0ULL;
inline constexpr std::uint64_t kCleanupSlotVaddr = 0x36efad8ULL;
inline constexpr std::uint64_t kInvalidWordVaddr = 0x371d538ULL;  // the GOT word that holds &SAddress::kInvalid

// SBroadcasterData and one peer entry, as CleanupPeers reads them.
inline constexpr std::uintptr_t kOffHead = 0x554, kOffPerChunk = 0x560, kOffChunks = 0x518, kOffNow = 0x478, kOffSent = 0x470;
inline constexpr std::uintptr_t kStride = 0x948, kOffNext = 0x8, kOffKey = 0x18, kOffStamp = 0x228;
inline constexpr std::size_t kKeyLen = 0x80;
inline constexpr std::uint32_t kEnd = 0xffffffffU;  // the list's end marker

// void CleanupPeers(SBroadcasterData* x0).
using CleanupSig = void(void* bd);
struct CleanupTag {};
using CleanupThunk = sentinel::CallbackThunk<CleanupTag, CleanupSig>;

// The code words the walk and the unsigned age were derived from (26).
inline constexpr pinned::Word kWords[] = {
    {"CleanupPeers: stp d13, d12, [sp, #-0x90]!", 0x24ef9b0, 0x6db733ed},  // CleanupPeers: the prologue
    {"CleanupPeers: stp d11, d10, [sp, #0x10]", 0x24ef9b4, 0x6d012beb},
    {"CleanupPeers: ldr w27, [x0, #0x554]", 0x24ef9d8, 0xb945541b},
    {"CleanupPeers: adrp x23", 0x24efa14, 0xd0009177},
    {"CleanupPeers: ldr x23, [x23, #0x538]", 0x24efa18, 0xf9429ef7},
    {"CleanupPeers: mov x19, x0", 0x24efa2c, 0xaa0003f3},
    {"CleanupPeers: mov w28, #0x948", 0x24efa30, 0x5281291c},
    {"CleanupPeers: ldr w8, [x19, #0x560]", 0x24efa68, 0xb9456268},
    {"CleanupPeers: ldr x9, [x19, #0x518]", 0x24efa6c, 0xf9428e69},
    {"CleanupPeers: ldr x24, [x19, #0x478]", 0x24efa74, 0xf9423e78},
    {"CleanupPeers: udiv w10, w27, w8", 0x24efa78, 0x1ac80b6a},
    {"CleanupPeers: ldr x20, [x9, w10, uxtw #3]", 0x24efa7c, 0xf86a5934},
    {"CleanupPeers: msub w26, w10, w8, w27", 0x24efa80, 0x1b08ed5a},
    {"CleanupPeers: mov w2, #0x80", 0x24efa84, 0x52801002},
    {"CleanupPeers: umaddl x8, w26, w28, x20", 0x24efa8c, 0x9bbc5348},
    {"CleanupPeers: ldr w27, [x8, #0x8]", 0x24efa90, 0xb940091b},  // ldr w27,[x8,#8]       next index
    {"CleanupPeers: ldr x25, [x8, #0x228]", 0x24efa94, 0xf9411519},
    {"CleanupPeers: add x22, x8, #0x18", 0x24efa98, 0x91006116},
    {"CleanupPeers: cbz x0, <target>", 0x24efaa4, 0xb4fffd40},  // cbz x0,<previous>     key == kInvalid
    {"CleanupPeers: sub x25, x24, x25", 0x24efae4, 0xcb190319},
    {"Receive: ldr x0, [x19]", 0x24eeec8, 0xf9400260},
    {"Receive: bl <target>", 0x24eeecc, 0x97a90645},  // bl <PLT CleanupPeers>
    {"CleanupPeers PLT stub: adrp x16", 0xf307e0, 0xf0013df0},
    {"CleanupPeers PLT stub: ldr x17, [x16, #0xad8]", 0xf307e4, 0xf9456e11},
    {"CleanupPeers PLT stub: add x16, x16, #0xad8", 0xf307e8, 0x912b6210},
    {"CleanupPeers PLT stub: br x17", 0xf307ec, 0xd61f0220},
};
inline constexpr std::size_t kWordCount = sizeof(kWords) / sizeof(kWords[0]);
inline constexpr pinned::Pins kWordPins = {kWords, kWordCount, nullptr, 0};
// The words carry the offsets the guard reads: a changed offset in this file fails the build, a different libr15 fails the install.
static_assert(pinned::LdrWOffset(kWords[2].word) == kOffHead, "the pinned head index does not carry kOffHead");
static_assert(pinned::LdrWOffset(kWords[7].word) == kOffPerChunk, "the pinned entries per chunk does not carry kOffPerChunk");
static_assert(pinned::LdrXOffset(kWords[8].word) == kOffChunks, "the pinned chunk pointer table does not carry kOffChunks");
static_assert(pinned::LdrXOffset(kWords[9].word) == kOffNow, "the pinned now does not carry kOffNow");
static_assert(pinned::MovImmediate(kWords[6].word) == kStride, "the pinned entry stride does not carry kStride");
static_assert(pinned::MovImmediate(kWords[13].word) == kKeyLen, "the pinned key length does not carry kKeyLen");
static_assert(pinned::LdrWOffset(kWords[15].word) == kOffNext, "the pinned next index does not carry kOffNext");
static_assert(pinned::LdrXOffset(kWords[16].word) == kOffStamp, "the pinned last-received stamp does not carry kOffStamp");
static_assert(pinned::AddImmediate(kWords[17].word) == kOffKey, "the pinned entry key does not carry kOffKey");

// The slot as a manager target on a libr15 loaded at `base`: it must hold CleanupPeers itself (base + its address), so a word something else
// already replaced is refused, not chained over.
inline sentinel::GotTarget CleanupTarget(std::uintptr_t base) {
  sentinel::GotTarget target(sentinel::pinned::kLibR15, kCleanupSymbol, sentinel::RelocKind::kJumpSlot,
                             sentinel::pinned::LibR15PatchedBuildId(), kCleanupSlotVaddr);
  target.expectedOriginal = reinterpret_cast<const void*>(base + kCleanupVaddr);
  return target.Owned("combat/peerclamp");
}

}  // namespace nevr_quest::combat::peerclamp
