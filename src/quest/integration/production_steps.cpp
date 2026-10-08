// The real `Steps` of the constructor sequence (ctor_sequence.h): each step bound to the library that
// does the work. Built with exceptions; it never includes callback_thunk.h (the hook translation units
// are dlopen_hook.cpp, social_shim.cpp, entry.cpp and the library packages' own).
#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "activation.h"
#include "hook_log.h"
#include "hook_report.h"
#include "sentinel.h"
#include "sentinel_log.h"

#include "quest/auth/quest_token_auth.h"
#include "quest/integration/ctor_sequence.h"
#include "quest/integration/dlopen_hook.h"
#include "quest/integration/entry_hooks.h"
#include "quest/integration/identity_source.h"
#include "quest/integration/integrated_bridge.h"
#include "quest/integration/post_load.h"
#include "quest/integration/social_gate.h"
#include "quest/integration/social_shim.h"
#include "quest/login/login_hook.h"
#include "quest/net/curl_ws_connector.h"
#include "quest/redirect/hook_adapter.h"
#include "quest/social/social_facade.h"
#include "quest/social/social_frames.h"
#include "runtime/compat/social_party.h"
#include "runtime/lifecycle/stable_string_pool.h"

#ifndef NEVR_QUEST_PROJECT_VERSION
#define NEVR_QUEST_PROJECT_VERSION "unknown"
#endif
#ifndef NEVR_QUEST_GIT_COMMIT
#define NEVR_QUEST_GIT_COMMIT "unknown"
#endif
#ifndef NEVR_QUEST_GIT_DESCRIBE
#define NEVR_QUEST_GIT_DESCRIBE "unknown"
#endif
#ifndef NEVR_QUEST_BUILD_TYPE
#define NEVR_QUEST_BUILD_TYPE "unknown"
#endif

