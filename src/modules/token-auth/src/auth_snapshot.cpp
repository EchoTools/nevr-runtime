#include "auth_snapshot.h"

#include <utility>

namespace nevr_token_auth {

AuthSnapshotStore::AuthSnapshotStore()
    : current_(std::make_shared<const AuthSnapshot>()) {}

std::shared_ptr<const AuthSnapshot> AuthSnapshotStore::Read() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return current_;
}

std::shared_ptr<const AuthSnapshot> AuthSnapshotStore::Publish(AuthSnapshot snapshot) {
  std::lock_guard<std::mutex> lock(mutex_);
  snapshot.generation = nextGeneration_++;
  current_ = std::make_shared<const AuthSnapshot>(std::move(snapshot));
  return current_;
}

void AuthCancellation::RequestStop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopRequested_ = true;
  }
  condition_.notify_all();
}

bool AuthCancellation::IsStopRequested() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return stopRequested_;
}

bool AuthCancellation::WaitFor(std::chrono::steady_clock::duration duration) {
  std::unique_lock<std::mutex> lock(mutex_);
  return condition_.wait_for(lock, duration, [this] { return stopRequested_; });
}

}  // namespace nevr_token_auth
