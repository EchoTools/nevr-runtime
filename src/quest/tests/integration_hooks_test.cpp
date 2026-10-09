// Host test for the integration's hook translation units (src/quest/integration/dlopen_hook.cpp,
// social_shim.cpp, built -fno-exceptions) and the reporter's counter budget: the dlopen handler's
// behaviour with a null and a non-null handle, the pinned dlopen target, a failed install leaving the
// original in place, and the total number of counters every hook of the sentinel registers.

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#include "got_hook.h"
#include "hook_report.h"
#include "quest/integration/dlopen_hook.h"
#include "quest/integration/post_load.h"
#include "quest/integration/social_shim.h"
#include "quest/redirect/hook_adapter.h"
#include "quest/tests/test_check.h"

using namespace nevr_quest::integration;

namespace {

int g_originalCalls = 0;
const char* g_lastName = nullptr;
int g_lastFlags = 0;
int g_actionRuns = 0;
int g_actionErrno = 0;
int g_marker = 0;

void* FakeDlopenOk(const char* name, int flags) {
  ++g_originalCalls;
  g_lastName = name;
  g_lastFlags = flags;
  errno = EAGAIN;  // the loader's own errno at return
  return &g_marker;
}
void* FakeDlopenFails(const char* name, int flags) {
  ++g_originalCalls;
  g_lastName = name;
  g_lastFlags = flags;
  errno = ENOENT;
  return nullptr;
}

ActionResult CountingAction() noexcept {
  ++g_actionRuns;
  errno = g_actionErrno;  // an action that clobbers errno (any libc call may)
  return {Settle::kRetryLater, "counting"};
}

void Reset(ActionFn login, ActionFn mm) {
  g_originalCalls = g_actionRuns = 0;
  g_lastName = nullptr;
  PostLoadActions a;
  a.login = login;
  a.matchmaking = mm;
  SetPostLoadActions(a);
}

void TestNullHandleRunsNothingAndReturnsNull() {
  Reset(&CountingAction, &CountingAction);
  errno = 0;
  void* const r = RunDlopenHandlerForTest(&FakeDlopenFails, "libmissing.so", 3);
  QCHECK(r == nullptr);
  QCHECK(g_originalCalls == 1 && g_lastFlags == 3 && std::strcmp(g_lastName, "libmissing.so") == 0);
  QCHECK(g_actionRuns == 0);        // a failed load installs nothing
  QCHECK(errno == ENOENT);          // and the game's failure reason is untouched
  QCHECK(PostLoadStatsView().calls == 0);
}

void TestNonNullHandleIsReturnedAndRunsThePostLoadActions() {
  Reset(&CountingAction, &CountingAction);
  g_actionErrno = EINTR;
  void* const r = RunDlopenHandlerForTest(&FakeDlopenOk, "/data/app/lib/libpnsovr.so", 2);
  QCHECK(r == &g_marker);           // the game gets exactly the handle the loader returned
  QCHECK(g_originalCalls == 1);
  QCHECK(g_actionRuns == 2);        // login and matchmaking each tried once
  QCHECK(errno == EAGAIN);          // the loader's errno survives the installs
  QCHECK(PostLoadStatsView().calls == 1);
}

void TestOnlyTheWantedActionsRun() {
  Reset(&CountingAction, nullptr);
  int dummy = RunDlopenHandlerForTest(&FakeDlopenOk, "libx.so", 0) != nullptr ? 1 : 0;
  QCHECK(dummy == 1);
  QCHECK(g_actionRuns == 1);
}

void TestPinnedDlopenTarget() {
  const sentinel::GotTarget t = LibR15Dlopen();
  QCHECK(std::strcmp(t.module, "libr15.so") == 0);
  QCHECK(std::strcmp(t.symbol, "dlopen") == 0);
  QCHECK(t.kind == sentinel::RelocKind::kJumpSlot);
  QCHECK(t.buildId != nullptr && std::strcmp(t.buildId, "b243509c08ce677aeb95fa348016949b3fc45230") == 0);
  QCHECK(t.slotVaddr.has_value() && *t.slotVaddr == 0x36c6380ULL);
}

// With no libr15.so in this process the install is refused and says why; the thunk stays disarmed, so
// nothing is redirected and a later call would go straight to the original.
void TestInstallWithoutTheModuleFailsCleanly() {
  sentinel::GotStatus status = sentinel::GotStatus::kOk;
  QCHECK(InstallDlopenHook(&status) == DlopenInstall::kFailed);
  QCHECK(status == sentinel::GotStatus::kModuleNotLoaded);
}

// The counter budget: every hook of the sentinel registers its counters before the single
// StartReporter; the table holds sentinel::kMaxReportCounters. Clock hook 2 (entry.cpp), redirect 10,
// dlopen 1, social 19.
void TestCounterBudget() {
  sentinel::StopReporter();  // forget anything registered earlier in this process
  static std::atomic<std::uint64_t> clockCalls{0}, clockFaults{0};
  QCHECK(sentinel::RegisterReportCounter("clock_gettime_calls", &clockCalls));
  QCHECK(sentinel::RegisterReportCounter("clock_gettime_thunk_faults", &clockFaults, sentinel::ReportKind::kFaults));
  QCHECK(nevr_quest::redirect::RegisterRedirectCounters());
  QCHECK(RegisterDlopenCounters());
  QCHECK(RegisterSocialCounters());

  constexpr int kUsed = 2 + 10 + 1 + 19;
  constexpr int kCapacity = static_cast<int>(sentinel::kMaxReportCounters);
  static std::atomic<std::uint64_t> spare[kCapacity];
  int extra = 0;
  while (extra < kCapacity && sentinel::RegisterReportCounter("spare", &spare[extra])) ++extra;
  QCHECK(extra == kCapacity - kUsed);  // exactly the counters above are in the table

  // A late registration is refused, not silently dropped (why the sequence registers before it starts).
  QCHECK(sentinel::StartReporter(1000, 10000, 60000));
  QCHECK(!sentinel::RegisterReportCounter("late", &spare[0]));
  sentinel::StopReporter();
}

}  // namespace

int main() {
  TestNullHandleRunsNothingAndReturnsNull();
  TestNonNullHandleIsReturnedAndRunsThePostLoadActions();
  TestOnlyTheWantedActionsRun();
  TestPinnedDlopenTarget();
  TestInstallWithoutTheModuleFailsCleanly();
  TestCounterBudget();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "integration_hooks_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("integration_hooks_test: all checks passed\n");
  return 0;
}
