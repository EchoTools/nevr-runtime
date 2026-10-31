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

#include <nlohmann/json.hpp>

#include "quest/integration/bridge_uri.h"
#include "quest/integration/ctor_sequence.h"
#include "quest/integration/drop_report.h"
#include "quest/integration/identity_source.h"
#include "quest/integration/post_load.h"
#include "quest/integration/self_check_wiring.h"
#include "quest/integration/stage_log.h"
#include "quest/net/frame_tap.h"
#include "quest/sentinel/hook_log.h"
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
  bool loginArg = false, mmArg = false, pluginsArg = false;
  PluginCounts plugins;  // what the plan answers (#438)
  int planCalls = 0;
  int finishCalls = 0;
  std::string finishReason;
  bool finishThrows = false;
  nevr_quest::TextureChoice textureSeen;
  nevr_quest::TextureChoice renderConfigSeen;
  int textureCalls = 0;
  bool dlopenArgsSeen = false;
  bool devBuild = false;  // nevr_build::kIsDev: the resource request hook's (#871) log lines are for development builds
  bool resourceLogLines = false;  // the `logLines` the resource request install was handed
  bool devQueryThrows = false;
  int devAsked = 0;
  // What ArmClockLevels (#928) was handed: the switch, and whether it ran at all.
  bool clockLevelsOverrideArg = false;
  bool clockLevelsArgSeen = false;
  // The CREATE GAME / JOIN VIA CODE observer (#966, ADR 0017 S2): whether its step is wanted, and whether asking throws.
  bool inviteWanted = false;
  bool inviteWantedThrows = false;

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
  PluginCounts PlanPlugins() override {
    ++planCalls;
    // Recorded only when a test configured plugins, so the call lists of the tests that have none stay what they were.
    if (plugins.early > 0 || plugins.normal > 0) Step("plugins_plan");
    return plugins;
  }
  bool RunEarlyPlugins() override { return Step("plugins_early"); }
  void FinishPlugins(const char* reason) override {
    ++finishCalls;
    finishReason = reason;
    calls.emplace_back("plugins_finish");
    if (finishThrows) throw std::runtime_error("injected");
  }
  bool RegisterClockCounters() override { return Step("reg_clock"); }
  bool RegisterRedirectCounters() override { return Step("reg_redirect"); }
  bool RegisterDlopenCounters() override { return Step("reg_dlopen"); }
  bool RegisterLoginCounters() override { return Step("reg_login"); }
  bool RegisterSocialCounters() override { return Step("reg_social"); }
  bool RegisterLoginPromptCounters() override { return Step("reg_prompt"); }
  bool RegisterObbSkipCounters() override { return Step("reg_obb"); }
  bool ResourceRequestLogWanted() override {
    ++devAsked;
    if (devQueryThrows) throw std::runtime_error("injected");
    return devBuild;
  }
  bool RegisterResourceRequestCounters() override { return Step("reg_resource"); }
  bool RegisterTextureQualityCounters() override { return Step("reg_texture"); }
  bool RegisterClockLevelsCounters() override { return Step("reg_clock_levels"); }
  bool StartReporter() override { return Step("reporter"); }
  bool AttachLibR15() override { return Step("attach"); }
  bool InstallClockHook() override { return Step("clock"); }
  bool InstallFrameDispatch() override { return Step("frame_dispatch"); }
  bool InstallObbSkip(bool counted) override { return Step(counted ? "obb" : "obb_uncounted"); }
  bool InstallProcessEnd() override { return Step("process_end"); }
  bool InstallCrashFirst() override { return Step("crash_first"); }
  bool InstallPermissionWait() override { return Step("permission_wait"); }
  bool InstallVkExtWindow() override { return Step("vk_ext_window"); }
  bool InstallOsBuildGate() override { return Step("os_build_gate"); }
  bool InstallResourceRequestLog(bool counted, bool logLines) override {
    resourceLogLines = logLines;
    return Step(counted ? "resource" : "resource_uncounted");
  }
  bool InstallTextureQuality(const nevr_quest::TextureChoice& choice, bool counted) override {
    textureSeen = choice;
    ++textureCalls;
    // Every level, `stock` too, registers counters and needs them (#1012: the settings menu can raise `stock`).
    return Step(counted ? "texture" : "texture_uncounted");
  }
  bool InstallRenderConfig(const nevr_quest::TextureChoice& choice) override {
    renderConfigSeen = choice;
    return Step("render_config");
  }
  bool InstallCreditsText() override { return Step("credits"); }
  bool InstallMenuCards() override { return Step("menu_cards"); }
  bool WizardMetricWanted() override {
    if (inviteWantedThrows) throw std::runtime_error("injected");
    return inviteWanted;
  }
  bool InstallWizardMetric() override { return Step("wizard_metric"); }
  bool ArmClockLevels(bool counted, bool overrideOn) override {
    clockLevelsOverrideArg = overrideOn;
    clockLevelsArgSeen = true;
    return Step(counted ? "clock_levels" : "clock_levels_uncounted");
  }
  bool StartTokenAuth() override { return Step("token"); }
  bool InstallLoginPrompt(bool counted) override { return Step(counted ? "prompt" : "prompt_uncounted"); }
  bool StartBridge() override { return Step("bridge"); }
  bool InstallRedirect() override { return Step("redirect"); }
  bool InstallSocial() override { return Step("social"); }
  bool InstallDlopenHook(bool login, bool mm, bool plugins) override {
    loginArg = login;
    mmArg = mm;
    pluginsArg = plugins;
    dlopenArgsSeen = true;
    return Step("dlopen");
  }
  bool StartHwDump() override { return Step("hwdump"); }
  bool InstallPnsProvider() override { return Step("pns_provider"); }
  bool PnsProviderAllowed() override { return devBuild; }
  void Note(const char*, const char*, const char*) override {}

  static FakeSteps With(bool redirect, bool bridge, bool login, bool socialOn) {
    FakeSteps s;
    s.config.effective.redirect = redirect;
    s.config.effective.bridge = bridge;
    s.config.effective.login = login;
    s.config.effective.social = socialOn;
    // `stock` pinned here so the other tests' call lists do not move when the default does.
    s.config.textureQuality = {nevr_quest::texture_quality::Level::kStock, nevr_quest::Source::kFile};
    return s;
  }
  // The same with a level above Low: the texture-quality tests.
  static FakeSteps WithTexture(bool redirect, bool bridge, bool login, bool socialOn) {
    FakeSteps s = With(redirect, bridge, login, socialOn);
    s.config.textureQuality = {nevr_quest::texture_quality::Level::kHigh, nevr_quest::Source::kFile};
    return s;
  }
  int Index(const char* name) const {
    for (std::size_t i = 0; i < calls.size(); ++i) if (calls[i] == name) return static_cast<int>(i);
    return -1;
  }
  bool Ran(const char* name) const { return Index(name) >= 0; }
};

void TestEverythingOffInstallsOnlyTheProofHookAndTheObbSkip() {
  FakeSteps s = FakeSteps::WithTexture(false, false, false, false);
  const ConstructorReport r = RunConstructorSequence(s);
  const std::vector<std::string> want = {"arm", "config", "reg_clock", "reg_obb", "reg_resource", "reg_texture", "reg_clock_levels", "reporter", "attach", "clock", "frame_dispatch", "obb", "process_end", "crash_first", "permission_wait", "vk_ext_window", "os_build_gate", "resource", "texture", "render_config", "credits", "menu_cards", "clock_levels"};
  QCHECK(s.calls == want);
  QCHECK(r.at(StepId::kStartTokenAuth).state == StepState::kSkipped);
  QCHECK(r.at(StepId::kInstallRedirect).state == StepState::kSkipped);
  QCHECK(r.at(StepId::kInstallDlopenHook).state == StepState::kSkipped);
  // The hardware dump (#335) is off by default and its step says so.
  QCHECK(r.at(StepId::kInstallHwDump).state == StepState::kSkipped);
  QCHECK(std::strcmp(r.at(StepId::kInstallHwDump).reason, "hwdump_off") == 0);
  // The OBB-mount skip (#319) is not a feature: it is installed with every feature off (#572).
  QCHECK(r.at(StepId::kRegisterObbSkipCounters).state == StepState::kOk);
  QCHECK(r.at(StepId::kInstallObbSkip).state == StepState::kOk);
}

void TestFullStackOrder() {
  FakeSteps s = FakeSteps::WithTexture(true, true, true, true);
  s.config.effective.hwdump = true;
  s.config.effective.pnsradProvider = true;
  s.devBuild = true;
  s.config.effective.clockLevels = true;
  s.plugins = {1, 1};
  s.inviteWanted = true;  // the observer's step runs in the full stack (and is the one step this test turns on)
  const ConstructorReport r = RunConstructorSequence(s);
  QCHECK(s.clockLevelsArgSeen && s.clockLevelsOverrideArg);
  const std::vector<std::string> want = {"arm",       "config",     "plugins_plan", "reg_clock",  "reg_redirect", "reg_dlopen",
                                         "reg_login", "reg_social", "reg_prompt", "reg_obb",      "reg_resource", "reg_texture", "reg_clock_levels", "reporter", "attach",
                                         "plugins_early", "clock",     "frame_dispatch", "obb", "process_end", "crash_first", "permission_wait", "vk_ext_window", "os_build_gate", "resource", "texture", "render_config", "credits", "menu_cards", "wizard_metric", "clock_levels", "token",      "prompt",       "bridge",
                                         "redirect",  "social",     "dlopen",     "hwdump",     "pns_provider"};
  QCHECK(s.calls == want);
  QCHECK(s.loginArg && s.mmArg && s.pluginsArg);
  for (int i = 0; i < static_cast<int>(StepId::kCount); ++i) QCHECK(r.steps[i].state == StepState::kOk);
}

