#pragma once

#include <cstdint>
#include <condition_variable>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>

namespace TokenAuth {

enum class AuthReadiness {
  Starting,
  Disabled,
  Refreshing,
  AwaitingUser,
  Ready,
  Expired,
  Failed,
  Stopping,
};

// A complete, immutable view of the authorization state for one generation.
// Refresh credentials stay private to the auth worker and are never published
// to the bridge.
struct AuthSnapshot {
  AuthReadiness readiness = AuthReadiness::Starting;
  uint64_t generation = 0;
  std::string access_token;
  uint64_t access_expiry = 0;
  uint64_t discord_id = 0;
  std::string user_id;
  std::string username;
};

// Readers receive one immutable generation; publication never mutates a
// snapshot that a bridge attempt may still hold.
class AuthSnapshotStore {
 public:
  AuthSnapshotStore();

  std::shared_ptr<const AuthSnapshot> Read() const;
  std::shared_ptr<const AuthSnapshot> Publish(AuthSnapshot snapshot);

 private:
  mutable std::mutex mutex_;
  uint64_t nextGeneration_ = 1;
  std::shared_ptr<const AuthSnapshot> current_;
};

// Shared cancellation primitive for the future auth worker. WaitFor returns
// true when stop was requested and false when the duration elapsed.
class AuthCancellation {
 public:
  void RequestStop();
  bool IsStopRequested() const;
  bool WaitFor(std::chrono::steady_clock::duration duration);

 private:
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  bool stopRequested_ = false;
};

}  // namespace TokenAuth
