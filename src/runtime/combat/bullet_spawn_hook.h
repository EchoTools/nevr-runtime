#pragma once

// The one detour on the bullet system's spawn, CR15NetBulletCS::FireBullet (echovr.exe 0x140ce2020; the name is
// quest/combat/bullet_velocity_guard_targets.h's PC note), which two of the combat module's ported files need (#2336):
//
//  * PowerBoost (powerboost.cpp) drops the bullet of a party gun's shot: a filter that runs before the game's spawn and
//    may stop it.
//  * DroneFix (dronefix.cpp) starts a drone bullet's tracer at the drone: a filter that runs after the game's spawn and
//    rewrites the record it made.
//
// In he_is_the_cat's three DLLs each module put its own Detours hook on the address and PowerBoost's wrapped DroneFix's
// jump. In the runtime every combat write passes the write gate (write_gate.h), which refuses a second detour on bytes
// HookGuard already records, so PowerBoost's was refused and OVERCHARGE and MEDIGUN never started. One owner, as
// patch/equipment_refire.h is for the slot setters: the detour is installed once, by whichever module asks first, and each
// module sets its filter on it. The order is his DLLs' order: PowerBoost's hook was the outer one, so its drop runs first
// and a dropped bullet reaches neither the game's spawn nor DroneFix's filter.

#include <cstdint>

namespace nevr_combat::bullet_spawn {

/// FireBullet's RVA in the supported echovr.exe build, and the first bytes the detour checks before it writes (MOV
/// [RSP+0x10],RBX; PUSH RBP,RSI,RDI,R12,R13), as both ported files checked them.
inline constexpr std::uintptr_t kRva = 0xce2020;
inline constexpr std::uint8_t kPrologue[12] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55};

/// One call's arguments, as the ported files name them.
struct Call {
  std::uint8_t* sys;      // the bullet component system
  std::uint64_t index;    // the bullet's handle
  void* ref;
  float speed;
  const void* pose;       // the spawn pose
  const float* velocity;
  int remote;
  std::uint32_t flags;
};

/// Before the game's spawn: true drops the bullet (the spawn is not called and no After filter runs).
using DropFilter = bool (*)(const Call& call);
/// After the game's spawn.
using AfterFilter = void (*)(const Call& call);

/// Sets (or, with null, clears) the filter. The first set installs the detour at gameBase + kRva: it checks kPrologue and
/// the write gate first, and on either failure installs nothing and returns false, with a Warning that names why. A set
/// while the detour is in place only stores the filter. `module` names the caller in the log line. Clearing the last
/// filter removes the detour.
bool SetDrop(std::uintptr_t gameBase, DropFilter filter, const char* module);
bool SetAfter(std::uintptr_t gameBase, AfterFilter filter, const char* module);

/// True while the detour is in place.
bool Installed();

}  // namespace nevr_combat::bullet_spawn