// The rad provider (#1220): off by default, development builds only, and only with the login and the dlopen hook that
// sends it; a failing or throwing install leaves every other step as it was.
void TestPnsProviderGates() {
  {  // the default: off, and the step says so
    FakeSteps s = FakeSteps::WithTexture(true, true, true, true);
    s.devBuild = true;
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(!s.Ran("pns_provider"));
    QCHECK(r.at(StepId::kInstallPnsProvider).state == StepState::kSkipped);
    QCHECK(std::strcmp(r.at(StepId::kInstallPnsProvider).reason, "pnsrad_provider_off") == 0);
  }
  {  // asked for in a release build: not installed, and the step says why
    FakeSteps s = FakeSteps::WithTexture(true, true, true, true);
    s.config.effective.pnsradProvider = true;
    s.devBuild = false;
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(!s.Ran("pns_provider"));
    QCHECK(r.at(StepId::kInstallPnsProvider).state == StepState::kSkipped);
    QCHECK(std::strcmp(r.at(StepId::kInstallPnsProvider).reason, "release_build") == 0);
  }
  {  // asked for in a development build, with no login: not installed
    FakeSteps s = FakeSteps::WithTexture(false, false, false, false);
    s.config.effective.pnsradProvider = true;
    s.devBuild = true;
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(!s.Ran("pns_provider"));
    QCHECK(std::strcmp(r.at(StepId::kInstallPnsProvider).reason, "login_unavailable") == 0);
  }
  {  // asked for in a development build, with the login: installed, last
    FakeSteps s = FakeSteps::WithTexture(true, true, true, true);
    s.config.effective.pnsradProvider = true;
    s.devBuild = true;
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(s.Ran("pns_provider"));
    QCHECK(s.Index("dlopen") < s.Index("pns_provider"));
    QCHECK(r.at(StepId::kInstallPnsProvider).state == StepState::kOk);
  }
  for (const char* mode : {"fail", "throw"}) {
    FakeSteps s = FakeSteps::WithTexture(true, true, true, true);
    s.config.effective.pnsradProvider = true;
    s.devBuild = true;
    if (std::strcmp(mode, "fail") == 0) s.failing = {"pns_provider"}; else s.throwing = {"pns_provider"};
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kInstallPnsProvider).state != StepState::kOk);
    QCHECK(s.Ran("dlopen") && s.Ran("social"));
  }
}

// The hardware dump needs no other feature, and a failing or throwing dump step leaves every other step as
// it was (#335).
void TestHwDumpIsIndependentAndContained() {
  {
    FakeSteps s = FakeSteps::With(false, false, false, false);
    s.config.effective.hwdump = true;
    const ConstructorReport r = RunConstructorSequence(s);
    const std::vector<std::string> want = {"arm", "config", "reg_clock", "reg_obb", "reg_resource", "reg_texture", "reg_clock_levels", "reporter", "attach", "clock", "frame_dispatch", "obb", "process_end", "crash_first", "permission_wait", "vk_ext_window", "os_build_gate", "resource", "texture", "render_config", "credits", "menu_cards", "clock_levels", "hwdump"};
    QCHECK(s.calls == want);
    QCHECK(r.at(StepId::kInstallHwDump).state == StepState::kOk);
  }
  for (const char* mode : {"fail", "throw"}) {
    FakeSteps s = FakeSteps::WithTexture(true, true, true, true);
    s.config.effective.hwdump = true;
    // The resource request hook (#871) runs in every build; devBuild decides only its log lines.
    s.devBuild = true;
    s.plugins = {1, 0};  // and no early plugin means a skipped early pass (#438)
    s.inviteWanted = true;  // the observer's step (#966) is ok here: the loop below asks every step before hwdump to be kOk
    if (std::strcmp(mode, "fail") == 0) s.failing = {"hwdump"}; else s.throwing = {"hwdump"};
    const ConstructorReport r = RunConstructorSequence(s);
    for (int i = 0; i < static_cast<int>(StepId::kInstallHwDump); ++i) QCHECK(r.steps[i].state == StepState::kOk);
    QCHECK(r.at(StepId::kInstallHwDump).state != StepState::kOk);
    QCHECK(s.Ran("dlopen") && s.Ran("social"));
  }
}

// The crash-first hook (#1605) runs on every launch like the process-end hook: no feature and no configuration turns it off,
// it needs no counters, it installs right after the process-end hook and before token auth, and a failing or a throwing step
// leaves every other step as it was.
void TestCrashFirstIsUnconditionalIndependentAndContained() {
  {
    FakeSteps s = FakeSteps::With(false, false, false, false);
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kInstallCrashFirst).state == StepState::kOk);
    QCHECK(std::strcmp(StepName(StepId::kInstallCrashFirst), "install_crash_first") == 0);
    QCHECK(s.Index("crash_first") == s.Index("process_end") + 1);
  }
  {  // before token auth, which a configuration with the network features does run
    FakeSteps s = FakeSteps::With(true, true, true, true);
    RunConstructorSequence(s);
    QCHECK(s.Index("crash_first") >= 0 && s.Index("crash_first") < s.Index("token"));
  }
  {  // the configuration cannot be resolved and the process-end hook fails: still installed
    FakeSteps s = FakeSteps::With(true, true, true, true);
    s.failing = {"config", "process_end"};
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kInstallCrashFirst).state == StepState::kOk);
    QCHECK(s.Ran("crash_first"));
  }
  for (const char* mode : {"fail", "throw"}) {
    FakeSteps s = FakeSteps::With(true, true, true, true);
    if (std::strcmp(mode, "fail") == 0) s.failing = {"crash_first"}; else s.throwing = {"crash_first"};
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kInstallCrashFirst).state != StepState::kOk);
    QCHECK(s.Ran("permission_wait") && s.Ran("token") && s.Ran("bridge") && s.Ran("redirect") && s.Ran("social") && s.Ran("dlopen"));
  }
}

// The permission-wait hook (#640) runs on every launch like the process-end hook: no feature and no configuration
// turns it off, it needs no counters, it installs right after the process-end hook and before token auth, and a
// failing or a throwing step leaves every other step as it was.
void TestPermissionWaitIsUnconditionalIndependentAndContained() {
  {
    FakeSteps s = FakeSteps::With(false, false, false, false);
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kInstallPermissionWait).state == StepState::kOk);
    QCHECK(std::strcmp(StepName(StepId::kInstallPermissionWait), "install_permission_wait") == 0);
    QCHECK(s.Index("permission_wait") == s.Index("crash_first") + 1);
  }
  {  // before token auth, which a configuration with the network features does run
    FakeSteps s = FakeSteps::With(true, true, true, true);
    RunConstructorSequence(s);
    QCHECK(s.Index("permission_wait") >= 0 && s.Index("permission_wait") < s.Index("token"));
  }
  {  // the configuration cannot be resolved and the process-end hook fails: still installed
    FakeSteps s = FakeSteps::With(true, true, true, true);
    s.failing = {"config", "process_end"};
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kInstallPermissionWait).state == StepState::kOk);
    QCHECK(s.Ran("permission_wait"));
  }
  for (const char* mode : {"fail", "throw"}) {
    FakeSteps s = FakeSteps::With(true, true, true, true);
    if (std::strcmp(mode, "fail") == 0) s.failing = {"permission_wait"}; else s.throwing = {"permission_wait"};
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kInstallPermissionWait).state != StepState::kOk);
    QCHECK(s.Ran("token") && s.Ran("bridge") && s.Ran("redirect") && s.Ran("social") && s.Ran("dlopen"));
  }
}

// The Vulkan extension window (#868) runs on every launch like the permission-wait hook: no feature and no configuration
// turns it off (it is in release builds too), it needs no counters, it installs right after the permission-wait hook and
// before token auth, and a failing or a throwing step leaves every other step as it was.
void TestVkExtWindowIsUnconditionalIndependentAndContained() {
  {
    FakeSteps s = FakeSteps::With(false, false, false, false);
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kInstallVkExtWindow).state == StepState::kOk);
    QCHECK(std::strcmp(StepName(StepId::kInstallVkExtWindow), "install_vk_ext_window") == 0);
    QCHECK(s.Index("vk_ext_window") == s.Index("permission_wait") + 1);
  }
  {  // before token auth, which a configuration with the network features does run
    FakeSteps s = FakeSteps::With(true, true, true, true);
    RunConstructorSequence(s);
    QCHECK(s.Index("vk_ext_window") >= 0 && s.Index("vk_ext_window") < s.Index("token"));
  }
  {  // the configuration cannot be resolved and the permission-wait hook fails: still installed
    FakeSteps s = FakeSteps::With(true, true, true, true);
    s.failing = {"config", "permission_wait"};
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kInstallVkExtWindow).state == StepState::kOk);
    QCHECK(s.Ran("vk_ext_window"));
  }
  for (const char* mode : {"fail", "throw"}) {
    FakeSteps s = FakeSteps::With(true, true, true, true);
    if (std::strcmp(mode, "fail") == 0) s.failing = {"vk_ext_window"}; else s.throwing = {"vk_ext_window"};
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kInstallVkExtWindow).state != StepState::kOk);
    QCHECK(s.Ran("token") && s.Ran("bridge") && s.Ran("redirect") && s.Ran("social") && s.Ran("dlopen"));
  }
}

// The render-configuration log (#1139) runs on every launch with no counters, after the texture-quality hook whose choice it
// reports, before the credits step, token auth and the hardware dump (which leaves the slots both hook to it), and is contained.
void TestRenderConfigIsUnconditionalOrderedAndContained() {
  namespace tq = nevr_quest::texture_quality;
  {  // every feature off: still runs, with the texture choice
    FakeSteps s = FakeSteps::WithTexture(false, false, false, false);
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kInstallRenderConfig).state == StepState::kOk);
    QCHECK(std::strcmp(StepName(StepId::kInstallRenderConfig), "install_render_config") == 0);
    QCHECK(s.renderConfigSeen.level == tq::Level::kHigh && s.renderConfigSeen.source == nevr_quest::Source::kFile);
    QCHECK(s.Index("render_config") == s.Index("texture") + 1);
    QCHECK(s.Index("render_config") < s.Index("credits"));
  }
  {  // before the hardware dump and token auth
    FakeSteps s = FakeSteps::WithTexture(true, true, true, true);
    s.config.effective.hwdump = true;
    RunConstructorSequence(s);
    QCHECK(s.Index("render_config") >= 0 && s.Index("render_config") < s.Index("token"));
    QCHECK(s.Index("render_config") < s.Index("hwdump"));
  }
  for (const char* mode : {"fail", "throw"}) {  // a failing or throwing step leaves every other one as it was
    FakeSteps s = FakeSteps::WithTexture(true, true, true, true);
    s.config.effective.hwdump = true;
    if (std::strcmp(mode, "fail") == 0) s.failing = {"render_config"}; else s.throwing = {"render_config"};
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kInstallRenderConfig).state != StepState::kOk);
    QCHECK(s.Ran("credits") && s.Ran("token") && s.Ran("hwdump") && s.Ran("texture"));
  }
}

