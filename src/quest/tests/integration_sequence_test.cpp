// Host test for the sentinel constructor's sequence and the integration's pure pieces
// (src/quest/integration): the order of the steps, the gating by feature, the counter registration
// before the single reporter start, the isolation of one failing piece from the others, the post-load
// policy, the identity source, the social switch and the frame tap. Fakes stand in for every library.

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "quest/integration/bridge_uri.h"
#include "quest/integration/ctor_sequence.h"
#include "quest/integration/drop_report.h"
#include "quest/integration/identity_source.h"
#include "quest/integration/post_load.h"
#include "quest/integration/self_check_wiring.h"
#include "quest/integration/stage_log.h"
#include "quest/net/frame_tap.h"
#include "quest/tests/test_check.h"
#include "runtime/compat/evr_codec.h"
#include "runtime/compat/self_check.h"

// Counts every allocation through the global operator new in this test binary, for the no-allocation check
// on IdentitySource::Ready (it runs on the Oculus message pump).
std::atomic<long> g_allocations{0};
void* operator new(std::size_t n) {
  g_allocations.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n == 0 ? 1 : n)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

using namespace nevr_quest;
using namespace nevr_quest::integration;
using quest_net::FrameTap;
using quest_net::FrameTapSinks;

namespace {

// ---- the sequence ---------------------------------------------------------------------------------

struct FakeSteps final : Steps {
  nevr_quest::ResolvedConfig config;
  std::vector<std::string> calls;
  // Per step: true = report failure, "throw" = throw.
  std::vector<std::string> failing;
  std::vector<std::string> throwing;
  bool loginArg = false, mmArg = false;
  bool dlopenArgsSeen = false;

  bool Fails(const char* name) const {
    for (const std::string& f : failing) if (f == name) return true;
    return false;
  }
  bool Throws(const char* name) const {
    for (const std::string& f : throwing) if (f == name) return true;
    return false;
  }
  bool Step(const char* name) {
    calls.emplace_back(name);
    if (Throws(name)) throw std::runtime_error("injected");
    return !Fails(name);
  }

  void ArmCrashReporter() override { Step("arm"); }
  const nevr_quest::ResolvedConfig& ResolveConfig() override {
    if (!Step("config")) throw std::runtime_error("config");
    return config;
  }
  bool RegisterClockCounters() override { return Step("reg_clock"); }
  bool RegisterRedirectCounters() override { return Step("reg_redirect"); }
  bool RegisterDlopenCounters() override { return Step("reg_dlopen"); }
  bool RegisterLoginCounters() override { return Step("reg_login"); }
  bool RegisterSocialCounters() override { return Step("reg_social"); }
  bool RegisterLoginPromptCounters() override { return Step("reg_prompt"); }
  bool RegisterObbSkipCounters() override { return Step("reg_obb"); }
  bool StartReporter() override { return Step("reporter"); }
  bool InstallClockHook() override { return Step("clock"); }
  bool InstallObbSkip(bool counted) override { return Step(counted ? "obb" : "obb_uncounted"); }
  bool StartTokenAuth() override { return Step("token"); }
  bool InstallLoginPrompt(bool counted) override { return Step(counted ? "prompt" : "prompt_uncounted"); }
  bool StartBridge() override { return Step("bridge"); }
  bool InstallRedirect() override { return Step("redirect"); }
  bool InstallSocial() override { return Step("social"); }
  bool InstallDlopenHook(bool login, bool mm) override {
    loginArg = login;
    mmArg = mm;
    dlopenArgsSeen = true;
    return Step("dlopen");
  }
  bool StartHwDump() override { return Step("hwdump"); }
  void Note(const char*, const char*, const char*) override {}

