#include "quest/auth/session.h"

#include "core/auth_refresh.h"
#include "core/device_auth_flow.h"

#include <nlohmann/json.hpp>

#include <ctime>
#include <exception>
#include <system_error>

#if defined(__linux__)
#include <pthread.h>
#endif

namespace nevr::quest_auth {

namespace {
using nevr::auth::LogLevel;
}

uint64_t SystemClock::UnixNow() { return static_cast<uint64_t>(std::time(nullptr)); }

std::chrono::steady_clock::time_point SystemClock::SteadyNow() { return std::chrono::steady_clock::now(); }

bool SystemClock::SleepFor(std::chrono::steady_clock::duration d) {
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait_for(lock, d, [this] { return interrupted_; });
  return interrupted_;
}

void SystemClock::Interrupt() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    interrupted_ = true;
  }
  cv_.notify_all();
}

const char* ReadinessName(Readiness r) {
  switch (r) {
    case Readiness::Starting: return "starting";
    case Readiness::Refreshing: return "refreshing";
    case Readiness::AwaitingUser: return "awaiting_user";
    case Readiness::Ready: return "ready";
    case Readiness::Expired: return "expired";
    case Readiness::Failed: return "failed";
    case Readiness::Stopped: return "stopped";
  }
  return "unknown";
}

Session::Session(SessionConfig config, nevr::auth::HttpClient& http, InterruptibleClock& clock,
                 CredentialStore& store, LinkPresenter& presenter, nevr::auth::LogSink log)
    : config_(std::move(config)), http_(http), clock_(clock), store_(store), presenter_(presenter),
      log_(std::move(log)) {}

Session::~Session() { Stop(); }

void Session::Log(LogLevel level, const std::string& message) const {
  if (log_) log_(level, message);
}

bool Session::StopRequested() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return stop_;
}

void Session::Start() {
  std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
  if (started_ || StopRequested()) return;
  try {
    worker_ = std::thread([this] {
      worker_id_ = std::this_thread::get_id();
#if defined(__linux__)
      pthread_setname_np(pthread_self(), "nevr-auth");  // visible in a tombstone or a thread dump
#endif
      try {
        Run();
      } catch (const std::exception& e) {
        // Nothing may escape a thread body: it would terminate the game process.
        Log(LogLevel::Error, std::string("[NEVR.AUTH] auth worker stopped on an exception: ") + e.what());
        SetState(Readiness::Failed);
      }
    });
    started_ = true;
  } catch (const std::exception& e) {
    // No thread could be created (system_error, bad_alloc): report it and stay down rather
    // than throw into the caller.
    Log(LogLevel::Error, std::string("[NEVR.AUTH] auth worker thread could not be started: ") + e.what());
    SetState(Readiness::Failed);
  }
}

void Session::Stop() {
  if (std::this_thread::get_id() == worker_id_.load()) {
    // Called from the worker itself (a log sink): joining would wait for ourselves, and the
    // thread that is joining already holds lifecycle_mutex_. Ask for the stop and return.
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    clock_.Interrupt();
    http_.Interrupt();
    return;
  }
  std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  clock_.Interrupt();
  http_.Interrupt();  // a request in flight returns now instead of at its own timeout
  if (worker_.joinable()) worker_.join();
  std::lock_guard<std::mutex> lock(mutex_);
  snapshot_.readiness = Readiness::Stopped;
}

Snapshot Session::Get() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_;
}

std::string Session::Token() const {
  const Snapshot s = Get();
  if (s.readiness != Readiness::Ready) return "";
  if (s.access_expiry <= clock_.UnixNow()) return "";
  return s.access_token;
}

void Session::SetState(Readiness state) {
  std::string transition;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot_.readiness == Readiness::Stopped) return;
    if (snapshot_.readiness != state) {
      transition = std::string("[NEVR.AUTH] auth state ") + ReadinessName(snapshot_.readiness) + " -> " +
                   ReadinessName(state);
    }
    snapshot_.readiness = state;
  }
  if (!transition.empty()) Log(quiet_ ? LogLevel::Debug : LogLevel::Info, transition);
}

