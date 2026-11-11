/* The game words fxtrail reads and hooks (#2178), as ELF vaddrs of the store libr15 (build ID b243509c...; the disassembly lists each
 * 0x100000 higher). got_pinned_test.cpp checks every line below against the store file: the relocation, the symbol's definition and each code
 * word.
 *
 *   CParticleEffectCS::EffectTime(SCompactPoolHandle) const   defined at 0x16647e4 (56 bytes), JUMP_SLOT 0x36e7b98
 *   (`readelf -rW`: R_AARCH64_JUMP_SLOT, symbol value 0x16647e4; the one slot that names it)
 *
 * The function, as the disassembly reads (0x16647e4..0x1664818): `ldr x8,[x0,#0xc8]` (the index table), `and x9,x1,#0xffff; lsl x9,x9,#2`,
 * `ldrh w8,[x8,x9]` (the data index), `ldr x9,[x0,#0xf8]` (the data array), `madd x8,x8,0x18,x9` (stride 0x18), `ldr x8,[x8,#0x10]` (the instance),
 * `cbz x8,<ret 0.0>`, else `ldr s0,[x8,#0x340]` (the effect's time). So a null instance answers 0.0, and the guard repeats exactly this lookup.
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

namespace nevr_quest::combat::fxtrail {

inline constexpr const char* kEffectTimeSymbol = "_ZNK10NRadEngine17CParticleEffectCS10EffectTimeENS_18SCompactPoolHandleE";
inline constexpr std::uint64_t kEffectTimeVaddr = 0x16647e4ULL;
inline constexpr std::uint64_t kEffectTimeSlotVaddr = 0x36e7b98ULL;

// CParticleEffectCS layout as EffectTime reads it: the index table pointer at +0xc8 (a u16 per handle, at 4 * (handle & 0xffff)), the data array
// pointer at +0xf8 (0x18 bytes an entry) and the entry's instance pointer at +0x10.
inline constexpr std::uintptr_t kOffIndex = 0xc8, kOffData = 0xf8, kDataStride = 0x18, kOffInstance = 0x10;

// float EffectTime(const CParticleEffectCS* x0, SCompactPoolHandle x1).
using EffectTimeSig = float(const void* cs, std::uint64_t handle);
struct EffectTimeTag {};
using EffectTimeThunk = sentinel::CallbackThunk<EffectTimeTag, EffectTimeSig>;

// The code words the lookup was derived from (the 11 of EffectTime's 13 instructions before the two after `ret`).
inline constexpr pinned::Word kWords[] = {
    {"EffectTime: ldr x8, [x0, #0xc8]", 0x16647e4, 0xf9406408},
    {"EffectTime: and x9, x1, #0xffff", 0x16647e8, 0x92403c29},
    {"EffectTime: lsl x9, x9, #2", 0x16647ec, 0xd37ef529},
    {"EffectTime: mov w10, #0x18", 0x16647f0, 0x5280030a},
    {"EffectTime: ldrh w8, [x8, x9]", 0x16647f4, 0x78696908},
    {"EffectTime: ldr x9, [x0, #0xf8]", 0x16647f8, 0xf9407c09},
    {"EffectTime: madd x8, x8, x10, x9", 0x1664800, 0x9b0a2508},
    {"EffectTime: ldr x8, [x8, #0x10]", 0x1664804, 0xf9400908},
    {"EffectTime: cbz x8, <target>", 0x1664808, 0xb4000068},  // cbz x8,+0xc
    {"EffectTime: ldr s0, [x8, #0x340]", 0x166480c, 0xbd434100},
    {"EffectTime: ret", 0x1664810, 0xd65f03c0},
};
inline constexpr std::size_t kWordCount = sizeof(kWords) / sizeof(kWords[0]);
inline constexpr pinned::Pins kWordPins = {kWords, kWordCount, nullptr, 0};
// The words carry the offsets the guard reads: a changed offset in this file fails the build, a different libr15 fails the install.
static_assert(pinned::LdrXOffset(kWords[0].word) == kOffIndex, "the pinned load of the index table does not carry kOffIndex");
static_assert(pinned::LdrXOffset(kWords[5].word) == kOffData, "the pinned load of the data array does not carry kOffData");
static_assert(pinned::MovImmediate(kWords[3].word) == kDataStride, "the pinned entry stride is not kDataStride");
static_assert(pinned::LdrXOffset(kWords[7].word) == kOffInstance, "the pinned load of the instance does not carry kOffInstance");

// The slot as a manager target on a libr15 loaded at `base`: it must hold EffectTime itself (base + its address), so a word something else
// already replaced is refused, not chained over.
inline sentinel::GotTarget EffectTimeTarget(std::uintptr_t base) {
  sentinel::GotTarget target(sentinel::pinned::kLibR15, kEffectTimeSymbol, sentinel::RelocKind::kJumpSlot,
                             sentinel::pinned::LibR15PatchedBuildId(), kEffectTimeSlotVaddr);
  target.expectedOriginal = reinterpret_cast<const void*>(base + kEffectTimeVaddr);
  return target.Owned("combat/fxtrail");
}

}  // namespace nevr_quest::combat::fxtrail
