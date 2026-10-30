#include "runtime/lifecycle/crash_first_filter.h"

#include <atomic>

#include "core/crash_first.h"

namespace nevr_crash_first_win {

namespace {

// Constant-initialised (atomics, a slot of zeroes, SRWLOCK_INIT), so it is ready before any dynamic initialiser runs.
nevr_crash_first::DisplacedSlot<Filter> g_game;
nevr_crash_first::FatalClaim g_claim;
SRWLOCK g_storeLock = SRWLOCK_INIT;  // serialises the game's requests; the filter never takes it
std::atomic<ReportFn> g_report{nullptr};
std::atomic<NoteFn> g_note{nullptr};
std::atomic<long> g_requests{0};
std::atomic<long> g_chained{0};

// A second thread that reaches the filter while the first writes the report waits this long for it, in steps. The wait is in
// the crash path, where there is nothing else to wait on.
constexpr int kClaimWaitSteps = 1000;
constexpr DWORD kClaimWaitStepMs = 10;

}  // namespace

void SetActions(ReportFn report, NoteFn note) noexcept {
  g_report.store(report, std::memory_order_release);
  g_note.store(note, std::memory_order_release);
}

Filter RememberGameFilter(Filter requested) noexcept {
  Filter previous = nullptr;
  g_game.Read(&previous);  // empty or busy: nothing was requested before
  g_requests.fetch_add(1, std::memory_order_relaxed);
  if (requested == &TopLevelFilter) return previous;
  AcquireSRWLockExclusive(&g_storeLock);
  g_game.Store(requested);
  ReleaseSRWLockExclusive(&g_storeLock);
  return previous;
}

void SeedPrior(Filter prior) noexcept {
  if (prior == &TopLevelFilter) return;
  AcquireSRWLockExclusive(&g_storeLock);
  if (!g_game.HasBeenSet()) g_game.Store(prior);
  ReleaseSRWLockExclusive(&g_storeLock);
}

LONG WINAPI TopLevelFilter(EXCEPTION_POINTERS* info) {
  if (g_claim.TryClaim()) {
    const ReportFn report = g_report.load(std::memory_order_acquire);
    if (report != nullptr) report(info);
    g_claim.MarkDone();
  } else {
    for (int step = 0; step < kClaimWaitSteps && !g_claim.Done(); ++step) Sleep(kClaimWaitStepMs);
  }
  Filter game = nullptr;
  const bool have = g_game.Read(&game) == nevr_crash_first::ReadResult::kRead && game != nullptr && game != &TopLevelFilter;
  LONG result = EXCEPTION_CONTINUE_SEARCH;
  if (have) {
    g_chained.fetch_add(1, std::memory_order_relaxed);
    result = game(info);
  }
  const NoteFn note = g_note.load(std::memory_order_acquire);
  if (note != nullptr) note(info, have, g_requests.load(std::memory_order_relaxed));
  return result;
}

long RequestCount() noexcept { return g_requests.load(std::memory_order_relaxed); }
long ChainedCount() noexcept { return g_chained.load(std::memory_order_relaxed); }

void ResetForTest() noexcept {
  g_game.ResetForTest();
  g_claim.ResetForTest();
  g_report.store(nullptr, std::memory_order_relaxed);
  g_note.store(nullptr, std::memory_order_relaxed);
  g_requests.store(0, std::memory_order_relaxed);
  g_chained.store(0, std::memory_order_relaxed);
}

}  // namespace nevr_crash_first_win