  static FakeSteps With(bool redirect, bool bridge, bool login, bool socialOn) {
    FakeSteps s;
    s.config.effective.redirect = redirect;
    s.config.effective.bridge = bridge;
    s.config.effective.login = login;
    s.config.effective.social = socialOn;
    return s;
  }
  int Index(const char* name) const {
    for (std::size_t i = 0; i < calls.size(); ++i) if (calls[i] == name) return static_cast<int>(i);
    return -1;
  }
  bool Ran(const char* name) const { return Index(name) >= 0; }
};

void TestEverythingOffInstallsOnlyTheProofHook() {
  FakeSteps s = FakeSteps::With(false, false, false, false);
  const ConstructorReport r = RunConstructorSequence(s);
  const std::vector<std::string> want = {"arm", "config", "reg_clock", "reporter", "clock"};
  QCHECK(s.calls == want);
  QCHECK(r.at(StepId::kStartTokenAuth).state == StepState::kSkipped);
  QCHECK(r.at(StepId::kInstallRedirect).state == StepState::kSkipped);
  QCHECK(r.at(StepId::kInstallDlopenHook).state == StepState::kSkipped);
  // The hardware dump (#335) is off by default and its step says so.
  QCHECK(r.at(StepId::kInstallHwDump).state == StepState::kSkipped);
  QCHECK(std::strcmp(r.at(StepId::kInstallHwDump).reason, "hwdump_off") == 0);
  // So is the OBB-mount skip (#319).
  QCHECK(r.at(StepId::kRegisterObbSkipCounters).state == StepState::kSkipped);
  QCHECK(r.at(StepId::kInstallObbSkip).state == StepState::kSkipped);
  QCHECK(std::strcmp(r.at(StepId::kInstallObbSkip).reason, "obb_skip_off") == 0);
}

void TestFullStackOrder() {
  FakeSteps s = FakeSteps::With(true, true, true, true);
  s.config.effective.hwdump = true;
  s.config.effective.obbSkip = true;
  const ConstructorReport r = RunConstructorSequence(s);
  const std::vector<std::string> want = {"arm",       "config",     "reg_clock",  "reg_redirect", "reg_dlopen",
                                         "reg_login", "reg_social", "reg_prompt", "reg_obb",      "reporter",
                                         "clock",     "obb",        "token",      "prompt",       "bridge",
                                         "redirect",  "social",     "dlopen",     "hwdump"};
  QCHECK(s.calls == want);
  QCHECK(s.loginArg && s.mmArg);
  for (int i = 0; i < static_cast<int>(StepId::kCount); ++i) QCHECK(r.steps[i].state == StepState::kOk);
}

// The hardware dump needs no other feature, and a failing or throwing dump step leaves every other step as
// it was (#335).
void TestHwDumpIsIndependentAndContained() {
  {
    FakeSteps s = FakeSteps::With(false, false, false, false);
    s.config.effective.hwdump = true;
    const ConstructorReport r = RunConstructorSequence(s);
    const std::vector<std::string> want = {"arm", "config", "reg_clock", "reporter", "clock", "hwdump"};
    QCHECK(s.calls == want);
    QCHECK(r.at(StepId::kInstallHwDump).state == StepState::kOk);
  }
  for (const char* mode : {"fail", "throw"}) {
    FakeSteps s = FakeSteps::With(true, true, true, true);
    s.config.effective.hwdump = true;
    s.config.effective.obbSkip = true;
    if (std::strcmp(mode, "fail") == 0) s.failing = {"hwdump"}; else s.throwing = {"hwdump"};
    const ConstructorReport r = RunConstructorSequence(s);
    for (int i = 0; i < static_cast<int>(StepId::kInstallHwDump); ++i) QCHECK(r.steps[i].state == StepState::kOk);
    QCHECK(r.at(StepId::kInstallHwDump).state != StepState::kOk);
    QCHECK(s.Ran("dlopen") && s.Ran("social"));
  }
}

// The OBB-mount skip (#319) needs no other feature, its counters are registered before the single reporter
// start, it installs right after the clock hook, and a refused counter, a failing or a throwing step leaves
// every other step as it was.
void TestObbSkipIsIndependentAndContained() {
  {
    FakeSteps s = FakeSteps::With(false, false, false, false);
    s.config.effective.obbSkip = true;
    const ConstructorReport r = RunConstructorSequence(s);
    const std::vector<std::string> want = {"arm", "config", "reg_clock", "reg_obb", "reporter", "clock", "obb"};
    QCHECK(s.calls == want);
    QCHECK(r.at(StepId::kRegisterObbSkipCounters).state == StepState::kOk);
    QCHECK(r.at(StepId::kInstallObbSkip).state == StepState::kOk);
    QCHECK(!s.Ran("token") && !s.Ran("bridge") && !s.Ran("redirect"));
  }
  {  // counters refused: the hook installs nothing (its own rule is called to log the skip) and says why
    FakeSteps s = FakeSteps::With(true, true, true, true);
    s.config.effective.obbSkip = true;
    s.failing = {"reg_obb"};
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kRegisterObbSkipCounters).state == StepState::kFailed);
    QCHECK(r.at(StepId::kInstallObbSkip).state == StepState::kSkipped);
    QCHECK(std::strcmp(r.at(StepId::kInstallObbSkip).reason, "counters_refused") == 0);
    QCHECK(s.Ran("obb_uncounted") && !s.Ran("obb"));
    QCHECK(s.Ran("redirect") && s.Ran("social") && s.Ran("dlopen"));
  }
  for (const char* mode : {"fail", "throw"}) {
    FakeSteps s = FakeSteps::With(true, true, true, true);
    s.config.effective.obbSkip = true;
    if (std::strcmp(mode, "fail") == 0) s.failing = {"obb"}; else s.throwing = {"obb"};
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kInstallObbSkip).state != StepState::kOk);
    QCHECK(s.Ran("token") && s.Ran("bridge") && s.Ran("redirect") && s.Ran("social") && s.Ran("dlopen"));
  }
  {  // before the reporter is up nothing installs, and the skip is in before token auth starts
    FakeSteps s = FakeSteps::With(true, true, true, true);
    s.config.effective.obbSkip = true;
    RunConstructorSequence(s);
    QCHECK(s.Index("reg_obb") < s.Index("reporter"));
    QCHECK(s.Index("obb") > s.Index("reporter") && s.Index("obb") < s.Index("token"));
  }
}

// Every counter is registered before the single StartReporter, and StartReporter runs once.
void TestCountersBeforeTheSingleReporterStart() {
  FakeSteps s = FakeSteps::With(true, true, true, true);
  RunConstructorSequence(s);
  int starts = 0;
  for (const std::string& c : s.calls) if (c == "reporter") ++starts;
  QCHECK(starts == 1);
  const int reporter = s.Index("reporter");
  for (const char* reg : {"reg_clock", "reg_redirect", "reg_dlopen", "reg_login", "reg_social", "reg_prompt"}) {
    QCHECK(s.Index(reg) >= 0 && s.Index(reg) < reporter);
  }
  // No hook is installed before the reporter is up.
  for (const char* hook : {"clock", "redirect", "social", "dlopen", "bridge", "token", "prompt"}) {
    QCHECK(s.Index(hook) > reporter);
  }
}

void TestCrashReporterAndConfigComeFirst() {
  FakeSteps s = FakeSteps::With(true, true, true, true);
  RunConstructorSequence(s);
  QCHECK(s.Index("arm") == 0);
  QCHECK(s.Index("config") == 1);
}

void TestFeatureGating() {
  {  // redirect only: no token auth, no bridge, no social, the dlopen hook for the matchmaking install only
    FakeSteps s = FakeSteps::With(true, false, false, false);
    RunConstructorSequence(s);
    QCHECK(s.Ran("redirect") && !s.Ran("token") && !s.Ran("bridge") && !s.Ran("social"));
    QCHECK(s.Ran("dlopen") && !s.loginArg && s.mmArg);
    QCHECK(s.Ran("reg_redirect") && s.Ran("reg_dlopen") && !s.Ran("reg_social"));
    QCHECK(!s.Ran("reg_prompt") && !s.Ran("prompt"));  // no token auth: nothing would publish a prompt
  }
  {  // redirect + bridge, no login: token auth runs (the remote needs a JWT), no login install
    FakeSteps s = FakeSteps::With(true, true, false, false);
    RunConstructorSequence(s);
    QCHECK(s.Ran("token") && s.Ran("bridge") && s.Ran("redirect"));
    QCHECK(s.Ran("dlopen") && !s.loginArg && s.mmArg);
    QCHECK(s.Ran("reg_prompt") && s.Ran("prompt"));  // token auth runs, so its prompt can be shown
  }
  {  // login on: the sign-in prompt hook is registered and installed after token auth (#239)
    FakeSteps s = FakeSteps::With(true, true, true, false);
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(s.Ran("reg_prompt") && s.Ran("prompt"));
    QCHECK(s.Index("reg_prompt") < s.Index("reporter") && s.Index("token") < s.Index("prompt"));
    QCHECK(r.at(StepId::kInstallLoginPrompt).state == StepState::kOk);
  }
  {  // social wanted but the config says login is off: the facade is not installed unless asked
    FakeSteps s = FakeSteps::With(true, true, true, false);
    RunConstructorSequence(s);
    QCHECK(!s.Ran("social") && !s.Ran("reg_social"));
  }
}