// The OS build floor (#945) runs on every launch like the Vulkan window: no feature and no configuration turns it off, it
// needs no counters, it installs right after the Vulkan window hook and before token auth, and a failing or a throwing
// step leaves every other step as it was.
void TestOsBuildGateIsUnconditionalIndependentAndContained() {
  {
    FakeSteps s = FakeSteps::With(false, false, false, false);
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kInstallOsBuildGate).state == StepState::kOk);
    QCHECK(std::strcmp(StepName(StepId::kInstallOsBuildGate), "install_os_build_gate") == 0);
    QCHECK(s.Index("os_build_gate") == s.Index("vk_ext_window") + 1);
  }
  {  // before token auth, which a configuration with the network features does run
    FakeSteps s = FakeSteps::With(true, true, true, true);
    RunConstructorSequence(s);
    QCHECK(s.Index("os_build_gate") >= 0 && s.Index("os_build_gate") < s.Index("token"));
  }
  {  // the configuration cannot be resolved and the Vulkan window hook fails: still installed
    FakeSteps s = FakeSteps::With(true, true, true, true);
    s.failing = {"config", "vk_ext_window"};
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kInstallOsBuildGate).state == StepState::kOk);
    QCHECK(s.Ran("os_build_gate"));
  }
  for (const char* mode : {"fail", "throw"}) {
    FakeSteps s = FakeSteps::With(true, true, true, true);
    if (std::strcmp(mode, "fail") == 0) s.failing = {"os_build_gate"}; else s.throwing = {"os_build_gate"};
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kInstallOsBuildGate).state != StepState::kOk);
    QCHECK(s.Ran("token") && s.Ran("bridge") && s.Ran("redirect") && s.Ran("social") && s.Ran("dlopen"));
    QCHECK(s.Ran("texture"));
  }
}

// The texture-quality hook (#943) runs on every launch like the Vulkan window: no feature turns it off, a configuration that
// cannot be resolved gives it `stock` (a mistake, never the default and never a level that costs memory), the level the configuration did give reaches it with its source,
// its counters are registered before the single reporter start (at every level: the settings menu can raise `stock`, #1012),
// it installs right after the OS build floor hook and before token auth and the hardware dump, and a refused counter, a
// failing or a throwing step leaves every other step as it was.
void TestTextureQualityIsUnconditionalIndependentAndContained() {
  namespace tq = nevr_quest::texture_quality;
  {  // every feature off: the level the configuration gave still reaches the hook, with its source
    FakeSteps s = FakeSteps::WithTexture(false, false, false, false);
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kRegisterTextureQualityCounters).state == StepState::kOk);
    QCHECK(r.at(StepId::kInstallTextureQuality).state == StepState::kOk);
    QCHECK(std::strcmp(StepName(StepId::kInstallTextureQuality), "install_texture_quality") == 0);
    QCHECK(std::strcmp(StepName(StepId::kRegisterTextureQualityCounters), "register_texture_quality_counters") == 0);
    QCHECK(s.textureCalls == 1);
    QCHECK(s.textureSeen.level == tq::Level::kHigh && s.textureSeen.source == nevr_quest::Source::kFile);
    QCHECK(s.Index("resource") == s.Index("os_build_gate") + 1);
    QCHECK(s.Index("texture") == s.Index("resource") + 1);
    QCHECK(s.Index("reg_resource") == s.Index("reg_obb") + 1);
    QCHECK(s.Index("reg_texture") == s.Index("reg_resource") + 1 && s.Index("reg_texture") < s.Index("reporter"));
  }
  {  // before token auth and before the hardware dump, which shares the slot when this hook owns it
    FakeSteps s = FakeSteps::WithTexture(true, true, true, true);
    s.config.effective.hwdump = true;
    RunConstructorSequence(s);
    QCHECK(s.Index("texture") >= 0 && s.Index("texture") < s.Index("token"));
    QCHECK(s.Index("texture") < s.Index("hwdump"));
  }
  for (const nevr_quest::texture_quality::Level level :
       {tq::Level::kVeryLow, tq::Level::kLow, tq::Level::kMedium, tq::Level::kHigh, tq::Level::kVeryHigh, tq::Level::kUltra}) {
    FakeSteps s = FakeSteps::WithTexture(false, false, false, false);
    s.config.textureQuality = {level, nevr_quest::Source::kFile};
    RunConstructorSequence(s);
    QCHECK(s.textureSeen.level == level && s.textureSeen.source == nevr_quest::Source::kFile);
    QCHECK(s.Ran("reg_texture") && s.Ran("texture"));
  }
  {  // stock: its counters are registered and the step runs like any other level's (the settings menu can raise it, #1012)
    FakeSteps s = FakeSteps::WithTexture(true, true, true, true);
    s.config.textureQuality = {tq::Level::kStock, nevr_quest::Source::kFile};
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(s.Ran("reg_texture"));
    QCHECK(r.at(StepId::kRegisterTextureQualityCounters).state == StepState::kOk);
    QCHECK(r.at(StepId::kInstallTextureQuality).state == StepState::kOk);
    QCHECK(s.textureCalls == 1 && s.textureSeen.level == tq::Level::kStock);
    QCHECK(s.Ran("texture") && !s.Ran("texture_uncounted"));
  }
  {  // the configuration cannot be resolved: a mistake, so `stock` whatever the default is and whatever the fake config holds
    FakeSteps s = FakeSteps::WithTexture(true, true, true, true);
    s.config.textureQuality = {tq::Level::kUltra, nevr_quest::Source::kFile};  // not read: the configuration threw
    s.failing = {"config"};
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kResolveConfig).state == StepState::kThrew);
    QCHECK(r.at(StepId::kInstallTextureQuality).state == StepState::kOk);
    QCHECK(s.textureSeen.level == tq::kFallbackLevel && s.textureSeen.level == tq::Level::kStock);
    QCHECK(s.textureSeen.source != nevr_quest::Source::kFile);
    QCHECK(s.Ran("reg_texture") && s.Ran("texture"));
    QCHECK(r.at(StepId::kRegisterTextureQualityCounters).state == StepState::kOk);
  }
  {  // counters refused: the hook is called once so its own rule logs the skip, and the step says why
    FakeSteps s = FakeSteps::WithTexture(true, true, true, true);
    s.failing = {"reg_texture"};
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kRegisterTextureQualityCounters).state == StepState::kFailed);
    QCHECK(r.at(StepId::kInstallTextureQuality).state == StepState::kSkipped);
    QCHECK(std::strcmp(r.at(StepId::kInstallTextureQuality).reason, "counters_refused") == 0);
    QCHECK(s.Ran("texture_uncounted") && !s.Ran("texture"));
    QCHECK(s.Ran("redirect") && s.Ran("social") && s.Ran("dlopen"));
  }
  {  // a throw from the skip-logging call is contained too
    FakeSteps s = FakeSteps::WithTexture(true, true, true, true);
    s.failing = {"reg_texture"};
    s.throwing = {"texture_uncounted"};
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kInstallTextureQuality).state == StepState::kSkipped);
    QCHECK(s.Ran("token") && s.Ran("bridge") && s.Ran("redirect") && s.Ran("social") && s.Ran("dlopen"));
  }
  for (const char* mode : {"fail", "throw"}) {
    FakeSteps s = FakeSteps::WithTexture(true, true, true, true);
    if (std::strcmp(mode, "fail") == 0) s.failing = {"texture"}; else s.throwing = {"texture"};
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kInstallTextureQuality).state != StepState::kOk);
    QCHECK(s.Ran("token") && s.Ran("bridge") && s.Ran("redirect") && s.Ran("social") && s.Ran("dlopen"));
  }
  // The steps beside it are not its prerequisites: any of them failing or throwing still leaves it installed.
  for (const char* other : {"reg_clock", "reporter", "clock", "obb", "process_end", "crash_first", "permission_wait", "vk_ext_window", "os_build_gate"}) {
    for (const char* mode : {"fail", "throw"}) {
      FakeSteps s = FakeSteps::WithTexture(false, false, false, false);
      if (std::strcmp(mode, "fail") == 0) s.failing = {other}; else s.throwing = {other};
      const ConstructorReport r = RunConstructorSequence(s);
      QCHECK(r.at(StepId::kInstallTextureQuality).state == StepState::kOk);
      QCHECK(s.Ran("texture"));
    }
  }
}