namespace nevr_quest::integration {

namespace {

// Process-lifetime state. Leaked on purpose: the sentinel is a DT_NEEDED dependency of the game and is
// never unloaded, and the hooks, the reporter and the transports' threads outlive any static destructor.
struct Runtime {
  std::atomic<nevr::quest_auth::QuestTokenAuth*> auth{nullptr};
  std::thread authThread;
  std::unique_ptr<quest_net::CurlWsConnector> connector;
  std::atomic<IntegratedBridge*> bridge{nullptr};
  std::atomic<unsigned> bridgePort{0};
  std::unique_ptr<TokenIdentitySource> identity;
  bool socialWanted = false;
};

Runtime& R() {
  static Runtime* const runtime = new Runtime();
  return *runtime;
}

// --- logging -------------------------------------------------------------------------------------

// The hook backend's JSON lines go to logcat and, through here, to the on-disk log as well.
void HookLogToDisk(sentinel::LogLevel level, const char* line) {
  nevr_quest::LogLevel mapped = nevr_quest::LogLevel::kInfo;
  if (level == sentinel::LogLevel::kWarn) mapped = nevr_quest::LogLevel::kWarn;
  if (level == sentinel::LogLevel::kError) mapped = nevr_quest::LogLevel::kError;
  try {
    sentinel::Emit(mapped, line);
  } catch (const std::exception&) {
    sentinel::EmitFixed(nevr_quest::LogLevel::kError, "hook log line could not be written to the disk log");
  }
}

nevr_quest::LogLevel MapRouter(SessionRouter::LogLevel level) {
  switch (level) {
    case SessionRouter::LogLevel::Warning: return nevr_quest::LogLevel::kWarn;
    case SessionRouter::LogLevel::Error: return nevr_quest::LogLevel::kError;
    case SessionRouter::LogLevel::Debug:
    case SessionRouter::LogLevel::Info: break;
  }
  return nevr_quest::LogLevel::kInfo;
}

nevr_quest::LogLevel MapAuth(nevr::auth::LogLevel level) {
  switch (level) {
    case nevr::auth::LogLevel::Warning: return nevr_quest::LogLevel::kWarn;
    case nevr::auth::LogLevel::Error: return nevr_quest::LogLevel::kError;
    case nevr::auth::LogLevel::Debug:
    case nevr::auth::LogLevel::Info: break;
  }
  return nevr_quest::LogLevel::kInfo;
}

// --- token auth ----------------------------------------------------------------------------------

nevr::quest_auth::Snapshot AuthSnapshot() {
  nevr::quest_auth::Snapshot snap;
  nevr::quest_auth::QuestTokenAuth* const auth = R().auth.load(std::memory_order_acquire);
  if (auth == nullptr) return snap;  // Starting
  snap = auth->Get();
  snap.access_token = auth->Token();  // empty once the access token is past its expiry
  return snap;
}

// --- bridge --------------------------------------------------------------------------------------

bool SendSocialFrame(const std::string& frame) {
  IntegratedBridge* const bridge = R().bridge.load(std::memory_order_acquire);
  return bridge != nullptr && bridge->SendToLogin(frame);
}

std::uint64_t SteadySeconds() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

nevr_quest::redirect::BridgeState BridgeProbe() {
  const unsigned port = R().bridgePort.load(std::memory_order_acquire);
  return {port != 0, port};
}

// --- post-load actions ---------------------------------------------------------------------------

QuestLogin::BuildInfo ThisBuild() {
  QuestLogin::BuildInfo build;
  build.project_version = NEVR_QUEST_PROJECT_VERSION;
  build.git_commit = NEVR_QUEST_GIT_COMMIT;
  build.git_describe = NEVR_QUEST_GIT_DESCRIBE;
  build.build_type = NEVR_QUEST_BUILD_TYPE;
  return build;
}

ActionResult LoginAction() noexcept {
  try {
    static const QuestLogin::BuildInfo build = ThisBuild();
    switch (QuestLogin::TryInstallLoginHook(R().identity.get(), build)) {
      case QuestLogin::InstallState::Installed: return {Settle::kDone, "installed"};
      case QuestLogin::InstallState::AlreadyInstalled: return {Settle::kDone, "already_installed"};
      case QuestLogin::InstallState::ModuleNotLoaded: return {Settle::kRetryLater, "module_not_loaded"};
      case QuestLogin::InstallState::BuildMismatch: return {Settle::kGiveUp, "build_mismatch"};
      case QuestLogin::InstallState::SlotInvalid: return {Settle::kGiveUp, "slot_invalid"};
      case QuestLogin::InstallState::SymbolMissing: return {Settle::kGiveUp, "symbol_missing"};
      case QuestLogin::InstallState::HookFailed: return {Settle::kGiveUp, "hook_failed"};
    }
    return {Settle::kGiveUp, "unknown_state"};
  } catch (const std::exception&) {
    return {Settle::kGiveUp, "exception"};
  }
}

ActionResult MatchmakingAction() noexcept {
  try {
    switch (nevr_quest::redirect::InstallMatchmakingRedirect()) {
      case sentinel::GotStatus::kOk: return {Settle::kDone, "installed"};
      case sentinel::GotStatus::kAlreadyInstalled: return {Settle::kDone, "already_installed"};
      case sentinel::GotStatus::kModuleNotLoaded: return {Settle::kRetryLater, "module_not_loaded"};
      default: break;
    }
    return {Settle::kGiveUp, "install_refused"};
  } catch (const std::exception&) {
    return {Settle::kGiveUp, "exception"};
  }
}

// --- the steps -----------------------------------------------------------------------------------

class ProductionSteps final : public Steps {
 public:
  void ArmCrashReporter() override {
    sentinel::SetLogSink(&HookLogToDisk);
    sentinel::Arm();
  }

  const nevr_quest::ResolvedConfig& ResolveConfig() override {
    sentinel::InitActivation();
    return sentinel::ActiveConfig();
  }

