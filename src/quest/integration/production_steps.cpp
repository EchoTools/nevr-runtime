// The real `Steps` of the constructor sequence (ctor_sequence.h): each step bound to the library that
// does the work. Built with exceptions; it never includes callback_thunk.h (the hook translation units
// are dlopen_hook.cpp, social_shim.cpp, entry.cpp and the library packages' own).
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
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

#include "quest/diag/hwdump_run.h"
#include "quest/auth/quest_token_auth.h"
#include "quest/integration/bridge_uri.h"
#include "quest/integration/ctor_sequence.h"
#include "quest/integration/dlopen_hook.h"
#include "quest/integration/entry_hooks.h"
#include "quest/integration/drop_report.h"
#include "quest/integration/identity_source.h"
#include "quest/integration/post_load.h"
#include "quest/integration/self_check_wiring.h"
#include "quest/integration/stage_log.h"
#include "quest/integration/social_shim.h"
#include "quest/login/login_hook.h"
#include "quest/net/curl_ws_connector.h"
#include "quest/net/session_bridge.h"
#include "quest/redirect/hook_adapter.h"
#include "quest/social/social_facade.h"
#include "quest/social/social_frames.h"
#include "runtime/compat/self_check.h"
#include "runtime/compat/social_level.h"
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
  std::mutex pollMutex;                 // guards stopPoll; the auth thread waits on pollCv between polls
  std::condition_variable pollCv;
  bool stopPoll = false;
  std::unique_ptr<quest_net::CurlWsConnector> connector;
  std::atomic<quest_net::SessionBridge*> bridge{nullptr};
  // The router's login gate, kept current by the token-auth poll (TokenIdentitySource::GateFor); the router
  // reads it with its lock held, so it is a plain atomic. Awaiting until the first poll says otherwise.
  std::atomic<int> loginGate{static_cast<int>(nevr_session_router::LoginGate::Awaiting)};
  std::atomic<unsigned> bridgePort{0};
  std::string loopbackUri;  // "ws://127.0.0.2:<port>/<token>/"; written once before the redirect is installed
  std::unique_ptr<TokenIdentitySource> identity;
  bool socialWanted = false;
  std::atomic<int> socialLevel{0};  // nevr_social_party::kSocialLevel once the facade is installed
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

