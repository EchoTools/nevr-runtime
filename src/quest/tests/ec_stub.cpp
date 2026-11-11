// See ec_stub.h.
#include "quest/tests/ec_stub.h"

namespace {
nevr_quest::combat::ec::DroneKilledFn g_killed = nullptr;
nevr_quest::combat::ec::NetTickFn g_ticks[4] = {};
int g_tickCount = 0;
}  // namespace

namespace nevr_quest::combat::ec {
bool OnNetTick(NetTickFn tick) noexcept {
  if (tick == nullptr || g_tickCount >= 4) return false;
  g_ticks[g_tickCount++] = tick;
  return true;
}
void OnDroneKilled(DroneKilledFn handler) noexcept { g_killed = handler; }
}  // namespace nevr_quest::combat::ec

namespace ec_stub {
nevr_quest::combat::ec::DroneKilledFn DroneKilled() noexcept { return g_killed; }
int NetTicks() noexcept { return g_tickCount; }
void Reset() noexcept {
  g_killed = nullptr;
  g_tickCount = 0;
}
}  // namespace ec_stub
