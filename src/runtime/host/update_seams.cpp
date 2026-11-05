#include "runtime/host/update_seams.h"

#include "runtime/host/update_swap.h"

#include <atomic>

namespace nevr_host {

#if defined(NEVR_HOST_TESTING)
namespace {
std::atomic<DWORD> g_lock_timeout_for_test{0};
std::atomic<DWORD> g_lock_step_for_test{0};
std::atomic<bool (*)(int)> g_step_hook_for_test{nullptr};
std::atomic<int> g_failing_move_for_test{0};
std::atomic<DWORD> g_failing_move_error_for_test{0};
}  // namespace

void SetUpdateLockTimingForTest(DWORD timeout_ms, DWORD step_ms) noexcept {
  g_lock_timeout_for_test.store(timeout_ms);
  g_lock_step_for_test.store(step_ms);
}
void SetUpdateStepHookForTest(bool (*hook)(int step)) noexcept { g_step_hook_for_test.store(hook); }
void SetUpdateMoveFailureForTest(int move_number, DWORD error) noexcept {
  g_failing_move_for_test.store(move_number);
  g_failing_move_error_for_test.store(error);
}
#endif

DWORD SwapLockTimeoutMs() noexcept {
#if defined(NEVR_HOST_TESTING)
  if (const DWORD ms = g_lock_timeout_for_test.load()) return ms;
#endif
  return kUpdateLockTimeoutMs;
}

DWORD SwapLockStepMs() noexcept {
#if defined(NEVR_HOST_TESTING)
  if (const DWORD ms = g_lock_step_for_test.load()) return ms;
#endif
  return kUpdateLockStepMs;
}

bool SwapStepEnds(int step) noexcept {
#if defined(NEVR_HOST_TESTING)
  if (const auto hook = g_step_hook_for_test.load()) return hook(step);
#endif
  static_cast<void>(step);
  return false;
}

bool SwapMoveFails(int move, DWORD* error) noexcept {
#if defined(NEVR_HOST_TESTING)
  if (move != 0 && move == g_failing_move_for_test.load()) {
    *error = g_failing_move_error_for_test.load();
    return true;
  }
#endif
  static_cast<void>(move);
  static_cast<void>(error);
  return false;
}

}  // namespace nevr_host
