// droneguard (source build RE-6 / RE-34 G28, the source build's droneguard; row R35; TEST ONLY, default OFF, option Q15T_DRONEGUARD; the
// second stage by option Q15T_DRONEGUARD_IDX, default OFF, only read when the first is on).
//
// CR15NetFollowPlayerCS::Update calls Follow(SCompactPoolHandle) once per enabled component through its PLT stub (the only
// caller). Follow loads [cs+0x140] (the CR15PlayerNavCS system the component follows) and passes it as `this` to
// CR15PlayerNavCS::FinalTransform with no null check; the drone's actor gets enabled before any orbit target exists.
// Wraps libr15's JUMP_SLOT for Follow (ELF 0x36f1218 -> the function at ELF 0x2149764). Stage 0 (null half): returns while
// [cs+0x140] is null, else calls the real function with the same two arguments; plus a read-only "state" line on a change of
// the followed player's cache (at most 16). Stage 1 (idx guard): before Follow, the engine's own CComponentHandle<
// CR15PlayerNavCS>::RefreshCache(cs+0x148), then skip when the cached index is unresolved (u16 [cs+0x190] == 0xffff), the
// cached system [cs+0x188] is 0, or CR15PlayerNavCS::IsComponentValid(target, idx) refuses the cached index.
// Differences from a stock run: none unless the target is null (stage 0) or the cache is stale (stage 1). There is no
// access-violation handler: a non-null but stale pointer still faults (as in the source build). Data-page hook only.
// Details, evidence and prediction: docs/quest/q15t/modules/droneguard.md.
#pragma once

#include <cstdint>

#include "import_hook.h"
#include "word_pins.h"

#ifndef Q15T_DRONEGUARD_IDX
#define Q15T_DRONEGUARD_IDX 0
#endif

namespace q15t {

inline constexpr std::uintptr_t kDroneGuardSlot = 0x36f1218;
inline constexpr std::uintptr_t kDroneGuardReal = 0x2149764;
inline constexpr std::uintptr_t kDroneRefreshCache = 0x1210848;  // CComponentHandle<CR15PlayerNavCS>::RefreshCache
inline constexpr std::uintptr_t kDroneIsValid = 0x1cdb3e0;       // CR15PlayerNavCS::IsComponentValid

// The two engine functions stage 1 calls.
struct DroneGuardFns {
  void (*refresh)(void* handle);                              // RefreshCache() const (called on cs+0x148)
  bool (*is_valid)(const void* sys, std::uint64_t handle);    // IsComponentValid(SCompactPoolHandle) const
};

// Stage 1's word pins, then dlsym on the loaded game library; false unless the pins match and both are exactly at base + their pinned vaddr.
bool ResolveDroneGuardFns(std::uintptr_t base, DroneGuardFns* out);
// The address check ResolveDroneGuardFns makes on what dlsym returned (a seam for the host test).
bool DroneGuardExportsMatch(std::uintptr_t base, const void* refresh, const void* valid, DroneGuardFns* out);

inline constexpr WordPin kDroneGuardWordPins[] = {
    {0x21497bc, 0xf940a274},
    {0x2149744, 0x97b7a7c7},
    {0x21497cc, 0xb9419261},
    {0x214980c, 0x79432268},
};
inline constexpr std::size_t kDroneGuardWordPinCount = sizeof kDroneGuardWordPins / sizeof kDroneGuardWordPins[0];
// Stage 1's own pins (RefreshCache call site, IsComponentValid's first words): a mismatch leaves the null half only.
inline constexpr WordPin kDroneGuardIdxWordPins[] = {
    {0x21497c8, 0x97b70b0a},
    {0x1cdb3e0, 0x7941e008},
    {0x1cdb3f8, 0x6b41413f},
};
inline constexpr std::size_t kDroneGuardIdxWordPinCount = sizeof kDroneGuardIdxWordPins / sizeof kDroneGuardIdxWordPins[0];
// True when every pin matches at base + its vaddr (one ERROR line naming the first that does not). No MD5 or other file-hash gate.
bool DroneGuardWordPinsMatch(std::uintptr_t base);
bool DroneGuardIdxWordPinsMatch(std::uintptr_t base);

// Stage 1 is on only when `idx_stage` is true AND both functions resolve; otherwise one ERROR line and the null half only.
InstallResult InstallDroneGuard(std::uintptr_t base);  // idx_stage = Q15T_DRONEGUARD_IDX
InstallResult InstallDroneGuardAt(std::uintptr_t base, std::uintptr_t slot_vaddr, std::uintptr_t real_vaddr, bool idx_stage,
                                  const DroneGuardFns* fns);

struct DroneGuardStats {
  unsigned skips, idx_skips;
};
DroneGuardStats GetDroneGuardStats();

void ResetDroneGuardForTest();

}  // namespace q15t