// The main-menu cards (#966) install on every launch, whatever else is on, right after the credits step and before token
// auth; the step has its own stable name; a failing or throwing step contained: every other step still runs, and it
// still runs when any single other early step fails or throws.
void TestMenuCardsAreUnconditionalOrderedAndContained() {
  QCHECK(std::strcmp(StepName(StepId::kInstallMenuCards), "install_menu_cards") == 0);
  for (const bool everything : {false, true}) {
    FakeSteps s = FakeSteps::With(everything, everything, everything, everything);
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kInstallMenuCards).state == StepState::kOk);
    QCHECK(s.Index("menu_cards") == s.Index("credits") + 1);
    QCHECK(s.Index("menu_cards") < s.Index("token") || !s.Ran("token"));
  }
  for (const char* mode : {"fail", "throw"}) {
    FakeSteps s = FakeSteps::With(true, true, true, true);
    if (std::strcmp(mode, "fail") == 0) s.failing = {"menu_cards"}; else s.throwing = {"menu_cards"};
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kInstallMenuCards).state != StepState::kOk);
    QCHECK(s.Ran("token") && s.Ran("bridge") && s.Ran("redirect") && s.Ran("social") && s.Ran("dlopen"));
  }
  for (const char* other : {"reg_clock", "reporter", "clock", "obb", "process_end", "crash_first", "permission_wait", "vk_ext_window", "texture", "credits"}) {
    for (const char* mode : {"fail", "throw"}) {
      FakeSteps s = FakeSteps::With(false, false, false, false);
      if (std::strcmp(mode, "fail") == 0) s.failing = {other}; else s.throwing = {other};
      const ConstructorReport r = RunConstructorSequence(s);
      QCHECK(r.at(StepId::kInstallMenuCards).state == StepState::kOk);
      QCHECK(s.Ran("menu_cards"));
    }
  }
}

// The CREATE GAME / JOIN VIA CODE observer (#966, ADR 0017 S2): its step names itself and runs right after the menu cards when
// the invite switch is wanted. When it is not wanted, or asking about it throws, the step is skipped and nothing is called. A
// failing or throwing step is contained: every other step still runs.
void TestWizardMetricIsGatedOrderedAndContained() {
  QCHECK(std::strcmp(StepName(StepId::kInstallWizardMetric), "install_wizard_metric") == 0);
  {
    FakeSteps s = FakeSteps::With(true, true, true, true);
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kInstallWizardMetric).state == StepState::kSkipped);
    QCHECK(std::strcmp(r.at(StepId::kInstallWizardMetric).reason, "feature_off") == 0);
    QCHECK(!s.Ran("wizard_metric"));
    QCHECK(r.at(StepId::kInstallMenuCards).state == StepState::kOk);
  }
  {
    FakeSteps s = FakeSteps::With(true, true, true, true);
    s.inviteWantedThrows = true;
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kInstallWizardMetric).state == StepState::kSkipped);
    QCHECK(!s.Ran("wizard_metric"));
    QCHECK(s.Ran("token") && s.Ran("bridge"));
  }
  {
    FakeSteps s = FakeSteps::With(true, true, true, true);
    s.inviteWanted = true;
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kInstallWizardMetric).state == StepState::kOk);
    QCHECK(s.Index("wizard_metric") == s.Index("menu_cards") + 1);
    QCHECK(s.Index("wizard_metric") < s.Index("token") || !s.Ran("token"));
  }
  for (const char* mode : {"fail", "throw"}) {
    FakeSteps s = FakeSteps::With(true, true, true, true);
    s.inviteWanted = true;
    if (std::strcmp(mode, "fail") == 0) s.failing = {"wizard_metric"}; else s.throwing = {"wizard_metric"};
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kInstallWizardMetric).state != StepState::kOk);
    QCHECK(s.Ran("token") && s.Ran("bridge") && s.Ran("redirect") && s.Ran("social") && s.Ran("dlopen"));
  }
}

// The resource request hook (#871, #1737 C5) runs in every build: its counters are registered before the single
// StartReporter, and it is installed right after the OS build floor hook, before token auth, with the development question
// (ResourceRequestLogWanted) passed as `logLines` only; and the hook is independent of every feature, of the other hooks,
// and contained when it fails, throws or has its counters refused (the install then runs with `counted` false).
void TestResourceRequestHookRunsInEveryBuildOrderedAndContained() {
  {  // release: the counters and the hook run, its log lines are off, and the other steps are exactly as before
    FakeSteps s = FakeSteps::With(false, false, false, false);
    const ConstructorReport r = RunConstructorSequence(s);
    const std::vector<std::string> want = {"arm", "config", "reg_clock", "reg_obb", "reg_resource", "reg_texture", "reg_clock_levels", "reporter", "attach", "clock", "frame_dispatch", "obb", "process_end", "crash_first", "permission_wait", "vk_ext_window", "os_build_gate", "resource", "texture", "render_config", "credits", "menu_cards", "clock_levels"};
    QCHECK(s.calls == want);
    QCHECK(s.devAsked == 1);
    QCHECK(r.at(StepId::kRegisterResourceRequestCounters).state == StepState::kOk);
    QCHECK(r.at(StepId::kInstallResourceRequestLog).state == StepState::kOk);
    QCHECK(s.resourceLogLines == false);
    QCHECK(std::strcmp(StepName(StepId::kRegisterResourceRequestCounters), "register_resource_request_counters") == 0);
    QCHECK(std::strcmp(StepName(StepId::kInstallResourceRequestLog), "install_resource_request_log") == 0);
  }
  {  // development, every feature off: counters before the reporter, install right after the OS build floor hook, lines on
    FakeSteps s = FakeSteps::With(false, false, false, false);
    s.devBuild = true;
    const ConstructorReport r = RunConstructorSequence(s);
    const std::vector<std::string> want = {"arm", "config", "reg_clock", "reg_obb", "reg_resource", "reg_texture", "reg_clock_levels", "reporter", "attach", "clock", "frame_dispatch", "obb", "process_end", "crash_first", "permission_wait", "vk_ext_window", "os_build_gate", "resource", "texture", "render_config", "credits", "menu_cards", "clock_levels"};
    QCHECK(s.calls == want);
    QCHECK(r.at(StepId::kRegisterResourceRequestCounters).state == StepState::kOk);
    QCHECK(r.at(StepId::kInstallResourceRequestLog).state == StepState::kOk);
    QCHECK(s.resourceLogLines == true);
    QCHECK(s.Index("reg_resource") < s.Index("reporter"));
    QCHECK(s.Index("resource") == s.Index("os_build_gate") + 1);
  }
  {  // before token auth, which a configuration with the network features does run
    FakeSteps s = FakeSteps::With(true, true, true, true);
    s.devBuild = true;
    RunConstructorSequence(s);
    QCHECK(s.Index("resource") >= 0 && s.Index("resource") < s.Index("token"));
  }
  {  // the configuration cannot be resolved and other hooks fail: still installed
    FakeSteps s = FakeSteps::With(true, true, true, true);
    s.devBuild = true;
    s.failing = {"config", "vk_ext_window", "obb"};
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kInstallResourceRequestLog).state == StepState::kOk);
    QCHECK(s.Ran("resource"));
  }
  {  // its counters are refused: the hook is installed anyway, told so (the install line says counters refused), nothing else changes
    FakeSteps s = FakeSteps::With(true, true, true, true);
    s.devBuild = true;
    s.failing = {"reg_resource"};
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kRegisterResourceRequestCounters).state == StepState::kFailed);
    QCHECK(r.at(StepId::kInstallResourceRequestLog).state == StepState::kOk);
    QCHECK(s.Ran("resource_uncounted") && !s.Ran("resource"));
    QCHECK(s.Ran("reporter") && s.Ran("token") && s.Ran("bridge") && s.Ran("redirect") && s.Ran("social") && s.Ran("dlopen"));
  }
  for (const char* mode : {"fail", "throw"}) {  // the install fails or throws: every other step runs
    FakeSteps s = FakeSteps::With(true, true, true, true);
    s.devBuild = true;
    if (std::strcmp(mode, "fail") == 0) s.failing = {"resource"}; else s.throwing = {"resource"};
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kInstallResourceRequestLog).state != StepState::kOk);
    QCHECK(s.Ran("token") && s.Ran("bridge") && s.Ran("redirect") && s.Ran("social") && s.Ran("dlopen"));
  }
  {  // the development question cannot be answered: its lines are not wanted; the hook and its counters still run
    FakeSteps s = FakeSteps::With(true, true, true, true);
    s.devBuild = true;
    s.devQueryThrows = true;
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(s.Ran("reg_resource") && s.Ran("resource"));
    QCHECK(s.resourceLogLines == false);
    QCHECK(r.at(StepId::kInstallResourceRequestLog).state == StepState::kOk);
    QCHECK(s.Ran("reporter") && s.Ran("dlopen"));
  }
}

// The clock-level override (#928) is armed on every launch like the OBB skip: the switch (`diagnostics.clock_levels_override`,
// Features::clockLevels) is an argument of the step and decides only whether the render-configuration hook's handler rewrites
// the levels. Its counters are registered with the others, before the single StartReporter. It is armed right after the menu
// cards, the last hook of the group, and before token auth, and a failing or a throwing step leaves every other step as it was.
void TestClockLevelsIsUnconditionalIndependentAndContained() {
  {  // everything off, the switch off: armed, handed "off"
    FakeSteps s = FakeSteps::With(false, false, false, false);
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kRegisterClockLevelsCounters).state == StepState::kOk);
    QCHECK(r.at(StepId::kArmClockLevels).state == StepState::kOk);
    QCHECK(std::strcmp(StepName(StepId::kRegisterClockLevelsCounters), "register_clock_levels_counters") == 0);
    QCHECK(std::strcmp(StepName(StepId::kArmClockLevels), "arm_clock_levels") == 0);
    QCHECK(s.clockLevelsArgSeen && !s.clockLevelsOverrideArg);
    QCHECK(s.Index("clock_levels") == s.Index("menu_cards") + 1);
    QCHECK(s.Index("reg_clock_levels") < s.Index("reporter"));
  }
  {  // the switch on: handed "on"
    FakeSteps s = FakeSteps::With(false, false, false, false);
    s.config.effective.clockLevels = true;
    RunConstructorSequence(s);
    QCHECK(s.clockLevelsArgSeen && s.clockLevelsOverrideArg);
  }
  {  // before token auth, which a configuration with the network features does run
    FakeSteps s = FakeSteps::With(true, true, true, true);
    RunConstructorSequence(s);
    QCHECK(s.Index("clock_levels") >= 0 && s.Index("clock_levels") < s.Index("token"));
  }
  {  // the configuration cannot be resolved and the Vulkan hook fails: still armed, the switch off
    FakeSteps s = FakeSteps::With(true, true, true, true);
    s.config.effective.clockLevels = true;
    s.failing = {"config", "vk_ext_window"};
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kArmClockLevels).state == StepState::kOk);
    QCHECK(s.clockLevelsArgSeen && !s.clockLevelsOverrideArg);
  }
  {  // its counters are refused: the step is asked once, told so, and the step is skipped as an error
    FakeSteps s = FakeSteps::With(true, true, true, true);
    s.failing = {"reg_clock_levels"};
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kRegisterClockLevelsCounters).state == StepState::kFailed);
    QCHECK(r.at(StepId::kArmClockLevels).state == StepState::kSkipped);
    QCHECK(std::strcmp(r.at(StepId::kArmClockLevels).reason, "counters_refused") == 0);
    QCHECK(StepLogLevel(StepStateName(r.at(StepId::kArmClockLevels).state), r.at(StepId::kArmClockLevels).reason) ==
           StepLevel::kError);
    QCHECK(s.Ran("clock_levels_uncounted") && !s.Ran("clock_levels"));
    QCHECK(s.Ran("token") && s.Ran("bridge") && s.Ran("redirect") && s.Ran("social") && s.Ran("dlopen"));
  }
  for (const char* mode : {"fail", "throw"}) {
    FakeSteps s = FakeSteps::With(true, true, true, true);
    if (std::strcmp(mode, "fail") == 0) s.failing = {"clock_levels"}; else s.throwing = {"clock_levels"};
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kArmClockLevels).state != StepState::kOk);
    QCHECK(s.Ran("token") && s.Ran("bridge") && s.Ran("redirect") && s.Ran("social") && s.Ran("dlopen"));
  }
}