void Session::Adopt(const CachedAuthToken& auth, Readiness state) {
  std::string transition;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot_.readiness == Readiness::Stopped) return;
    snapshot_.access_token = auth.token;
    snapshot_.access_expiry = auth.token_expiry;
    snapshot_.discord_id = auth.GetDiscordId();
    snapshot_.user_id = auth.user_id;
    snapshot_.username = auth.username;
    if (snapshot_.readiness != state) {
      transition = std::string("[NEVR.AUTH] auth state ") + ReadinessName(snapshot_.readiness) + " -> " +
                   ReadinessName(state);
    }
    snapshot_.readiness = state;
  }
  if (!transition.empty()) Log(LogLevel::Info, transition);
}

// Loads the cache and, when only a refresh token is there, refreshes it. A failed
// refresh never writes: the cached login stays exactly as it was on disk.
Session::LoginResult Session::TryCachedLogin(CachedAuthToken& auth, int attempts) {
  const uint64_t now = clock_.UnixNow();
  auth = store_.Load(now);
  if (auth.token.empty() && auth.refresh_token.empty()) {
    Log(LogLevel::Info, "[NEVR.AUTH] no cached credentials");
    return LoginResult::NeedDevice;
  }
  // A legacy cache file may still carry a short-lived access token (clamped at load).
  if (auth.HasValidToken(now)) {
    Log(LogLevel::Info, "[NEVR.AUTH] using cached access token");
    return LoginResult::Ok;
  }
  if (auth.refresh_token.empty()) {
    Log(LogLevel::Info, "[NEVR.AUTH] cached access token expired and no refresh token");
    return LoginResult::NeedDevice;
  }
  if (!auth.HasValidRefreshToken(now)) {
    Log(LogLevel::Info, "[NEVR.AUTH] cached refresh token has expired");
    return LoginResult::NeedDevice;
  }

  SetState(Readiness::Refreshing);
  const auto sink = [this](LogLevel l, const std::string& m) { Log(l, m); };
  if (attempts < 1) attempts = 1;
  nevr::auth::RefreshOutcome outcome = nevr::auth::RefreshOutcome::TransportFailed;
  for (int i = 1; i <= attempts; ++i) {
    CachedAuthToken candidate = auth;  // a failed attempt must not alter `auth`
    outcome = nevr::auth::RefreshAccessToken(candidate, config_.base_url, config_.http_key, http_,
                                             clock_.UnixNow(), sink);
    if (outcome == nevr::auth::RefreshOutcome::Refreshed) {
      auth = candidate;
      if (!store_.Save(auth)) {
        Log(LogLevel::Warning,
            "[NEVR.AUTH] refreshed login could not be written; the previous cache file is unchanged");
      }
      Log(LogLevel::Info, "[NEVR.AUTH] cached-login refresh succeeded attempt=" + std::to_string(i));
      return LoginResult::Ok;
    }
    Log(LogLevel::Warning, std::string("[NEVR.AUTH] cached-login refresh failed attempt=") +
                               std::to_string(i) + "/" + std::to_string(attempts) +
                               " outcome=" + nevr::auth::RefreshOutcomeName(outcome));
    using nevr::auth::RefreshOutcome;
    if (outcome == RefreshOutcome::Denied) {
      Log(LogLevel::Warning, "[NEVR.AUTH] the server rejected the refresh token itself; retrying cannot help");
      return LoginResult::NeedDevice;
    }
    if (outcome == RefreshOutcome::Unauthorized || outcome == RefreshOutcome::ClientError) {
      // The refresh was refused for a reason that says nothing about the token (a wrong
      // http_key, a missing RPC, a rejected payload). One attempt; the cache stays, the player
      // is not prompted, and the login waits for the next recovery attempt.
      failure_class_ = std::string("refresh_") + nevr::auth::RefreshOutcomeName(outcome);
      Log(LogLevel::Error, std::string("[NEVR.AUTH] refresh refused (") + nevr::auth::RefreshOutcomeName(outcome) +
                               "), not about the refresh token; cache kept, player not prompted");
      return LoginResult::Held;
    }
    if (i < attempts && clock_.SleepFor(config_.refresh_retry_pause)) return LoginResult::Transient;
  }
  failure_class_ = "refresh_transient";
  Log(LogLevel::Warning, "[NEVR.AUTH] cached-login refresh unsuccessful for a transient reason; cache file kept");
  return LoginResult::Transient;
}