// Each piece failing leaves the others running (and turns off only what depends on it).
void TestClockHookFailureLeavesTheRest() {
  FakeSteps s = FakeSteps::With(true, true, true, true);
  s.failing = {"clock"};
  const ConstructorReport r = RunConstructorSequence(s);
  QCHECK(r.at(StepId::kInstallClockHook).state == StepState::kFailed);
  for (const char* ran : {"token", "bridge", "redirect", "social", "dlopen"}) QCHECK(s.Ran(ran));
}

void TestTokenAuthFailureTurnsOffTheBridgeAndWhatNeedsIt() {
  FakeSteps s = FakeSteps::With(true, true, true, true);
  s.failing = {"token"};
  const ConstructorReport r = RunConstructorSequence(s);
  QCHECK(!s.Ran("bridge") && !s.Ran("redirect") && !s.Ran("social"));
  QCHECK(!s.Ran("dlopen"));  // login needs the bridge; the matchmaking install needs the redirect
  QCHECK(!s.Ran("prompt"));  // nothing publishes a prompt without token auth
  QCHECK(s.Ran("clock") && s.Ran("reporter"));
  QCHECK(r.at(StepId::kStartBridge).state == StepState::kSkipped);
  QCHECK(std::strcmp(r.at(StepId::kStartBridge).reason, "token_auth_unavailable") == 0);
  QCHECK(r.at(StepId::kInstallLoginPrompt).state == StepState::kSkipped);
  QCHECK(std::strcmp(r.at(StepId::kInstallLoginPrompt).reason, "token_auth_unavailable") == 0);
}

void TestBridgeFailureLeavesTheGameOnItsOwnHosts() {
  FakeSteps s = FakeSteps::With(true, true, true, true);
  s.failing = {"bridge"};
  const ConstructorReport r = RunConstructorSequence(s);
  QCHECK(!s.Ran("redirect"));  // a redirect to a loopback port nobody listens on is worse than no redirect
  QCHECK(!s.Ran("social") && !s.Ran("dlopen"));
  QCHECK(r.at(StepId::kInstallRedirect).state == StepState::kSkipped);
  QCHECK(std::strcmp(r.at(StepId::kInstallRedirect).reason, "bridge_unavailable") == 0);
  QCHECK(s.Ran("token") && s.Ran("clock"));
}

void TestRedirectFailureLeavesLoginAndSocialRunning() {
  FakeSteps s = FakeSteps::With(true, true, true, true);
  s.failing = {"redirect"};
  RunConstructorSequence(s);
  QCHECK(s.Ran("social") && s.Ran("dlopen"));
  QCHECK(s.loginArg);
  QCHECK(!s.mmArg);  // the matchmaking redirect needs the redirect
}

void TestSocialFailureLeavesTheRest() {
  FakeSteps s = FakeSteps::With(true, true, true, true);
  s.failing = {"social"};
  RunConstructorSequence(s);
  QCHECK(s.Ran("dlopen") && s.loginArg && s.mmArg);
}

void TestLoginPromptFailureLeavesTheRest() {
  FakeSteps s = FakeSteps::With(true, true, true, true);
  s.failing = {"prompt"};
  const ConstructorReport r = RunConstructorSequence(s);
  QCHECK(r.at(StepId::kInstallLoginPrompt).state == StepState::kFailed);
  for (const char* ran : {"bridge", "redirect", "social", "dlopen"}) QCHECK(s.Ran(ran));
  QCHECK(s.loginArg && s.mmArg);
}

void TestCounterRefusalDisablesOnlyThatPiece() {
  {
    FakeSteps s = FakeSteps::With(true, true, true, true);
    s.failing = {"reg_prompt"};
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(!s.Ran("prompt"));
    QCHECK(s.Ran("prompt_uncounted"));  // the hook's rule ran with "no counters": nothing installed
    QCHECK(s.Ran("redirect") && s.Ran("social") && s.Ran("dlopen") && s.loginArg && s.mmArg);
    QCHECK(std::strcmp(r.at(StepId::kInstallLoginPrompt).reason, "counters_refused") == 0);
  }
  {
    FakeSteps s = FakeSteps::With(true, true, true, true);
    s.failing = {"reg_redirect"};
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(!s.Ran("redirect"));
    QCHECK(s.Ran("social") && s.Ran("dlopen") && s.loginArg && !s.mmArg);
    QCHECK(s.Ran("reporter"));
    QCHECK(std::strcmp(r.at(StepId::kInstallRedirect).reason, "counters_refused") == 0);
  }
  {
    FakeSteps s = FakeSteps::With(true, true, true, true);
    s.failing = {"reg_dlopen"};
    RunConstructorSequence(s);
    QCHECK(!s.Ran("dlopen"));  // no login hook, no matchmaking redirect
    QCHECK(s.Ran("redirect") && s.Ran("social"));
  }
  {
    FakeSteps s = FakeSteps::With(true, true, true, true);
    s.failing = {"reg_social"};
    RunConstructorSequence(s);
    QCHECK(!s.Ran("social"));
    QCHECK(s.Ran("redirect") && s.Ran("dlopen"));
  }
}

void TestEveryStepThrowingIsContained() {
  for (const char* name : {"arm", "config", "reg_clock", "reg_redirect", "reg_dlopen", "reg_social", "reg_prompt",
                           "reporter", "clock", "token", "prompt", "bridge", "redirect", "social", "dlopen"}) {
    FakeSteps s = FakeSteps::With(true, true, true, true);
    s.throwing = {name};
    const ConstructorReport r = RunConstructorSequence(s);  // must return, not terminate
    QCHECK(r.attempted > 0);
    // The crash reporter and the reporter are not allowed to be skipped because something else threw.
    if (std::strcmp(name, "arm") != 0) QCHECK(s.Ran("arm"));
    if (std::strcmp(name, "config") != 0 && std::strcmp(name, "arm") != 0) QCHECK(s.Ran("reporter"));
  }
}

void TestConfigFailureLeavesAllFeaturesOff() {
  FakeSteps s = FakeSteps::With(true, true, true, true);
  s.throwing = {"config"};
  const ConstructorReport r = RunConstructorSequence(s);
  QCHECK(r.at(StepId::kResolveConfig).state == StepState::kThrew);
  QCHECK(!s.Ran("redirect") && !s.Ran("bridge") && !s.Ran("token") && !s.Ran("social") && !s.Ran("dlopen"));
  QCHECK(!s.Ran("prompt"));
  QCHECK(s.Ran("clock"));  // the proof hook does not depend on configuration
}

// ---- post-load policy ------------------------------------------------------------------------------

