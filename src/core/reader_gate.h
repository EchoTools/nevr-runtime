#pragma once

#include <atomic>
#include <chrono>
#include <thread>

namespace nevr {

/// Lets a lock-free reader tell a writer "I may still be using what you are about to free".
///
/// Reader (hot path, wait-free):   { ReaderGate::Scope s(gate); use(ptr.load()); }
/// Writer: publish the replacement (or nullptr) so no new reader can pick up the old object, then
///         gate.WaitIdle(); only then free the old object.
/// A reader that loaded the old pointer before the publish is inside a Scope, so WaitIdle() does not
/// return true until it has left. Readers that enter after the publish see the new pointer.
class ReaderGate {
 public:
  class Scope {
   public:
    explicit Scope(ReaderGate& gate) : gate_(gate) { gate_.readers_.fetch_add(1, std::memory_order_acq_rel); }
    ~Scope() { gate_.readers_.fetch_sub(1, std::memory_order_acq_rel); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

   private:
    ReaderGate& gate_;
  };

  /// True when no reader is inside a Scope; waits up to `timeout` for the last one to leave.
  /// False on timeout: the caller must not free what the readers use.
  bool WaitIdle(std::chrono::milliseconds timeout) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (readers_.load(std::memory_order_acquire) != 0) {
      if (std::chrono::steady_clock::now() >= deadline) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
  }

 private:
  std::atomic<int> readers_{0};
};

}  // namespace nevr
