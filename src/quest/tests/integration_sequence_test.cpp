// Host test for the sentinel constructor's sequence and the integration's pure pieces
// (src/quest/integration): the order of the steps, the gating by feature, the counter registration
// before the single reporter start, the isolation of one failing piece from the others, the post-load
// policy, the identity source, the social switch and the frame tap. Fakes stand in for every library.

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "quest/integration/ctor_sequence.h"
#include "quest/integration/frame_tap.h"
#include "quest/integration/identity_source.h"
#include "quest/integration/post_load.h"
#include "quest/integration/social_gate.h"
#include "quest/integration/stage_log.h"
#include "quest/tests/test_check.h"
#include "runtime/compat/evr_codec.h"

using namespace nevr_quest;
using namespace nevr_quest::integration;

namespace {

// ---- the sequence ---------------------------------------------------------------------------------

struct FakeSteps final : Steps {
  nevr_quest::ResolvedConfig config;
  bool social = false;
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
  bool SocialWanted(const nevr_quest::Features&) override { return social; }
  bool RegisterClockCounters() override { return Step("reg_clock"); }
  bool RegisterRedirectCounters() override { return Step("reg_redirect"); }
  bool RegisterDlopenCounters() override { return Step("reg_dlopen"); }
  bool RegisterSocialCounters() override { return Step("reg_social"); }
  bool StartReporter() override { return Step("reporter"); }
  bool InstallClockHook() override { return Step("clock"); }
  bool StartTokenAuth() override { return Step("token"); }
  bool StartBridge() override { return Step("bridge"); }
  bool InstallRedirect() override { return Step("redirect"); }
  bool InstallSocial() override { return Step("social"); }
  bool InstallDlopenHook(bool login, bool mm) override {
    loginArg = login;
    mmArg = mm;
    dlopenArgsSeen = true;
    return Step("dlopen");
  }
  void Note(const char*, const char*, const char*) override {}

