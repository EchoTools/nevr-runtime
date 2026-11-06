/* The pinned slots, functions and code words of the scanner-sphere watchdog (sensorwd.h).
 *
 * Every address is an ELF virtual address in the store libr15 (build ID in pinned_targets.h, `LibR15PatchedBuildId`); the disassembly's address is the ELF
 * address plus 0x100000. src/quest/tests/got_pinned_test.cpp checks the three relocations and every code word against the real file
 * (`just test-quest-hooks-pinned`, which needs the store APK).
 *
 *   CModelCS::OnComponentEnabled   ABS64 0x36495c8 -> 0x15b127c  (a vtable word)
 *   CModelCS::OnComponentDisabled  ABS64 0x36495d0 -> 0x15b12a8  (the next word of the same vtable)
 *   CncaGameSpace::SendComponentEventGlobal  JUMP_SLOT 0x36fcae8 -> 0x180735c
 *
 * The same words are the ones kotorvr's echoquest-combat hooks (runtime/native/afkxr/afk_fx.cpp, build 4987566).
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "quest/sentinel/callback_thunk.h"
#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/pinned_targets.h"

namespace nevr_quest::combat::sensorwd::targets {

inline constexpr const char* kEnabledSymbol = "_ZN10NRadEngine8CModelCS18OnComponentEnabledENS_18SCompactPoolHandleE";
inline constexpr const char* kDisabledSymbol = "_ZN10NRadEngine8CModelCS19OnComponentDisabledENS_18SCompactPoolHandleE";
inline constexpr const char* kEventSymbol = "_ZN10NRadEngine13CncaGameSpace24SendComponentEventGlobalENS_9CSymbol64ENS_6SActorES1_j";

// The slots, the functions they hold, and the first word of each function (ELF vaddrs).
struct Pins {
  std::uint64_t enabledSlot, enabledFn;
  std::uint64_t disabledSlot, disabledFn;
  std::uint64_t eventSlot, eventFn;
};
inline constexpr Pins kPins = {0x36495c8ULL, 0x15b127cULL, 0x36495d0ULL, 0x15b12a8ULL, 0x36fcae8ULL, 0x180735cULL};

// CModelCS::OnComponentEnabled / OnComponentDisabled (this, SCompactPoolHandle): integers in x0 and x1, nothing on the stack. The result is handed back as it
// came. SendComponentEventGlobal (this, CSymbol64, SActor, CSymbol64, unsigned): integers in x0..x4.
struct EnabledTag {};
struct DisabledTag {};
struct EventTag {};
using ModelSig = std::uint64_t(void* component, std::uint64_t handle);
using EventSig = std::uint64_t(void* gamespace, std::uint64_t symbol, std::uint64_t actorA, std::uint64_t actorB, std::uint32_t flags);
using EnabledThunk = sentinel::CallbackThunk<EnabledTag, ModelSig>;
using DisabledThunk = sentinel::CallbackThunk<DisabledTag, ModelSig>;
using EventThunk = sentinel::CallbackThunk<EventTag, EventSig>;

// The code words the wrappers' reads were derived from, read before any word is changed: OnComponentEnabled (11), OnComponentDisabled (11) and
// the first 12 of SendComponentEventGlobal.
struct Word {
  std::uint64_t vaddr;
  std::uint32_t word;
};
inline constexpr std::size_t kWordCount = 34;
inline constexpr Word kWords[kWordCount] = {
    // CModelCS::OnComponentEnabled
    {0x15b127c, 0xf9406808},
    {0x15b1280, 0x92403c29},
    {0x15b1284, 0xd37ef529},
    {0x15b1288, 0x78696908},
    {0x15b128c, 0xf9408009},
    {0x15b1290, 0x8b081128},
    {0x15b1294, 0xf9400500},
    {0x15b1298, 0xb9400501},
    {0x15b129c, 0xf9400009},
    {0x15b12a0, 0xf9403122},
    {0x15b12a4, 0xd61f0040},
    // CModelCS::OnComponentDisabled
    {0x15b12a8, 0xf9406808},
    {0x15b12ac, 0x92403c29},
    {0x15b12b0, 0xd37ef529},
    {0x15b12b4, 0x2a1f03e1},
    {0x15b12b8, 0x78696908},
    {0x15b12bc, 0xf9408009},
    {0x15b12c0, 0x8b081128},
    {0x15b12c4, 0xf9400500},
    {0x15b12c8, 0xf9400008},
    {0x15b12cc, 0xf9403102},
    {0x15b12d0, 0xd61f0040},
    // CncaGameSpace::SendComponentEventGlobal
    {0x180735c, 0xd10103ff},
    {0x1807360, 0x92800008},
    {0x1807364, 0xa902fbe8},
    {0x1807368, 0xf9402c08},
    {0x180736c, 0xa9018fe2},
    {0x1807370, 0x2a0403e2},
    {0x1807374, 0xa900a3e1},
    {0x1807378, 0x910023e1},
    {0x180737c, 0x97dbd111},
    {0x1807380, 0xf9401bfe},
    {0x1807384, 0x910103ff},
    {0x1807388, 0xd65f03c0},
};

// The targets on a libr15 loaded at `base`: each slot must hold the game's function (a word another hook already replaced is refused, not chained over).
inline sentinel::GotTarget EnabledTarget(std::uintptr_t base, const Pins& pins = kPins) {
  sentinel::GotTarget target(sentinel::pinned::kLibR15, kEnabledSymbol, sentinel::RelocKind::kAbs64, sentinel::pinned::LibR15PatchedBuildId(),
                             pins.enabledSlot);
  target.expectedOriginal = reinterpret_cast<const void*>(base + pins.enabledFn);
  return target.Owned("combat_sensorwd");
}
inline sentinel::GotTarget DisabledTarget(std::uintptr_t base, const Pins& pins = kPins) {
  sentinel::GotTarget target(sentinel::pinned::kLibR15, kDisabledSymbol, sentinel::RelocKind::kAbs64, sentinel::pinned::LibR15PatchedBuildId(),
                             pins.disabledSlot);
  target.expectedOriginal = reinterpret_cast<const void*>(base + pins.disabledFn);
  return target.Owned("combat_sensorwd");
}
inline sentinel::GotTarget EventTarget(std::uintptr_t base, const Pins& pins = kPins) {
  sentinel::GotTarget target(sentinel::pinned::kLibR15, kEventSymbol, sentinel::RelocKind::kJumpSlot, sentinel::pinned::LibR15PatchedBuildId(),
                             pins.eventSlot);
  target.expectedOriginal = reinterpret_cast<const void*>(base + pins.eventFn);
  return target.Owned("combat_sensorwd");
}

}  // namespace nevr_quest::combat::sensorwd::targets
