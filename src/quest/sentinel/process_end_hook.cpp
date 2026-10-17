#include "process_end_hook.h"

// Placeholder, filled in by the next commit: the handler only calls the original.

#include <atomic>

#include "hook_install.h"

namespace nevr_quest::process_end {

namespace {

std::atomic<unsigned long long> g_returned{0};

void MainHandler(MainThunk::Fn original, sentinel::pinned::AndroidAppOpaque* app) noexcept {
  original(app);
  g_returned.fetch_add(1, std::memory_order_relaxed);
}

NEVR_HOOK_RECORD(kMainHook, MainThunk, &MainHandler);

}  // namespace

void SetBeforeExit(BeforeExitFn) noexcept {}
bool Install() noexcept { return false; }
void EndProcess() noexcept {}
void ArmForTest(EnderFn, unsigned) noexcept { MainThunk::Arm(kMainHook); }
void ResetForTest() noexcept { g_returned.store(0, std::memory_order_relaxed); }
Counts CurrentCounts() noexcept { return {g_returned.load(std::memory_order_relaxed)}; }

}  // namespace nevr_quest::process_end
