/* The pinned slot, call site and structure offsets of the wall-fire check (wallfire.h).
 *
 * Every address is an ELF virtual address in the store libr15 (build ID in pinned_targets.h, `LibR15PatchedBuildId`; the disassembly's
 * address is the ELF address plus 0x100000). src/quest/tests/got_pinned_test.cpp checks the two JUMP_SLOT relocations and the code word against
 * the real file (`just test-quest-hooks-pinned`, which needs the store APK):
 *
 *   QueryCollision           JUMP_SLOT 0x36e0c38 -> 0x1806784   CncaGameSpace::QueryCollision(SPhQueryCollisionRequest&, CPhId) const
 *   FireGunLocal's site      0x215566c, the instruction after its `bl QueryCollision@plt`: `ldrb w8, [sp, #0x6c]` (0x3941b3e8)
 *   StaticRaycastOccluded    JUMP_SLOT 0x36ce380 -> 0x18066f4   CncaGameSpace::StaticRaycastOccluded: read at install, never hooked; the check
 *                            calls the function itself, which reaches CBVHTree::Occluded through CncaGame's virtual +0x278
 *   CncaGame::StaticRaycastOccluded  0x17f2614, what CncaGame's vtable +0x278 holds
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstdint>

#include "quest/sentinel/callback_thunk.h"
#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/pinned_targets.h"

namespace nevr_quest::combat::wallfire {

struct Pins {
  std::uint64_t querySlot;      // the QueryCollision JUMP_SLOT (ELF vaddr)
  std::uint64_t queryFn;        // the function that slot holds in the pinned build
  std::uint64_t site;           // FireGunLocal's return address after `bl QueryCollision@plt`
  std::uint32_t siteWord;       // the instruction word there (ldrb w8, [sp, #0x6c])
  std::uint64_t occludedSlot;   // StaticRaycastOccluded's JUMP_SLOT: checked to hold occludedFn, not hooked
  std::uint64_t occludedFn;     // CncaGameSpace::StaticRaycastOccluded
  std::uint64_t gameOccluded;   // CncaGame::StaticRaycastOccluded: what CncaGame's vtable +0x278 holds
};

inline constexpr Pins kPins = {0x36e0c38ULL, 0x1806784ULL, 0x215566cULL, 0x3941b3e8U, 0x36ce380ULL, 0x18066f4ULL, 0x17f2614ULL};

inline constexpr const char* kQuerySymbol = "_ZNK10NRadEngine13CncaGameSpace14QueryCollisionERNS_24SPhQueryCollisionRequestENS_5CPhIdE";

// The game's structures read at a shot.
inline constexpr std::uintptr_t kGsGame = 0x8;           // CncaGameSpace: CncaGame* (ldr x0, [x0, #8] in the StaticRaycastOccluded stub)
inline constexpr std::uintptr_t kGameVtblSlot = 0x278;   // CncaGame vtable: the slot the stub branches through
inline constexpr unsigned kReqSegFloats = 6;             // SPhQueryCollisionRequest: start xyz, end xyz at its first 24 bytes

struct QueryTag {};
// kCaller: the handler is told the instruction after the game's call, which is how it tells FireGunLocal's call from every other.
using QueryThunk = sentinel::CallbackThunk<QueryTag, bool(std::uintptr_t gs, std::uintptr_t req, std::uint64_t id), /*kCaller=*/true>;

// The slot as a manager target on a libr15 loaded at `base`: it must hold the real function (base + queryFn).
inline sentinel::GotTarget QueryTarget(std::uintptr_t base, const Pins& pins) {
  sentinel::GotTarget target(sentinel::pinned::kLibR15, kQuerySymbol, sentinel::RelocKind::kJumpSlot, sentinel::pinned::LibR15PatchedBuildId(),
                             pins.querySlot);
  target.expectedOriginal = reinterpret_cast<const void*>(base + pins.queryFn);
  return target.Owned("combat_wallfire");
}

}  // namespace nevr_quest::combat::wallfire