int g_loginRuns = 0, g_mmRuns = 0;
Settle g_loginSettle = Settle::kRetryLater, g_mmSettle = Settle::kRetryLater;

ActionResult FakeLogin() noexcept {
  ++g_loginRuns;
  return {g_loginSettle, "fake_login"};
}
ActionResult FakeMatchmaking() noexcept {
  ++g_mmRuns;
  return {g_mmSettle, "fake_mm"};
}
ActionResult ThrowingLogin() noexcept {
  ++g_loginRuns;
  return {Settle::kGiveUp, "exception"};
}

void ResetPostLoad(ActionFn login, ActionFn mm, Settle loginSettle, Settle mmSettle) {
  g_loginRuns = g_mmRuns = 0;
  g_loginSettle = loginSettle;
  g_mmSettle = mmSettle;
  PostLoadActions a;
  a.login = login;
  a.matchmaking = mm;
  SetPostLoadActions(a);
}

void TestPostLoadIgnoresANullHandle() {
  ResetPostLoad(&FakeLogin, &FakeMatchmaking, Settle::kDone, Settle::kDone);
  AfterDlopen("libpnsovr.so", nullptr);
  QCHECK(g_loginRuns == 0 && g_mmRuns == 0);
  QCHECK(PostLoadStatsView().calls == 0);
  QCHECK(PostLoadPending());
}

void TestPostLoadRetriesUntilSettledThenStops() {
  int dummy = 0;
  ResetPostLoad(&FakeLogin, &FakeMatchmaking, Settle::kRetryLater, Settle::kRetryLater);
  AfterDlopen("/data/app/x/lib/arm64/libc++_shared.so", &dummy);
  AfterDlopen("libGLESv3.so", &dummy);
  QCHECK(g_loginRuns == 2 && g_mmRuns == 2);  // not settled: tried after every non-null dlopen
  QCHECK(PostLoadPending());

  g_loginSettle = Settle::kDone;
  AfterDlopen("libpnsovr.so", &dummy);
  QCHECK(g_loginRuns == 3 && g_mmRuns == 3);
  AfterDlopen("libpnsrad.so", &dummy);
  QCHECK(g_loginRuns == 3);  // done: never again
  QCHECK(g_mmRuns == 4);     // matchmaking still waits for its module

  g_mmSettle = Settle::kDone;
  AfterDlopen("/x/libpnsradmatchmaking.so", &dummy);
  QCHECK(g_mmRuns == 5);
  QCHECK(!PostLoadPending());
  AfterDlopen("libfoo.so", &dummy);
  QCHECK(g_loginRuns == 3 && g_mmRuns == 5);  // nothing pending: not even a lock
  const PostLoadStats stats = PostLoadStatsView();
  QCHECK(stats.loginSettled == 1 && stats.matchmakingSettled == 1);
}

// Self-check "matchmaking_reload_redirect" (#451): the number of distinct libpnsradmatchmaking images is what the
// once-installed redirect is compared with.
void TestPostLoadCountsEachDistinctMatchmakingImage() {
  int imageA = 0;
  int imageB = 0;
  ResetPostLoad(nullptr, nullptr, Settle::kDone, Settle::kDone);
  QCHECK(MatchmakingImages() == 0);
  AfterDlopen("libpnsovr.so", &imageA);  // another library: not counted
  QCHECK(MatchmakingImages() == 0);
  AfterDlopen("/x/libpnsradmatchmaking.so", &imageA);
  QCHECK(MatchmakingImages() == 1);
  AfterDlopen("/x/libpnsradmatchmaking.so", &imageA);  // the same handle again: a refcount, not a new image
  QCHECK(MatchmakingImages() == 1);
  AfterDlopen("/x/libpnsradmatchmaking.so", &imageB);  // closed and mapped again elsewhere
  QCHECK(MatchmakingImages() == 2);
  AfterDlopen("/x/libpnsradmatchmaking.so", nullptr);  // a failed dlopen is not an image
  QCHECK(MatchmakingImages() == 2);
  ResetPostLoad(nullptr, nullptr, Settle::kDone, Settle::kDone);
  QCHECK(MatchmakingImages() == 0);
}

// The matchmaking check's probe (self_check_wiring.cpp): an image without an install is not a failure while the
// matchmaking action is unsettled, and is one once it settled without covering that image.
void TestMatchmakingReloadProbeWaitsForTheActionAndThenSpeaksOnce() {
  int imageA = 0;
  int imageB = 0;
  nevr_quest::integration::ResetMatchmakingReloadCheckForTest();
  nevr_self_check::Observation seen;
  ResetPostLoad(&FakeLogin, &FakeMatchmaking, Settle::kRetryLater, Settle::kRetryLater);
  QCHECK(!nevr_quest::integration::MatchmakingReloadProbe(&seen));  // no image yet

  AfterDlopen("/x/libpnsradmatchmaking.so", &imageA);  // mapped, the action is retrying: wait
  QCHECK(!nevr_quest::integration::MatchmakingReloadProbe(&seen));

  g_mmSettle = Settle::kDone;
  AfterDlopen("/x/libpnsradmatchmaking.so", &imageA);  // the action settles on this call
  nevr_quest::integration::NoteMatchmakingRedirectInstalled();
  QCHECK(nevr_quest::integration::MatchmakingReloadProbe(&seen));
  QCHECK(seen.observed == "images=1 installs=1" && seen.pass);
  QCHECK(!nevr_quest::integration::MatchmakingReloadProbe(&seen));  // said once

  AfterDlopen("/x/libpnsradmatchmaking.so", &imageB);  // another mapping: the once-installed hook misses it
  QCHECK(nevr_quest::integration::MatchmakingReloadProbe(&seen));
  QCHECK(seen.observed == "images=2 installs=1" && !seen.pass);
  QCHECK(!nevr_quest::integration::MatchmakingReloadProbe(&seen));
}

// The action gave up with the module mapped: the one image has no install, and that is a failure.
void TestMatchmakingReloadProbeFailsWhenTheActionGaveUp() {
  int image = 0;
  nevr_quest::integration::ResetMatchmakingReloadCheckForTest();
  nevr_self_check::Observation seen;
  ResetPostLoad(&FakeLogin, &FakeMatchmaking, Settle::kDone, Settle::kGiveUp);
  AfterDlopen("/x/libpnsradmatchmaking.so", &image);
  QCHECK(nevr_quest::integration::MatchmakingReloadProbe(&seen));
  QCHECK(seen.observed == "images=1 installs=0" && !seen.pass);
}

