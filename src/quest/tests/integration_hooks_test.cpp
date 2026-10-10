// Host test for the integration's hook translation units (src/quest/integration/dlopen_hook.cpp,
// social_shim.cpp, built -fno-exceptions) and the reporter's counter budget: the dlopen handler's
// behaviour with a null and a non-null handle, the pinned dlopen target, a failed install leaving the
// original in place, and the total number of counters every hook of the sentinel registers.

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "got_hook.h"
#include "hook_report.h"
#include "quest/integration/ctor_sequence.h"
#include "quest/integration/dlopen_hook.h"
#include "quest/integration/post_load.h"
#include "quest/integration/social_shim.h"
#include "quest/login/login_hook.h"
#include "quest/redirect/hook_adapter.h"
#include "quest/tests/test_check.h"

using namespace nevr_quest::integration;

// sentinel/login_prompt_hook.h's registration function (#239). The header includes callback_thunk.h, which
// only -fno-exceptions translation units may include, so it is declared here; the definition is the real
// one (login_prompt_hook.cpp is linked in), and changed parameters fail the link.
namespace nevr_quest::login_prompt {
bool RegisterCounters() noexcept;
}  // namespace nevr_quest::login_prompt

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

// The counter budget, through the real constructor sequence (ctor_sequence.cpp) with every feature on: each
// registration step calls the package's real registration function, in the sequence's order, before its
// single StartReporter. Every registration must be accepted (a refused one would leave its hook out), and
// the table must have exactly the counters below in it. The clock hook's two are registered under its real
// names here because entry.cpp (jni.h, breakpad) cannot be built on the host. Clock 2, redirect 10,
// dlopen 1, login 2 (#237: calls and faults of the SendLogInRequest thunk), login prerequisites 16 (#338:
// one calls counter per hook), social 19, login prompt 14 (#239, login_prompt_hook.h kCounterCount).
constexpr int kSentinelCounters = 2 + 10 + 1 + 2 + 16 + 19 + 14;
static_assert(kSentinelCounters <= static_cast<int>(sentinel::kMaxReportCounters),
              "the sentinel's hooks register more counters than the reporter holds");

struct RealCounterSteps final : Steps {
  nevr_quest::ResolvedConfig config;
  void ArmCrashReporter() override {}
  const nevr_quest::ResolvedConfig& ResolveConfig() override { return config; }
  bool RegisterClockCounters() override {
    static std::atomic<std::uint64_t> calls{0}, faults{0};
    bool ok = sentinel::RegisterReportCounter("clock_gettime_calls", &calls);
    ok = sentinel::RegisterReportCounter("clock_gettime_thunk_faults", &faults, sentinel::ReportKind::kFaults) && ok;
    return ok;
  }
  bool RegisterRedirectCounters() override { return nevr_quest::redirect::RegisterRedirectCounters(); }
  bool RegisterDlopenCounters() override { return nevr_quest::integration::RegisterDlopenCounters(); }
  bool RegisterLoginCounters() override { return QuestLogin::RegisterLoginHookCounters(); }
  bool RegisterSocialCounters() override { return nevr_quest::integration::RegisterSocialCounters(); }
  bool RegisterLoginPromptCounters() override { return nevr_quest::login_prompt::RegisterCounters(); }
  // The reporter is started after the budget is measured (below), so a refused registration shows here.
  bool StartReporter() override { return true; }
  bool InstallClockHook() override { return true; }
  bool StartTokenAuth() override { return true; }
  bool InstallLoginPrompt(bool counted) override { return counted; }
  bool StartBridge() override { return true; }
  bool InstallRedirect() override { return true; }
  bool InstallSocial() override { return true; }
  bool InstallDlopenHook(bool, bool) override { return true; }
  bool StartHwDump() override { return true; }
  void Note(const char*, const char*, const char*) override {}
};

void TestCounterBudget() {
  sentinel::StopReporter();  // forget anything registered earlier in this process
  RealCounterSteps steps;
  steps.config.effective.redirect = steps.config.effective.bridge = true;
  steps.config.effective.login = steps.config.effective.social = true;
  const ConstructorReport r = RunConstructorSequence(steps);
  for (StepId id : {StepId::kRegisterClockCounters, StepId::kRegisterRedirectCounters, StepId::kRegisterDlopenCounters,
                    StepId::kRegisterLoginCounters, StepId::kRegisterSocialCounters,
                    StepId::kRegisterLoginPromptCounters}) {
    QCHECK(r.at(id).state == StepState::kOk);
  }
  // No hook was left out for want of a counter slot.
  for (StepId id : {StepId::kInstallRedirect, StepId::kInstallSocial, StepId::kInstallLoginPrompt,
                    StepId::kInstallDlopenHook}) {
    QCHECK(r.at(id).state == StepState::kOk);
  }

  constexpr int kCapacity = static_cast<int>(sentinel::kMaxReportCounters);
  static std::atomic<std::uint64_t> spare[kCapacity];
  int extra = 0;
  while (extra < kCapacity && sentinel::RegisterReportCounter("spare", &spare[extra])) ++extra;
  QCHECK(extra == kCapacity - kSentinelCounters);  // exactly the sequence's counters are in the table

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