// The process-end hook (#621) runs on every launch like the OBB skip: no feature and no configuration turns it
// off, it needs no counters, it installs right after the OBB skip and before token auth, and a failing or a
// throwing step leaves every other step as it was.
void TestProcessEndIsUnconditionalIndependentAndContained() {
  {
    FakeSteps s = FakeSteps::With(false, false, false, false);
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kInstallProcessEnd).state == StepState::kOk);
    QCHECK(std::strcmp(StepName(StepId::kInstallProcessEnd), "install_process_end") == 0);
    QCHECK(s.Index("process_end") == s.Index("obb") + 1);
  }
  {  // the configuration cannot be resolved, and the OBB skip's counters are refused: still installed
    FakeSteps s = FakeSteps::With(true, true, true, true);
    s.failing = {"config", "reg_obb"};
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kInstallProcessEnd).state == StepState::kOk);
    QCHECK(s.Ran("process_end"));
  }
  for (const char* mode : {"fail", "throw"}) {
    FakeSteps s = FakeSteps::With(true, true, true, true);
    if (std::strcmp(mode, "fail") == 0) s.failing = {"process_end"}; else s.throwing = {"process_end"};
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kInstallProcessEnd).state != StepState::kOk);
    QCHECK(s.Ran("token") && s.Ran("bridge") && s.Ran("redirect") && s.Ran("social") && s.Ran("dlopen"));
  }
}

