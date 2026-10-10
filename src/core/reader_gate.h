#pragma once

#include <atomic>
#include <chrono>
#include <thread>

namespace nevr {

/// Lets a lock-free reader tell a writer "I may still be using what you are about to free".
///
/// Reader (hot path, wait-free):   { ReaderGate::Scope s(gate); use(ReaderGate::Load(slot)); }
/// Writer: ReaderGate::Publish(slot, replacement or nullptr), then gate.WaitIdle(); only then free
///         the old object.
///
/// This is a store/load handshake (writer: store the pointer, then load the reader count; reader:
/// raise the count, then load the pointer). Without a total order over those four operations both
/// sides can read the other's stale value, so WaitIdle() would see 0 while a reader goes on to load
/// the old pointer. Every one of them is therefore seq_cst: the counter RMWs, the counter load in
/// WaitIdle(), and the pointer store/load through Publish()/Load(). Do not weaken any of them
/// (test_reader_gate stresses the race and fails with them relaxed).
class ReaderGate {
 public:
  class Scope {
   public:
    explicit Scope(ReaderGate& gate) : gate_(gate) { gate_.readers_.fetch_add(1, std::memory_order_seq_cst); }
    ~Scope() { gate_.readers_.fetch_sub(1, std::memory_order_seq_cst); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

   private:
    ReaderGate& gate_;
  };

  /// True when no reader is inside a Scope; waits up to `timeout` for the last one to leave.
  /// False on timeout: the caller must not free what the readers use.
  bool WaitIdle(std::chrono::milliseconds timeout) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    // Yield first: a reader that is in and out of the gate in microseconds is missed by a 1 ms poll
    // for as long as the next one arrives before the sleep ends.
    for (int spin = 0; readers_.load(std::memory_order_seq_cst) != 0; spin++) {
      if (std::chrono::steady_clock::now() >= deadline) return false;
      if (spin < 1000) {
        std::this_thread::yield();
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    return true;
  }

  /// Readers currently inside a Scope; for diagnostics (a reader that left its Scope by a longjmp
  /// never decrements, so a count that stays non-zero names that).
  int Readers() const { return readers_.load(std::memory_order_seq_cst); }

  template <class T>
  static void Publish(std::atomic<T*>& slot, T* value) {
    slot.store(value, std::memory_order_seq_cst);
  }

  template <class T>
  static T* Load(const std::atomic<T*>& slot) {
    return slot.load(std::memory_order_seq_cst);
  }

 private:
  std::atomic<int> readers_{0};
};

}  // namespace nevr
