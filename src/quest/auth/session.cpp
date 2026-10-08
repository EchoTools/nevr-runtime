#include "quest/auth/session.h"

#include "core/auth_refresh.h"
#include "core/device_auth_flow.h"

#include <nlohmann/json.hpp>

#include <ctime>
#include <exception>

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

void Session::Start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (started_) return;
  started_ = true;
  worker_ = std::thread([this] {
    try {
      Run();
    } catch (const std::exception& e) {
      // Nothing may escape a thread body: it would terminate the game process.
      Log(LogLevel::Error, std::string("[NEVR.AUTH] auth worker stopped on an exception: ") + e.what());
      SetState(Readiness::Failed);
    }
  });
}

void Session::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  clock_.Interrupt();
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
  std::lock_guard<std::mutex> lock(mutex_);
  if (snapshot_.readiness == Readiness::Stopped) return;
  if (snapshot_.readiness != state) {
    Log(LogLevel::Info, std::string("[NEVR.AUTH] auth state ") + ReadinessName(snapshot_.readiness) +
                            " -> " + ReadinessName(state));
  }
  snapshot_.readiness = state;
}

void Session::Adopt(const CachedAuthToken& auth, Readiness state) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (snapshot_.readiness == Readiness::Stopped) return;
  snapshot_.access_token = auth.token;
  snapshot_.access_expiry = auth.token_expiry;
  snapshot_.discord_id = auth.GetDiscordId();
  snapshot_.user_id = auth.user_id;
  snapshot_.username = auth.username;
  if (snapshot_.readiness != state) {
    Log(LogLevel::Info, std::string("[NEVR.AUTH] auth state ") + ReadinessName(snapshot_.readiness) +
                            " -> " + ReadinessName(state));
  }
  snapshot_.readiness = state;
}

// Loads the cache and, when only a refresh token is there, refreshes it. A failed
// refresh never writes: the cached login stays exactly as it was on disk.
bool Session::TryCachedLogin(CachedAuthToken& auth) {
  const uint64_t now = clock_.UnixNow();
  auth = store_.Load(now);
  if (auth.token.empty() && auth.refresh_token.empty()) {
    Log(LogLevel::Info, "[NEVR.AUTH] no cached credentials");
    return false;
  }
  // A legacy cache file may still carry a short-lived access token (clamped at load).
  if (auth.HasValidToken(now)) {
    Log(LogLevel::Info, "[NEVR.AUTH] using cached access token");
    return true;
  }
  if (auth.refresh_token.empty()) {
    Log(LogLevel::Info, "[NEVR.AUTH] cached access token expired and no refresh token");
    return false;
  }
  if (!auth.HasValidRefreshToken(now)) {
    Log(LogLevel::Info, "[NEVR.AUTH] cached refresh token has expired");
    return false;
  }

  SetState(Readiness::Refreshing);
  const auto sink = [this](LogLevel l, const std::string& m) { Log(l, m); };
  const int attempts = config_.refresh_attempts < 1 ? 1 : config_.refresh_attempts;
  for (int i = 1; i <= attempts; ++i) {
    CachedAuthToken candidate = auth;  // a failed attempt must not alter `auth`
    const nevr::auth::RefreshOutcome outcome = nevr::auth::RefreshAccessToken(
        candidate, config_.base_url, config_.http_key, http_, clock_.UnixNow(), sink);
    if (outcome == nevr::auth::RefreshOutcome::Refreshed) {
      auth = candidate;
      if (!store_.Save(auth)) {
        Log(LogLevel::Warning,
            "[NEVR.AUTH] refreshed login could not be written; the previous cache file is unchanged");
      }
      Log(LogLevel::Info, "[NEVR.AUTH] cached login refreshed attempt=" + std::to_string(i));
      return true;
    }
    Log(LogLevel::Warning, std::string("[NEVR.AUTH] cached-login refresh failed attempt=") +
                               std::to_string(i) + "/" + std::to_string(attempts) +
                               " outcome=" + nevr::auth::RefreshOutcomeName(outcome));
    if (i < attempts && clock_.SleepFor(config_.refresh_retry_pause)) return false;
  }
  Log(LogLevel::Warning,
      "[NEVR.AUTH] cached-login refresh exhausted; cache file kept, falling back to device-code login");
  return false;
}

