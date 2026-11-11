/* The pinned slots, functions and call sites of dronemuzzle (dronemuzzle.h).
 *
 * ELF virtual addresses in the store libr15 (build ID in pinned_targets.h, `LibR15PatchedBuildId`); the disassembly's address is the ELF address
 * plus 0x100000. src/quest/tests/got_pinned_test.cpp checks both JUMP_SLOT relocations against the real file (`just test-quest-hooks-pinned`).
 * `llvm-readelf -rW` of the store libr15 (sha256 8dd9a961...):
 *
 *   CncaGameSpace::WorldTeleport  JUMP_SLOT 0x36cfa18 -> 0x180a384  (25 callers; also one GLOB_DAT word, which this module does not touch)
 *   CR15NetBulletCS::FireBullet   JUMP_SLOT 0x36f3cf8 -> 0x210c9c4  (2 callers)
 *
 * The two return addresses are the instructions after FireBulletLocal's `bl` to each PLT stub (disassembly 0x220c228 `bl 0xff0660`, 0x220c2a0
 * `bl 0x1038c20`): ELF 0x210c22c and 0x210c2a4.
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstdint>

#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/pinned_targets.h"

namespace nevr_quest::combat::dronemuzzle {

inline constexpr const char* kTeleportSymbol =
    "_ZN10NRadEngine13CncaGameSpace13WorldTeleportENS_6SActorENS_8CTransfQENS_24EComponentTeleportReasonE";
inline constexpr const char* kFireSymbol =
    "_ZN10NRadEngine8NRadGame15CR15NetBulletCS10FireBulletENS_18SCompactPoolHandleENS_16CComponentHandleINS0_12CR15NetGunCSEEEfRKNS_11CQuaternionERKNS_8C3VectorEjj";

struct Pins {
  std::uint64_t teleportSlot, teleportFn, teleportRet;  // JUMP_SLOT, the function, FireBulletLocal's return address after its call
  std::uint64_t fireSlot, fireFn, fireRet;
};
inline constexpr Pins kPins = {0x36cfa18ULL, 0x180a384ULL, 0x210c22cULL, 0x36f3cf8ULL, 0x210c9c4ULL, 0x210c2a4ULL};

// The slot as a manager target on a libr15 loaded at `base`: the slot must hold the pinned function (base + fn) or the chain's oldest original.
inline sentinel::GotTarget Target(const char* symbol, std::uint64_t slot, std::uintptr_t base, std::uint64_t fn) {
  sentinel::GotTarget target(sentinel::pinned::kLibR15, symbol, sentinel::RelocKind::kJumpSlot, sentinel::pinned::LibR15PatchedBuildId(), slot);
  target.expectedOriginal = reinterpret_cast<const void*>(base + fn);
  return target.Owned("combat_dronemuzzle");
}

}  // namespace nevr_quest::combat::dronemuzzle
