/* The game words droneguard reads and hooks (#2178), as ELF vaddrs of the store libr15 (build ID b243509c...; the disassembly lists each
 * 0x100000 higher). got_pinned_test.cpp checks every line below against the store file: the relocation, each symbol's definition and each code
 * word.
 *
 *   CR15NetFollowPlayerCS::Follow(SCompactPoolHandle)                 defined at 0x2149764, JUMP_SLOT 0x36f1218 (the one slot that names it; its one caller
 *                                                                     is CR15NetFollowPlayerCS::Update, through the PLT stub)
 *   CComponentHandle<CR15PlayerNavCS>::RefreshCache() const           defined at 0x1210848 (stage 1 calls it; exported, resolved by dlsym)
 *   CR15PlayerNavCS::IsComponentValid(SCompactPoolHandle) const       defined at 0x1cdb3e0 (stage 1 calls it; exported, resolved by dlsym)
 *
 * What Follow does with the words pinned below: it loads the followed system from [cs+0x140] (0x21497bc `ldr x20,[x19,#0x140]`), passes it as `this`
 * to CR15PlayerNavCS::FinalTransform with no null check, and reads the player handle's cache at cs+0x148: the cached system [cs+0x188], the flags
 * byte [cs+0x16c] and the cached index [cs+0x190] (0x21497cc `ldr w1,[x19,#0x190]`, 0x214980c `ldrh w8,[x19,#0x190]`).
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

namespace nevr_quest::combat::droneguard {

inline constexpr const char* kFollowSymbol = "_ZN10NRadEngine8NRadGame21CR15NetFollowPlayerCS6FollowENS_18SCompactPoolHandleE";
inline constexpr const char* kRefreshCacheSymbol = "_ZNK10NRadEngine16CComponentHandleINS_8NRadGame15CR15PlayerNavCSEE12RefreshCacheEv";
inline constexpr const char* kIsValidSymbol = "_ZNK10NRadEngine8NRadGame15CR15PlayerNavCS16IsComponentValidENS_18SCompactPoolHandleE";
inline constexpr std::uint64_t kFollowVaddr = 0x2149764ULL;
inline constexpr std::uint64_t kFollowSlotVaddr = 0x36f1218ULL;
inline constexpr std::uint64_t kRefreshCacheVaddr = 0x1210848ULL;
inline constexpr std::uint64_t kIsValidVaddr = 0x1cdb3e0ULL;

// CR15NetFollowPlayerCS layout as Follow reads it: the followed system, the player handle at +0x148 and its cached flags, system and index.
inline constexpr std::uintptr_t kOffTarget = 0x140, kOffHandle = 0x148, kOffHandleFlags = 0x16c, kOffHandleSys = 0x188, kOffHandleIdx = 0x190;

// void Follow(CR15NetFollowPlayerCS* x0, SCompactPoolHandle x1).
using FollowSig = void(void* cs, std::uint64_t handle);
struct FollowTag {};
using FollowThunk = sentinel::CallbackThunk<FollowTag, FollowSig>;

// The two engine functions stage 1 calls.
struct EngineFns {
  void (*refresh)(void* handle);                           // RefreshCache() const, called on cs+0x148
  bool (*isValid)(const void* sys, std::uint64_t handle);  // IsComponentValid(SCompactPoolHandle) const
};

// Stage 0: the words Follow's null-target read was derived from.
inline constexpr pinned::Word kWords[] = {
    {"Follow: ldr x20, [x19, #0x140]", 0x21497bc, 0xf940a274},
    {"Update: bl <target>", 0x2149744, 0x97b7a7c7},
    {"Follow: ldr w1, [x19, #0x190]", 0x21497cc, 0xb9419261},
    {"Follow: ldrh w8, [x19, #0x190]", 0x214980c, 0x79432268},
};
inline constexpr std::size_t kWordCount = sizeof(kWords) / sizeof(kWords[0]);
inline constexpr pinned::Pins kWordPins = {kWords, kWordCount, nullptr, 0};
// The words carry the offsets the guard reads: a changed offset in this file fails the build, a different libr15 fails the install.
static_assert(pinned::LdrXOffset(kWords[0].word) == kOffTarget, "the pinned load of the followed system does not carry kOffTarget");
static_assert(pinned::LdrWOffset(kWords[2].word) == kOffHandleIdx, "the pinned load of the cached index does not carry kOffHandleIdx");
static_assert(pinned::LdrhOffset(kWords[3].word) == kOffHandleIdx, "the pinned half-word load of the cached index does not carry kOffHandleIdx");
// Stage 1's own words (the RefreshCache call site, IsComponentValid's first words): a mismatch leaves the null half only.
inline constexpr pinned::Word kIdxWords[] = {
    {"Follow: bl <target>", 0x21497c8, 0x97b70b0a},
    {"IsComponentValid: ldrh w8, [x0, #0xf0]", 0x1cdb3e0, 0x7941e008},
    {"IsComponentValid: cmp w9, w1, lsr #16", 0x1cdb3f8, 0x6b41413f},
};
inline constexpr std::size_t kIdxWordCount = sizeof(kIdxWords) / sizeof(kIdxWords[0]);
inline constexpr pinned::Pins kIdxWordPins = {kIdxWords, kIdxWordCount, nullptr, 0};

// The slot as a manager target on a libr15 loaded at `base`: it must hold Follow itself (base + its address).
inline sentinel::GotTarget FollowTarget(std::uintptr_t base) {
  sentinel::GotTarget target(sentinel::pinned::kLibR15, kFollowSymbol, sentinel::RelocKind::kJumpSlot, sentinel::pinned::LibR15PatchedBuildId(),
                             kFollowSlotVaddr);
  target.expectedOriginal = reinterpret_cast<const void*>(base + kFollowVaddr);
  return target.Owned("combat/droneguard");
}

}  // namespace nevr_quest::combat::droneguard
