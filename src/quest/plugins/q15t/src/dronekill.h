// Drone kill / lock-on / gun / chase / shot (rows R45, R47, R48 + the Actor slot Enter's capture needs), ported from the source
// build's the source build's dronekill. GOT word swaps on data pages and calls into libr15 only; no code page is written.
//
// What it hooks (pinned build, ELF vaddrs; docs/quest/q15t/modules/dronekill.md has the disassembly reads):
//   CEnableComponentNode::Enter          JUMP_SLOT 0x36cacf8 (fn 0x263b700)  R45  the drone's enable/disable nodes
//   SComponentNodeUtils::Actor           JUMP_SLOT 0x36e2080 (fn 0x2059714)  (not a numbered row: Enter's capture of the drone's game space)
//   CR15NetAutoTargetCS::FireBulletLocal JUMP_SLOT 0x36c37a8 (fn 0x210bff8)  R48  lock-on and the shot's velocity
//   CR15NetAutoTargetCS::AttackTarget    JUMP_SLOT 0x36e6610 (fn 0x210b8c4)  R47  chase
// What it calls (never writes): FindComponentSystemMT (const, export 0x1804a10), the systems' vtable slots Component /
// DisableComponent / EnableComponent (only after the read shapes of dronekill_core.h match), CTransformCS::ComputeLocalToWorldTransfQ,
// CPhysicsCS::PrimaryBody / SetWorldVelocity, CPhTarget::ResolveBody, CR15NetPhysicsCS::IsLocallyControlled.
//
// Not in the plugin: the 'K' feed (the source's source-build ec module hands each server "drone killed" datagram to DroneKillOnKilled). Until
// a module with the recvfrom row (R43) calls DroneKillOnKilled, `kill` never fires; lock-on / gun / chase / shot do not need it.
#pragma once

#include <cstdint>

#include "import_hook.h"

namespace q15t {

// The source's TEST words (dronekill2.on = all of them). Defaults: everything on, as the source's QA file.
struct DroneKillWords {
  bool kill = true, lockon = true, gun = true, chase = true, shot = true;
};

struct DkC3 {  // C3Vector: an HFA, passed in s0-s2
  float x, y, z;
};
struct DkTransfQ {  // CTransfQ, 0x20 bytes (returned through x8)
  float q[4];
  float p[3];
  std::uint32_t w;
};
struct DkPhTarget {  // CPhTarget, 0x18 bytes (returned through x8)
  std::uint64_t tag;
  std::uint32_t body, zone, gen, pad;
};

// The libr15 functions the module calls. Resolved by ResolveDroneKillFns (every export must sit at its pinned vaddr), faked in tests.
struct DroneKillFns {
  void* (*find)(const void* space, std::uint64_t system_sym) = nullptr;  // FindComponentSystemMT const
  DkTransfQ (*l2w)(const void* transforms, std::uint32_t handle) = nullptr;
  DkPhTarget (*primary)(const void* physics, std::uint32_t handle) = nullptr;
  void* (*resolve)(const DkPhTarget* t) = nullptr;
  void (*set_vel)(void* physics, std::uint32_t handle, DkC3 v) = nullptr;
  std::uint32_t (*local)(const void* net_physics, std::uint32_t handle) = nullptr;
};

// ELF vaddrs of the pinned build (readelf -rW of the pinned libr15; disassembly VA = ELF + 0x100000).
struct DroneKillSlots {
  std::uintptr_t actor_slot, actor_fn;
  std::uintptr_t enter_slot, enter_fn;
  std::uintptr_t fire_slot, fire_fn;
  std::uintptr_t attack_slot, attack_fn;
  std::uintptr_t cmp_slot, cmp_fn;  // the BinarySearch comparator's JUMP_SLOT and function (read only)
  std::uintptr_t cmp_plt;           // its PLT stub: where every Component() body's bl must land
};
extern const DroneKillSlots kDroneKillPinned;

// Resolves the six functions by name from the loaded libr15.so and requires each at base + its pinned vaddr. False (and one ERROR
// line naming the function) when one is missing or elsewhere.
bool ResolveDroneKillFns(std::uintptr_t base, DroneKillFns* out);

struct DroneKillInstall {
  bool ok = false;           // all words swapped (all or none)
  InstallResult actor, enter, fire, attack;
  bool comparator_ok = false;  // the comparator's JUMP_SLOT holds its function: Component() look-ups are allowed
  bool needs_tick = false;     // kill / lockon / chase are on: the caller must register DroneKillTick (AddGameTick) before InstallGameTick
};

// All words or none (the source's DK-6): every slot is checked before the first swap; a failing swap puts the earlier ones back. The words
// are final before this returns; call it before the game tick is installed so no wrapper runs on another thread early. FireBulletLocal is
// swapped when lockon or shot is on; AttackTarget when chase is on. Logs one line per hook and one summary line.
DroneKillInstall InstallDroneKill(std::uintptr_t base, const DroneKillWords& words, const DroneKillFns& fns,
                                  const DroneKillSlots& slots = kDroneKillPinned);

// The game-thread tick (the source's net tick: disables after a 'K', the touch interact on/off, the chase velocities). One atomic
// load while nothing is due. Register with AddGameTick when DroneKillInstall::needs_tick.
void DroneKillTick(std::uintptr_t game);

// The 'K' feed: the damageable index a server "drone killed" datagram carries (16 bits are compared). Safe from any thread; table work
// under the module's lock, one capped log line.
void DroneKillOnKilled(std::uint32_t index);

// The shot accessor (the source's source-build drone_shot_velocity): the velocity of the shot FireBulletLocal is making on this thread, null
// outside the real call or when the FireBulletLocal word was not swapped. *remote = 1 for a remote shot.
const float* DroneKillShotVelocity(int* remote);
bool DroneKillShotActive();  // the FireBulletLocal word is swapped (the accessor can be non-null)

struct DroneKillStats {
  std::uint32_t enter_drone, enter_other, fires, attacks, ticks, due_runs;
};
DroneKillStats GetDroneKillStats();

// ---- test seams ----
// The check ResolveDroneKillFns makes once the names are looked up: addrs[i] must be base + the pinned vaddr of function i (order: find,
// l2w, primary, resolve, set_vel, local). The host has no libr15.so to look the names up in; the test feeds the addresses.
bool ResolveDroneKillFnsFromAddressesForTest(std::uintptr_t base, const void* const addrs[6], DroneKillFns* out);
// How the module calls game code through vtable slots; the host test replaces them (a slot's words are aarch64 data on the host).
struct DroneKillCalls {
  std::uint64_t (*component)(std::uintptr_t fn, const void* system, std::uint64_t actor, std::uint64_t sym);
  void (*enable)(std::uintptr_t fn, void* system, std::uint64_t handle, std::uint32_t category);
  void (*disable)(std::uintptr_t fn, void* system, std::uint64_t handle, std::uint32_t category);
};
void SetDroneKillCallsForTest(const DroneKillCalls* calls);  // null = the real calls
void SetDroneKillClockForTest(std::uint64_t (*now_ms)());
// Makes DroneKillShotVelocity answer `v3` (3 floats) on this thread, as inside a FireBulletLocal call; null clears it.
void SetDroneKillShotForTest(const float* v3, int remote);
void SetDroneKillShotActiveForTest(bool active);  // what DroneKillShotActive answers (the FireBulletLocal word swapped)
void ResetDroneKillForTest();

}  // namespace q15t
