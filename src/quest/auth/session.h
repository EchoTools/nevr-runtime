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

namespace nevr::quest_auth {

// A Clock whose pending sleeps can be cut short at shutdown.
class InterruptibleClock : public nevr::auth::Clock {
 public:
  virtual void Interrupt() = 0;
};

// Wall clock + real condition-variable sleeps.
class SystemClock : public InterruptibleClock {
 public:
  uint64_t UnixNow() override;
  std::chrono::steady_clock::time_point SteadyNow() override;
  bool SleepFor(std::chrono::steady_clock::duration d) override;
  void Interrupt() override;

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
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

// How the player is told where to log in. Present returns a value above
// nevr::auth::kBrowserOpenAcceptedAbove when the link was delivered, 0 otherwise.
class LinkPresenter {
 public:
  virtual ~LinkPresenter() = default;
  virtual intptr_t Present(const std::string& login_url_with_code) = 0;
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
  // Consecutive failed poll requests (transport error or non-200) after which the
  // device-code login gives up. A 3 s poll interval makes the default a 15 s outage.
  int poll_failure_limit = 5;
  std::chrono::seconds refresh_retry_pause{2};
  std::chrono::seconds background_period{60};
  // Pauses before each retry of a login that failed for a transient reason (no connection,
  // 5xx, 429, an unreadable response). Bounded: after the last one the login is Failed.
  // A refusal (4xx) is never retried: the cached login is dropped for the device flow, or
  // the login fails at once.
  std::vector<std::chrono::seconds> login_retry_delays = {std::chrono::seconds(5), std::chrono::seconds(15),
                                                          std::chrono::seconds(45), std::chrono::seconds(135),
                                                          std::chrono::seconds(300)};
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
  enum class LoginResult { Ok, Permanent, Transient };
  LoginResult TryCachedLogin(CachedAuthToken& auth);
  bool RunDeviceLogin(CachedAuthToken& out);
  // Cached login (when use_cache) then device-code login, retrying transient failures on
  // the configured delays.
  bool EstablishLogin(CachedAuthToken& auth, bool use_cache);
  void Adopt(const CachedAuthToken& auth, Readiness state);
  void SetState(Readiness state);
  // Runs until stopped, or until a re-login after the credentials died fails.
  void BackgroundRefresh(CachedAuthToken auth);
  bool StopRequested() const;
  void Log(nevr::auth::LogLevel level, const std::string& message) const;

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
  bool device_request_transient_ = false;  // worker thread only
};

}  // namespace nevr::quest_auth