Session::DeviceResult Session::RunDeviceLogin(CachedAuthToken& out) {
  SetState(Readiness::AwaitingUser);
  device_result_ = DeviceResult::Ended;
  // The link file carries the device code: remove it however this function ends, including
  // by an exception out of one of the operations below.
  struct ClearLink {
    LinkPresenter& presenter;
    ~ClearLink() { presenter.Clear(); }
  } clear_link{presenter_};

  nevr::auth::DeviceFlowOps ops;
  ops.now = [this] { return clock_.SteadyNow(); };
  ops.request_device_code = [this]() -> std::string {
    const nevr::auth::HttpResponse r = http_.PostJson(
        nevr::auth::BuildDeviceAuthUrl(config_.base_url, config_.http_key, "request"), "{}");
    if (!r.transport_ok || r.status != 200) {
      // No answer, a server error or a rate limit say nothing about the request itself.
      const bool transient = !r.transport_ok || r.status >= 500 || r.status == 429 || r.status == 408;
      device_result_ = transient ? DeviceResult::RequestTransient : DeviceResult::RequestRefused;
      failure_class_ = transient ? "device_request_transient" : "device_request_refused";
      Log(LogLevel::Warning, "[NEVR.AUTH] device code request failed transport_ok=" +
                                 std::to_string(r.transport_ok ? 1 : 0) +
                                 " code=" + std::to_string(r.transport_code) +
                                 " http_status=" + std::to_string(r.status));
      return "";
    }
    // A 200 that carries no usable code (HTML from a captive portal, an object without "code")
    // shows the player nothing, so it is as transient as any other unreadable response.
    std::string code;
    try {
      const nlohmann::json j = nlohmann::json::parse(r.body);
      if (j.is_object() && j.contains("code") && j.at("code").is_string()) code = j.at("code").get<std::string>();
    } catch (const nlohmann::json::exception&) {
    }
    if (code.empty()) {
      device_result_ = DeviceResult::RequestTransient;
      failure_class_ = "device_request_transient";
      Log(LogLevel::Warning, "[NEVR.AUTH] device code request: the response carried no usable code");
    }
    return code;
  };
  ops.open_browser = [this](const std::string& url) { return presenter_.Present(url); };
  // Nobody can see a link that was not delivered: stop rather than wait out the code.
  ops.show_open_failure = [](const std::string&, const std::string&, intptr_t) { return 0; };
  // The server answers "code unknown or expired" with a 200 (status "expired"). While the
  // player holds a link, an outage (no connection, 5xx, 429) is waited out until the code's own
  // deadline, polling no faster than the poll interval; any other 4xx ends the login.
  auto consecutive_failures = std::make_shared<int>(0);  // the poll op runs on this one worker thread
  ops.poll = [this, consecutive_failures](const std::string& code) {
    nlohmann::json body;
    body["code"] = code;
    const nevr::auth::HttpResponse r = http_.PostJson(
        nevr::auth::BuildDeviceAuthUrl(config_.base_url, config_.http_key, "poll"), body.dump());
    TokenAuth::DevicePollResponse response;
    if (r.transport_ok && r.status == 200) {
      *consecutive_failures = 0;
      return TokenAuth::ParseDevicePollResponse(r.body);
    }
    const bool transient = !r.transport_ok || r.status >= 500 || r.status == 429 || r.status == 408;
    ++*consecutive_failures;
    const std::string what = " transport_ok=" + std::to_string(r.transport_ok ? 1 : 0) +
                             " code=" + std::to_string(r.transport_code) +
                             " http_status=" + std::to_string(r.status);
    if (transient) {
      // One line for the first failure of a run and then one in ten: bounded log rate.
      if (*consecutive_failures == 1 || *consecutive_failures % 10 == 0) {
        Log(LogLevel::Info, "[NEVR.AUTH] device poll request failed (transient, " +
                                std::to_string(*consecutive_failures) + " consecutive); polling on" + what);
      }
      response.status = TokenAuth::DevicePollStatus::Pending;
    } else {
      Log(LogLevel::Warning, "[NEVR.AUTH] device poll refused by the server; ending the login" + what);
      response.status = TokenAuth::DevicePollStatus::Error;
    }
    return response;
  };
  ops.sleep = [this](std::chrono::steady_clock::duration d) { (void)clock_.SleepFor(d); };
  ops.cancelled = [this] { return StopRequested(); };
  ops.log = [this](LogLevel l, const std::string& m) { Log(l, m); };

  const nevr::auth::DeviceFlowResult flow = nevr::auth::RunDeviceCodeFlow(ops, config_.login_url);
  if (!flow.verified) return device_result_;

  const uint64_t now = clock_.UnixNow();
  CachedAuthToken auth;
  auth.token = flow.response.access_token;
  auth.token_expiry = ResolveAccessTokenExpirySec(now, auth.token, flow.response.expires_in);
  auth.refresh_token = flow.response.refresh_token;
  auth.refresh_token_expiry = ResolveRefreshTokenExpirySec(now, flow.response.refresh_token_expires_in);
  auth.user_id = flow.response.user_id;
  auth.username = flow.response.username;
  out = auth;
  if (!store_.Save(auth)) {
    Log(LogLevel::Warning,
        "[NEVR.AUTH] credential cache save failed; in-memory authentication remains active");
  }
  Log(LogLevel::Info, "[NEVR.AUTH] Device authorization completed");
  return DeviceResult::Verified;
}