// The OBB-mount skip (#319) runs on every launch (#572): it needs no feature, a configuration that cannot be
// resolved does not stop it, its counters are registered before the single reporter start, it installs right
// after the clock hook, and a refused counter, a failing or a throwing step leaves every other step as it was.
void TestObbSkipIsUnconditionalIndependentAndContained() {
  {
    FakeSteps s = FakeSteps::WithTexture(false, false, false, false);
    const ConstructorReport r = RunConstructorSequence(s);
    const std::vector<std::string> want = {"arm", "config", "reg_clock", "reg_obb", "reg_resource", "reg_texture", "reg_clock_levels", "reporter", "attach", "clock", "frame_dispatch", "obb", "process_end", "crash_first", "permission_wait", "vk_ext_window", "os_build_gate", "resource", "texture", "render_config", "credits", "menu_cards", "clock_levels"};
    QCHECK(s.calls == want);
    QCHECK(r.at(StepId::kRegisterObbSkipCounters).state == StepState::kOk);
    QCHECK(r.at(StepId::kInstallObbSkip).state == StepState::kOk);
    QCHECK(!s.Ran("token") && !s.Ran("bridge") && !s.Ran("redirect"));
  }
  {  // the configuration cannot be resolved: every feature is off, and the skip is still installed
    FakeSteps s = FakeSteps::With(true, true, true, true);
    s.failing = {"config"};
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kResolveConfig).state == StepState::kThrew);
    // The unresolved configuration is a mistake, so the level is `stock`: its counters are registered and the step runs.
    const std::vector<std::string> want = {"arm", "config", "reg_clock", "reg_obb", "reg_resource", "reg_texture", "reg_clock_levels", "reporter", "attach", "clock", "frame_dispatch", "obb", "process_end", "crash_first", "permission_wait", "vk_ext_window", "os_build_gate", "resource", "texture", "render_config", "credits", "menu_cards", "clock_levels"};
    QCHECK(s.calls == want);
    QCHECK(r.at(StepId::kInstallObbSkip).state == StepState::kOk);
  }
  {  // counters refused: the hook installs nothing (its own rule is called to log the skip) and says why
    FakeSteps s = FakeSteps::With(true, true, true, true);
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
    if (std::strcmp(mode, "fail") == 0) s.failing = {"obb"}; else s.throwing = {"obb"};
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kInstallObbSkip).state != StepState::kOk);
    QCHECK(s.Ran("token") && s.Ran("bridge") && s.Ran("redirect") && s.Ran("social") && s.Ran("dlopen"));
  }
  // The clock counters, the reporter start and the clock hook are steps beside the skip, not its prerequisites: any
  // of them failing or throwing still leaves the skip installed with its counters.
  for (const char* other : {"reg_clock", "reporter", "clock"}) {
    for (const char* mode : {"fail", "throw"}) {
      FakeSteps s = FakeSteps::With(false, false, false, false);
      if (std::strcmp(mode, "fail") == 0) s.failing = {other}; else s.throwing = {other};
      const ConstructorReport r = RunConstructorSequence(s);
      QCHECK(r.at(StepId::kInstallObbSkip).state == StepState::kOk);
      QCHECK(s.Ran("obb") && !s.Ran("obb_uncounted"));
    }
  }
  {  // before the reporter is up nothing installs, and the skip is in before token auth starts
    FakeSteps s = FakeSteps::With(true, true, true, true);
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
  for (const char* ran : {"obb", "process_end", "crash_first", "permission_wait", "vk_ext_window", "os_build_gate", "texture", "token", "bridge", "redirect", "social", "dlopen"}) QCHECK(s.Ran(ran));
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
                           "reg_obb", "reg_texture", "reg_clock_levels", "reporter", "attach", "clock", "frame_dispatch", "obb", "process_end", "crash_first", "permission_wait", "vk_ext_window", "os_build_gate", "texture", "render_config", "credits", "menu_cards", "clock_levels", "token", "prompt", "bridge", "redirect", "social",
                           "dlopen"}) {
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

// The module-loaded observer (#1605): every non-null dlopen handle reaches it, with the name and the handle, before the actions run
// and whether or not any action is wanted; a null handle does not; SetPostLoadActions leaves it in place.
std::vector<std::string> g_observed;
std::vector<int> g_observedBeforeActions;
void Observe(const char* name, void* handle) noexcept {
  g_observed.push_back(std::string(name != nullptr ? name : "(null)") + (handle != nullptr ? "" : "!null-handle"));
  g_observedBeforeActions.push_back(g_loginRuns);
}

void TestTheModuleLoadedObserverSeesEveryNonNullDlopenBeforeTheActions() {
  int dummy = 0;
  g_observed.clear();
  g_observedBeforeActions.clear();
  ResetPostLoad(nullptr, nullptr, Settle::kDone, Settle::kDone);  // no action wanted at all
  SetModuleLoadedObserver(&Observe);
  AfterDlopen("/x/libpnsrad.so", &dummy);
  AfterDlopen("libfoo.so", nullptr);  // a failed dlopen is not a moment
  AfterDlopen("libpnsovr.so", &dummy);
  QCHECK(g_observed.size() == 2 && g_observed[0] == "/x/libpnsrad.so" && g_observed[1] == "libpnsovr.so");

  // With an action wanted: the observer runs first.
  ResetPostLoad(&FakeLogin, nullptr, Settle::kDone, Settle::kDone);
  g_observed.clear();
  g_observedBeforeActions.clear();
  AfterDlopen("libpnsovr.so", &dummy);
  QCHECK(g_observed.size() == 1 && g_observedBeforeActions[0] == 0 && g_loginRuns == 1);  // saw it before the login ran

  // SetPostLoadActions did not clear it; clearing it is explicit.
  QCHECK(PostLoadStatsView().calls == 1);
  SetModuleLoadedObserver(nullptr);
  AfterDlopen("libpnsrad.so", &dummy);
  QCHECK(g_observed.size() == 1);
}

// The login-failed page's title hook (#1137) waits for the script library like the matchmaking redirect waits for its own:
// tried after every non-null dlopen until it settles, never again after, and its settling is reported.
int g_titleRuns = 0;
Settle g_titleSettle = Settle::kRetryLater;
ActionResult FakeLoginTitle() noexcept {
  ++g_titleRuns;
  return {g_titleSettle, "fake"};
}

void TestPostLoadRunsTheLoginTitleActionUntilItSettles() {
  int dummy = 0;
  g_titleRuns = 0;
  g_titleSettle = Settle::kRetryLater;
  PostLoadActions a;
  a.loginTitle = &FakeLoginTitle;
  SetPostLoadActions(a);
  QCHECK(PostLoadPending());
  AfterDlopen("libGLESv3.so", &dummy);
  AfterDlopen("/data/app/x/lib/arm64/libc++_shared.so", &dummy);
  QCHECK(g_titleRuns == 2);  // the library is not mapped yet: asked again after each dlopen
  QCHECK(PostLoadStatsView().loginTitleSettled == 0);
  QCHECK(PostLoadPending());
  g_titleSettle = Settle::kDone;
  AfterDlopen("/data/app/x/lib/arm64/lib7f2363b8976491aa.so", &dummy);
  QCHECK(g_titleRuns == 3);
  QCHECK(PostLoadStatsView().loginTitleSettled == 1);
  QCHECK(!PostLoadPending());
  AfterDlopen("libfoo.so", &dummy);
  QCHECK(g_titleRuns == 3);  // done: never again
  // A refusal a retry cannot change also ends it.
  g_titleRuns = 0;
  g_titleSettle = Settle::kGiveUp;
  SetPostLoadActions(a);
  AfterDlopen("libfoo.so", &dummy);
  AfterDlopen("libbar.so", &dummy);
  QCHECK(g_titleRuns == 1);
  // No action wanted: nothing is pending and nothing runs.
  g_titleRuns = 0;
  SetPostLoadActions(PostLoadActions{});
  QCHECK(!PostLoadPending());
  AfterDlopen("libfoo.so", &dummy);
  QCHECK(g_titleRuns == 0);
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

// Self-check "android_main_returned" (#621): called by the process-end hook just before the process goes, it must
// put its result on the login connection by itself (it is the last chance), and log it whatever the connection does.
void TestAndroidMainReturnedIsSentAtOnceAndLogged() {
  static std::vector<std::string> sent;
  static std::vector<std::string> logged;
  sent.clear();
  logged.clear();
  nevr_self_check::ResetForTest();
  quest_net::FrameTapSinks sinks;
  nevr_quest::integration::SelfCheckHooks hooks;
  hooks.sender = [](const std::string& frame) { sent.push_back(frame); return true; };
  hooks.log = [](const nevr_self_check::LogRecord& r) { logged.push_back(r.name + " " + r.observed); };
  hooks.build = "5.0.0";
  nevr_quest::integration::ApplySelfCheck(&sinks, hooks);

  // Before the login: logged, nothing sent (the unit never sends without a user).
  nevr_quest::integration::ReportAndroidMainReturned();
  QCHECK(sent.empty());
  QCHECK(logged.size() == 1 && logged[0] == "android_main_returned ending_process exit_code=0");

  quest_net::FrameTap tap(sinks);
  tap.ServerToGame(nevr_evr_codec::BuildLoginSuccess(5, 0x2222ULL));
  sent.clear();
  logged.clear();
  nevr_quest::integration::ReportAndroidMainReturned();  // no Flush by the caller
  QCHECK(logged.size() == 1);
  QCHECK(sent.size() == 1);  // it left by itself
  if (sent.size() == 1) {
    QCHECK(sent[0].find("android_main_returned") != std::string::npos);
    QCHECK(sent[0].find("ending_process exit_code=0") != std::string::npos);
  }
  nevr_self_check::ResetForTest();
}

// ---- the plugin slot of the post-load policy (#438) ------------------------------------------------------

std::vector<std::string> g_order;
int g_pluginRuns = 0;
int g_pluginReentries = 0;
bool g_pluginReenters = false;
bool g_pluginReadsStatsInside = false;
std::uint64_t g_statsSeenInside = 99;

ActionResult OrderedLogin() noexcept {
  g_order.emplace_back("login");
  return {Settle::kDone, "login"};
}
ActionResult OrderedMatchmaking() noexcept {
  g_order.emplace_back("matchmaking");
  return {Settle::kDone, "mm"};
}
ActionResult FakePlugins() noexcept {
  ++g_pluginRuns;
  g_order.emplace_back("plugins");
  if (g_pluginReadsStatsInside) g_statsSeenInside = PostLoadStatsView().calls;  // takes the policy's lock
  if (g_pluginReenters) {
    // A plugin's code that calls dlopen comes back through the hook on this thread.
    int inner = 0;
    AfterDlopen("libfromplugin.so", &inner);
    ++g_pluginReentries;
  }
  return {Settle::kDone, "fake_plugins"};
}

void ResetPlugins(ActionFn login, ActionFn mm, ActionFn plugins) {
  g_order.clear();
  g_pluginRuns = g_pluginReentries = 0;
  g_pluginReenters = g_pluginReadsStatsInside = false;
  g_statsSeenInside = 99;
  g_loginRuns = g_mmRuns = 0;
  PostLoadActions a;
  a.login = login;
  a.matchmaking = mm;
  a.plugins = plugins;
  SetPostLoadActions(a);
}

void TestThePluginPassRunsOnceOnTheFirstDlopenAfterTheOtherInstalls() {
  int dummy = 0;
  ResetPlugins(&OrderedLogin, &OrderedMatchmaking, &FakePlugins);
  QCHECK(PostLoadPending());
  AfterDlopen("libc++_shared.so", &dummy);
  QCHECK((g_order == std::vector<std::string>{"login", "matchmaking", "plugins"}));
  QCHECK(g_pluginRuns == 1);
  QCHECK(PostLoadStatsView().pluginsSettled == 1);
  QCHECK(!PostLoadPending());
  AfterDlopen("libpnsovr.so", &dummy);
  QCHECK(g_pluginRuns == 1);  // once
}

void TestThePluginPassWaitsForAHandleAndNeedsNoOtherAction() {
  int dummy = 0;
  ResetPlugins(nullptr, nullptr, &FakePlugins);
  QCHECK(PostLoadPending());
  AfterDlopen("libfoo.so", nullptr);  // a failed dlopen is not a moment
  QCHECK(g_pluginRuns == 0 && PostLoadStatsView().pluginsSettled == 0);
  AfterDlopen("libfoo.so", &dummy);
  QCHECK(g_pluginRuns == 1);
  // Not requested: nothing runs and nothing is pending.
  ResetPlugins(nullptr, nullptr, nullptr);
  QCHECK(!PostLoadPending());
  AfterDlopen("libfoo.so", &dummy);
  QCHECK(g_pluginRuns == 0 && PostLoadStatsView().pluginsSettled == 0);
}

// A plugin's init runs with no policy lock held (it may read the stats, which take it) and a dlopen it makes does not start
// the pass again or wait on itself.
void TestThePluginPassRunsOutsideThePolicyLockAndNeverReentersItself() {
  int dummy = 0;
  ResetPlugins(&OrderedLogin, nullptr, &FakePlugins);
  g_pluginReadsStatsInside = true;
  g_pluginReenters = true;
  AfterDlopen("libfoo.so", &dummy);
  QCHECK(g_pluginRuns == 1);
  QCHECK(g_pluginReentries == 1);
  QCHECK(g_statsSeenInside == 1);  // the outer call was counted before the plugins ran
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
  QCHECK(source.HoldsAnswers() && gate::HoldsAnswers());  // a new source is Starting: token auth is working, hold
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
  QCHECK(next.HoldsAnswers());
}

// #581: every observed state maps to one (ready, hold) pair, published together. Hold: a token is coming without
// the player (starting, refreshing, expired, a failure the session retries, a ready snapshot whose token ran
// out). Deliver (not ready, no hold): the player is awaited, or no token will come (a final failure, stopped, a
// token without an account). Ready never holds.
void TestIdentitySourcePublishesTheHoldWithTheReady() {
  using nevr::quest_auth::Readiness;
  namespace gate = nevr_quest_login::attempt_gate;
  struct Row {
    const char* what;
    nevr::quest_auth::Snapshot snap;
    bool ready;
    bool hold;
  };
  nevr::quest_auth::Snapshot failedRetry = Snap(Readiness::Failed, "", 0, "p");
  failedRetry.will_retry = true;
  const Row rows[] = {
      {"starting", Snap(Readiness::Starting, "", 0, "p"), false, true},
      {"refreshing", Snap(Readiness::Refreshing, "", 0, "p"), false, true},
      {"awaiting the player", Snap(Readiness::AwaitingUser, "", 0, "p"), false, false},
      {"expired", Snap(Readiness::Expired, "", 0, "p"), false, true},
      {"failed, will retry", failedRetry, false, true},
      {"failed, final", Snap(Readiness::Failed, "", 0, "p"), false, false},
      {"stopped", Snap(Readiness::Stopped, "", 0, "p"), false, false},
      {"ready", Snap(Readiness::Ready, "tok", 4242, "p"), true, false},
      {"ready, token ran out", Snap(Readiness::Ready, "", 4242, "p"), false, true},
      {"ready, no account", Snap(Readiness::Ready, "tok", 0, "p"), false, false},
  };
  for (const Row& row : rows) {
    TokenIdentitySource viaObserve(nullptr);
    viaObserve.Observe(row.snap);
    const nevr_quest_login::IdentitySource& base = viaObserve;
    if (base.Ready() != row.ready || base.HoldsAnswers() != row.hold || gate::LoginMayProceed() != row.ready) {
      std::fprintf(stderr, "Observe(%s): ready %d hold %d, want %d %d\n", row.what, base.Ready(), base.HoldsAnswers(),
                   row.ready, row.hold);
    }
    QCHECK(base.Ready() == row.ready && base.HoldsAnswers() == row.hold && gate::LoginMayProceed() == row.ready);
    QCHECK(TokenIdentitySource::HoldsFor(row.snap) == row.hold);
    // Fetch publishes the same pair from the snapshot it read.
    const nevr::quest_auth::Snapshot snap = row.snap;
    TokenIdentitySource viaFetch([snap] { return snap; });
    nevr_quest_login::Identity id;
    static_cast<void>(viaFetch.Fetch(id));
    if (viaFetch.Ready() != row.ready || viaFetch.HoldsAnswers() != row.hold) {
      std::fprintf(stderr, "Fetch(%s): ready %d hold %d, want %d %d\n", row.what, viaFetch.Ready(),
                   viaFetch.HoldsAnswers(), row.ready, row.hold);
    }
    QCHECK(viaFetch.Ready() == row.ready && viaFetch.HoldsAnswers() == row.hold);
  }
  // A Fetch with no snapshot function answers NotReady and does not hold.
  TokenIdentitySource noSnapshot(nullptr);
  nevr_quest_login::Identity id;
  QCHECK(noSnapshot.Fetch(id) == nevr_quest_login::IdentityStatus::NotReady);
  QCHECK(!noSnapshot.Ready() && !noSnapshot.HoldsAnswers());
  // The default IdentitySource never holds.
  struct Plain final : nevr_quest_login::IdentitySource {
    nevr_quest_login::IdentityStatus Fetch(nevr_quest_login::Identity&) override {
      return nevr_quest_login::IdentityStatus::NotReady;
    }
  } plain;
  QCHECK(!plain.HoldsAnswers());
  static_assert(noexcept(std::declval<const TokenIdentitySource&>().HoldsAnswers()), "HoldsAnswers must be noexcept");
  static_assert(noexcept(TokenIdentitySource::HoldsFor(std::declval<const nevr::quest_auth::Snapshot&>())),
                "HoldsFor must be noexcept");
}

// If token auth never exists, a final startup failure must survive a later Fetch of the missing
// session. Otherwise Fetch republishes Starting and holds the game's local login answers again.
void TestMissingAuthStaysFailedAfterFetch() {
  using nevr::quest_auth::Readiness;
  using nevr_session_router::LoginGate;
  TokenIdentitySource source([] { return nevr::quest_auth::Snapshot{}; });  // no QuestTokenAuth
  QCHECK(source.HoldsAnswers());
  nevr::quest_auth::Snapshot failed;
  failed.readiness = Readiness::Failed;
  source.Observe(failed);
  QCHECK(!source.HoldsAnswers());
  QCHECK(TokenIdentitySource::GateFor(failed) == LoginGate::Refused);
  nevr_quest_login::Identity identity;
  QCHECK(source.Fetch(identity) == nevr_quest_login::IdentityStatus::NoToken);
  QCHECK(!source.HoldsAnswers());
  source.Observe(nevr::quest_auth::Snapshot{});  // a stale Starting observation cannot reopen the hold
  QCHECK(!source.HoldsAnswers());
  QCHECK(source.Fetch(identity) == nevr_quest_login::IdentityStatus::NoToken);
}

std::vector<std::string> g_holdLines;
void CaptureHoldLine(sentinel::LogLevel, const char* line) { g_holdLines.emplace_back(line); }

// #581: one Info record each time the hold decision changes, and on the first poll. The poll calls it with
// HoldsFor of each snapshot; a state change that leaves the decision alone logs nothing.
void TestTheHoldDecisionIsLoggedOnEachChange() {
  using nevr::quest_auth::Readiness;
  g_holdLines.clear();
  const sentinel::LogSink previous = sentinel::SetLogSink(&CaptureHoldLine);
  int last = -1;
  const auto poll = [&last](const nevr::quest_auth::Snapshot& snap, const char* name) {
    TokenIdentitySource::ReportHold(TokenIdentitySource::HoldsFor(snap), name, &last);
  };
  poll(Snap(Readiness::Starting, "", 0, "player-name-secret"), "starting");  // first poll: logs
  QCHECK(g_holdLines.size() == 1);
  poll(Snap(Readiness::Starting, "", 0, "player-name-secret"), "starting");  // same decision: nothing
  poll(Snap(Readiness::Refreshing, "", 0, "player-name-secret"), "refreshing");
  QCHECK(g_holdLines.size() == 1);
  poll(Snap(Readiness::AwaitingUser, "", 0, "player-name-secret"), "awaiting_user");  // held -> answered
  QCHECK(g_holdLines.size() == 2);
  poll(Snap(Readiness::Ready, "access-token-secret", 4242, "player-name-secret"), "ready");  // answered -> answered
  QCHECK(g_holdLines.size() == 2);
  poll(Snap(Readiness::Ready, "", 4242, "player-name-secret"), "ready");  // the token ran out: held again
  QCHECK(g_holdLines.size() == 3);
  sentinel::SetLogSink(previous);
  if (g_holdLines.size() == 3) {
    const auto has = [](const std::string& line, const char* part) { return line.find(part) != std::string::npos; };
    QCHECK(has(g_holdLines[0], "\"event\":\"quest_login_answers\"") && has(g_holdLines[0], "\"mode\":\"held\"") &&
           has(g_holdLines[0], "\"status\":\"starting\"") && has(g_holdLines[0], "\"level\":\"info\""));
    QCHECK(has(g_holdLines[1], "\"mode\":\"answered\"") && has(g_holdLines[1], "\"status\":\"awaiting_user\""));
    QCHECK(has(g_holdLines[2], "\"mode\":\"held\""));
  }
  for (const std::string& line : g_holdLines) {  // no token, nonce, user name or id
    // Each record opens with the wall-clock {"ts_ms":N, so the check reads only from ,"level": on: the
    // digits of the clock must not decide it (#1461).
    const std::string::size_type fields = line.find(",\"level\":");
    QCHECK(fields != std::string::npos);
    if (fields == std::string::npos) continue;
    const std::string body = line.substr(fields);
    QCHECK(body.find("secret") == std::string::npos && body.find("4242") == std::string::npos);
  }
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
  QCHECK(std::strcmp(StageForStep("attach_libr15"), "libr15_attach") == 0);
  QCHECK(std::strcmp(StepName(StepId::kAttachLibR15), "attach_libr15") == 0);
  QCHECK(std::strcmp(StepName(StepId::kInstallFrameDispatch), "install_frame_dispatch") == 0);
  QCHECK(StageForStep("arm_crash_reporter") == nullptr);
  // Every step the sequence names that has a stage is mapped by its real name.
  for (int i = 0; i < static_cast<int>(StepId::kCount); ++i) {
    const char* step = StepName(static_cast<StepId>(i));
    const char* stage = StageForStep(step);
    if (stage != nullptr) QCHECK(std::strlen(stage) > 0);
  }
}


// ---- the libr15 attach step (docs/adr/0021 sections 6, 7, 10) -----------------------------------------------

// Attach runs on every launch, once, after the one reporter start and before the early plugin pass, the clock hook and every
// other libr15 step; a launch that wants nothing else still attaches.
void TestAttachRunsOnceOnEveryLaunchBeforeEveryLibr15Step() {
  for (const bool wanted : {false, true}) {
    FakeSteps s = FakeSteps::With(wanted, wanted, wanted, wanted);
    const ConstructorReport r = RunConstructorSequence(s);
    int attaches = 0;
    for (const std::string& c : s.calls) if (c == "attach") ++attaches;
    QCHECK(attaches == 1);
    QCHECK(r.at(StepId::kAttachLibR15).state == StepState::kOk);
    QCHECK(s.Index("attach") == s.Index("reporter") + 1);
    for (const char* later : {"plugins_early", "clock", "obb", "texture", "credits", "menu_cards"}) {
      if (s.Ran(later)) QCHECK(s.Index("attach") < s.Index(later));
    }
  }
}

// A refused attach (a stock libr15, a mismatched file) or a throwing one fails only its own step: every later step still
// runs, because the GOT-only hooks refuse on the stock file by themselves and the inline rows and credits refuse as
// not_verified; the sequence adds no gating of its own.
void TestAFailingAttachNeverMovesAnotherStep() {
  for (const bool throws : {false, true}) {
    FakeSteps s = FakeSteps::With(true, true, true, true);
    if (throws) s.throwing = {"attach"}; else s.failing = {"attach"};
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kAttachLibR15).state == (throws ? StepState::kThrew : StepState::kFailed));
    QCHECK(s.Ran("clock") && s.Ran("obb") && s.Ran("texture") && s.Ran("credits") && s.Ran("menu_cards"));
    QCHECK(s.Ran("token") && s.Ran("social") && s.Ran("redirect") && s.Ran("dlopen"));
  }
}

