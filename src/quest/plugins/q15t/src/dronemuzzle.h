// Drone bullets leave the drone's gun (rows R49 CncaGameSpace::WorldTeleport and R50 CR15NetBulletCS::FireBullet), ported from the
// source build's dronemuzzle (drone fix F4 + F6). Two GOT swaps on data pages; each slot gets a naked arm64 thunk
// (the source's mechanism: it needs the caller's registers untouched and a check of the return address). No code page is written.
//
// WorldTeleport has 25 callers and FireBullet 2; only the calls that return into FireBulletLocal (ELF 0x210c22c / 0x210c2a4) are acted
// on, every other call goes straight to the real function with x0..x7 / s0 / x30 / sp as the caller left them. During a drone shot
// (dronekill's shot accessor non-null) the first call sends a moved copy of the pose; the second fixes the bullet record's shooter fields.
// Needs dronekill's FireBulletLocal word (Q15T_DRONEKILL with `shot` or `lockon`): without it both thunks only pass through.
#pragma once

#include <atomic>
#include <cstdint>

#include "import_hook.h"

namespace q15t {

struct DroneMuzzleSlots {
  std::uintptr_t teleport_slot, teleport_fn, teleport_ret;  // JUMP_SLOT, the function, FireBulletLocal's return address after its call
  std::uintptr_t fire_slot, fire_fn, fire_ret;
};
extern const DroneMuzzleSlots kDroneMuzzlePinned;

struct DroneMuzzleWrappers {
  std::uintptr_t teleport, fire;  // what the two slots get: the naked thunks on arm64, 0 elsewhere (the host test passes its own)
};
DroneMuzzleWrappers DroneMuzzleThunks();

struct DroneMuzzleInstall {
  bool ok = false;           // both words swapped (all or none)
  InstallResult teleport, fire;
  bool shot_source = false;  // dronekill's FireBulletLocal word is swapped: the thunks can act
};
DroneMuzzleInstall InstallDroneMuzzle(std::uintptr_t base, const DroneMuzzleSlots& slots = kDroneMuzzlePinned,
                                      const DroneMuzzleWrappers& wrappers = DroneMuzzleThunks());

// Once a frame on the game thread (register with AddGameTick): the frame-time average the muzzle's "back by one frame" uses.
void DroneMuzzleTick(std::uintptr_t game);

struct DroneMuzzleStats {
  std::uint32_t shots, tracers;
};
DroneMuzzleStats GetDroneMuzzleStats();

void SetDroneMuzzleClockForTest(std::uint64_t (*now_ns)());
void ResetDroneMuzzleForTest();

}  // namespace q15t

// The plain functions the thunks call, and the words the thunks read. C names: the asm refers to them by symbol. Hidden.
extern "C" {
// the return address each thunk acts on, and the next function (the real one); written (release) before the slot store, never again
extern std::atomic<std::uintptr_t> q15t_dm_tp_ret __attribute__((visibility("hidden")));
extern std::atomic<std::uintptr_t> q15t_dm_tp_next __attribute__((visibility("hidden")));
extern std::atomic<std::uintptr_t> q15t_dm_fire_ret __attribute__((visibility("hidden")));
extern std::atomic<std::uintptr_t> q15t_dm_fire_next __attribute__((visibility("hidden")));

// F4: FireBulletLocal's teleport of a new bullet (x0..x3 as the caller passed them). A drone shot's pose goes on as a moved copy.
__attribute__((visibility("hidden"))) std::uint64_t q15t_dm_teleport(void* gs, std::uint64_t actor, const std::uint8_t* pose,
                                                                       std::uint64_t reason);
// F6: FireBulletLocal's FireBullet (x0..x6 + s0). After it, a drone shot's record: shooter velocity 0, shooter position = its +0x134.
__attribute__((visibility("hidden"))) std::uint64_t q15t_dm_fire(std::uint8_t* bcs, std::uint64_t handle, const void* gun, float speed,
                                                                   const void* quat, const void* vel, std::uint64_t remote,
                                                                   std::uint64_t flag);
}