// The self-check wiring: on in every build, the user the service names, the sender; no debug query anywhere.
void TestApplySelfCheckWiresTheDebugQueryTheUserAndTheSender() {
  static std::vector<std::string> sent;
  sent.clear();
  nevr_self_check::ResetForTest();
  quest_net::FrameTapSinks sinks;
  int previousCalls = 0;
  sinks.onLoginUser = [&](std::uint64_t, std::uint64_t) { ++previousCalls; };
  nevr_quest::integration::SelfCheckHooks hooks;
  hooks.sender = [](const std::string& frame) { sent.push_back(frame); return true; };
  hooks.log = [](const nevr_self_check::LogRecord&) {};
  hooks.build = "5.0.0";
  QCHECK(!nevr_self_check::Enabled());  // off until the bridge is configured
  nevr_quest::integration::ApplySelfCheck(&sinks, hooks);
  QCHECK(nevr_self_check::Enabled());
  QCHECK(static_cast<bool>(sinks.onLoginUser));

  const nevr_self_check::CheckId id = nevr_self_check::Register({"wiring", "ok", nullptr});
  nevr_self_check::Report(id, "ok", true);
  nevr_self_check::Flush();
  QCHECK(sent.empty());  // not logged in yet

  quest_net::FrameTap tap(sinks);
  tap.ServerToGame(nevr_evr_codec::BuildLoginSuccess(5, 0x2222ULL));
  QCHECK(previousCalls == 1);  // the consumer that was there is still called
  nevr_self_check::Flush();
  QCHECK(sent.size() == 1);
  if (sent.size() == 1) {
    nevr_evr_codec::Message message;
    QCHECK(nevr_evr_codec::ReadMessage(sent[0], 0, &message) == nevr_evr_codec::ReadStatus::Ok);
    QCHECK(message.symbol == nevr_evr_codec::kSymRemoteLogSet);
    QCHECK(nevr_evr_codec::ReadLE64(message.payload) == 5 && nevr_evr_codec::ReadLE64(message.payload + 8) == 0x2222ULL);
  }
  nevr_self_check::ResetForTest();
}

void TestPostLoadGiveUpEndsOnlyThatAction() {
  int dummy = 0;
  ResetPostLoad(&ThrowingLogin, &FakeMatchmaking, Settle::kGiveUp, Settle::kRetryLater);
  AfterDlopen("libpnsovr.so", &dummy);
  AfterDlopen("libother.so", &dummy);
  QCHECK(g_loginRuns == 1);  // gave up: not retried
  QCHECK(g_mmRuns == 2);     // the other action is unaffected
}

void TestPostLoadWithNoActionsIsInert() {
  int dummy = 0;
  ResetPostLoad(nullptr, nullptr, Settle::kDone, Settle::kDone);
  QCHECK(!PostLoadPending());
  AfterDlopen("libpnsovr.so", &dummy);
  QCHECK(PostLoadStatsView().attempts == 0);
}

void TestPostLoadAcceptsANullName() {
  int dummy = 0;
  ResetPostLoad(&FakeLogin, nullptr, Settle::kDone, Settle::kDone);
  AfterDlopen(nullptr, &dummy);  // dlopen(NULL) is legal
  QCHECK(g_loginRuns == 1);
}

// ---- identity source -------------------------------------------------------------------------------

nevr::quest_auth::Snapshot Snap(nevr::quest_auth::Readiness r, const char* token, std::uint64_t id, const char* name) {
  nevr::quest_auth::Snapshot s;
  s.readiness = r;
  s.access_token = token;
  s.discord_id = id;
  s.username = name;
  return s;
}

nevr_quest_login::IdentityStatus Fetch(nevr::quest_auth::Snapshot snap, nevr_quest_login::Identity* out) {
  TokenIdentitySource source([snap] { return snap; });
  return source.Fetch(*out);
}

void TestIdentitySourceAnswers() {
  using nevr::quest_auth::Readiness;
  nevr_quest_login::Identity id;
  QCHECK(Fetch(Snap(Readiness::Ready, "tok", 4242, "player"), &id) == nevr_quest_login::IdentityStatus::Ok);
  QCHECK(id.account_id == 4242 && id.access_token == "tok" && id.display_name == "player");
  QCHECK(id.social_level == 0);  // no facade installed: the login declares no social level
  {
    TokenIdentitySource withSocial([] { return Snap(Readiness::Ready, "tok", 4242, "player"); }, [] { return 1; });
    nevr_quest_login::Identity declared;
    QCHECK(withSocial.Fetch(declared) == nevr_quest_login::IdentityStatus::Ok && declared.social_level == 1);
  }

  for (Readiness r : {Readiness::Starting, Readiness::Refreshing, Readiness::AwaitingUser}) {
    nevr_quest_login::Identity none;
    QCHECK(Fetch(Snap(r, "tok", 4242, "p"), &none) == nevr_quest_login::IdentityStatus::NotReady);
    QCHECK(none.access_token.empty() && none.account_id == 0);  // nothing leaks out of a refusal
  }
  for (Readiness r : {Readiness::Expired, Readiness::Failed, Readiness::Stopped}) {
    nevr_quest_login::Identity none;
    QCHECK(Fetch(Snap(r, "tok", 4242, "p"), &none) == nevr_quest_login::IdentityStatus::NoToken);
    QCHECK(none.access_token.empty());
  }
  nevr_quest_login::Identity none;
  QCHECK(Fetch(Snap(Readiness::Ready, "", 4242, "p"), &none) == nevr_quest_login::IdentityStatus::NoToken);
  QCHECK(Fetch(Snap(Readiness::Ready, "tok", 0, "p"), &none) == nevr_quest_login::IdentityStatus::NoAccount);
  TokenIdentitySource empty(nullptr);
  QCHECK(empty.Fetch(none) == nevr_quest_login::IdentityStatus::NotReady);
}

