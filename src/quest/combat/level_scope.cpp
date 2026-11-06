#include "quest/combat/level_scope.h"

#include <time.h>

#include <atomic>

#include "quest/combat/combat_level.h"
#include "quest/combat/combat_tick.h"

namespace nevr_quest::combat::scope {

namespace {

std::atomic<bool> g_map{false};
std::atomic<bool> g_mapOrLobby{false};
std::atomic<std::uint32_t> g_looks{0};
std::atomic<bool> g_started{false};
std::atomic<bool> g_registered{false};
std::atomic<double (*)() noexcept> g_clock{nullptr};
double g_nextLook = 0.0;  // the game thread only

double Now() noexcept {
  const auto clock = g_clock.load(std::memory_order_acquire);
  if (clock != nullptr) return clock();
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

}  // namespace

bool InCombatMap() noexcept { return g_map.load(std::memory_order_relaxed); }
bool InCombatMapOrLobby() noexcept { return g_mapOrLobby.load(std::memory_order_relaxed); }
std::uint32_t Looks() noexcept { return g_looks.load(std::memory_order_relaxed); }

bool Start() noexcept {
  if (g_started.exchange(true, std::memory_order_acq_rel)) return g_registered.load(std::memory_order_acquire);
  const bool added = tick::Add(&Tick);
  g_registered.store(added, std::memory_order_release);
  return added;
}

void Tick(std::uintptr_t game) noexcept {
  const double now = Now();
  if (now < g_nextLook) return;
  g_nextLook = now + kLookEverySeconds;
  const level::Loaded loaded = level::Scan(game);
  // An Echo Arena level wins over any combat level still loaded during a transition (combat_level.h).
  g_map.store(loaded.combatMap && !loaded.echoArena, std::memory_order_relaxed);
  g_mapOrLobby.store((loaded.combatMap || loaded.combatLobby) && !loaded.echoArena, std::memory_order_relaxed);
  g_looks.fetch_add(1, std::memory_order_relaxed);
}

void SetClockForTest(double (*clock)() noexcept) noexcept { g_clock.store(clock, std::memory_order_release); }

void ResetForTest() noexcept {
  g_map.store(false);
  g_mapOrLobby.store(false);
  g_looks.store(0);
  g_started.store(false);
  g_registered.store(false);
  g_nextLook = 0.0;
  g_clock.store(nullptr);
}

}  // namespace nevr_quest::combat::scope
