#include "quest/auth/session.h"

#include "core/auth_refresh.h"
#include "core/device_auth_flow.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <ctime>
#include <exception>
#include <system_error>

#if defined(__linux__)
#include <pthread.h>
#endif

namespace nevr::quest_auth {

namespace {
using nevr::auth::LogLevel;

// True when the body is a JSON object with an "error" member: the server's own refusal.
bool BodyHasErrorKey(const std::string& body) {
  try {
    const nlohmann::json j = nlohmann::json::parse(body);
    return j.is_object() && j.contains("error");
  } catch (const nlohmann::json::exception&) {
    return false;
  }
}
}  // namespace

SystemClock::SystemClock() {
  pthread_mutex_init(&mutex_, nullptr);
  pthread_condattr_t attr;
  pthread_condattr_init(&attr);
  pthread_condattr_setclock(&attr, kWaitClock);
  pthread_cond_init(&cv_, &attr);
  pthread_condattr_destroy(&attr);
}

SystemClock::~SystemClock() {
  pthread_cond_destroy(&cv_);
  pthread_mutex_destroy(&mutex_);
}

uint64_t SystemClock::UnixNow() { return static_cast<uint64_t>(std::time(nullptr)); }

std::chrono::steady_clock::time_point SystemClock::SteadyNow() {
  timespec ts{};
  clock_gettime(kSteadyClock, &ts);
  return std::chrono::steady_clock::time_point(std::chrono::seconds(ts.tv_sec) + std::chrono::nanoseconds(ts.tv_nsec));
}

bool SystemClock::SleepFor(std::chrono::steady_clock::duration d) {
  using std::chrono::nanoseconds;
  const nanoseconds capped = std::min<nanoseconds>(std::chrono::duration_cast<nanoseconds>(d), std::chrono::hours(24));
  timespec deadline{};
  clock_gettime(kWaitClock, &deadline);
  const long long total_ns = static_cast<long long>(deadline.tv_nsec) + std::max<long long>(capped.count(), 0);
  deadline.tv_sec += static_cast<time_t>(total_ns / 1000000000LL);
  deadline.tv_nsec = static_cast<long>(total_ns % 1000000000LL);
  pthread_mutex_lock(&mutex_);
  while (!interrupted_) {
    if (pthread_cond_timedwait(&cv_, &mutex_, &deadline) == ETIMEDOUT) break;
  }
  const bool interrupted = interrupted_;
  pthread_mutex_unlock(&mutex_);
  return interrupted;
}

void SystemClock::Interrupt() {
  pthread_mutex_lock(&mutex_);
  interrupted_ = true;
  pthread_mutex_unlock(&mutex_);
  pthread_cond_broadcast(&cv_);
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

Session::~Session() {
  try {
    Stop();
  } catch (const std::exception&) {
    // A destructor must not throw; Stop() reports what it can itself.
  }
}

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
        // Nothing may escape a thread body: it would terminate the game process. Reporting the
        // failure can itself throw (the log sink, an allocation), so that is contained too.
        try {
          Log(LogLevel::Error, std::string("[NEVR.AUTH] auth worker stopped on an exception: ") + e.what());
          SetState(Readiness::Failed);
        } catch (const std::exception&) {
          // Nowhere left to report to.
        }
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
  if (worker_.joinable()) {
    try {
      worker_.join();
    } catch (const std::system_error& e) {
      // join() failing (EINVAL, EDEADLK) is not expected here. A joinable std::thread that is
      // destroyed terminates the process, so release it and say so; the stop flag is already set.
      worker_.detach();
      try {
        Log(LogLevel::Error, std::string("[NEVR.AUTH] auth worker could not be joined: ") + e.what());
      } catch (const std::exception&) {
      }
    }
  }
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

void Session::SetState(Readiness state, bool will_retry) {
  std::string transition;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (snapshot_.readiness == Readiness::Stopped) return;
    if (snapshot_.readiness != state) {
      transition = std::string("[NEVR.AUTH] auth state ") + ReadinessName(snapshot_.readiness) + " -> " +
                   ReadinessName(state);
    }
    snapshot_.readiness = state;
    snapshot_.will_retry = state == Readiness::Failed && will_retry;
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
    snapshot_.will_retry = false;
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
  const auto sink = [this](LogLevel l, const std::string& m) { Log(Quiet(l), m); };
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
    Log(Quiet(LogLevel::Warning), std::string("[NEVR.AUTH] cached-login refresh failed attempt=") +
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
      Log(Quiet(LogLevel::Error), std::string("[NEVR.AUTH] refresh refused (") + nevr::auth::RefreshOutcomeName(outcome) +
                               "), not about the refresh token; cache kept, player not prompted");
      return LoginResult::Held;
    }
    if (i < attempts && clock_.SleepFor(config_.refresh_retry_pause)) return LoginResult::Transient;
  }
  failure_class_ = "refresh_transient";
  Log(Quiet(LogLevel::Warning), "[NEVR.AUTH] cached-login refresh unsuccessful for a transient reason; cache file kept");
  return LoginResult::Transient;
}

Session::DeviceResult Session::RunDeviceLogin(CachedAuthToken& out) {
  // The bound is checked before the player is asked for anything: a recovery attempt after the last
  // code does not announce a new sign-in (AwaitingUser) it will not run.
  if (unanswered_codes_ >= kMaxUnansweredCodes) {
    ConcludeBound();
    return DeviceResult::Ended;
  }
  quiet_ = false;  // a player prompt is always worth an Info line, also when it starts from recovery
  SetState(Readiness::AwaitingUser);
  // The prompt carries the device code: take it down however this function ends, including by an
  // exception out of one of the operations below, unless the outcome replaced it (Conclude). It is
  // NOT taken down between one code and the next, so what the player sees goes from the old code
  // straight to the new one.
  struct ClearLink {
    LinkPresenter& presenter;
    bool concluded = false;
    void Conclude(LoginOutcome outcome) {
      concluded = true;
      presenter.Conclude(outcome);
    }
    ~ClearLink() {
      if (!concluded) presenter.Clear();
    }
  } clear_link{presenter_};

  for (;;) {
    // The bound is for the whole session, not one call: a code-request failure in between leads to a
    // fresh call after the recovery period, and that must not start the count again.
    if (unanswered_codes_ >= kMaxUnansweredCodes) {
      clear_link.concluded = true;
      ConcludeBound();
      return DeviceResult::Ended;
    }
    const auto requested_at = clock_.SteadyNow();
    const DeviceResult r = RunDeviceCode(out);
    if (r == DeviceResult::Verified) {
      unanswered_codes_ = 0;
      shown_codes_ = 0;
      clear_link.Conclude(LoginOutcome::SignedIn);
      return r;
    }
    if (r == DeviceResult::Undelivered) {
      // A code was issued but no mechanism could show it. Try again at the recovery period (the
      // cause may pass: storage, a full disk); the code counts toward the bound.
      ++unanswered_codes_;
      return r;
    }
    if (r != DeviceResult::CodeExpired) return r;
    ++unanswered_codes_;
    ++shown_codes_;
    if (StopRequested()) return DeviceResult::Ended;
    if (unanswered_codes_ >= kMaxUnansweredCodes) continue;  // the bound, at the top of the loop
    // The player was shown a code and it ran out (the server's "expired", or its five minutes
    // passed): the login is still wanted, so ask for a new code and show it in place of the old
    // one. Codes are asked for no more often than once per kMinCodeInterval, whatever the server
    // says about them.
    const auto since = clock_.SteadyNow() - requested_at;
    Log(LogLevel::Info, "[NEVR.AUTH] the device code ran out without a sign-in; requesting a new one (code " +
                            std::to_string(unanswered_codes_ + 1) + " of " + std::to_string(kMaxUnansweredCodes) +
                            ")");
    if (since < kMinCodeInterval && clock_.SleepFor(kMinCodeInterval - since)) return DeviceResult::Ended;
    if (StopRequested()) return DeviceResult::Ended;
  }
}

void Session::ConcludeBound() {
  // What the player is told depends on whether any of those codes reached them.
  const bool anyShown = shown_codes_ > 0;
  Log(LogLevel::Warning, "[NEVR.AUTH] no sign-in after " + std::to_string(unanswered_codes_) + " device codes (" +
                             std::to_string(shown_codes_) +
                             " shown); no more are requested until the game restarts");
  presenter_.Conclude(anyShown ? LoginOutcome::TimedOut : LoginOutcome::NoCodeShown);
}

Session::DeviceResult Session::RunDeviceCode(CachedAuthToken& out) {
  device_result_ = DeviceResult::Ended;
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
  ops.open_browser = [this](const std::string& link) {
    LoginPrompt prompt;
    prompt.link = link;
    const size_t q = link.find("?code=");
    prompt.url = q == std::string::npos ? link : link.substr(0, q);
    prompt.code = q == std::string::npos ? std::string() : link.substr(q + 6);
    const auto lifetime = std::chrono::duration_cast<std::chrono::seconds>(nevr::auth::kDeviceAuthLifetime);
    prompt.expires_unix = clock_.UnixNow() + static_cast<uint64_t>(lifetime.count());
    const intptr_t delivered = presenter_.Present(prompt);
    // A delivered code that ends without a sign-in ran out, unless the server refuses a poll.
    device_result_ = delivered > nevr::auth::kBrowserOpenAcceptedAbove ? DeviceResult::CodeExpired
                                                                         : DeviceResult::Undelivered;
    if (device_result_ == DeviceResult::Undelivered) failure_class_ = "prompt_not_shown";
    nevr::auth::WipeSecret(prompt.code);
    nevr::auth::WipeSecret(prompt.link);
    return delivered;
  };
  // Nobody can see a link that was not delivered: stop rather than wait out the code.
  ops.show_open_failure = [](const std::string&, const std::string&, intptr_t) { return 0; };
  // The server answers "code unknown or expired" with a 200 (status "expired"). While the
  // player holds a link, an outage (no connection, 5xx, 408, 429, or a 200 the parser cannot read) is waited out until the code's own
  // deadline, polling no faster than the poll interval; any other 4xx ends the login.
  auto consecutive_failures = std::make_shared<int>(0);  // the poll op runs on this one worker thread
  ops.poll = [this, consecutive_failures](const std::string& code) {
    nlohmann::json body;
    body["code"] = code;
    const nevr::auth::HttpResponse r = http_.PostJson(
        nevr::auth::BuildDeviceAuthUrl(config_.base_url, config_.http_key, "poll"), body.dump());
    nevr_token_auth::DevicePollResponse response;
    // What the server did answer, for the per-poll log line (0: no HTTP answer arrived).
    response.http_code = r.transport_ok ? r.status : 0;
    response.body_prefix = nevr_token_auth::PollBodyPrefix(r.body, code);
    if (r.transport_ok && r.status == 200) {
      nevr_token_auth::DevicePollResponse parsed = nevr_token_auth::ParseDevicePollResponse(r.body);
      // The parser reports Error both for the server's own {"error":...} and for a body it could not
      // read (HTML from a captive portal). Only the first is the server's answer.
      parsed.http_code = r.status;
      parsed.body_prefix = nevr_token_auth::PollBodyPrefix(r.body, code);
      if (parsed.status != nevr_token_auth::DevicePollStatus::Error || BodyHasErrorKey(r.body)) {
        *consecutive_failures = 0;
        if (parsed.status == nevr_token_auth::DevicePollStatus::Error) device_result_ = DeviceResult::Ended;
        return parsed;
      }
    }
    // A 200 the parser could not read is treated like an outage: keep polling until the deadline.
    const bool transient = !r.transport_ok || r.status >= 500 || r.status == 429 || r.status == 408 || r.status == 200;
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
      response.status = nevr_token_auth::DevicePollStatus::Pending;
    } else {
      Log(LogLevel::Warning, "[NEVR.AUTH] device poll refused by the server; ending the login" + what);
      device_result_ = DeviceResult::Ended;
      response.status = nevr_token_auth::DevicePollStatus::Error;
    }
    return response;
  };
  ops.sleep = [this](std::chrono::steady_clock::duration d) { (void)clock_.SleepFor(d); };
  ops.cancelled = [this] { return StopRequested(); };
  ops.log = [this](LogLevel l, const std::string& m) { Log(l, m); };
  ops.renews_expired_codes = true;  // RunDeviceLogin answers a code that runs out with a new one

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
      if (d == DeviceResult::RequestRefused || d == DeviceResult::Undelivered) return LoginEnd::Recoverable;
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
    SetState(Readiness::Failed, /*will_retry=*/true);
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
  // A refresh refused for a reason that is not about the token (a wrong http_key, a missing RPC) is
  // retried at the recovery cadence, with one Warning per class, as at startup.
  uint64_t held_until = 0;
  std::string held_class;
  while (!clock_.SleepFor(config_.background_period)) {
    const uint64_t now = clock_.UnixNow();
    if (auth.token_expiry <= now) SetState(Readiness::Expired);  // logs the transition once
    if (!nevr::auth::AccessTokenNeedsRefresh(auth.token_expiry, now)) continue;
    if (now < held_until) continue;

    bool relogin = false;
    if (!auth.HasValidRefreshToken(now)) {
      Log(LogLevel::Warning, "[NEVR.AUTH] the refresh token has expired; starting a new device-code login");
      relogin = true;
    } else {
      CachedAuthToken candidate = auth;
      const bool repeat = !held_class.empty();
      const auto sink = [this, repeat](LogLevel l, const std::string& m) {
        Log(repeat && l == LogLevel::Warning ? LogLevel::Debug : l, m);
      };
      const nevr::auth::RefreshOutcome outcome = nevr::auth::RefreshAccessToken(
          candidate, config_.base_url, config_.http_key, http_, now, sink);
      if (outcome == nevr::auth::RefreshOutcome::Refreshed) {
        held_class.clear();
        held_until = 0;
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
      } else if (outcome == nevr::auth::RefreshOutcome::Unauthorized ||
                 outcome == nevr::auth::RefreshOutcome::ClientError) {
        const std::string klass = nevr::auth::RefreshOutcomeName(outcome);
        Log(klass != held_class ? LogLevel::Warning : LogLevel::Debug,
            "[NEVR.AUTH] token refresh held (" + klass + "), not about the refresh token; login kept, trying again every " +
                std::to_string(config_.recovery_period.count()) + "s");
        held_class = klass;
        held_until = now + static_cast<uint64_t>(config_.recovery_period.count());
      } else {
        // A transient failure leaves the login alone: the cache is kept, the player is not
        // prompted, and the next period tries again.
        held_class.clear();
        held_until = 0;
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
  presenter_.Clear();  // a login file left by an earlier session shows a dead code
  CachedAuthToken auth;
  if (!LoginWithRecovery(auth, /*use_cache=*/true)) return;
  Adopt(auth, Readiness::Ready);
  BackgroundRefresh(auth);
}

}  // namespace nevr::quest_auth