// #239: the router holds the login connection exactly while Fetch would say NotReady, opens it when Fetch
// says Ok, and refuses it in every state that needs a new sign-in. Failure caught: a gate that disagrees with
// the login rewrite (a held connection nobody will ever release, or a login let through with no token).
void TestLoginGateFollowsTheIdentityAnswer() {
  using nevr::quest_auth::Readiness;
  using nevr_session_router::LoginGate;
  // A token is still to come: starting, refreshing, waiting for the player, expired (being replaced).
  for (Readiness r : {Readiness::Starting, Readiness::Refreshing, Readiness::AwaitingUser, Readiness::Expired}) {
    QCHECK(TokenIdentitySource::GateFor(Snap(r, "tok", 4242, "p")) == LoginGate::Awaiting);
    QCHECK(TokenIdentitySource::GateFor(Snap(r, "", 0, "p")) == LoginGate::Awaiting);
  }
  QCHECK(TokenIdentitySource::GateFor(Snap(Readiness::Ready, "tok", 4242, "p")) == LoginGate::Ready);
  // Failed: held while the session retries (a recoverable failure), refused when it is final.
  nevr::quest_auth::Snapshot failed = Snap(Readiness::Failed, "", 0, "p");
  failed.will_retry = true;
  QCHECK(TokenIdentitySource::GateFor(failed) == LoginGate::Awaiting);
  failed.will_retry = false;
  QCHECK(TokenIdentitySource::GateFor(failed) == LoginGate::Refused);
  QCHECK(TokenIdentitySource::GateFor(Snap(Readiness::Stopped, "tok", 4242, "p")) == LoginGate::Refused);
  // Ready with the token run out: a refresh is under way. Ready with no account: none will come.
  QCHECK(TokenIdentitySource::GateFor(Snap(Readiness::Ready, "", 4242, "p")) == LoginGate::Awaiting);
  QCHECK(TokenIdentitySource::GateFor(Snap(Readiness::Ready, "tok", 0, "p")) == LoginGate::Refused);
  static_assert(noexcept(TokenIdentitySource::GateFor(std::declval<const nevr::quest_auth::Snapshot&>())),
                "GateFor must be noexcept");
}

// #239 review H2: the page-enable hook and the login prerequisites read one word. The source publishes it as it
// observes; a poisoned attempt (its logging-in page was skipped) fails the prerequisites whatever the state is,
// until the poison is cleared; a new source starts not ready.
void TestIdentitySourcePublishesTheSharedAttemptGate() {
  using nevr::quest_auth::Readiness;
  namespace gate = nevr_quest_login::attempt_gate;
  TokenIdentitySource source(nullptr);
  QCHECK(!source.Ready() && !gate::IsReady());
  source.Observe(Snap(Readiness::Ready, "tok", 4242, "p"));
  QCHECK(source.Ready() && gate::IsReady() && gate::LoginMayProceed());
  gate::Poison();  // an attempt's logging-in page was skipped
  QCHECK(gate::IsReady());  // the readiness itself is unchanged: the page-enable skip has stopped
  QCHECK(!source.Ready());  // but that attempt fails its prerequisites, through the base-class call too
  const nevr_quest_login::IdentitySource& base = source;
  QCHECK(!base.Ready());
  source.Observe(Snap(Readiness::Ready, "tok", 4242, "p"));  // a later observation does not lift it
  QCHECK(!source.Ready());
  gate::ClearPoison();  // the attempt ended
  QCHECK(source.Ready());
  source.Observe(Snap(Readiness::AwaitingUser, "", 0, "p"));
  QCHECK(!source.Ready() && !gate::IsReady());  // one flip, both readers
  TokenIdentitySource next(nullptr);            // a new source starts clean
  QCHECK(!next.Ready());
}

// Dropped Unrequires are reported once per change, with the increase since the last report.
void TestDropReportsOnlyChanges() {
  std::uint64_t last = 0, delta = 0;
  QCHECK(!DropsChanged(0, &last, &delta));
  QCHECK(DropsChanged(3, &last, &delta) && last == 3 && delta == 3);
  QCHECK(!DropsChanged(3, &last, &delta));
  QCHECK(DropsChanged(4, &last, &delta) && last == 4 && delta == 1);
  QCHECK(DropsChanged(1, &last, &delta) && last == 1 && delta == 1);  // a counter that went back reports itself
}

// #240 fail-closed: the login prerequisites stand in for an Oculus answer only while Ready() is true, and
// IdentitySource's default Ready() is false. The production source's Ready() is one load of a flag that
// Observe (token-auth state changes) and Fetch keep equal to (Classify(state) == Ok).
void TestIdentitySourceReadyFollowsObservedState() {
  using nevr::quest_auth::Readiness;
  const Readiness kAll[] = {Readiness::Starting, Readiness::Refreshing, Readiness::AwaitingUser, Readiness::Ready,
                            Readiness::Expired,  Readiness::Failed,     Readiness::Stopped};
  int readyCount = 0;
  for (Readiness r : kAll) {
    for (const char* token : {"", "tok"}) {
      for (std::uint64_t account : {std::uint64_t{0}, std::uint64_t{4242}}) {
        const nevr::quest_auth::Snapshot snap = Snap(r, token, account, "p");
        // Fetch's answer from a separate source, so the flag below comes from Observe alone.
        TokenIdentitySource fetcher([snap] { return snap; });
        nevr_quest_login::Identity id;
        const bool fetchOk = fetcher.Fetch(id) == nevr_quest_login::IdentityStatus::Ok;
        for (bool startReady : {false, true}) {  // from either earlier flag value
          TokenIdentitySource source([snap] { return snap; });
          source.Observe(startReady ? Snap(Readiness::Ready, "tok", 4242, "p") : Snap(Readiness::Starting, "", 0, "p"));
          QCHECK(source.Ready() == startReady);
          source.Observe(snap);
          const nevr_quest_login::IdentitySource& asBase = source;  // the prerequisites call it through the base
          QCHECK(asBase.Ready() == fetchOk);
          if (!startReady) readyCount += asBase.Ready() ? 1 : 0;
        }
      }
    }
  }
  QCHECK(readyCount == 1);  // only Ready + token + account
  // Every way out of Ready clears the flag.
  for (Readiness r : {Readiness::Starting, Readiness::Refreshing, Readiness::AwaitingUser, Readiness::Expired,
                      Readiness::Failed, Readiness::Stopped}) {
    TokenIdentitySource source(nullptr);
    source.Observe(Snap(Readiness::Ready, "tok", 4242, "p"));
    QCHECK(source.Ready());
    source.Observe(Snap(r, "tok", 4242, "p"));
    QCHECK(!source.Ready());
  }
  {  // an access token that ran out (empty) or a missing account clears it too
    TokenIdentitySource source(nullptr);
    source.Observe(Snap(Readiness::Ready, "tok", 4242, "p"));
    source.Observe(Snap(Readiness::Ready, "", 4242, "p"));
    QCHECK(!source.Ready());
    source.Observe(Snap(Readiness::Ready, "tok", 4242, "p"));
    source.Observe(Snap(Readiness::Ready, "tok", 0, "p"));
    QCHECK(!source.Ready());
  }
  {  // Fetch updates the flag with the state it saw; a source without a snapshot is never ready
    nevr::quest_auth::Snapshot state = Snap(Readiness::Ready, "tok", 4242, "p");
    TokenIdentitySource source([&state] { return state; });
    QCHECK(!source.Ready());  // nothing observed yet
    nevr_quest_login::Identity id;
    QCHECK(source.Fetch(id) == nevr_quest_login::IdentityStatus::Ok && source.Ready());
    state = Snap(Readiness::Expired, "tok", 4242, "p");
    QCHECK(source.Fetch(id) == nevr_quest_login::IdentityStatus::NoToken && !source.Ready());
    TokenIdentitySource none(nullptr);
    QCHECK(!none.Ready() && none.Fetch(id) == nevr_quest_login::IdentityStatus::NotReady && !none.Ready());
  }
  static_assert(noexcept(std::declval<const TokenIdentitySource&>().Ready()), "Ready must be noexcept");
  static_assert(noexcept(std::declval<TokenIdentitySource&>().Observe(std::declval<const nevr::quest_auth::Snapshot&>())),
                "Observe must be noexcept");
}