Session::LoginEnd Session::EstablishLogin(CachedAuthToken& auth, bool use_cache, bool with_backoff) {
  const std::vector<std::chrono::seconds>& delays = config_.login_retry_delays;
  for (size_t round = 0;; ++round) {
    bool transient = false;
    if (use_cache) {
      // A recovery attempt (no backoff) makes one request, not the full run of attempts.
      const LoginResult cached = TryCachedLogin(auth, with_backoff ? config_.refresh_attempts : 1);
      if (cached == LoginResult::Ok) return LoginEnd::Ok;
      if (cached == LoginResult::Held) return StopRequested() ? LoginEnd::Stopped : LoginEnd::Recoverable;
      transient = cached == LoginResult::Transient;
    }
    if (StopRequested()) return LoginEnd::Stopped;
    // A transient cached-login failure does not start the interactive login: the player is
    // not asked to sign in again because the network blinked.
    if (!transient) {
      const DeviceResult d = RunDeviceLogin(auth);
      if (d == DeviceResult::Verified) return LoginEnd::Ok;
      if (StopRequested()) return LoginEnd::Stopped;
      if (d == DeviceResult::Ended) return LoginEnd::Final;
      if (d == DeviceResult::RequestRefused) return LoginEnd::Recoverable;
      transient = true;  // RequestTransient
    }
    if (!with_backoff || round >= delays.size()) return LoginEnd::Recoverable;
    Log(LogLevel::Warning, "[NEVR.AUTH] login failed for a transient reason; retry " + std::to_string(round + 1) +
                               "/" + std::to_string(delays.size()) + " in " + std::to_string(delays[round].count()) +
                               "s");
    if (clock_.SleepFor(delays[round])) return LoginEnd::Stopped;
  }
}

