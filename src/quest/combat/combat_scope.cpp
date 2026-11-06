#include "quest/combat/combat_scope.h"

#include <time.h>

#include <atomic>

#include "quest/combat/combat_level.h"
#include "quest/combat/combat_tick.h"
#include "quest/sentinel/hook_log.h"

namespace nevr_quest::combat::scope {

namespace {

std::atomic<bool> g_in{false};
std::atomic<bool> g_enrolled{false};
std::atomic<std::int64_t> g_nextLookMs{0};
std::atomic<std::uint32_t> g_looks{0};
std::atomic<std::int64_t (*)() noexcept> g_clock{nullptr};

std::int64_t NowMs() noexcept {
  const auto injected = g_clock.load(std::memory_order_acquire);
  if (injected != nullptr) return injected();
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

}  // namespace

bool Enroll() noexcept {
  if (g_enrolled.exchange(true, std::memory_order_acq_rel)) return true;
  const bool added = tick::Add(&Refresh);
  if (!added) g_enrolled.store(false, std::memory_order_release);
  sentinel::LogFields(added ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "combat_scope_enroll",
                      {{"result", added ? "registered" : "refused"}, {"callbacks", tick::Count()},
                       {"msg", added ? "the combat level answer is refreshed from the game-thread tick, four times a second"
                                     : "the tick's callback table is full: the combat level answer stays false and the modules that read it do nothing"}});
  return added;
}

bool InCombat() noexcept { return g_in.load(std::memory_order_relaxed); }

void Refresh(std::uintptr_t game) noexcept {
  const std::int64_t now = NowMs();
  if (now < g_nextLookMs.load(std::memory_order_relaxed)) return;
  g_nextLookMs.store(now + kLookEveryMs, std::memory_order_relaxed);
  const std::uint32_t looks = g_looks.fetch_add(1, std::memory_order_relaxed) + 1;
  std::uint64_t levelId = 0;
  const bool in = level::Classify(game, /*lobby=*/true, &levelId) == level::Kind::kCombatOrLobby;
  if (in == g_in.load(std::memory_order_relaxed)) return;
  g_in.store(in, std::memory_order_release);
  char levelText[19];
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_scope_change",
                      {{"from", in ? 0 : 1}, {"to", in ? 1 : 0}, {"level", sentinel::HexString(levelText, levelId)}, {"looks", looks},
                       {"msg", in ? "a combat level is loaded: the wrist HUD and shield colour modules act"
                                  : "no combat level is loaded: the wrist HUD and shield colour modules pass every call through"}});
}

std::uint32_t Looks() noexcept { return g_looks.load(std::memory_order_relaxed); }

void SetClockForTest(std::int64_t (*clockMs)() noexcept) noexcept { g_clock.store(clockMs, std::memory_order_release); }

void ResetForTest() noexcept {
  g_in.store(false);
  g_enrolled.store(false);
  g_nextLookMs.store(0);
  g_looks.store(0);
  g_clock.store(nullptr);
}

}  // namespace nevr_quest::combat::scope