// Ready() runs on the Oculus message pump: it must not allocate. The snapshot here has strings longer than
// any small-string buffer, so a Ready() that took or copied a snapshot would allocate.
void TestIdentitySourceReadyDoesNotAllocate() {
  const std::string longToken(96, 't');
  const std::string longName(96, 'n');
  const nevr::quest_auth::Snapshot snap = Snap(nevr::quest_auth::Readiness::Ready, longToken.c_str(), 4242, longName.c_str());
  TokenIdentitySource source([snap] { return snap; });
  source.Observe(snap);
  const long before = g_allocations.load();
  bool all = true;
  for (int i = 0; i < 1000; ++i) all = source.Ready() && all;
  QCHECK(all);
  QCHECK(g_allocations.load() == before);
  // Observe does not allocate either (it is called with a snapshot the caller already holds).
  const long beforeObserve = g_allocations.load();
  for (int i = 0; i < 1000; ++i) source.Observe(snap);
  QCHECK(g_allocations.load() == beforeObserve);
  // The counter itself works: copying the snapshot allocates.
  const long beforeCopy = g_allocations.load();
  const nevr::quest_auth::Snapshot copy = snap;
  QCHECK(g_allocations.load() > beforeCopy && copy.access_token == longToken);
}

// ---- frame tap -------------------------------------------------------------------------------------

std::string LoginSuccessFrame(std::uint64_t account) { return nevr_evr_codec::BuildLoginSuccess(nevr_evr_codec::kBridgeLoginPlatform, account); }

void TestFrameTapSignalsLoginSuccessOnlyFromTheServer() {
  std::vector<bool> directions;
  std::vector<std::uint64_t> accounts;
  FrameTapSinks sinks;
  sinks.observe = [&](bool s2g, const std::uint8_t*, std::size_t) { directions.push_back(s2g); };
  sinks.onLoginSuccess = [&](std::uint64_t a) { accounts.push_back(a); };
  FrameTap tap(sinks);

  const std::string success = LoginSuccessFrame(0x1122334455667788ULL);
  tap.GameToServer(success);  // a game-to-server frame with that symbol is not a login acceptance
  QCHECK(accounts.empty());
  tap.ServerToGame(success);
  QCHECK(accounts.size() == 1 && accounts[0] == 0x1122334455667788ULL);
  QCHECK(directions.size() == 2 && !directions[0] && directions[1]);

  // A LoginSuccess behind another message in the same transport frame is found.
  accounts.clear();
  tap.ServerToGame(nevr_evr_codec::BuildMessage(0x1234, "abc") + success);
  QCHECK(accounts.size() == 1);

  // A truncated one, and other symbols, are not.
  accounts.clear();
  tap.ServerToGame(success.substr(0, success.size() - 3));
  tap.ServerToGame(nevr_evr_codec::BuildMessage(0x1234, std::string(64, 'x')));
  tap.ServerToGame("garbage");
  QCHECK(accounts.empty());
}

void TestFrameTapNamesThePlatformAndTheAccountOfTheLogin() {
  std::vector<std::pair<std::uint64_t, std::uint64_t>> users;
  FrameTapSinks sinks;
  sinks.onLoginUser = [&](std::uint64_t platform, std::uint64_t account) { users.emplace_back(platform, account); };
  FrameTap tap(sinks);
  const std::string success = nevr_evr_codec::BuildLoginSuccess(5, 0x1122334455667788ULL);
  tap.GameToServer(success);
  QCHECK(users.empty());
  tap.ServerToGame(nevr_evr_codec::BuildMessage(0x1234, "abc") + success);
  QCHECK(users.size() == 1 && users[0].first == 5 && users[0].second == 0x1122334455667788ULL);
  tap.ServerToGame("garbage");
  QCHECK(users.size() == 1);
}

void TestFrameTapContainsAThrowingConsumer() {
  int logins = 0;
  FrameTapSinks sinks;
  sinks.observe = [](bool, const std::uint8_t*, std::size_t) { throw std::runtime_error("consumer"); };
  sinks.onLoginSuccess = [&](std::uint64_t) { ++logins; throw std::runtime_error("consumer"); };
  FrameTap tap(sinks);
  tap.ServerToGame(LoginSuccessFrame(7));  // must return
  QCHECK(logins == 1);                      // the observe failure did not stop the login signal
}

// ---- bridge redirect value ------------------------------------------------------------------------

void TestBareBridgeUriBecomesTheTokenedOne() {
  const std::string uri = "ws://127.0.0.1:41234/ab12cd34/";
  QCHECK(ReplaceBareBridgeUri("ws://127.0.0.1:41234", 41234, uri) == uri);
  QCHECK(ReplaceBareBridgeUri("ws://127.0.0.1:41235", 41234, uri) == "ws://127.0.0.1:41235");  // another port
  QCHECK(ReplaceBareBridgeUri("wss://service.example/nevr", 41234, uri) == "wss://service.example/nevr");
  QCHECK(ReplaceBareBridgeUri("ws://127.0.0.1:41234", 0, uri) == "ws://127.0.0.1:41234");  // no bridge yet
  QCHECK(ReplaceBareBridgeUri("ws://127.0.0.1:41234", 41234, "") == "ws://127.0.0.1:41234");
}

// ---- stage lines -----------------------------------------------------------------------------------

