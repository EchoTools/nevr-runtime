// A stand-in for the EC client's contract (src/quest/combat/ec.h) in host-test programs that do not link ec.cpp: the two registrations
// keep what they are given and do nothing else. A test that needs ec's real behaviour links ec.cpp instead.
#include "quest/combat/ec.h"

namespace nevr_quest::combat::ec {

namespace {
NetTickFn g_tick = nullptr;
DroneKilledFn g_killed = nullptr;
}  // namespace

bool OnNetTick(NetTickFn tick) noexcept {
  if (tick == nullptr) return false;
  g_tick = tick;
  return true;
}

void OnDroneKilled(DroneKilledFn handler) noexcept { g_killed = handler; }

}  // namespace nevr_quest::combat::ec
