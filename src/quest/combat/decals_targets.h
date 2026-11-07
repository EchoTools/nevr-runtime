/* The pinned slots and functions of the level-decals module (decals.h).
 *
 * Every address is an ELF virtual address in the store libr15 (build ID in pinned_targets.h, `LibR15PatchedBuildId`); the disassembly's address is the
 * ELF address plus 0x100000. src/quest/tests/got_pinned_test.cpp checks both relocations against the real file (`just test-quest-hooks-pinned`, which
 * needs the store APK).
 *
 *   CR15NetBulletCS::UpdateMovement  GLOB_DAT 0x370dbc8 -> 0x2124290   (`readelf -rW`; FUNC, 5208 bytes, also exported by name; the word
 *                                                                       R15NetRegisterComponentSystemUpdates reads when a level loads)
 *   CR15NetBulletCS::Shutdown        ABS64    0x36933e8 -> 0x21226f0   (a vtable word; the word impactsnd also hooks, so this handler chains above it)
 */
#pragma once

#include <cstdint>

#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/pinned_targets.h"

namespace nevr_quest::combat::decals {

inline constexpr const char* kUpdateSymbol = "_ZN10NRadEngine8NRadGame15CR15NetBulletCS14UpdateMovementEv";
inline constexpr const char* kShutdownSymbol = "_ZN10NRadEngine8NRadGame15CR15NetBulletCS8ShutdownEv";

inline constexpr std::uintptr_t kUpdateSlotVa = 0x370dbc8;    // GLOB_DAT: &CR15NetBulletCS::UpdateMovement
inline constexpr std::uintptr_t kUpdateRealVa = 0x2124290;    // CR15NetBulletCS::UpdateMovement
inline constexpr std::uintptr_t kShutdownSlotVa = 0x36933e8;  // vtable word: &CR15NetBulletCS::Shutdown
inline constexpr std::uintptr_t kShutdownRealVa = 0x21226f0;

// Each slot holds the game's function, or the handler the impactsnd module put on the Shutdown word first (the manager takes either: got_hook.h).
inline sentinel::GotTarget UpdateTarget(std::uintptr_t base) {
  sentinel::GotTarget target(sentinel::pinned::kLibR15, kUpdateSymbol, sentinel::RelocKind::kGlobDat, sentinel::pinned::LibR15PatchedBuildId(),
                             kUpdateSlotVa);
  target.expectedOriginal = reinterpret_cast<const void*>(base + kUpdateRealVa);
  return target.Owned("combat_decals");
}
inline sentinel::GotTarget ShutdownTarget(std::uintptr_t base) {
  sentinel::GotTarget target(sentinel::pinned::kLibR15, kShutdownSymbol, sentinel::RelocKind::kAbs64, sentinel::pinned::LibR15PatchedBuildId(),
                             kShutdownSlotVa);
  target.expectedOriginal = reinterpret_cast<const void*>(base + kShutdownRealVa);
  return target.Owned("combat_decals");
}

}  // namespace nevr_quest::combat::decals
