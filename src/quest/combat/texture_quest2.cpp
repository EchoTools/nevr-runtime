// Quest 2 eye buffer one step down in combat (texture_quest2.h, docs/quest/combat/texture_quest2.md): the look combat hands the texture-quality owner.
#include "quest/combat/texture_quest2.h"

#include <time.h>

#include <atomic>
#include <cstdint>

#include "quest/combat/combat_active.h"
#include "quest/combat/combat_level.h"
#include "quest/combat/combat_level_targets.h"
#include "quest/combat/combat_tick.h"
#include "quest/combat/safe_read.h"
#include "quest/sentinel/hook_log.h"
#include "quest/sentinel/texture_quality_input.h"

namespace nevr_quest::combat::texture_quest2 {

namespace {

namespace rules = nevr_quest::texture_quality::rules;
namespace input = nevr_quest::texture_quality::input;
using sentinel::LogLevel;

// The published look: the class and loading are stored before the time, which is the release store a reader pairs with.
std::atomic<int> g_where{static_cast<int>(rules::Where::kNone)};
std::atomic<bool> g_loading{true};
std::atomic<std::int64_t> g_atMs{0};
std::atomic<bool> g_registered{false};
std::atomic<std::int64_t> g_nextLookMs{0};
std::atomic<std::uint32_t> g_looks{0};
std::atomic<int> g_changeLines{0};
std::atomic<std::int64_t (*)() noexcept> g_clock{nullptr};

std::int64_t NowMs() noexcept {
  if (const auto clock = g_clock.load(std::memory_order_acquire)) return clock();
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

rules::Where WhereOf(const level::Loaded& loaded) noexcept {
  if (loaded.echoArena) return rules::Where::kEchoArena;  // an Echo Arena level wins over any other still loaded (combat_level.h)
  if (loaded.combatMap) return rules::Where::kCombatMap;
  if (loaded.combatLobby) return rules::Where::kCombatLobby;
  return rules::Where::kNone;
}

ModuleResult Refused(const char* reason, bool lobby) noexcept {
  sentinel::LogFields(LogLevel::kError, "combat_texture_quest2_install",
                      {{"result", "refused"}, {"reason", reason}, {"lobby", lobby ? 1 : 0},
                       {"msg", "the Quest 2 step-down cannot see the loaded levels, so it is not registered: the player's level is sent in combat"}});
  return {false, reason};
}

}  // namespace

rules::Look CurrentLook() noexcept {
  rules::Look look;
  look.atMs = g_atMs.load(std::memory_order_acquire);
  look.where = static_cast<rules::Where>(g_where.load(std::memory_order_relaxed));
  look.loading = g_loading.load(std::memory_order_relaxed);
  return look;
}

void Tick(std::uintptr_t game) noexcept {
  const std::int64_t now = NowMs();
  if (now < g_nextLookMs.load(std::memory_order_relaxed)) return;
  g_nextLookMs.store(now + kLookEveryMs, std::memory_order_relaxed);
  const std::uint32_t looks = g_looks.fetch_add(1, std::memory_order_relaxed) + 1;
  const level::Loaded loaded = level::Scan(game);
  bool ok = true;
  const std::uint64_t fibers = SafeRead<std::uint64_t>(game + level::targets::kFiberCount, &ok);
  // A count that cannot be read is a load in progress: the owner makes no look-driven Resize on what it cannot see.
  const bool loading = !ok || fibers != 0;
  const rules::Where where = WhereOf(loaded);
  const bool changed = static_cast<int>(where) != g_where.load(std::memory_order_relaxed) || loading != g_loading.load(std::memory_order_relaxed) ||
                       g_atMs.load(std::memory_order_relaxed) == 0;
  g_where.store(static_cast<int>(where), std::memory_order_relaxed);
  g_loading.store(loading, std::memory_order_relaxed);
  g_atMs.store(now, std::memory_order_release);
  if (!changed) return;
  const int lines = g_changeLines.fetch_add(1, std::memory_order_relaxed);
  if (lines >= kChangeLines) return;
  sentinel::LogFields(LogLevel::kInfo, "combat_texture_quest2_look",
                      {{"where", rules::WhereName(where)},
                       {"loading", loading ? 1 : 0},
                       {"fibers", ok ? static_cast<long long>(fibers) : -1LL},
                       {"looks", looks},
                       {"msg", loading ? "a level is loading: the Quest 2 step-down makes no Resize until 10 s after it ends"
                                       : "no level is loading: the Quest 2 step-down may act once this has held for 10 s"}});
}

ModuleResult Install(bool on, bool lobby) noexcept {
  if (!on) {
    sentinel::LogFields(LogLevel::kInfo, "combat_texture_quest2_install",
                        {{"result", "off"}, {"reason", "off_config"}, {"lobby", lobby ? 1 : 0},
                         {"msg", "combat.texture_quest2_step_down is false: the player's texture level is sent in combat too"}});
    return {false, "off", /*off=*/true};
  }
  if (!Active()) return Refused("inactive", lobby);
  if (g_registered.load(std::memory_order_acquire)) return {true, "installed"};
  if (!tick::Installed()) return Refused("tick_not_installed", lobby);
  if (!tick::Add(&Tick)) return Refused("tick_table_full", lobby);
  g_registered.store(true, std::memory_order_release);
  rules::StepDownSwitches switches;
  switches.lobby = lobby;
  input::RegisterStepDown(&CurrentLook, switches);
  sentinel::LogFields(LogLevel::kInfo, "combat_texture_quest2_install",
                      {{"result", "installed"}, {"reason", "ok"}, {"lobby", lobby ? 1 : 0},
                       {"msg", "on a Quest 2 whose player chose no level, a settled combat level is sent Very Low; its Resize is made only with no level "
                               "loading for 10 s"}});
  return {true, "installed"};
}

void SetClockForTest(std::int64_t (*clockMs)() noexcept) noexcept { g_clock.store(clockMs, std::memory_order_release); }

void ResetForTest() noexcept {
  g_where.store(static_cast<int>(rules::Where::kNone));
  g_loading.store(true);
  g_atMs.store(0);
  g_registered.store(false);
  g_nextLookMs.store(0);
  g_looks.store(0);
  g_changeLines.store(0);
  g_clock.store(nullptr);
  input::ResetForTest();
}

}  // namespace nevr_quest::combat::texture_quest2
