/* The pinned slots and functions of dronekill (dronekill.h).
 *
 * Every address is an ELF virtual address in the store libr15 (build ID in pinned_targets.h, `LibR15PatchedBuildId`); the disassembly's address
 * is the ELF address plus 0x100000. src/quest/tests/got_pinned_test.cpp checks each JUMP_SLOT relocation against the real file
 * (`just test-quest-hooks-pinned`, which needs the store APK); `llvm-readelf -rW` of the store libr15 (sha256 8dd9a961...) gives:
 *
 *   SComponentNodeUtils::Actor            JUMP_SLOT 0x36e2080 -> 0x2059714  (Enter's lookup of the drone's game space and SActor)
 *   CEnableComponentNode::Enter           JUMP_SLOT 0x36cacf8 -> 0x263b700  (the scripts' enable/disable-component node)
 *   CR15NetAutoTargetCS::FireBulletLocal  JUMP_SLOT 0x36c37a8 -> 0x210bff8  (a drone's shot: lock-on and the shot velocity)
 *   CR15NetAutoTargetCS::AttackTarget     JUMP_SLOT 0x36e6610 -> 0x210b8c4  (a drone's per-frame attack: chase)
 *   BinarySearch's comparator             JUMP_SLOT 0x36f91f0 -> 0x1524cb4, its PLT stub 0xf43610 (read only: every Component() body's bl lands there)
 *
 * Each of the four hooked symbols has exactly one relocation in the file (no GLOB_DAT word), so the JUMP_SLOT is every call the game makes
 * through the PLT.
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstdint>

#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/pinned_targets.h"

namespace nevr_quest::combat::dronekill {

inline constexpr const char* kActorSymbol =
    "_ZN10NRadEngine19SComponentNodeUtils5ActorERKNS_13SScriptThreadERKNS_15CncaGameSpaceMTENS_9CSymbol64ERPS4_RNS_6SActorERj";
inline constexpr const char* kEnterSymbol = "_ZN10NRadEngine20CEnableComponentNode5EnterERKNS_24SEnableComponentNodeDataE";
inline constexpr const char* kFireSymbol =
    "_ZN10NRadEngine8NRadGame19CR15NetAutoTargetCS15FireBulletLocalENS_18SCompactPoolHandleENS_8C3VectorEj";
inline constexpr const char* kAttackSymbol = "_ZN10NRadEngine8NRadGame19CR15NetAutoTargetCS12AttackTargetENS_18SCompactPoolHandleE";

// The slots and the functions they hold, the comparator's slot, function and PLT stub (ELF vaddrs).
struct Pins {
  std::uint64_t actorSlot, actorFn;
  std::uint64_t enterSlot, enterFn;
  std::uint64_t fireSlot, fireFn;
  std::uint64_t attackSlot, attackFn;
  std::uint64_t cmpSlot, cmpFn, cmpPlt;
};
inline constexpr Pins kPins = {0x36e2080ULL, 0x2059714ULL, 0x36cacf8ULL, 0x263b700ULL, 0x36c37a8ULL, 0x210bff8ULL,
                               0x36e6610ULL, 0x210b8c4ULL, 0x36f91f0ULL, 0x1524cb4ULL, 0xf43610ULL};

// The slot as a manager target on a libr15 loaded at `base`: the slot must hold the pinned function (base + fn).
inline sentinel::GotTarget Target(const char* symbol, std::uint64_t slot, std::uintptr_t base, std::uint64_t fn) {
  sentinel::GotTarget target(sentinel::pinned::kLibR15, symbol, sentinel::RelocKind::kJumpSlot, sentinel::pinned::LibR15PatchedBuildId(), slot);
  target.expectedOriginal = reinterpret_cast<const void*>(base + fn);
  return target.Owned("combat_dronekill");
}

}  // namespace nevr_quest::combat::dronekill