  static FakeSteps With(bool redirect, bool bridge, bool login, bool socialOn) {
    FakeSteps s;
    s.config.effective.redirect = redirect;
    s.config.effective.bridge = bridge;
    s.config.effective.login = login;
    s.social = socialOn;
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
}

void TestFullStackOrder() {
  FakeSteps s = FakeSteps::With(true, true, true, true);
  const ConstructorReport r = RunConstructorSequence(s);
  const std::vector<std::string> want = {"arm",     "config", "reg_clock", "reg_redirect", "reg_dlopen",
                                         "reg_social", "reporter", "clock",    "token",        "bridge",
                                         "redirect", "social", "dlopen"};
  QCHECK(s.calls == want);
  QCHECK(s.loginArg && s.mmArg);
  for (int i = 0; i < static_cast<int>(StepId::kCount); ++i) QCHECK(r.steps[i].state == StepState::kOk);
}

// Every counter is registered before the single StartReporter, and StartReporter runs once.
void TestCountersBeforeTheSingleReporterStart() {
  FakeSteps s = FakeSteps::With(true, true, true, true);
  RunConstructorSequence(s);
  int starts = 0;
  for (const std::string& c : s.calls) if (c == "reporter") ++starts;
  QCHECK(starts == 1);
  const int reporter = s.Index("reporter");
  for (const char* reg : {"reg_clock", "reg_redirect", "reg_dlopen", "reg_social"}) {
    QCHECK(s.Index(reg) >= 0 && s.Index(reg) < reporter);
  }
  // No hook is installed before the reporter is up.
  for (const char* hook : {"clock", "redirect", "social", "dlopen", "bridge", "token"}) QCHECK(s.Index(hook) > reporter);
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
  }
  {  // redirect + bridge, no login: token auth runs (the remote needs a JWT), no login install
    FakeSteps s = FakeSteps::With(true, true, false, false);
    RunConstructorSequence(s);
    QCHECK(s.Ran("token") && s.Ran("bridge") && s.Ran("redirect"));
    QCHECK(s.Ran("dlopen") && !s.loginArg && s.mmArg);
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
  QCHECK(s.Ran("clock") && s.Ran("reporter"));
  QCHECK(r.at(StepId::kStartBridge).state == StepState::kSkipped);
  QCHECK(std::strcmp(r.at(StepId::kStartBridge).reason, "token_auth_unavailable") == 0);
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

void TestCounterRefusalDisablesOnlyThatPiece() {
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
  for (const char* name : {"arm", "config", "reg_clock", "reg_redirect", "reg_dlopen", "reg_social", "reporter",
                           "clock", "token", "bridge", "redirect", "social", "dlopen"}) {
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

QuestLogin::IdentityStatus Fetch(nevr::quest_auth::Snapshot snap, QuestLogin::Identity* out) {
  TokenIdentitySource source([snap] { return snap; });
  return source.Fetch(*out);
}

void TestIdentitySourceAnswers() {
  using nevr::quest_auth::Readiness;
  QuestLogin::Identity id;
  QCHECK(Fetch(Snap(Readiness::Ready, "tok", 4242, "player"), &id) == QuestLogin::IdentityStatus::Ok);
  QCHECK(id.account_id == 4242 && id.access_token == "tok" && id.display_name == "player");

  for (Readiness r : {Readiness::Starting, Readiness::Refreshing, Readiness::AwaitingUser}) {
    QuestLogin::Identity none;
    QCHECK(Fetch(Snap(r, "tok", 4242, "p"), &none) == QuestLogin::IdentityStatus::NotReady);
    QCHECK(none.access_token.empty() && none.account_id == 0);  // nothing leaks out of a refusal
  }
  for (Readiness r : {Readiness::Expired, Readiness::Failed, Readiness::Stopped}) {
    QuestLogin::Identity none;
    QCHECK(Fetch(Snap(r, "tok", 4242, "p"), &none) == QuestLogin::IdentityStatus::NoToken);
    QCHECK(none.access_token.empty());
  }
  QuestLogin::Identity none;
  QCHECK(Fetch(Snap(Readiness::Ready, "", 4242, "p"), &none) == QuestLogin::IdentityStatus::NoToken);
  QCHECK(Fetch(Snap(Readiness::Ready, "tok", 0, "p"), &none) == QuestLogin::IdentityStatus::NoAccount);
  TokenIdentitySource empty(nullptr);
  QCHECK(empty.Fetch(none) == QuestLogin::IdentityStatus::NotReady);
}

// ---- social switch ---------------------------------------------------------------------------------

void TestSocialGate() {
  const std::string on = R"({"features":{"login":true,"social":true}})";
  const std::string off = R"({"features":{"login":true}})";
  const std::string asString = R"({"features":{"social":"true"}})";
  const std::string notObject = "[1,2]";
  const std::string broken = "{";
  QCHECK(SocialRequested(&on));
  QCHECK(!SocialRequested(&off));
  QCHECK(!SocialRequested(&asString));
  QCHECK(!SocialRequested(&notObject));
  QCHECK(!SocialRequested(&broken));
  QCHECK(!SocialRequested(nullptr));
  const std::string huge(nevr_quest::kMaxConfigBytes + 1, ' ');
  QCHECK(!SocialRequested(&huge));

  const char* reason = "";
  nevr_quest::Features f;
  QCHECK(!SocialEffective(false, f, &reason) && std::strcmp(reason, "not_requested") == 0);
  QCHECK(!SocialEffective(true, f, &reason) && std::strcmp(reason, "login_not_enabled") == 0);
  f.redirect = f.bridge = f.login = true;
  QCHECK(SocialEffective(true, f, &reason) && std::strcmp(reason, "ok") == 0);
}

// ---- frame tap -------------------------------------------------------------------------------------

std::string LoginSuccessFrame(std::uint64_t account) { return EvrCodec::BuildLoginSuccess(EvrCodec::kBridgeLoginPlatform, account); }

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
  tap.ServerToGame(EvrCodec::BuildMessage(0x1234, "abc") + success);
  QCHECK(accounts.size() == 1);

  // A truncated one, and other symbols, are not.
  accounts.clear();
  tap.ServerToGame(success.substr(0, success.size() - 3));
  tap.ServerToGame(EvrCodec::BuildMessage(0x1234, std::string(64, 'x')));
  tap.ServerToGame("garbage");
  QCHECK(accounts.empty());
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

// ---- stage lines -----------------------------------------------------------------------------------

void TestStageNamesAreStable() {
  QCHECK(std::strcmp(StageForStep("resolve_config"), "config_loaded") == 0);
  QCHECK(std::strcmp(StageForStep("install_redirect"), "redirect_installed") == 0);
  QCHECK(std::strcmp(StageForStep("install_dlopen_hook"), "dlopen_hook_installed") == 0);
  QCHECK(std::strcmp(StageForStep("start_bridge"), "router_listening") == 0);
  QCHECK(std::strcmp(StageForStep("start_token_auth"), "token_auth_state") == 0);
  QCHECK(std::strcmp(StageForStep("install_social"), "social_hook_installed") == 0);
  QCHECK(StageForStep("arm_crash_reporter") == nullptr);
  // Every step the sequence names that has a stage is mapped by its real name.
  for (int i = 0; i < static_cast<int>(StepId::kCount); ++i) {
    const char* step = StepName(static_cast<StepId>(i));
    const char* stage = StageForStep(step);
    if (stage != nullptr) QCHECK(std::strlen(stage) > 0);
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
  TestStageNamesAreStable();
  TestRouterLinesClassify();
  TestEverythingOffInstallsOnlyTheProofHook();
  TestFullStackOrder();
  TestCountersBeforeTheSingleReporterStart();
  TestCrashReporterAndConfigComeFirst();
  TestFeatureGating();
  TestClockHookFailureLeavesTheRest();
  TestTokenAuthFailureTurnsOffTheBridgeAndWhatNeedsIt();
  TestBridgeFailureLeavesTheGameOnItsOwnHosts();
  TestRedirectFailureLeavesLoginAndSocialRunning();
  TestSocialFailureLeavesTheRest();
  TestCounterRefusalDisablesOnlyThatPiece();
  TestEveryStepThrowingIsContained();
  TestConfigFailureLeavesAllFeaturesOff();
  TestPostLoadIgnoresANullHandle();
  TestPostLoadRetriesUntilSettledThenStops();
  TestPostLoadGiveUpEndsOnlyThatAction();
  TestPostLoadWithNoActionsIsInert();
  TestPostLoadAcceptsANullName();
  TestIdentitySourceAnswers();
  TestSocialGate();
  TestFrameTapSignalsLoginSuccessOnlyFromTheServer();
  TestFrameTapContainsAThrowingConsumer();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "integration_sequence_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("integration_sequence_test: all checks passed\n");
  return 0;
}
