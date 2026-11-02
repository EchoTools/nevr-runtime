// Host test for the integration's hook translation units (src/quest/integration/dlopen_hook.cpp,
// social_shim.cpp, built -fno-exceptions) and the reporter's counter budget: the dlopen handler's
// behaviour with a null and a non-null handle, the pinned dlopen target, a failed install leaving the
// original in place, and the total number of counters every hook of the sentinel registers.

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <unistd.h>

#include "got_hook.h"
#include "hook_log.h"
#include "hook_report.h"
#include "libr15_identity.h"
#include "quest/integration/ctor_sequence.h"
#include "quest/integration/dlopen_hook.h"
#include "quest/integration/post_load.h"
#include "quest/integration/social_shim.h"
#include "quest/login/login_hook.h"
#include "quest/login/login_prerequisites.h"
#include "quest/plugin/script_redirect.h"
#include "quest/redirect/hook_adapter.h"
#include "quest/tests/test_check.h"

using namespace nevr_quest::integration;

// sentinel/login_prompt_hook.h's registration function (#239). The header includes callback_thunk.h, which
// only -fno-exceptions translation units may include, so it is declared here; the definition is the real
// one (login_prompt_hook.cpp is linked in), and changed parameters fail the link.
namespace nevr_quest::login_prompt {
bool RegisterCounters() noexcept;
}  // namespace nevr_quest::login_prompt
// The same for sentinel/login_title_hook.h (#1137).
namespace nevr_quest::login_title {
bool RegisterCounters() noexcept;
}  // namespace nevr_quest::login_title
// The same for sentinel/obb_skip_hook.h (#319).
namespace nevr_quest::obb_skip {
bool RegisterCounters() noexcept;
}  // namespace nevr_quest::obb_skip
// And for sentinel/resource_request_hook.h (#871).
namespace nevr_quest::resource_request {
bool RegisterCounters() noexcept;
}  // namespace nevr_quest::resource_request
// The same for sentinel/texture_quality_hook.h (#943).
namespace nevr_quest::texture_quality {
bool RegisterCounters() noexcept;
}  // namespace nevr_quest::texture_quality
// And for sentinel/texture_menu_hook.h (#1012): the Texture Resolution control's thirteen counters, registered by the same step.
namespace nevr_quest::texture_menu {
bool RegisterCounters() noexcept;
}  // namespace nevr_quest::texture_menu
// The same for sentinel/clock_levels_hook.h (#928).
namespace nevr_quest::clock_levels {
bool RegisterCounters() noexcept;
}  // namespace nevr_quest::clock_levels

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

std::string g_hookLines;
void CaptureHookLine(sentinel::LogLevel, const char* line) {
  g_hookLines += line;
  g_hookLines += '\n';
}

// The script redirect through the real handler (#1737): with a registry set, the handler opens the combat library's name
// where the game asked for the stock one, tells the post-load observer the game's own name, and a failed redirected open is
// logged and reissued; a name that is not registered passes through untouched.
void TestHandlerRedirectsARegisteredScriptLibrary() {
  namespace sr = nevr_quest::script_redirect;
  sr::ResetForTest();
  const char* const base = std::getenv("TMPDIR");
  std::string dir = std::string(base != nullptr && base[0] != '\0' ? base : "/tmp") + "/q15t_dlopen_hook_XXXXXX";
  QCHECK(mkdtemp(dir.data()) != nullptr);
  const std::string file = dir + "/lib37f88adb0d8066f7_q15t.so";
  if (std::FILE* f = std::fopen(file.c_str(), "wb")) {
    std::fputs("aarch64", f);
    std::fclose(f);
  }
  const std::uint64_t hashes[] = {0x37f88adb0d8066f7ULL};
  sentinel::SetLogSink(nullptr);  // the registration's own line goes to stderr; the cases below are captured
  QCHECK(sr::RegisterIn(dir.c_str(), "test", hashes, 1) == NEVR_SCRIPT_REDIRECT_OK);
  sentinel::SetLogSink(&CaptureHookLine);

  Reset(&CountingAction, &CountingAction);
  g_actionErrno = 0;
  g_hookLines.clear();
  void* const r = RunDlopenHandlerForTest(&FakeDlopenOk, "/data/app/x/lib/arm64/lib37f88adb0d8066f7.so", 2);
  QCHECK(r == &g_marker);
  QCHECK(g_originalCalls == 1 && g_lastFlags == 2);
  QCHECK(g_lastName != nullptr && std::strcmp(g_lastName, "lib37f88adb0d8066f7_q15t.so") == 0);  // the loader got the combat name
  QCHECK(g_hookLines.find("\"event\":\"script_redirected\"") != std::string::npos);
  QCHECK(PostLoadStatsView().calls == 1);  // the post-load actions still ran after the redirected open

  // An unregistered script name, and a name that is not a script library: both reach the loader as asked.
  Reset(&CountingAction, &CountingAction);
  g_hookLines.clear();
  QCHECK(RunDlopenHandlerForTest(&FakeDlopenOk, "lib0123456789abcdef.so", 2) == &g_marker);
  QCHECK(std::strcmp(g_lastName, "lib0123456789abcdef.so") == 0);
  QCHECK(RunDlopenHandlerForTest(&FakeDlopenOk, "libpnsovr.so", 2) == &g_marker);
  QCHECK(std::strcmp(g_lastName, "libpnsovr.so") == 0);
  QCHECK(g_hookLines.find("script_redirect") == std::string::npos);

  // A registered name whose redirected open fails: null to the game, the failure logged, and no post-load action.
  Reset(&CountingAction, &CountingAction);
  g_hookLines.clear();
  QCHECK(RunDlopenHandlerForTest(&FakeDlopenFails, "lib37f88adb0d8066f7.so", 2) == nullptr);
  QCHECK(g_hookLines.find("\"event\":\"script_redirect_failed\"") != std::string::npos);
  QCHECK(g_actionRuns == 0);

  sentinel::SetLogSink(nullptr);
  sr::ResetForTest();
  std::remove(file.c_str());
  rmdir(dir.c_str());
}

