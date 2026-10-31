// wallfire (source build the source build's wallfire; rows R26-R28; on by default in the source, kill file wallfire.off): a gun's shot is
// refused when the segment from the player to the muzzle crosses static level geometry, as on the PC. Depends on bvh (the
// static BVH has to answer CBVHTree::Occluded).
//
//   R26 CncaGameSpace::QueryCollision (JUMP_SLOT wrapper): runs the stock function; only when it returns to FireGunLocal's call site
//       (R27, the return address) and found nothing, asks the engine's own static query for the same segment.
//   R27 FireGunLocal's return site: its instruction word is read at install (a check, nothing is written there).
//   R28 CncaGameSpace::StaticRaycastOccluded(this, C3Vector*, C3Vector*): the call the wrapper makes (the pinned function, called
//       directly; its JUMP_SLOT is checked, not swapped). It reaches CBVHTree::Occluded through CncaGame's virtual +0x278, which is
//       bvh's R23 wrapper.
// A segment that is blocked makes QueryCollision answer 1, and FireGunLocal returns without firing. Every other caller of
// QueryCollision gets the stock answer. Details, evidence, behaviour, prediction: docs/quest/q15t/modules/wallfire.md.
#pragma once

#include <cstdint>

#include "import_hook.h"

namespace q15t {

struct WallfirePins {
  std::uintptr_t query_slot;      // R26 JUMP_SLOT (ELF vaddr)
  std::uintptr_t query_fn;        // the function that slot holds in the pinned build
  std::uintptr_t site;            // R27 FireGunLocal's return address after `bl QueryCollision@plt`
  std::uint32_t site_word;        // the instruction word there (ldrb w8, [sp, #0x6c])
  std::uintptr_t occluded_slot;   // R28 JUMP_SLOT: checked to hold occluded_fn, not swapped
  std::uintptr_t occluded_fn;     // CncaGameSpace::StaticRaycastOccluded
  std::uintptr_t game_occluded;   // CncaGame::StaticRaycastOccluded: what CncaGame's vtable +0x278 holds
};

// ELF vaddrs of the pinned build (disassembly VA = ELF + 0x100000).
inline constexpr WallfirePins kWallfirePinned = {0x36e0c38, 0x1806784, 0x215566c, 0x3941b3e8, 0x36ce380, 0x18066f4, 0x17f2614};

// Game structures read at a shot (docs/quest/q15t/modules/wallfire.md section 3).
inline constexpr std::uintptr_t kGsGame = 0x8;           // CncaGameSpace: CncaGame* (ldr x0, [x0, #8] in the StaticRaycastOccluded stub)
inline constexpr std::uintptr_t kGameVtblSlot = 0x278;   // CncaGame vtable: the slot the stub branches through
inline constexpr unsigned kReqSegFloats = 6;             // SPhQueryCollisionRequest: start xyz, end xyz at its first 24 bytes

// Option file in the plugin's data directory: wallfire.off installs nothing (reason "off_file").
// Returns kOk when R26 is replaced; otherwise nothing is replaced and `reason` says why.
InstallResult InstallWallfire(std::uintptr_t base);
InstallResult InstallWallfireAt(std::uintptr_t base, const WallfirePins& pins);

struct WallfireCounters {
  std::uint64_t at_site;        // QueryCollision calls from FireGunLocal's site whose stock answer was "nothing"
  std::uint64_t checked;        // ... of those, the ones whose segment and CncaGame were as pinned, so the static level was asked
  std::uint64_t blocked;        // ... and the static level blocked it
  std::uint64_t skipped_nan;    // at_site calls with a non-finite segment or an unreadable request: the stock answer
  std::uint64_t skipped_game;   // at_site calls whose game structure chain is unreadable or whose CncaGame virtual is not the pinned function
};
WallfireCounters WallfireGetCounters();

// Test seams.
void SetWallfireDirForTest(const char* dir);
void ResetWallfireForTest();
std::uintptr_t WallfireLastRetForTest();  // the last return address the wrapper saw, as an ELF vaddr

}  // namespace q15t
