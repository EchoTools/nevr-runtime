#include "quest/combat/combat_scope.h"

#include <time.h>

#include "quest/combat/combat_level.h"
#include "quest/combat/combat_tick.h"
#include "quest/sentinel/hook_log.h"

namespace nevr_quest::combat::scope {

namespace {

std::atomic<bool> g_registered{false};
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

bool CombatOrLobby(std::uint32_t bits) noexcept {
  return (bits & kBitEchoArena) == 0 && (bits & (kBitCombatMap | kBitCombatLobby)) != 0;
}
bool CombatMap(std::uint32_t bits) noexcept { return (bits & (kBitCombatMap | kBitEchoArena)) == kBitCombatMap; }

}  // namespace

bool LookIsFresh() noexcept { return NowMs() - detail::g_lookedAtMs.load(std::memory_order_relaxed) <= kStaleAfterMs; }

ModuleResult InstallNormal() noexcept {
  if (g_registered.load(std::memory_order_acquire)) return {true, "installed"};
  const bool tickUp = tick::Installed();
  const bool added = tickUp && tick::Add(&Refresh);
  if (added) g_registered.store(true, std::memory_order_release);
  const char* const reason = added ? "ok" : (tickUp ? "tick_table_full" : "tick_not_installed");
  sentinel::LogFields(added ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "combat_scope_install",
                      {{"result", added ? "registered" : "refused"}, {"reason", reason}, {"callbacks", tick::Count()},
                       {"msg", added ? "the combat level answer is refreshed from the game-thread tick, four times a second"
                                     : "the combat level answer cannot change, so the modules that read it are not installed and the game keeps its own behaviour"}});
  return {added, added ? "installed" : reason};
}

bool Live() noexcept { return g_registered.load(std::memory_order_acquire) && tick::Installed(); }

void Refresh(std::uintptr_t game) noexcept {
  const std::int64_t now = NowMs();
  if (now < g_nextLookMs.load(std::memory_order_relaxed)) return;
  g_nextLookMs.store(now + kLookEveryMs, std::memory_order_relaxed);
  const std::uint32_t looks = g_looks.fetch_add(1, std::memory_order_relaxed) + 1;
  const level::Loaded loaded = level::Scan(game);
  const std::uint32_t bits = (loaded.combatMap ? kBitCombatMap : 0U) | (loaded.combatLobby ? kBitCombatLobby : 0U) |
                             (loaded.echoArena ? kBitEchoArena : 0U);
  const std::uint32_t before = detail::g_bits.load(std::memory_order_relaxed);
  detail::g_lookedAtMs.store(now, std::memory_order_relaxed);  // before the bits: a reader that sees them sees this look's time or a later one
  detail::g_bits.store(bits, std::memory_order_release);
  if (CombatOrLobby(bits) == CombatOrLobby(before) && CombatMap(bits) == CombatMap(before)) return;
  const std::uint64_t levelId = loaded.echoArena ? loaded.echoArenaLevel : (loaded.combatMap ? loaded.combatMapLevel : loaded.combatLobbyLevel);
  char levelText[19];
  const bool in = CombatOrLobby(bits);
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_scope_change",
                      {{"from", CombatOrLobby(before) ? 1 : 0}, {"to", in ? 1 : 0}, {"map", CombatMap(bits) ? 1 : 0},
                       {"level", sentinel::HexString(levelText, levelId)}, {"looks", looks},
                       {"msg", in ? "a combat level is loaded: the wrist HUD and shield colour modules act"
                                  : "no combat level is loaded: the wrist HUD and shield colour modules pass every call through"}});
}

std::uint32_t Looks() noexcept { return g_looks.load(std::memory_order_relaxed); }

void SetClockForTest(std::int64_t (*clockMs)() noexcept) noexcept { g_clock.store(clockMs, std::memory_order_release); }

void ResetForTest() noexcept {
  detail::g_bits.store(0);
  detail::g_lookedAtMs.store(0);
  g_registered.store(false);
  g_nextLookMs.store(0);
  g_looks.store(0);
  g_clock.store(nullptr);
}

}  // namespace nevr_quest::combat::scope