// A hook that stays out because its counters were refused is an error line, not an info one (#239 review).
// ---- the plugin steps (#438, docs/adr/0019 section 3) ---------------------------------------------------

// The plan runs on every launch, once, right after the configuration and before any counter is registered; it is the only
// plugin step that does.
void TestThePluginPlanRunsOnceAfterTheConfigurationOnEveryLaunch() {
  FakeSteps s = FakeSteps::With(false, false, false, false);
  const ConstructorReport r = RunConstructorSequence(s);
  QCHECK(s.planCalls == 1);
  QCHECK(r.at(StepId::kPlanPlugins).state == StepState::kOk);
  QCHECK(std::strcmp(StepName(StepId::kPlanPlugins), "plan_plugins") == 0);
  QCHECK(r.order[0] == StepId::kArmCrashReporter && r.order[1] == StepId::kResolveConfig && r.order[2] == StepId::kPlanPlugins);
  QCHECK(r.order[3] == StepId::kRegisterClockCounters);
  // Nothing configured: nothing asks for a moment.
  QCHECK(r.at(StepId::kRunEarlyPlugins).state == StepState::kSkipped);
  QCHECK(std::strcmp(r.at(StepId::kRunEarlyPlugins).reason, "early_plugins_off") == 0);
  QCHECK(s.finishCalls == 0);
  QCHECK(!s.Ran("plugins_early"));
}

// The early pass runs after the reporter started and before the clock hook, and only when the plan has an early entry.
void TestTheEarlyPluginPassRunsAfterTheReporterAndBeforeTheClockHook() {
  FakeSteps s = FakeSteps::With(false, false, false, false);
  s.plugins = {2, 0};
  const ConstructorReport r = RunConstructorSequence(s);
  QCHECK(r.at(StepId::kRunEarlyPlugins).state == StepState::kOk);
  QCHECK(std::strcmp(StepName(StepId::kRunEarlyPlugins), "run_early_plugins") == 0);
  QCHECK(s.Index("attach") == s.Index("reporter") + 1);
  QCHECK(s.Index("plugins_early") == s.Index("attach") + 1);
  QCHECK(s.Index("clock") == s.Index("plugins_early") + 1);
  // An early entry asks for no dlopen hook: the early pass is the whole of it.
  QCHECK(!s.Ran("reg_dlopen") && !s.Ran("dlopen"));
  QCHECK(s.finishCalls == 0);
}

