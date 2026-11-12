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
std::atomic<std::uint64_t> g_lastLevel{0};  // the level that decided the last look (named by the stale record)
std::atomic<std::int64_t (*)() noexcept> g_clock{nullptr};
// The sequence count of the look record (LastLook): odd while Refresh writes a look. Written by the game thread only.
std::atomic<std::uint32_t> g_seq{0};
std::atomic<void (*)()> g_lookSeam{nullptr};
std::atomic<void (*)()> g_readSeam{nullptr};
std::atomic<void (*)()> g_closedSeam{nullptr};

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

// Closes the sequence count of the look Refresh is writing (`seq` is its value before the write): every field of the look is stored before this call.
void CloseLook(std::uint32_t seq) noexcept {
  g_seq.store(seq + 2, std::memory_order_release);
  const auto closed = g_closedSeam.load(std::memory_order_acquire);
  if (closed != nullptr) closed();
}

}  // namespace

bool LookIsFresh() noexcept {
  // The acquire load pairs with the release store of the bits in Refresh: a caller that saw a combat bit sees, after this, the stamp of that look or a later one.
  (void)detail::g_bits.load(std::memory_order_acquire);
  return NowMs() - detail::g_lookedAtMs.load(std::memory_order_relaxed) <= kStaleAfterMs;
}

Look LastLook() noexcept {
  for (int attempt = 0; attempt < kLookReadTries; ++attempt) {
    const std::uint32_t begin = g_seq.load(std::memory_order_acquire);
    if ((begin & 1U) != 0) continue;  // a look is being written
    Look look;
    look.bits = detail::g_bits.load(std::memory_order_relaxed);
    const auto readSeam = g_readSeam.load(std::memory_order_acquire);
    if (readSeam != nullptr) readSeam();
    look.level = g_lastLevel.load(std::memory_order_relaxed);
    look.looks = g_looks.load(std::memory_order_relaxed);
    const std::int64_t at = detail::g_lookedAtMs.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (g_seq.load(std::memory_order_relaxed) != begin) continue;  // a look was written meanwhile: the fields may be of two looks
    look.fresh = look.looks != 0 && NowMs() - at <= kStaleAfterMs;
    return look;
  }
  return Look{};  // never whole: no look, not fresh (fails closed)
}

bool SeenEchoArena(const Look& look) noexcept { return !look.fresh || (look.bits & kBitEchoArena) != 0; }
bool SeenEchoArena() noexcept { return SeenEchoArena(LastLook()); }

ModuleResult InstallNormal() noexcept {
  if (g_registered.load(std::memory_order_acquire)) return {true, "installed"};
  const bool tickUp = tick::Installed();
  const bool added = tickUp && tick::Add(&Refresh);
  if (added) g_registered.store(true, std::memory_order_release);
  const char* const reason = added ? "ok" : (tickUp ? "tick_table_full" : "tick_not_installed");
  sentinel::LogFields(added ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "combat_scope_install",
                      {{"result", added ? "registered" : "refused"}, {"reason", reason}, {"callbacks", tick::Count()},
                       {"msg", added ? "the combat level answer is refreshed from the game-thread tick, four times a second, for every combat module that reads it"
                                     : "the combat level answer cannot change, so the combat modules that read it are not installed and the game keeps its own behaviour"}});
  return {added, added ? "installed" : reason};
}

bool Live() noexcept { return g_registered.load(std::memory_order_acquire) && tick::Installed(); }

void Refresh(std::uintptr_t game) noexcept {
  const std::int64_t now = NowMs();
  if (now < g_nextLookMs.load(std::memory_order_relaxed)) return;
  g_nextLookMs.store(now + kLookEveryMs, std::memory_order_relaxed);
  const level::Loaded loaded = level::Scan(game);
  const std::uint32_t bits = (loaded.combatMap ? kBitCombatMap : 0U) | (loaded.combatLobby ? kBitCombatLobby : 0U) |
                             (loaded.echoArena ? kBitEchoArena : 0U);
  const std::uint32_t before = detail::g_bits.load(std::memory_order_relaxed);
  const std::int64_t last = detail::g_lookedAtMs.load(std::memory_order_relaxed);
  if (CombatOrLobby(before) && now - last > kStaleAfterMs) {
    // For this gap the readers saw a stale look and answered with the game's own functions, and nothing could say so while the tick was stopped: the
    // first look after it is back does (once per gap), naming the level the answer was held for.
    char levelText[19];
    sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_scope_stale",
                        {{"gap_ms", static_cast<std::int64_t>(now - last)}, {"stale_after_ms", kStaleAfterMs},
                         {"level", sentinel::HexString(levelText, g_lastLevel.load(std::memory_order_relaxed))},
                         {"msg", "the combat tick stopped for longer than the level answer is believed: every combat module that reads it passed every call "
                                 "through to the game in that gap"}});
  }
  const std::uint64_t levelId = loaded.echoArena ? loaded.echoArenaLevel : (loaded.combatMap ? loaded.combatMapLevel : loaded.combatLobbyLevel);
  // The record is written under the sequence count (odd while it is written), so LastLook reads one look whole on any thread.
  const std::uint32_t seq = g_seq.load(std::memory_order_relaxed);
  g_seq.store(seq + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  const std::uint32_t looks = g_looks.fetch_add(1, std::memory_order_relaxed) + 1;
  const auto seam = g_lookSeam.load(std::memory_order_acquire);
  if (seam != nullptr) seam();
  // The stamp is stored before the bits, which are a release store; LookIsFresh's acquire load of the bits pairs with it.
  detail::g_lookedAtMs.store(now, std::memory_order_relaxed);
  g_lastLevel.store(levelId, std::memory_order_relaxed);
  detail::g_bits.store(bits, std::memory_order_release);
  CloseLook(seq);
  if (CombatOrLobby(bits) == CombatOrLobby(before) && CombatMap(bits) == CombatMap(before)) return;
  char levelText[19];
  const bool in = CombatOrLobby(bits);
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_scope_change",
                      {{"from", CombatOrLobby(before) ? 1 : 0}, {"to", in ? 1 : 0}, {"map", CombatMap(bits) ? 1 : 0},
                       {"level", sentinel::HexString(levelText, levelId)}, {"looks", looks},
                       {"msg", in ? "a combat level is loaded: the combat modules that read the level answer act"
                                  : "no combat level is loaded: the combat modules that read the level answer pass every call through"}});
}

std::uint32_t Looks() noexcept { return g_looks.load(std::memory_order_relaxed); }

void SetClockForTest(std::int64_t (*clockMs)() noexcept) noexcept { g_clock.store(clockMs, std::memory_order_release); }
void SetLookSeamForTest(void (*seam)()) noexcept { g_lookSeam.store(seam, std::memory_order_release); }
void SetLookReadSeamForTest(void (*seam)()) noexcept { g_readSeam.store(seam, std::memory_order_release); }
void SetLookClosedSeamForTest(void (*seam)()) noexcept { g_closedSeam.store(seam, std::memory_order_release); }

void ResetForTest() noexcept {
  detail::g_bits.store(0);
  detail::g_lookedAtMs.store(0);
  g_registered.store(false);
  g_nextLookMs.store(0);
  g_looks.store(0);
  g_lastLevel.store(0);
  g_clock.store(nullptr);
  g_seq.store(0);
  g_lookSeam.store(nullptr);
  g_readSeam.store(nullptr);
  g_closedSeam.store(nullptr);
}

}  // namespace nevr_quest::combat::scope
