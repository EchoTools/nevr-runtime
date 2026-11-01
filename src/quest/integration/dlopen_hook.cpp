// Built with -fno-exceptions (callback_thunk.h refuses otherwise).
#include "quest/integration/dlopen_hook.h"

#include <dlfcn.h>

#include <atomic>
#include <cerrno>

#include "callback_thunk.h"
#include "hook_install.h"
#include "hook_log.h"
#include "hook_report.h"
#include "pinned_targets.h"
#include "quest/integration/post_load.h"
#include "quest/plugin/script_redirect.h"

namespace nevr_quest::integration {

namespace {

using DlopenSig = void*(const char* name, int flags);
struct DlopenTag {};
using DlopenThunk = sentinel::CallbackThunk<DlopenTag, DlopenSig>;

sentinel::GotHook g_dlopenHook;
std::atomic<bool> g_installed{false};

void* HookedDlopen(DlopenThunk::Fn original, const char* name, int flags) noexcept {
  // The script redirect (script_redirect.h): a stock script library's name becomes its combat build's name when the
  // combat unit registered it. Every other name, and every name in a process with no registration, is passed unchanged.
  char redirected[script_redirect::kNameCapacity];
  const char* const loadName = script_redirect::RewriteLoadName(name, redirected, sizeof redirected);
  void* const handle = original(loadName, flags);
  if (handle == nullptr && loadName != name) {
    // A redirected load that failed fails loudly: the game gets the loader's null, and the name and dlerror() text say why.
    const int savedErrno = errno;
    const char* const reason = dlerror();
    sentinel::LogFields(sentinel::LogLevel::kError, "script_redirect_failed",
                        {{"from", name}, {"to", loadName}, {"dlerror", reason != nullptr ? reason : "(none)"}});
    errno = savedErrno;
  }
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
  // Service-side (#1737): the login and matchmaking installs run from this hook.
  return sentinel::GotTarget{sentinel::pinned::kLibR15, "dlopen", sentinel::RelocKind::kJumpSlot,
                             sentinel::pinned::LibR15PatchedBuildId(), 0x36c6380ULL}
      .ServiceSide();
}

DlopenInstall InstallDlopenHook(sentinel::GotStatus* status) noexcept {
  // The crash-first step installs the hook on every launch (it needs the moment after a provider library loads), so the step
  // that installs it for the post-load actions finds it live. A second call is the same hook, not a second owner of the slot.
  if (g_installed.load(std::memory_order_acquire)) {
    if (status != nullptr) *status = sentinel::GotStatus::kOk;
    return DlopenInstall::kOk;
  }
  DlopenThunk::Arm(kDlopenHook);
  const sentinel::GotStatus got = sentinel::InstallThunk<DlopenThunk>(g_dlopenHook, LibR15Dlopen());
  if (status != nullptr) *status = got;
  if (got != sentinel::GotStatus::kOk) {
    DlopenThunk::Disarm();
    return DlopenInstall::kFailed;
  }
  g_installed.store(true, std::memory_order_release);
  script_redirect::LogState();  // registry=0 status=pass-through until a combat unit registers (its own line says armed)
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
