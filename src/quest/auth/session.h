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
#include <memory>
#include <mutex>
#include <string>
#include <thread>

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

enum class Readiness { Starting, Refreshing, AwaitingUser, Ready, Failed, Stopped };

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
};

class Session {
 public:
  Session(SessionConfig config, nevr::auth::HttpClient& http, InterruptibleClock& clock,
          CredentialStore& store, LinkPresenter& presenter, nevr::auth::LogSink log);
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;

  // Spawns the worker and returns immediately. A second call is a no-op.
  void Start();
  // Interrupts sleeps and joins the worker. Idempotent.
  void Stop();

  Snapshot Get() const;
  // The access token if (and only if) it is Ready and not past its expiry now.
  std::string Token() const;

 private:
  void Run();
  bool TryCachedLogin(CachedAuthToken& auth);
  bool RunDeviceLogin(CachedAuthToken& out);
  void Adopt(const CachedAuthToken& auth, Readiness state);
  void SetState(Readiness state);
  void BackgroundRefresh(CachedAuthToken auth);
  void Log(nevr::auth::LogLevel level, const std::string& message) const;

  SessionConfig config_;
  nevr::auth::HttpClient& http_;
  InterruptibleClock& clock_;
  CredentialStore& store_;
  LinkPresenter& presenter_;
  nevr::auth::LogSink log_;

  mutable std::mutex mutex_;
  Snapshot snapshot_;
  bool stop_ = false;
  std::thread worker_;
  bool started_ = false;
};

}  // namespace nevr::quest_auth