  bool SocialWanted(const nevr_quest::Features& effective) override {
    // Social needs login, so a file that does not enable login is not read a second time.
    if (!effective.login) {
      R().socialWanted = false;
      return false;
    }
    std::string text;
    int err = 0;
    const sentinel::ReadStatus status =
        sentinel::ReadConfigFile(nevr_quest::ConfigFilePath(sentinel::FilesDir()), &text, &err);
    const bool requested = SocialRequested(status == sentinel::ReadStatus::kRead ? &text : nullptr);
    const char* reason = "";
    R().socialWanted = SocialEffective(requested, effective, &reason);
    sentinel::LogFields(sentinel::LogLevel::kInfo, "feature",
                        {{"name", "social"}, {"requested", requested ? 1 : 0},
                         {"effective", R().socialWanted ? 1 : 0}, {"reason", reason}});
    return R().socialWanted;
  }

  bool RegisterClockCounters() override { return nevr_quest::integration::RegisterClockCounters(); }
  bool RegisterRedirectCounters() override { return nevr_quest::redirect::RegisterRedirectCounters(); }
  bool RegisterDlopenCounters() override { return nevr_quest::integration::RegisterDlopenCounters(); }
  bool RegisterSocialCounters() override { return nevr_quest::integration::RegisterSocialCounters(); }
  bool StartReporter() override { return sentinel::StartReporter(/*firstMs=*/1000, /*graceMs=*/10000, /*steadyMs=*/60000); }

  bool InstallClockHook() override { return nevr_quest::integration::InstallClockHook(); }

  bool StartTokenAuth() override {
    const nevr_quest::ResolvedConfig& cfg = sentinel::ActiveConfig();
    if (cfg.httpUri.text.empty() || cfg.httpKey.text.empty()) {
      sentinel::LogFields(sentinel::LogLevel::kError, "token_auth",
                          {{"status", "not_started"}, {"reason", "http_uri_or_key_absent"}});
      return false;
    }
    Runtime& rt = R();
    rt.identity = std::make_unique<TokenIdentitySource>(&AuthSnapshot);
    nevr::quest_auth::QuestAuthConfig authConfig;
    authConfig.base_url = cfg.httpUri.text;
    authConfig.http_key = cfg.httpKey.text;
    // Construction (curl, the CA directories) and Start run on their own thread: the constructor
    // never waits on either.
    rt.authThread = std::thread([authConfig] {
      try {
        auto* const auth = new nevr::quest_auth::QuestTokenAuth(
            authConfig, [](nevr::auth::LogLevel level, const std::string& message) {
              try {
                sentinel::Emit(MapAuth(level), message);
              } catch (const std::exception&) {
                sentinel::EmitFixed(nevr_quest::LogLevel::kError, "token auth log line could not be written");
              }
            });
        auth->Start();
        R().auth.store(auth, std::memory_order_release);
        sentinel::LogFields(sentinel::LogLevel::kInfo, "token_auth", {{"status", "started"}});
      } catch (const std::exception&) {
        sentinel::LogFields(sentinel::LogLevel::kError, "token_auth", {{"status", "start_threw"}});
      }
    });
    return true;
  }

  bool StartBridge() override {
    const nevr_quest::ResolvedConfig& cfg = sentinel::ActiveConfig();
    Runtime& rt = R();
    const std::string serverKey = cfg.serverKey.text;
    quest_net::CurlWsConnector::Config tls;
    tls.log = RouterLog();
    rt.connector = std::make_unique<quest_net::CurlWsConnector>(std::move(tls));

    IntegratedBridge::Config config;
    config.remoteUri = cfg.socketUri.text;
    config.connector = rt.connector.get();
    config.log = RouterLog();
    config.identity = [serverKey] {
      quest_net::Identity id;
      nevr::quest_auth::QuestTokenAuth* const auth = R().auth.load(std::memory_order_acquire);
      if (auth != nullptr) id.jwt = auth->Token();
      id.serverKey = serverKey;
      return id;
    };
    if (rt.socialWanted) {
      config.tap.observe = [](bool serverToGame, const std::uint8_t* data, std::size_t len) {
        quest_social::ObserveFrames(quest_social::ProductionPorts(),
                                    serverToGame ? quest_social::Direction::kServerToGame
                                                 : quest_social::Direction::kGameToServer,
                                    data, len, SteadySeconds());
      };
      config.tap.onLoginSuccess = [](std::uint64_t account) {
        const nevr::quest_auth::Snapshot snap = AuthSnapshot();
        quest_social::SetLocalAccount(account, snap.username.c_str());
      };
      SocialParty::SetSender(&SendSocialFrame);
    }
    auto bridge = std::make_unique<IntegratedBridge>(std::move(config));
    const std::uint16_t port = bridge->Start();
    if (port == 0) return false;
    sentinel::LogFields(sentinel::LogLevel::kInfo, "bridge", {{"status", "listening"}, {"port", port}});
    rt.bridge.store(bridge.release(), std::memory_order_release);  // leaked: threads and hooks outlive statics
    rt.bridgePort.store(port, std::memory_order_release);
    return true;
  }