bool Session::LoginWithRecovery(CachedAuthToken& auth, bool use_cache) {
  std::string last_class_logged;
  for (bool first = true;; first = false) {
    const LoginEnd end = EstablishLogin(auth, use_cache, /*with_backoff=*/first);
    if (end == LoginEnd::Ok) {
      quiet_ = false;
      return true;
    }
    if (end == LoginEnd::Stopped || StopRequested()) return false;
    if (end == LoginEnd::Final) {
      Log(LogLevel::Warning, "[NEVR.AUTH] Authentication failed -- social features may be limited");
      SetState(Readiness::Failed);
      return false;
    }
    // Recoverable: Failed for now. Say so when the kind of failure changes, not on every attempt.
    SetState(Readiness::Failed);
    quiet_ = true;
    Log(failure_class_ != last_class_logged ? LogLevel::Warning : LogLevel::Debug,
        "[NEVR.AUTH] login failed (" + failure_class_ + "); trying again every " +
            std::to_string(config_.recovery_period.count()) + "s");
    last_class_logged = failure_class_;
    if (clock_.SleepFor(config_.recovery_period)) return false;
  }
}

void Session::BackgroundRefresh(CachedAuthToken auth) {
  int consecutiveFailures = 0;
  while (!clock_.SleepFor(config_.background_period)) {
    const uint64_t now = clock_.UnixNow();
    if (auth.token_expiry <= now) SetState(Readiness::Expired);  // logs the transition once
    if (!nevr::auth::AccessTokenNeedsRefresh(auth.token_expiry, now)) continue;

    bool relogin = false;
    if (!auth.HasValidRefreshToken(now)) {
      Log(LogLevel::Warning, "[NEVR.AUTH] the refresh token has expired; starting a new device-code login");
      relogin = true;
    } else {
      CachedAuthToken candidate = auth;
      const auto sink = [this](LogLevel l, const std::string& m) { Log(l, m); };
      const nevr::auth::RefreshOutcome outcome = nevr::auth::RefreshAccessToken(
          candidate, config_.base_url, config_.http_key, http_, now, sink);
      if (outcome == nevr::auth::RefreshOutcome::Refreshed) {
        auth = candidate;
        consecutiveFailures = 0;
        Adopt(auth, Readiness::Ready);
        if (!store_.Save(auth)) {
          Log(LogLevel::Warning,
              "[NEVR.AUTH] refreshed login could not be written; the previous cache file is unchanged");
        }
        Log(LogLevel::Info, "[NEVR.AUTH] token refreshed expires_in=" +
                                std::to_string(auth.token_expiry - now) + "s");
      } else if (outcome == nevr::auth::RefreshOutcome::Denied) {
        Log(LogLevel::Warning,
            "[NEVR.AUTH] the server rejected the refresh token itself; starting a new device-code login");
        relogin = true;
      } else {
        // Anything else leaves the login alone, as at startup: the cache is kept, the player is
        // not prompted, and the next period tries again.
        ++consecutiveFailures;
        Log(LogLevel::Warning, "[NEVR.AUTH] token refresh failed (" + std::to_string(consecutiveFailures) +
                                   " consecutive) outcome=" + nevr::auth::RefreshOutcomeName(outcome) +
                                   "; cache kept, retrying next period");
      }
    }

    if (relogin) {
      SetState(Readiness::Expired);
      CachedAuthToken fresh;
      if (!LoginWithRecovery(fresh, /*use_cache=*/false)) return;
      auth = fresh;
      consecutiveFailures = 0;
      Adopt(auth, Readiness::Ready);
    }
  }
}

void Session::Run() {
  CachedAuthToken auth;
  if (!LoginWithRecovery(auth, /*use_cache=*/true)) return;
  Adopt(auth, Readiness::Ready);
  BackgroundRefresh(auth);
}

}  // namespace nevr::quest_auth
