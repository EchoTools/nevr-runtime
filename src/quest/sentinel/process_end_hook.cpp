#include "process_end_hook.h"

#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <new>

#include "hook_install.h"
#include "hook_log.h"

namespace nevr_quest::process_end {

namespace {

using AfterFn = void (*)() noexcept;

sentinel::GotHook g_hook;
std::atomic<BeforeExitFn> g_beforeExit{nullptr};
std::atomic<EnderFn> g_ender{&EndProcess};
std::atomic<AfterFn> g_after{nullptr};
std::atomic<unsigned> g_budgetMs{kFlushBudgetMs};
std::atomic<unsigned long long> g_returned{0};
std::atomic<sentinel::pinned::AndroidAppOpaque*> g_app{nullptr};

// The before-exit callback and the wait for it. One Shared per call, never freed: a callback that outlives
// its wait still has somewhere to write when it finally returns.
struct Shared {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  bool done;
  BeforeExitFn fn;
};

void* CallbackThread(void* arg) {
  Shared* const shared = static_cast<Shared*>(arg);
  shared->fn();
  pthread_mutex_lock(&shared->mutex);
  shared->done = true;
  pthread_cond_broadcast(&shared->cond);
  pthread_mutex_unlock(&shared->mutex);
  return nullptr;
}

// Runs `fn` on a helper thread and waits for it for at most `budgetMs`. Returns true when it finished.
bool RunBounded(BeforeExitFn fn, unsigned budgetMs) noexcept {
  Shared* const shared = new (std::nothrow) Shared;
  if (shared == nullptr) return false;
  shared->done = false;
  shared->fn = fn;
  pthread_mutex_init(&shared->mutex, nullptr);
  pthread_condattr_t attr;
  pthread_condattr_init(&attr);
  pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
  pthread_cond_init(&shared->cond, &attr);
  pthread_condattr_destroy(&attr);

  pthread_t thread;
  if (pthread_create(&thread, nullptr, &CallbackThread, shared) != 0) return false;
  pthread_detach(thread);

  timespec deadline{};
  clock_gettime(CLOCK_MONOTONIC, &deadline);
  deadline.tv_sec += static_cast<time_t>(budgetMs / 1000);
  deadline.tv_nsec += static_cast<long>(budgetMs % 1000) * 1000000L;
  if (deadline.tv_nsec >= 1000000000L) {
    deadline.tv_sec += 1;
    deadline.tv_nsec -= 1000000000L;
  }
  pthread_mutex_lock(&shared->mutex);
  while (!shared->done) {
    if (pthread_cond_timedwait(&shared->cond, &shared->mutex, &deadline) != 0) break;  // timed out
  }
  const bool done = shared->done;
  pthread_mutex_unlock(&shared->mutex);
  return done;
}

void RawExit(int code) noexcept { _exit(code); }
std::atomic<ExitFn> g_exit{&RawExit};

// Step 2: the before-exit callback for at most the budget; a callback that does not finish in time is named under `event`.
void RunBeforeExit(const char* event) noexcept {
  const BeforeExitFn callback = g_beforeExit.load(std::memory_order_acquire);
  if (callback == nullptr) return;
  const bool finished = RunBounded(callback, g_budgetMs.load(std::memory_order_relaxed));
  if (!finished) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, event,
                        {{"action", "before_exit_not_finished"}, {"budget_ms", g_budgetMs.load()}});
  }
}

// Steps 1 and 2: the line and the bounded before-exit callback. `reason` is null on the ordinary return.
void RunAfterReturn(int exitCode, const char* reason) noexcept {
  if (reason == nullptr) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "android_main_returned",
                        {{"action", "ending_process"}, {"exit_code", exitCode}});
  } else {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "android_main_returned",
                        {{"action", "ending_process"}, {"exit_code", exitCode}, {"reason", reason}});
  }
  RunBeforeExit("android_main_returned");
}

// Steps 1 and 2, after android_main returned. Reached only through g_after.
void AfterReturn() noexcept { RunAfterReturn(0, nullptr); }

// android_main. The handler touches the game only through `original` and reaches the rest through pointers.
void MainHandler(MainThunk::Fn original, sentinel::pinned::AndroidAppOpaque* app) noexcept {
  g_app.store(app, std::memory_order_release);
  original(app);
  g_returned.fetch_add(1, std::memory_order_relaxed);
  const AfterFn after = g_after.load(std::memory_order_acquire);
  if (after != nullptr) after();
  const EnderFn ender = g_ender.load(std::memory_order_acquire);
  if (ender != nullptr) ender();
}

NEVR_HOOK_RECORD(kMainHook, MainThunk, &MainHandler);

}  // namespace

sentinel::pinned::AndroidAppOpaque* CurrentApp() noexcept { return g_app.load(std::memory_order_acquire); }

void SetBeforeExit(BeforeExitFn fn) noexcept { g_beforeExit.store(fn, std::memory_order_release); }

void EndProcess() noexcept { _exit(0); }

void EndWithExitCode(int exitCode, const char* reason) noexcept {
  RunAfterReturn(exitCode, reason);
  g_exit.load(std::memory_order_acquire)(exitCode);
}

void EndNow(const char* reason) noexcept {
  sentinel::LogFields(sentinel::LogLevel::kInfo, "process_ending",
                      {{"action", "ending_process"}, {"exit_code", 0}, {"reason", reason}});
  RunBeforeExit("process_ending");
  g_exit.load(std::memory_order_acquire)(0);
}

void SetExitForTest(ExitFn fn) noexcept { g_exit.store(fn != nullptr ? fn : &RawExit, std::memory_order_release); }

bool InstallAt(const sentinel::GotTarget& target, EnderFn ender) noexcept {
  g_ender.store(ender, std::memory_order_release);
  g_budgetMs.store(kFlushBudgetMs, std::memory_order_relaxed);
  g_after.store(&AfterReturn, std::memory_order_release);
  MainThunk::Arm(kMainHook);
  const sentinel::GotStatus status = sentinel::InstallThunk<MainThunk>(g_hook, target);
  const bool ok = status == sentinel::GotStatus::kOk;
  sentinel::LogFields(ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "process_end_install",
                      {{"result", ok ? "installed" : "failed"}, {"status", sentinel::GotStatusName(status)}});
  return ok;
}

bool Install() noexcept { return InstallAt(sentinel::pinned::LibR15AndroidMain(), &EndProcess); }

void ArmForTest(EnderFn ender, unsigned budgetMs) noexcept {
  g_ender.store(ender, std::memory_order_release);
  g_budgetMs.store(budgetMs, std::memory_order_relaxed);
  g_after.store(&AfterReturn, std::memory_order_release);
  MainThunk::Arm(kMainHook);
}

void ResetForTest() noexcept {
  g_beforeExit.store(nullptr, std::memory_order_relaxed);
  g_ender.store(&EndProcess, std::memory_order_relaxed);
  g_after.store(nullptr, std::memory_order_relaxed);
  g_budgetMs.store(kFlushBudgetMs, std::memory_order_relaxed);
  g_returned.store(0, std::memory_order_relaxed);
  g_app.store(nullptr, std::memory_order_relaxed);
  g_exit.store(&RawExit, std::memory_order_relaxed);
}

Counts CurrentCounts() noexcept { return {g_returned.load(std::memory_order_relaxed)}; }

}  // namespace nevr_quest::process_end