bool Session::RunDeviceLogin(CachedAuthToken& out) {
  SetState(Readiness::AwaitingUser);

  nevr::auth::DeviceFlowOps ops;
  ops.now = [this] { return clock_.SteadyNow(); };
  ops.request_device_code = [this]() -> std::string {
    const nevr::auth::HttpResponse r = http_.PostJson(
        nevr::auth::BuildDeviceAuthUrl(config_.base_url, config_.http_key, "request"), "{}");
    if (!r.transport_ok || r.status != 200) {
      Log(LogLevel::Warning, "[NEVR.AUTH] device code request failed transport_ok=" +
                                 std::to_string(r.transport_ok ? 1 : 0) +
                                 " code=" + std::to_string(r.transport_code) +
                                 " http_status=" + std::to_string(r.status));
      return "";
    }
    try {
      return nlohmann::json::parse(r.body).value("code", "");
    } catch (const nlohmann::json::exception&) {
      Log(LogLevel::Warning, "[NEVR.AUTH] device code request: malformed JSON response");
      return "";
    }
  };
  ops.open_browser = [this](const std::string& url) { return presenter_.Present(url); };
  // Nobody can see a link that was not delivered: stop rather than wait out the code.
  ops.show_open_failure = [](const std::string&, const std::string&, intptr_t) { return 0; };
  ops.poll = [this](const std::string& code) {
    nlohmann::json body;
    body["code"] = code;
    const nevr::auth::HttpResponse r = http_.PostJson(
        nevr::auth::BuildDeviceAuthUrl(config_.base_url, config_.http_key, "poll"), body.dump());
    if (!r.transport_ok || r.status != 200) {
      // A dropped request mid-login is not the end of the login: keep polling until
      // the code's own deadline (the Windows module aborts on the first one).
      Log(LogLevel::Debug, "[NEVR.AUTH] poll request failed; will poll again code=" +
                               std::to_string(r.transport_code) + " http_status=" + std::to_string(r.status));
      TokenAuth::DevicePollResponse pending;
      pending.status = TokenAuth::DevicePollStatus::Pending;
      return pending;
    }
    return TokenAuth::ParseDevicePollResponse(r.body);
  };
  ops.sleep = [this](std::chrono::steady_clock::duration d) { (void)clock_.SleepFor(d); };
  ops.cancelled = [this] {
    std::lock_guard<std::mutex> lock(mutex_);
    return stop_;
  };
  ops.log = [this](LogLevel l, const std::string& m) { Log(l, m); };

  const nevr::auth::DeviceFlowResult flow = nevr::auth::RunDeviceCodeFlow(ops, config_.login_url);
  presenter_.Clear();
  if (!flow.verified) return false;

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
  return true;
}

void Session::BackgroundRefresh(CachedAuthToken auth) {
  int consecutiveFailures = 0;
  while (!clock_.SleepFor(config_.background_period)) {
    const uint64_t now = clock_.UnixNow();
    if (!nevr::auth::AccessTokenNeedsRefresh(auth.token_expiry, now)) continue;
    if (!auth.HasValidRefreshToken(now)) continue;

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
    } else {
      ++consecutiveFailures;
      Log(LogLevel::Warning, "[NEVR.AUTH] token refresh failed (" + std::to_string(consecutiveFailures) +
                                 " consecutive) outcome=" + nevr::auth::RefreshOutcomeName(outcome) +
                                 "; cache kept, retrying next period");
    }
  }
}

void Session::Run() {
  CachedAuthToken auth;
  bool ok = TryCachedLogin(auth);
  if (!ok) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stop_) return;
    }
    ok = RunDeviceLogin(auth);
  }
  if (!ok) {
    Log(LogLevel::Warning, "[NEVR.AUTH] Authentication failed -- social features may be limited");
    SetState(Readiness::Failed);
    return;
  }
  Adopt(auth, Readiness::Ready);
  BackgroundRefresh(auth);
}

}  // namespace nevr::quest_auth
