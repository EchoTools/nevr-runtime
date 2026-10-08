#pragma once
// Quest token-auth session: cached-login load, refresh, device-code login and the
// background refresh loop, built from the platform-neutral nevr::auth core
// (core/auth_refresh.h, core/device_auth_flow.h, core/auth_token_model.h).
//
// Nothing here blocks the caller: Start() spawns one worker thread and returns, so
// the game's loader constructor / bootstrap thread never waits on a network call,
// a file read or the player's browser. Readers poll Get()/Token() for the state.
// All collaborators are injected, so the whole thing runs under a fake HTTP server
// and a fake clock on the host.

#include "core/auth_token_model.h"
#include "core/auth_types.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <pthread.h>
#include <time.h>

namespace nevr::quest_auth {

// A Clock whose pending sleeps can be cut short at shutdown.
class InterruptibleClock : public nevr::auth::Clock {
 public:
  virtual void Interrupt() = 0;
};

// Wall clock + real sleeps.
//  - Sleeps wait on a pthread condition variable whose clock is CLOCK_MONOTONIC. With libc++ on
//    Android API 26 a std::condition_variable wait_for becomes a CLOCK_REALTIME deadline, so a
//    backward wall-clock step would stretch every wait.
//  - SteadyNow() reads CLOCK_BOOTTIME, which keeps counting while the headset sleeps. The
//    device-code deadline is judged on it because the server expires the code in wall time: after
//    a long sleep the client sees the deadline has passed and stops, instead of polling a dead code.
//    (The sleeps themselves do not count suspended time, so one that straddles a suspend ends late
//    and the next deadline check catches it.)
class SystemClock : public InterruptibleClock {
 public:
  static constexpr clockid_t kWaitClock = CLOCK_MONOTONIC;
  static constexpr clockid_t kSteadyClock = CLOCK_BOOTTIME;

  SystemClock();
  ~SystemClock() override;
  SystemClock(const SystemClock&) = delete;
  SystemClock& operator=(const SystemClock&) = delete;

  uint64_t UnixNow() override;
  std::chrono::steady_clock::time_point SteadyNow() override;
  bool SleepFor(std::chrono::steady_clock::duration d) override;
  void Interrupt() override;

 private:
  pthread_mutex_t mutex_;
  pthread_cond_t cv_;
  bool interrupted_ = false;
};

// Where the refresh token (and identity) persist. Save must never leave a
// half-written file behind, and a failed Save must leave the previous file intact.
class CredentialStore {
 public:
  virtual ~CredentialStore() = default;
  virtual CachedAuthToken Load(uint64_t now) = 0;
  virtual bool Save(const CachedAuthToken& auth) = 0;
};

// What the player needs to sign in: the verification page, the code to enter there, the link with
// the code filled in, and when the code stops working (unix seconds).
struct LoginPrompt {
  std::string url;   // verification page, e.g. https://echovrce.com/login/device
  std::string code;
  std::string link;  // url + "?code=" + code
  uint64_t expires_unix = 0;
};

// How the player is told where to log in. Present returns a value above
// nevr::auth::kBrowserOpenAcceptedAbove when the prompt was delivered, 0 otherwise.
class LinkPresenter {
 public:
  virtual ~LinkPresenter() = default;
  virtual intptr_t Present(const LoginPrompt& prompt) = 0;
  virtual void Clear() = 0;
};

// Expired: the access token (or the refresh token behind it) is dead and no replacement
// is in hand yet; Token() serves nothing. Failed: login gave up.
enum class Readiness { Starting, Refreshing, AwaitingUser, Ready, Expired, Failed, Stopped };

const char* ReadinessName(Readiness r);

struct Snapshot {
  Readiness readiness = Readiness::Starting;
  std::string access_token;
  uint64_t access_expiry = 0;
  uint64_t discord_id = 0;
  std::string user_id;
  std::string username;
};

struct SessionConfig {
  std::string base_url;   // nakama HTTP base, no trailing slash
  std::string http_key;
  std::string login_url;  // page the player opens, without the code
  int refresh_attempts = 3;
  std::chrono::seconds refresh_retry_pause{2};
  std::chrono::seconds background_period{60};
  // Failure handling, by what the failure says:
  //  - transient (no connection, 5xx, 408, 429, an unreadable response): the cached refresh or the
  //    device-code request is retried after each of these pauses; the cached login is kept and
  //    the player is never prompted for it.
  //  - held (a 4xx that is not "this refresh token is refused": a wrong http_key, a missing RPC,
  //    a rejected payload): one attempt, no fast retry, the cache is kept and the player is not
  //    prompted; the login is Failed until the next recovery attempt.
  //  - the server refuses the refresh token itself (400/401/403 whose JSON message is one of the
  //    refresh RPC's own errors) or there is no usable cache: the device-code login runs.
  // After the pauses are used up, or on a held failure, the login is Failed and is attempted again
  // every recovery_period until it succeeds, a stop, or the player's own device login ends without
  // a login (expired code, deadline, a refused poll), which is final.
  std::vector<std::chrono::seconds> login_retry_delays = {std::chrono::seconds(5), std::chrono::seconds(15),
                                                          std::chrono::seconds(45), std::chrono::seconds(135),
                                                          std::chrono::seconds(300)};
  std::chrono::seconds recovery_period{300};
};

class Session {
 public:
  // `log` is called from the worker thread (and from Start/Stop callers) with no Session lock
  // held, so it may call Get(). A call to Stop() from inside `log` made on the worker thread
  // only requests the stop; it never joins itself.
  Session(SessionConfig config, nevr::auth::HttpClient& http, InterruptibleClock& clock,
          CredentialStore& store, LinkPresenter& presenter, nevr::auth::LogSink log);
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  // Spawns the worker and returns immediately. A second call is a no-op.
  void Start();
  // Interrupts sleeps and in-flight requests and joins the worker. Idempotent; concurrent
  // calls wait for the same join. Do not call it from a LinkPresenter/CredentialStore/HttpClient
  // callback: those run on the worker thread and Stop would wait for itself (a call from the
  // log sink is handled, see the constructor).
  void Stop();