  bool InstallRedirect() override {
    const nevr_quest::ResolvedConfig& cfg = sentinel::ActiveConfig();
    nevr_quest::redirect::InstallOptions options{nevr_quest::redirect::PinnedTargets(), sentinel::FindLoadedImage,
                                                 &nevr_runtime::lifecycle::InternStableCStr, &BridgeProbe};
    const nevr_quest::redirect::InstallReport report = nevr_quest::redirect::InstallRedirectHooksWith(cfg, options);
    return report.featureEnabled && (report.libr15 == sentinel::GotStatus::kOk ||
                                     report.libr15 == sentinel::GotStatus::kAlreadyInstalled);
  }

  bool InstallSocial() override { return nevr_quest::integration::InstallSocialHook(); }

  bool InstallDlopenHook(bool login, bool matchmaking) override {
    PostLoadActions actions;
    if (login) actions.login = &LoginAction;
    if (matchmaking) actions.matchmaking = &MatchmakingAction;
    SetPostLoadActions(actions);
    sentinel::GotStatus status = sentinel::GotStatus::kNotInstalled;
    return nevr_quest::integration::InstallDlopenHook(&status) == DlopenInstall::kOk;
  }

  void Note(const char* step, const char* state, const char* reason) override {
    sentinel::LogFields(std::string_view(state) == "ok" || std::string_view(state) == "skipped"
                            ? sentinel::LogLevel::kInfo
                            : sentinel::LogLevel::kError,
                        "sentinel_step", {{"step", step}, {"state", state}, {"reason", reason}});
  }

 private:
  static SessionRouter::LogSink RouterLog() {
    return [](SessionRouter::LogLevel level, const std::string& line) {
      try {
        sentinel::Emit(MapRouter(level), line);
      } catch (const std::exception&) {
        sentinel::EmitFixed(nevr_quest::LogLevel::kError, "router log line could not be written");
      }
    };
  }
};

}  // namespace

void ShutdownIntegration() noexcept {
  // The teardown path for the threads started above. Nothing calls it at process exit: the sentinel is
  // a DT_NEEDED dependency and is never unloaded (hook_report.h states the same for the reporter).
  Runtime& rt = R();
  try {
    if (IntegratedBridge* const bridge = rt.bridge.exchange(nullptr)) {
      bridge->Stop();
      delete bridge;
    }
    if (rt.authThread.joinable()) rt.authThread.join();
    if (nevr::quest_auth::QuestTokenAuth* const auth = rt.auth.exchange(nullptr)) {
      auth->Stop();
      delete auth;
    }
  } catch (const std::exception&) {
    sentinel::EmitFixed(nevr_quest::LogLevel::kError, "integration shutdown threw");
  }
}

void RunSentinelConstructor() noexcept {
  ProductionSteps steps;
  const ConstructorReport report = RunConstructorSequence(steps);
  int ok = 0, failed = 0, skipped = 0;
  for (int i = 0; i < static_cast<int>(StepId::kCount); ++i) {
    switch (report.steps[i].state) {
      case StepState::kOk: ++ok; break;
      case StepState::kSkipped: ++skipped; break;
      case StepState::kFailed:
      case StepState::kThrew: ++failed; break;
    }
  }
  sentinel::LogFields(sentinel::LogLevel::kInfo, "sentinel_ctor",
                      {{"action", "done"}, {"steps_ok", ok}, {"steps_failed", failed}, {"steps_skipped", skipped}});
}

}  // namespace nevr_quest::integration