nevr_quest::LogLevel MapRouter(nevr_session_router::LogLevel level) {
  switch (level) {
    case nevr_session_router::LogLevel::Warning: return nevr_quest::LogLevel::kWarn;
    case nevr_session_router::LogLevel::Error: return nevr_quest::LogLevel::kError;
    case nevr_session_router::LogLevel::Debug:
    case nevr_session_router::LogLevel::Info: break;
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

// Logs the token session's readiness whenever it changes (stage line `token_auth_state`), until shutdown.
// This is the thread that outlives Start(); it waits on a condition variable, so ShutdownIntegration
// wakes and joins it.
void PollTokenAuthState() {
  Runtime& rt = R();
  int last = -1;
  std::uint64_t lastDropped = 0, lastUnmatched = 0;
  for (;;) {
    const nevr::quest_auth::Snapshot snap = AuthSnapshot();
    // The login prerequisites' Ready() flag (#240) follows every observed state, including an access
    // token that ran out without a readiness change.
    if (rt.identity) rt.identity->Observe(snap);
    // The held login connection opens when the account appears and closes when it will not (router gate).
    // One structured line each time the router has dropped more Unrequires (a drop is deliberate; see drop_report.h).
    if (quest_net::SessionBridge* const reporting = rt.bridge.load(std::memory_order_acquire)) {
      std::uint64_t delta = 0, unmatchedDelta = 0;
      const bool dropped = DropsChanged(reporting->DroppedUnrequires(), &lastDropped, &delta);
      const bool unmatched = DropsChanged(reporting->UnmatchedEmbeddedUnrequires(), &lastUnmatched, &unmatchedDelta);
      if (dropped || unmatched) {
        sentinel::LogFields(sentinel::LogLevel::kWarn, "router_unrequire_dropped",
                            {{"total", static_cast<long long>(lastDropped)},
                             {"since_last", static_cast<long long>(delta)},
                             {"unmatched_embedded_total", static_cast<long long>(lastUnmatched)},
                             {"unmatched_embedded_since_last", static_cast<long long>(unmatchedDelta)}});
      }
    }
    const int gate = static_cast<int>(TokenIdentitySource::GateFor(snap));
    if (rt.loginGate.exchange(gate) != gate) {
      if (quest_net::SessionBridge* const bridge = rt.bridge.load(std::memory_order_acquire)) bridge->ReevaluateLoginGate();
    }
    if (static_cast<int>(snap.readiness) != last) {
      last = static_cast<int>(snap.readiness);
      const bool bad = snap.readiness == nevr::quest_auth::Readiness::Failed ||
                       snap.readiness == nevr::quest_auth::Readiness::Expired;
      sentinel::LogFields(bad ? sentinel::LogLevel::kWarn : sentinel::LogLevel::kInfo, "token_auth_state",
                          {{"status", nevr::quest_auth::ReadinessName(snap.readiness)}});
    }
    nevr_self_check::Flush();  // probes and queued results, every poll (cheap: nothing is sent before LoginSuccess)
    std::unique_lock<std::mutex> lock(rt.pollMutex);
    if (rt.pollCv.wait_for(lock, std::chrono::seconds(2), [&rt] { return rt.stopPoll; })) return;
  }
}

// --- bridge --------------------------------------------------------------------------------------

bool SendSocialFrame(const std::string& frame) {
  quest_net::SessionBridge* const bridge = R().bridge.load(std::memory_order_acquire);
  return bridge != nullptr && bridge->SendToLogin(frame);
}

std::uint64_t SteadySeconds() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// The shared redirect policy answers the bare "ws://127.0.0.1:<port>" for a redirected game URL, which the
// game cannot reach (libr15 dials a non-loopback interface for 127.0.0.1) and which has no access token. The pool receives the tokened URI instead (docs/adr/0003,
// "Integration"). Only the exact bare value is replaced; anything else is interned as it is.
nevr::lifecycle::InternResult InternBridgeAware(std::string_view value) {
  Runtime& rt = R();
  const unsigned port = rt.bridgePort.load(std::memory_order_acquire);
  return nevr::lifecycle::InternStableCStr(ReplaceBareBridgeUri(value, port, rt.loopbackUri));
}

nevr_quest::redirect::BridgeState BridgeProbe() {
  const unsigned port = R().bridgePort.load(std::memory_order_acquire);
  return {port != 0, port};
}

// --- post-load actions ---------------------------------------------------------------------------

nevr_quest_login::BuildInfo ThisBuild() {
  nevr_quest_login::BuildInfo build;
  build.project_version = NEVR_QUEST_PROJECT_VERSION;
  build.git_commit = NEVR_QUEST_GIT_COMMIT;
  build.git_describe = NEVR_QUEST_GIT_DESCRIBE;
  build.build_type = NEVR_QUEST_BUILD_TYPE;
  return build;
}

// The login library's own records, plus the stage line `login_rewritten` for each login it handled.
void LoginLog(nevr_quest_login::Level level, const char* event, const nevr_quest_login::LogKv* fields, std::size_t count) {
  nevr_quest_login::SentinelLog(level, event, fields, count);
  if (std::strcmp(event, "quest_login") != 0) return;
  const char* outcome = "unknown";
  for (std::size_t i = 0; i < count; ++i) {
    if (std::strcmp(fields[i].key, "outcome") == 0 && fields[i].text != nullptr) outcome = fields[i].text;
  }
  const bool ok = std::strcmp(outcome, nevr_quest_login::OutcomeName(nevr_quest_login::Outcome::Rewritten)) == 0;
  sentinel::LogFields(ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn, "login_rewritten",
                      {{"status", ok ? "ok" : "failed"}, {"class", outcome}});
}

// A stage line for a post-load install once it is settled (a retry is not a result).
ActionResult Staged(const char* stage, ActionResult result) {
  if (result.settle != Settle::kRetryLater) {
    const bool ok = result.settle == Settle::kDone;
    sentinel::LogFields(ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, stage,
                        {{"status", ok ? "ok" : "failed"}, {"class", result.status}});
  }
  return result;
}

ActionResult LoginAction() noexcept {
  try {
    static const nevr_quest_login::BuildInfo build = ThisBuild();
    switch (nevr_quest_login::TryInstallLoginHook(R().identity.get(), build, &LoginLog)) {
      case nevr_quest_login::InstallState::Installed: return Staged("login_hook_installed", {Settle::kDone, "installed"});
      case nevr_quest_login::InstallState::AlreadyInstalled:
        return Staged("login_hook_installed", {Settle::kDone, "already_installed"});
      case nevr_quest_login::InstallState::ModuleNotLoaded: return {Settle::kRetryLater, "module_not_loaded"};
      case nevr_quest_login::InstallState::BuildMismatch:
        return Staged("login_hook_installed", {Settle::kGiveUp, "build_mismatch"});
      case nevr_quest_login::InstallState::SlotInvalid: return Staged("login_hook_installed", {Settle::kGiveUp, "slot_invalid"});
      case nevr_quest_login::InstallState::SymbolMissing:
        return Staged("login_hook_installed", {Settle::kGiveUp, "symbol_missing"});
      case nevr_quest_login::InstallState::HookFailed: return Staged("login_hook_installed", {Settle::kGiveUp, "hook_failed"});
    }
    return Staged("login_hook_installed", {Settle::kGiveUp, "unknown_state"});
  } catch (const std::exception&) {
    return Staged("login_hook_installed", {Settle::kGiveUp, "exception"});
  }
}

void SelfCheckLog(const nevr_self_check::LogRecord& record) {
  sentinel::LogFields(sentinel::LogLevel::kInfo, "self_check",
                      {{"check", record.name.c_str()}, {"pass", record.pass ? 1 : 0},
                       {"expected", record.expected.c_str()}, {"observed", record.observed.c_str()}});
}

ActionResult MatchmakingAction() noexcept {
  try {
    const sentinel::GotStatus status = nevr_quest::redirect::InstallMatchmakingRedirect();
    switch (status) {
      case sentinel::GotStatus::kOk:
        NoteMatchmakingRedirectInstalled();
        return Staged("matchmaking_redirect_installed", {Settle::kDone, "installed"});
      case sentinel::GotStatus::kAlreadyInstalled:
        return Staged("matchmaking_redirect_installed", {Settle::kDone, "already_installed"});
      case sentinel::GotStatus::kModuleNotLoaded: return {Settle::kRetryLater, "module_not_loaded"};
      default: break;
    }
    return Staged("matchmaking_redirect_installed", {Settle::kGiveUp, sentinel::GotStatusName(status)});
  } catch (const std::exception&) {
    return Staged("matchmaking_redirect_installed", {Settle::kGiveUp, "exception"});
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
    const nevr_quest::ResolvedConfig& cfg = sentinel::ActiveConfig();
    sentinel::LogFields(sentinel::LogLevel::kInfo, "config_loaded",
                        {{"status", "ok"}, {"redirect", cfg.effective.redirect ? 1 : 0},
                         {"bridge", cfg.effective.bridge ? 1 : 0}, {"login", cfg.effective.login ? 1 : 0},
                         {"social", cfg.effective.social ? 1 : 0}, {"presence_names", cfg.effective.presenceNames ? 1 : 0},
                         {"presence_local", cfg.effective.presenceLocal ? 1 : 0},
                         {"socket_uri", nevr_quest::SourceName(cfg.socketUri.source)},
                         {"http_uri", nevr_quest::SourceName(cfg.httpUri.source)},
                         {"http_key", nevr_quest::SourceName(cfg.httpKey.source)},
                         {"server_key", nevr_quest::SourceName(cfg.serverKey.source)}});
    R().socialWanted = cfg.effective.social;
    return cfg;
  }

  bool RegisterClockCounters() override { return nevr_quest::integration::RegisterClockCounters(); }
  bool RegisterRedirectCounters() override { return nevr_quest::redirect::RegisterRedirectCounters(); }
  bool RegisterDlopenCounters() override { return nevr_quest::integration::RegisterDlopenCounters(); }
  bool RegisterLoginCounters() override { return nevr_quest_login::RegisterLoginHookCounters(); }
  bool RegisterSocialCounters() override { return nevr_quest::integration::RegisterSocialCounters(); }
  bool RegisterLoginPromptCounters() override { return nevr_quest::integration::RegisterLoginPromptCounters(); }
  bool RegisterObbSkipCounters() override { return nevr_quest::integration::RegisterObbSkipCounters(); }
  bool StartReporter() override { return sentinel::StartReporter(/*firstMs=*/1000, /*graceMs=*/10000, /*steadyMs=*/60000); }

  bool InstallClockHook() override {
    const bool ok = nevr_quest::integration::InstallClockHook();
    detail_ = ok ? "ok" : "got_hook_refused";
    return ok;
  }

  bool StartTokenAuth() override {
    const nevr_quest::ResolvedConfig& cfg = sentinel::ActiveConfig();
    if (cfg.httpUri.text.empty() || cfg.httpKey.text.empty()) {
      detail_ = "http_uri_or_key_absent";
      return false;
    }
    Runtime& rt = R();
    rt.identity = std::make_unique<TokenIdentitySource>(&AuthSnapshot, [] { return R().socialLevel.load(); });
    nevr::quest_auth::QuestAuthConfig authConfig;
    authConfig.base_url = cfg.httpUri.text;
    authConfig.http_key = cfg.httpKey.text;
    // Construction (curl, the CA directories) and Start run on their own thread: the constructor
    // never waits on either.
    rt.authThread = std::thread([authConfig] {
      try {
        std::unique_ptr<nevr::quest_auth::QuestTokenAuth> created = nevr::quest_auth::QuestTokenAuth::Create(
            authConfig, [](nevr::auth::LogLevel level, const std::string& message) {
              try {
                sentinel::Emit(MapAuth(level), message);
              } catch (const std::exception&) {
                sentinel::EmitFixed(nevr_quest::LogLevel::kError, "token auth log line could not be written");
              }
            });
        if (!created) {
          sentinel::LogFields(sentinel::LogLevel::kError, "token_auth_state",
                              {{"status", "failed"}, {"class", "create_failed"}});
          return;
        }
        nevr::quest_auth::QuestTokenAuth* const auth = created.release();  // process lifetime
        auth->Start();
        R().auth.store(auth, std::memory_order_release);
      } catch (const std::exception&) {
        sentinel::LogFields(sentinel::LogLevel::kError, "token_auth_state",
                            {{"status", "failed"}, {"class", "start_threw"}});
        return;
      }
      PollTokenAuthState();
    });
    detail_ = "launched";
    return true;
  }

  bool InstallObbSkip(bool countersRegistered) override {
    const bool ok = nevr_quest::integration::InstallObbSkipHook(countersRegistered);
    detail_ = ok ? "ok" : "got_hook_refused";
    return ok;
  }

  bool InstallLoginPrompt(bool countersRegistered) override {
    const bool ok = nevr_quest::integration::InstallLoginPromptHook(countersRegistered);
    detail_ = ok ? "ok" : "got_hook_refused";
    return ok;
  }

  bool StartBridge() override {
    const nevr_quest::ResolvedConfig& cfg = sentinel::ActiveConfig();
    Runtime& rt = R();
    const std::string serverKey = cfg.serverKey.text;
    quest_net::CurlWsConnector::Config tls;
    tls.log = RouterLog();
    rt.connector = std::make_unique<quest_net::CurlWsConnector>(std::move(tls));

    quest_net::SessionBridge::Config config;
    config.remoteUri = cfg.socketUri.text;
    config.subscribeFriendList = rt.socialWanted;
    {
      // Self-checks (#451) are on in every build: each run-card check's result goes out on the login connection
      // as the user the service names at LoginSuccess. The runtime adds nothing to the connection for it.
      SelfCheckHooks hooks;
      hooks.sender = &SendSocialFrame;
      hooks.log = &SelfCheckLog;
      hooks.build = NEVR_QUEST_PROJECT_VERSION;
      ApplySelfCheck(&config.tap, hooks);
      nevr_self_check::Register({"matchmaking_reload_redirect",
                                 "the matchmaking redirect is installed on every libpnsradmatchmaking image the game mapped (installs >= images)",
                                 &MatchmakingReloadProbe});
    }
    config.connector = rt.connector.get();
    config.log = RouterLog();
    config.loginGate = [] { return static_cast<nevr_session_router::LoginGate>(R().loginGate.load(std::memory_order_acquire)); };
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
      nevr_social_party::SetSender(&SendSocialFrame);
    }
    if (!quest_net::IsAcceptableRemoteUrl(config.remoteUri)) {
      detail_ = "socket_uri_not_wss";
      return false;
    }
    auto bridge = std::make_unique<quest_net::SessionBridge>(std::move(config));
    const std::uint16_t port = bridge->Start();
    if (port == 0) {
      detail_ = "listener_failed";
      return false;
    }
    port_ = port;
    rt.loopbackUri = bridge->LocalUri();
    quest_net::SessionBridge* const published = bridge.release();
    rt.bridge.store(published, std::memory_order_release);  // leaked: threads and hooks outlive statics
    published->ReevaluateLoginGate();  // a gate change that raced the publication
    rt.bridgePort.store(port, std::memory_order_release);
    return true;
  }

  bool InstallRedirect() override {
    const nevr_quest::ResolvedConfig& cfg = sentinel::ActiveConfig();
    const nevr_quest::redirect::InstallReport report =
        nevr_quest::redirect::InstallRedirectHooks(cfg, &InternBridgeAware, &BridgeProbe);
    detail_ = report.featureEnabled ? sentinel::GotStatusName(report.libr15) : "feature_off_or_allocation_failure";
    return report.featureEnabled && (report.libr15 == sentinel::GotStatus::kOk ||
                                     report.libr15 == sentinel::GotStatus::kAlreadyInstalled);
  }

  bool InstallSocial() override {
    const char* detail = "unknown";
    const bool ok = nevr_quest::integration::InstallSocialHook(&detail,
                                                              sentinel::ActiveConfig().effective.presenceNames,
                                                              sentinel::ActiveConfig().effective.presenceLocal);
    detail_ = detail;
    // The login declares the social level only when the facade is in place (docs/adr/0003, contract 5).
    if (ok) R().socialLevel.store(nevr_social_party::kSocialLevel);
    return ok;
  }

  bool StartHwDump() override { return nevr_quest::hwdump::StartHwDump(); }

  bool InstallDlopenHook(bool login, bool matchmaking) override {
    PostLoadActions actions;
    if (login) actions.login = &LoginAction;
    if (matchmaking) actions.matchmaking = &MatchmakingAction;
    SetPostLoadActions(actions);
    sentinel::GotStatus status = sentinel::GotStatus::kNotInstalled;
    const bool ok = nevr_quest::integration::InstallDlopenHook(&status) == DlopenInstall::kOk;
    detail_ = sentinel::GotStatusName(status);
    return ok;
  }

  void Note(const char* step, const char* state, const char* reason) override {
    sentinel::LogLevel level = sentinel::LogLevel::kInfo;
    switch (StepLogLevel(state, reason)) {
      case StepLevel::kInfo: break;
      case StepLevel::kWarn: level = sentinel::LogLevel::kWarn; break;
      case StepLevel::kError: level = sentinel::LogLevel::kError; break;
    }
    sentinel::LogFields(level, "sentinel_step", {{"step", step}, {"state", state}, {"reason", reason}});
    // The stage line: stable name, status, and the class that says why (the step's detail when it set
    // one, else the sequence's reason).
    const char* stage = StageForStep(step);
    const bool ok = std::string_view(state) == "ok";
    if (stage == nullptr || (ok && std::string_view(step) == "resolve_config")) {
      detail_ = nullptr;  // config_loaded was logged with its fields by ResolveConfig
      return;
    }
    const char* cls = detail_ != nullptr ? detail_ : reason;
    if (ok && port_ != 0 && std::string_view(step) == "start_bridge") {
      sentinel::LogFields(level, stage, {{"status", state}, {"class", cls}, {"port", port_}});
    } else {
      sentinel::LogFields(level, stage, {{"status", state}, {"class", cls}});
    }
    detail_ = nullptr;
  }

 private:
  const char* detail_ = nullptr;  // set by a step to say why it ended as it did (a fixed token)
  unsigned port_ = 0;
  static nevr_session_router::LogSink RouterLog() {
    return [](nevr_session_router::LogLevel level, const std::string& line) {
      try {
        sentinel::Emit(MapRouter(level), line);
        // The stage lines (stage_log.h) that only the router's own log can tell: a remote session
        // connected or failed, the service accepted or refused the login.
        if (const std::optional<StageEvent> ev = ClassifyRouterLine(line)) {
          sentinel::LogFields(ev->status[0] == 'o' ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn,
                              ev->event, {{"status", ev->status}, {"class", ev->cls}});
        }
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
    // The token-auth thread runs PollTokenAuthState, which loads rt.bridge and calls into it every 2 s: it is
    // stopped and joined first, so no call can reach the bridge once it is deleted. (Other threads that reach
    // the bridge, the social facade's sender among them, are the game's own and outlive this function.)
    {
      const std::lock_guard<std::mutex> lock(rt.pollMutex);
      rt.stopPoll = true;
    }
    rt.pollCv.notify_all();
    if (rt.authThread.joinable()) rt.authThread.join();
    if (quest_net::SessionBridge* const bridge = rt.bridge.exchange(nullptr)) {
      bridge->Stop();
      delete bridge;
    }
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
