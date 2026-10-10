// Built with -fno-exceptions (callback_thunk.h refuses otherwise).
#include "quest/integration/dlopen_hook.h"

#include <cerrno>

#include "callback_thunk.h"
#include "hook_install.h"
#include "hook_report.h"
#include "pinned_targets.h"
#include "quest/integration/post_load.h"

namespace nevr_quest::integration {

namespace {

using DlopenSig = void*(const char* name, int flags);
struct DlopenTag {};
using DlopenThunk = sentinel::CallbackThunk<DlopenTag, DlopenSig>;

sentinel::GotHook g_dlopenHook;

void* HookedDlopen(DlopenThunk::Fn original, const char* name, int flags) noexcept {
  void* const handle = original(name, flags);
  if (handle != nullptr) {
    // The game may read errno/dlerror after a successful dlopen; leave them as the loader left them.
    const int savedErrno = errno;
    AfterDlopen(name, handle);
    errno = savedErrno;
  }
  return handle;
}

NEVR_HOOK_RECORD(kDlopenHook, DlopenThunk, &HookedDlopen);

}  // namespace

sentinel::GotTarget LibR15Dlopen() {
  return {sentinel::pinned::kLibR15, "dlopen", sentinel::RelocKind::kJumpSlot,
          sentinel::pinned::kLibR15BuildId, 0x36c6380ULL};
}

DlopenInstall InstallDlopenHook(sentinel::GotStatus* status) noexcept {
  DlopenThunk::Arm(kDlopenHook);
  const sentinel::GotStatus got = sentinel::InstallThunk<DlopenThunk>(g_dlopenHook, LibR15Dlopen());
  if (status != nullptr) *status = got;
  if (got != sentinel::GotStatus::kOk) {
    DlopenThunk::Disarm();
    return DlopenInstall::kFailed;
  }
  return DlopenInstall::kOk;
}

bool RegisterDlopenCounters() noexcept {
  // One counter, as when the reporter's table held 32 (clock 2, redirect 10, dlopen 1, social 19). The thunk's
  // fault counter (no original published) cannot move for a hook GotHook installed, and a failed install
  // is its own stage line.
  return sentinel::RegisterReportCounter("dlopen_calls", &DlopenThunk::CallCounter());
}

void* RunDlopenHandlerForTest(DlopenFn original, const char* name, int flags) noexcept {
  return HookedDlopen(original, name, flags);
}

}  // namespace nevr_quest::integration