  Snapshot Get() const;
  // The access token if (and only if) it is Ready and not past its expiry now.
  std::string Token() const;

 private:
  void Run();
  // Ok; NeedDevice: no usable cache or the server refused the token; Transient: retry later;
  // Held: a 4xx that says nothing about the token, keep the cache and do not prompt.
  enum class LoginResult { Ok, NeedDevice, Transient, Held };
  // Verified; RequestTransient / RequestRefused: the device-code request failed (nothing shown to
  // the player); Ended: the flow ran and ended without a login (expired code, deadline, a poll
  // the server refused, an undeliverable link) -- the player was involved, so it is final.
  enum class DeviceResult { Verified, RequestTransient, RequestRefused, Ended };
  // Ok; Stopped; Recoverable: Failed for now, try again later; Final: do not try again.
  enum class LoginEnd { Ok, Stopped, Recoverable, Final };
  LoginResult TryCachedLogin(CachedAuthToken& auth, int attempts);
  DeviceResult RunDeviceLogin(CachedAuthToken& out);
  // Cached login (when use_cache) then device-code login; with_backoff retries transient
  // failures on login_retry_delays.
  LoginEnd EstablishLogin(CachedAuthToken& auth, bool use_cache, bool with_backoff);
  // EstablishLogin, then recovery attempts every recovery_period after a recoverable failure.
  bool LoginWithRecovery(CachedAuthToken& auth, bool use_cache);
  void Adopt(const CachedAuthToken& auth, Readiness state);
  void SetState(Readiness state);
  // Runs until stopped, or until a re-login after the credentials died fails.
  void BackgroundRefresh(CachedAuthToken auth);
  bool StopRequested() const;
  void Log(nevr::auth::LogLevel level, const std::string& message) const;
  // During recovery attempts a repeated failure is logged at Debug, not Warning/Error.
  nevr::auth::LogLevel Quiet(nevr::auth::LogLevel level) const {
    return quiet_ && (level == nevr::auth::LogLevel::Warning || level == nevr::auth::LogLevel::Error)
               ? nevr::auth::LogLevel::Debug
               : level;
  }

  SessionConfig config_;
  nevr::auth::HttpClient& http_;
  InterruptibleClock& clock_;
  CredentialStore& store_;
  LinkPresenter& presenter_;
  nevr::auth::LogSink log_;

  // Serialises Start/Stop (and so every touch of worker_). Never held by the worker.
  std::mutex lifecycle_mutex_;
  // Guards snapshot_ and stop_. The log sink is never called with it held, so a sink may
  // call Get().
  mutable std::mutex mutex_;
  Snapshot snapshot_;
  bool stop_ = false;
  std::thread worker_;
  std::atomic<std::thread::id> worker_id_{};
  bool started_ = false;
  // Worker thread only:
  DeviceResult device_result_ = DeviceResult::Ended;
  std::string failure_class_;
  bool quiet_ = false;  // recovery attempts log state changes at Debug
};

}  // namespace nevr::quest_auth