void TestPinnedDlopenTarget() {
  const sentinel::GotTarget t = LibR15Dlopen();
  QCHECK(std::strcmp(t.module, "libr15.so") == 0);
  QCHECK(std::strcmp(t.symbol, "dlopen") == 0);
  QCHECK(t.kind == sentinel::RelocKind::kJumpSlot);
  QCHECK(t.buildId != nullptr && std::strcmp(t.buildId, sentinel::pinned::LibR15PatchedBuildId()) == 0);
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
// names here because entry.cpp (jni.h, breakpad) cannot be built on the host. Clock 2, redirect 17 (#408: the
// CSysHttp::CreateConnection thunk's calls and faults, and the URL decisions' two; #1737: the login_host fallback's three,
// redirect_login_fallback_versioned, _kept and _failed in hook_adapter.cpp),
// dlopen 1, login 2 (#237: calls and faults of the SendLogInRequest thunk), login prerequisites 24 (#944: ovr_GetLoggedInUserID's calls and stand-ins; #411 adds the refused social org-id requests, the local entitlement answer and the local message pump's four: pops, deliveries, deferrals, SDK ids in the local range; #338:
// one calls counter per hook), social 26 (the facade's, the invite gate's, the rich presence trace's four, its three local answers), login prompt 17 (#239, #672, #1391, login_prompt_hook.h kCounterCount), login title 5 (#1137, login_title_hook.h kCounterCount;
// registered with the login prompt's, through the same step), OBB-mount
// skip 6 (#319, #866: the mount, the path and the data-root question, obb_skip_hook.h kCounterCount), texture quality 4 (#943,
// texture_quality_hook.h kCounterCount) and the Texture Resolution control 13 (#1012, texture_menu_hook.h kCounterCount; registered by the same step), clock levels 2 (#928, clock_levels_hook.h kCounterCount; no slot of its own), resource request hook 4 (#871, #1737 C5,
// resource_request_hook.h kCounterCount: the calls, the faults, and the overlay's served and failed; registered in every build).
// 91 on main before #943, 95 with it, which filled the 96 the table held
// then; the table holds 128 since #1137. The resource request term stays last: tools/tests/test_quest_resource_request_wiring.py
// reads it there.
constexpr int kSentinelCounters = 2 + 17 + 1 + 2 + 24 + 26 + 17 + 5 + 6 + 4 + 13 + 2 + 4;
static_assert(kSentinelCounters <= static_cast<int>(sentinel::kMaxReportCounters),
              "the sentinel's hooks register more counters than the reporter holds");

struct RealCounterSteps final : Steps {
  nevr_quest::ResolvedConfig config;
  void ArmCrashReporter() override {}
  const nevr_quest::ResolvedConfig& ResolveConfig() override { return config; }
  PluginCounts PlanPlugins() override { return {}; }  // the plugin loader registers no reporter counter
  bool RunEarlyPlugins() override { return true; }
  void FinishPlugins(const char*) override {}
  bool RegisterClockCounters() override {
    static std::atomic<std::uint64_t> calls{0}, faults{0};
    bool ok = sentinel::RegisterReportCounter("clock_gettime_calls", &calls);
    ok = sentinel::RegisterReportCounter("clock_gettime_thunk_faults", &faults, sentinel::ReportKind::kFaults) && ok;
    return ok;
  }
  bool RegisterRedirectCounters() override { return nevr_quest::redirect::RegisterRedirectCounters(); }
  bool RegisterDlopenCounters() override { return nevr_quest::integration::RegisterDlopenCounters(); }
  bool RegisterLoginCounters() override { return nevr_quest_login::RegisterLoginHookCounters(); }
  bool RegisterSocialCounters() override { return nevr_quest::integration::RegisterSocialCounters(); }
  // entry.cpp registers the login title hook's counters with the login prompt's, in the same step.
  bool RegisterLoginPromptCounters() override {
    return nevr_quest::login_prompt::RegisterCounters() && nevr_quest::login_title::RegisterCounters();
  }
  bool RegisterObbSkipCounters() override { return nevr_quest::obb_skip::RegisterCounters(); }
  bool ResourceRequestLogWanted() override { return true; }  // the log lines are the development build's; the counters are the same
  bool RegisterResourceRequestCounters() override { return nevr_quest::resource_request::RegisterCounters(); }
  bool RegisterTextureQualityCounters() override {
    const bool quality = nevr_quest::texture_quality::RegisterCounters();
    const bool menu = nevr_quest::texture_menu::RegisterCounters();
    return quality && menu;
  }
  bool RegisterClockLevelsCounters() override { return nevr_quest::clock_levels::RegisterCounters(); }
  // The reporter is started after the budget is measured (below), so a refused registration shows here.
  bool StartReporter() override { return true; }
  bool AttachLibR15() override { return true; }
  bool InstallClockHook() override { return true; }
  bool InstallFrameDispatch() override { return true; }
  bool InstallObbSkip(bool counted) override { return counted; }
  bool InstallProcessEnd() override { return true; }
  bool InstallCrashFirst() override { return true; }
  bool InstallPermissionWait() override { return true; }
  bool InstallVkExtWindow() override { return true; }
  bool InstallOsBuildGate() override { return true; }
  bool InstallResourceRequestLog(bool counted, bool) override { return counted; }
  bool InstallTextureQuality(const nevr_quest::TextureChoice&, bool counted) override { return counted; }
  bool InstallRenderConfig(const nevr_quest::TextureChoice&) override { return true; }
  bool InstallCreditsText() override { return true; }
  bool InstallMenuCards() override { return true; }
  bool WizardMetricWanted() override { return false; }  // the invite switch is not on main (ADR 0017 slice S1)
  bool InstallWizardMetric() override { return true; }
  bool ArmClockLevels(bool counted, bool) override { return counted; }
  bool StartTokenAuth() override { return true; }
  bool InstallLoginPrompt(bool counted) override { return counted; }
  bool StartBridge() override { return true; }
  bool InstallRedirect() override { return true; }
  bool InstallSocial() override { return true; }
  bool InstallDlopenHook(bool, bool, bool) override { return true; }
  bool StartHwDump() override { return true; }
  bool InstallPnsProvider() override { return true; }
  bool PnsProviderAllowed() override { return true; }
  void Note(const char*, const char*, const char*) override {}
};

void TestCounterBudget() {
  sentinel::StopReporter();  // forget anything registered earlier in this process
  RealCounterSteps steps;
  steps.config.effective.redirect = steps.config.effective.bridge = true;
  steps.config.effective.login = steps.config.effective.social = true;
  // The texture counters are registered at every level now (the settings menu can raise `stock`); the budget is the same.
  steps.config.textureQuality = {nevr_quest::texture_quality::Level::kHigh, nevr_quest::Source::kFile};
  const ConstructorReport r = RunConstructorSequence(steps);
  for (StepId id : {StepId::kRegisterClockCounters, StepId::kRegisterRedirectCounters, StepId::kRegisterDlopenCounters,
                    StepId::kRegisterLoginCounters, StepId::kRegisterSocialCounters,
                    StepId::kRegisterLoginPromptCounters, StepId::kRegisterObbSkipCounters,
                    StepId::kRegisterResourceRequestCounters, StepId::kRegisterTextureQualityCounters,
                    StepId::kRegisterClockLevelsCounters}) {
    QCHECK(r.at(id).state == StepState::kOk);
  }
  // No hook was left out for want of a counter slot.
  for (StepId id : {StepId::kInstallRedirect, StepId::kInstallSocial, StepId::kInstallLoginPrompt,
                    StepId::kInstallDlopenHook, StepId::kInstallObbSkip, StepId::kInstallResourceRequestLog,
                    StepId::kInstallTextureQuality, StepId::kArmClockLevels}) {
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

// The social shim records whether the facade's Social() hook went in, which ovr_GetLoggedInUserID's handler reads
// (#944). On the host the hook cannot install (libr15 is not loaded), so a flag left set is cleared.
void TestSocialShimRecordsTheFacadeInstall() {
  nevr_quest_login::local::SetSocialFacadeInstalled(true);
  const char* detail = nullptr;
  QCHECK(!InstallSocialHook(&detail, false, false, false));
  QCHECK(!nevr_quest_login::local::SocialFacadeInstalled());
}

int main() {
  TestNullHandleRunsNothingAndReturnsNull();
  TestNonNullHandleIsReturnedAndRunsThePostLoadActions();
  TestOnlyTheWantedActionsRun();
  TestHandlerRedirectsARegisteredScriptLibrary();
  TestPinnedDlopenTarget();
  TestInstallWithoutTheModuleFailsCleanly();
  TestCounterBudget();
  TestSocialShimRecordsTheFacadeInstall();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "integration_hooks_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("integration_hooks_test: all checks passed\n");
  return 0;
}