// A failing or throwing early pass, or plan, leaves every other step as it was.
void TestAFailingPluginStepNeverMovesAnotherStep() {
  {
    FakeSteps s = FakeSteps::With(true, true, true, true);
    s.plugins = {1, 1};
    s.failing = {"plugins_early"};
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kRunEarlyPlugins).state == StepState::kFailed);
    QCHECK(s.Ran("clock") && s.Ran("redirect") && s.Ran("social") && s.Ran("dlopen"));
    QCHECK(s.pluginsArg);
  }
  {
    FakeSteps s = FakeSteps::With(true, true, true, true);
    s.plugins = {1, 1};
    s.throwing = {"plugins_early"};
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kRunEarlyPlugins).state == StepState::kThrew);
    QCHECK(s.Ran("clock") && s.Ran("dlopen") && s.pluginsArg);
  }
  {  // a plan that throws plans nothing: no moment is asked for
    FakeSteps s = FakeSteps::With(false, false, false, false);
    s.plugins = {1, 1};
    s.throwing = {"plugins_plan"};
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(r.at(StepId::kPlanPlugins).state == StepState::kThrew);
    QCHECK(r.at(StepId::kRunEarlyPlugins).state == StepState::kSkipped);
    QCHECK(!s.Ran("dlopen") && !s.Ran("reg_dlopen") && s.Ran("clock") && s.Ran("credits"));
  }
}

// A normal entry makes the dlopen hook wanted whatever the features say; the other two installs stay off.
void TestANormalPluginEntryWantsTheDlopenHookOnItsOwn() {
  FakeSteps s = FakeSteps::With(false, false, false, false);
  s.plugins = {0, 1};
  const ConstructorReport r = RunConstructorSequence(s);
  QCHECK(s.Ran("reg_dlopen") && s.Ran("dlopen"));
  QCHECK(r.at(StepId::kInstallDlopenHook).state == StepState::kOk);
  QCHECK(s.pluginsArg && !s.loginArg && !s.mmArg);
  QCHECK(s.Index("reg_dlopen") < s.Index("reporter"));  // its counter is registered before the one reporter start
  QCHECK(s.finishCalls == 0);
  // Without a normal entry the hook stays off for a configuration that wants neither install.
  FakeSteps none = FakeSteps::With(false, false, false, false);
  RunConstructorSequence(none);
  QCHECK(!none.Ran("dlopen"));
}

// The login and matchmaking installs are told apart from the plugin pass: the hook is installed for all that are wanted.
void TestThePluginPassRidesTheSameDlopenHookAsTheOtherInstalls() {
  FakeSteps s = FakeSteps::With(true, true, true, true);
  s.plugins = {0, 3};
  RunConstructorSequence(s);
  QCHECK(s.loginArg && s.mmArg && s.pluginsArg);
  FakeSteps off = FakeSteps::With(true, true, true, true);
  RunConstructorSequence(off);
  QCHECK(off.loginArg && off.mmArg && !off.pluginsArg);
}

// The normal pass has no moment but the first post-load call. When the hook that makes it cannot be live, the entries still
// waiting are ended at once, with the reason, instead of at the end of the process.
void TestNormalEntriesEndWhenTheDlopenHookCannotBeLive() {
  {  // counters refused
    FakeSteps s = FakeSteps::With(false, false, false, false);
    s.plugins = {0, 1};
    s.failing = {"reg_dlopen"};
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(!s.Ran("dlopen"));
    QCHECK(r.at(StepId::kInstallDlopenHook).state == StepState::kSkipped);
    QCHECK(s.finishCalls == 1);
    QCHECK(s.finishReason == "post_load_hook_unavailable");
  }
  {  // the hook step reports failure
    FakeSteps s = FakeSteps::With(false, false, false, false);
    s.plugins = {0, 1};
    s.failing = {"dlopen"};
    RunConstructorSequence(s);
    QCHECK(s.finishCalls == 1 && s.finishReason == "post_load_hook_unavailable");
  }
  {  // the hook step throws
    FakeSteps s = FakeSteps::With(false, false, false, false);
    s.plugins = {0, 1};
    s.throwing = {"dlopen"};
    RunConstructorSequence(s);
    QCHECK(s.finishCalls == 1);
  }
  {  // a finish that throws is contained: the later steps run
    FakeSteps s = FakeSteps::With(false, false, false, true);
    s.config.effective.hwdump = true;
    s.plugins = {0, 1};
    s.failing = {"reg_dlopen"};
    s.finishThrows = true;
    const ConstructorReport r = RunConstructorSequence(s);
    QCHECK(s.finishCalls == 1);
    QCHECK(r.at(StepId::kInstallHwDump).state == StepState::kOk);
  }
  {  // an early-only plan asked for no hook, so a hook that is not live ends nothing
    FakeSteps s = FakeSteps::With(false, false, false, false);
    s.plugins = {1, 0};
    RunConstructorSequence(s);
    QCHECK(s.finishCalls == 0);
  }
}

void TestStepLogLevels() {
  QCHECK(StepLogLevel("ok", "ok") == StepLevel::kInfo);
  QCHECK(StepLogLevel("skipped", "social_off") == StepLevel::kInfo);
  QCHECK(StepLogLevel("skipped", "login_off") == StepLevel::kInfo);
  QCHECK(StepLogLevel("skipped", "bridge_and_login_off") == StepLevel::kInfo);
  QCHECK(StepLogLevel("skipped", "nothing_to_install_after_load") == StepLevel::kInfo);
  QCHECK(StepLogLevel("skipped", "early_plugins_off") == StepLevel::kInfo);  // no plugin asked for the early pass
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

// The constructor's begin record names the build (#1391): the version and the commit, as fields of one sentinel_ctor line.
std::vector<std::string> g_beginLines;
void CaptureBeginLine(sentinel::LogLevel, const char* line) { g_beginLines.emplace_back(line); }

std::size_t CountEventKeys(const std::string& text) {
  const std::string needle = "\"event\":";
  std::size_t count = 0;
  for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) {
    ++count;
  }
  return count;
}

void TestConstructorBeginNamesTheBuild() {
  constexpr const char* kVersion = "6.0.1-dev.2+0123456789abcdef0123456789abcdef01234567";
  constexpr const char* kCommit = "0123456789abcdef0123456789abcdef01234567";
  g_beginLines.clear();
  const sentinel::LogSink previous = sentinel::SetLogSink(&CaptureBeginLine);
  nevr_quest::integration::LogConstructorBegin(kVersion, kCommit);
  sentinel::SetLogSink(previous);
  QCHECK(g_beginLines.size() == 1U);
  if (g_beginLines.size() != 1U) return;
  const std::string& line = g_beginLines[0];
  const nlohmann::json j = nlohmann::json::parse(line);
  QCHECK(j.value("event", "") == "sentinel_ctor");
  QCHECK(j.value("action", "") == "begin");
  QCHECK(j.value("version", "") == std::string(kVersion));
  QCHECK(j.value("commit", "") == std::string(kCommit));
  QCHECK(CountEventKeys(line) == 1U);
}

}  // namespace

int main() {
  TestConstructorBeginNamesTheBuild();
  TestBareBridgeUriBecomesTheTokenedOne();
  TestStageNamesAreStable();
  TestAttachRunsOnceOnEveryLaunchBeforeEveryLibr15Step();
  TestAFailingAttachNeverMovesAnotherStep();
  TestRouterLinesClassify();
  TestStepLogLevels();
  TestEverythingOffInstallsOnlyTheProofHookAndTheObbSkip();
  TestThePluginPlanRunsOnceAfterTheConfigurationOnEveryLaunch();
  TestTheEarlyPluginPassRunsAfterTheReporterAndBeforeTheClockHook();
  TestAFailingPluginStepNeverMovesAnotherStep();
  TestANormalPluginEntryWantsTheDlopenHookOnItsOwn();
  TestThePluginPassRidesTheSameDlopenHookAsTheOtherInstalls();
  TestNormalEntriesEndWhenTheDlopenHookCannotBeLive();
  TestFullStackOrder();
  TestHwDumpIsIndependentAndContained();
  TestPnsProviderGates();
  TestObbSkipIsUnconditionalIndependentAndContained();
  TestProcessEndIsUnconditionalIndependentAndContained();
  TestCrashFirstIsUnconditionalIndependentAndContained();
  TestPermissionWaitIsUnconditionalIndependentAndContained();
  TestVkExtWindowIsUnconditionalIndependentAndContained();
  TestOsBuildGateIsUnconditionalIndependentAndContained();
  TestResourceRequestHookRunsInEveryBuildOrderedAndContained();
  TestTextureQualityIsUnconditionalIndependentAndContained();
  TestRenderConfigIsUnconditionalOrderedAndContained();
  TestMenuCardsAreUnconditionalOrderedAndContained();
  TestWizardMetricIsGatedOrderedAndContained();
  TestClockLevelsIsUnconditionalIndependentAndContained();
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
  TestPostLoadRunsTheLoginTitleActionUntilItSettles();
  TestTheModuleLoadedObserverSeesEveryNonNullDlopenBeforeTheActions();
  TestMatchmakingReloadProbeWaitsForTheActionAndThenSpeaksOnce();
  TestMatchmakingReloadProbeFailsWhenTheActionGaveUp();
  TestApplySelfCheckWiresTheDebugQueryTheUserAndTheSender();
  TestAndroidMainReturnedIsSentAtOnceAndLogged();
  TestPostLoadGiveUpEndsOnlyThatAction();
  TestThePluginPassRunsOnceOnTheFirstDlopenAfterTheOtherInstalls();
  TestThePluginPassWaitsForAHandleAndNeedsNoOtherAction();
  TestThePluginPassRunsOutsideThePolicyLockAndNeverReentersItself();
  TestPostLoadWithNoActionsIsInert();
  TestPostLoadAcceptsANullName();
  TestIdentitySourceAnswers();
  TestLoginGateFollowsTheIdentityAnswer();
  TestIdentitySourcePublishesTheSharedAttemptGate();
  TestIdentitySourcePublishesTheHoldWithTheReady();
  TestMissingAuthStaysFailedAfterFetch();
  TestTheHoldDecisionIsLoggedOnEachChange();
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