void TestStageNamesAreStable() {
  QCHECK(std::strcmp(StageForStep("resolve_config"), "config_loaded") == 0);
  QCHECK(std::strcmp(StageForStep("install_redirect"), "redirect_installed") == 0);
  QCHECK(std::strcmp(StageForStep("install_dlopen_hook"), "dlopen_hook_installed") == 0);
  QCHECK(std::strcmp(StageForStep("start_bridge"), "router_listening") == 0);
  QCHECK(std::strcmp(StageForStep("start_token_auth"), "token_auth_state") == 0);
  QCHECK(std::strcmp(StageForStep("install_social"), "social_hook_installed") == 0);
  QCHECK(std::strcmp(StageForStep("install_login_prompt"), "login_prompt_hook_installed") == 0);
  QCHECK(StageForStep("arm_crash_reporter") == nullptr);
  // Every step the sequence names that has a stage is mapped by its real name.
  for (int i = 0; i < static_cast<int>(StepId::kCount); ++i) {
    const char* step = StepName(static_cast<StepId>(i));
    const char* stage = StageForStep(step);
    if (stage != nullptr) QCHECK(std::strlen(stage) > 0);
  }
}

// A hook that stays out because its counters were refused is an error line, not an info one (#239 review).
void TestStepLogLevels() {
  QCHECK(StepLogLevel("ok", "ok") == StepLevel::kInfo);
  QCHECK(StepLogLevel("skipped", "social_off") == StepLevel::kInfo);
  QCHECK(StepLogLevel("skipped", "login_off") == StepLevel::kInfo);
  QCHECK(StepLogLevel("skipped", "bridge_and_login_off") == StepLevel::kInfo);
  QCHECK(StepLogLevel("skipped", "nothing_to_install_after_load") == StepLevel::kInfo);
  QCHECK(StepLogLevel("skipped", "token_auth_unavailable") == StepLevel::kWarn);
  QCHECK(StepLogLevel("skipped", "bridge_unavailable") == StepLevel::kWarn);
  QCHECK(StepLogLevel("skipped", "counters_refused") == StepLevel::kError);
  QCHECK(StepLogLevel("failed", "step_reported_failure") == StepLevel::kError);
  QCHECK(StepLogLevel("threw", "exception") == StepLevel::kError);
  QCHECK(StepLogLevel(nullptr, nullptr) == StepLevel::kError);
  // Every skip reason the sequence can produce for a refused registration is an error.
  FakeSteps s = FakeSteps::With(true, true, true, true);
  s.failing = {"reg_social", "reg_prompt", "reg_redirect", "reg_dlopen"};
  const ConstructorReport r = RunConstructorSequence(s);
  for (StepId id : {StepId::kInstallSocial, StepId::kInstallLoginPrompt, StepId::kInstallRedirect, StepId::kInstallDlopenHook}) {
    QCHECK(StepLogLevel(StepStateName(r.at(id).state), r.at(id).reason) == StepLevel::kError);
  }
}

void TestRouterLinesClassify() {
  auto c = [](const char* line) { return ClassifyRouterLine(line); };
  QCHECK(c("[remote] remote=3 connected") && std::strcmp(c("[remote] remote=3 connected")->event, "router_remote_connected") == 0);
  const auto tls = c("[remote] remote=3 connect failed: tls verification failed http_status=0 native_code=60 (no retry, no downgrade)");
  QCHECK(tls && std::strcmp(tls->event, "router_remote_failed") == 0 && std::strcmp(tls->cls, "tls_verification_failed") == 0);
  const auto net = c("[remote] remote=1 connect failed: network error http_status=0 native_code=6 (no retry, no downgrade)");
  QCHECK(net && std::strcmp(net->cls, "network_error") == 0);
  const auto up = c("[remote] remote=1 connect failed: websocket upgrade rejected http_status=401 native_code=0 (no retry, no downgrade)");
  QCHECK(up && std::strcmp(up->cls, "handshake_rejected") == 0);
  const auto jwt = c("[bridge] remote=2 not started: neither an account JWT nor configured credentials (no unauthenticated session)");
  QCHECK(jwt && std::strcmp(jwt->cls, "no_jwt") == 0);
  const auto nocr = c("[remote] remote=2 not started: no connect request (identity or configuration missing)");
  QCHECK(nocr && std::strcmp(nocr->cls, "no_connect_request") == 0);
  const auto ok = c("[router] LOGIN SUCCESS remote=2");
  QCHECK(ok && std::strcmp(ok->event, "login_accepted") == 0);
  const auto bad = c("[router] LOGIN FAILURE remote=2 status=4 message_bytes=12");
  QCHECK(bad && std::strcmp(bad->event, "login_refused") == 0);
  QCHECK(!c("[router] game=1 conn=0 (config)"));
}

}  // namespace

int main() {
  TestBareBridgeUriBecomesTheTokenedOne();
  TestStageNamesAreStable();
  TestRouterLinesClassify();
  TestStepLogLevels();
  TestEverythingOffInstallsOnlyTheProofHook();
  TestFullStackOrder();
  TestHwDumpIsIndependentAndContained();
  TestObbSkipIsIndependentAndContained();
  TestCountersBeforeTheSingleReporterStart();
  TestCrashReporterAndConfigComeFirst();
  TestFeatureGating();
  TestClockHookFailureLeavesTheRest();
  TestTokenAuthFailureTurnsOffTheBridgeAndWhatNeedsIt();
  TestBridgeFailureLeavesTheGameOnItsOwnHosts();
  TestRedirectFailureLeavesLoginAndSocialRunning();
  TestSocialFailureLeavesTheRest();
  TestLoginPromptFailureLeavesTheRest();
  TestCounterRefusalDisablesOnlyThatPiece();
  TestEveryStepThrowingIsContained();
  TestConfigFailureLeavesAllFeaturesOff();
  TestPostLoadIgnoresANullHandle();
  TestPostLoadRetriesUntilSettledThenStops();
  TestPostLoadCountsEachDistinctMatchmakingImage();
  TestMatchmakingReloadProbeWaitsForTheActionAndThenSpeaksOnce();
  TestMatchmakingReloadProbeFailsWhenTheActionGaveUp();
  TestApplySelfCheckWiresTheDebugQueryTheUserAndTheSender();
  TestPostLoadGiveUpEndsOnlyThatAction();
  TestPostLoadWithNoActionsIsInert();
  TestPostLoadAcceptsANullName();
  TestIdentitySourceAnswers();
  TestLoginGateFollowsTheIdentityAnswer();
  TestIdentitySourcePublishesTheSharedAttemptGate();
  TestDropReportsOnlyChanges();
  TestIdentitySourceReadyFollowsObservedState();
  TestIdentitySourceReadyDoesNotAllocate();
  TestFrameTapSignalsLoginSuccessOnlyFromTheServer();
  TestFrameTapNamesThePlatformAndTheAccountOfTheLogin();
  TestFrameTapContainsAThrowingConsumer();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "integration_sequence_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("integration_sequence_test: all checks passed\n");
  return 0;
}
