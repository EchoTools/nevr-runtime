#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>

namespace nevr {

/// Lets a lock-free reader tell a writer "I may still be using what you are about to free".
///
/// Reader (hot path, lock-free):   { ReaderGate::Scope s(gate); use(ReaderGate::Load(slot)); }
/// Writer: ReaderGate::Publish(slot, replacement or nullptr), then gate.WaitIdle(); only then free
///         the old object.
///
/// WaitIdle() returns true once every reader that was inside a Scope when it was called has left. It
/// does not wait for the count to be zero: a reader that entered after the call loaded the new pointer
/// and is no concern of the writer, and a stream of such readers must not keep the writer waiting
/// (a count of zero at one instant is not guaranteed under a continuous stream, #469).
///
/// This is a grace period over two reader counters. A Scope registers in the counter of the current
/// epoch; WaitIdle() advances the epoch and waits for the counter of the epoch that ended. The proof
/// is a store/load handshake (writer: store the pointer, advance the epoch, load the counter; reader:
/// load the epoch, raise the counter, re-load the epoch, then load the pointer): a reader whose
/// counter increment the writer's load did not see raised it after that load, so its pointer load is
/// after the writer's store and returns the replacement; a reader that registered before is waited
/// for. Without a total order over those operations both sides can read the other's stale value, so
/// every one of them is seq_cst: the epoch RMW and loads, the counter RMWs and loads, and the pointer
/// store/load through Publish()/Load(). Do not weaken any of them (test_reader_gate stresses the
/// race and fails with them relaxed).
class ReaderGate {
 public:
  class Scope {
   public:
    explicit Scope(ReaderGate& gate) : gate_(gate), slot_(gate.Enter([] {})) {}
    /// Test seam: `between` runs after the epoch was read and before the counter is raised, the one
    /// place a reader can be overtaken by a writer (test_reader_gate holds it there).
    template <class Between>
    Scope(ReaderGate& gate, Between&& between) : gate_(gate), slot_(gate.Enter(between)) {}
    ~Scope() { gate_.counts_[slot_].fetch_sub(1, std::memory_order_seq_cst); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

   private:
    ReaderGate& gate_;
    unsigned slot_;
  };

  /// True when every reader that was inside a Scope at the call has left; waits up to `timeout` for the
  /// last of them. False on timeout (or when another WaitIdle holds the gate that long): the caller
  /// must not free what the readers use. A reader a timed-out call did not see leave is waited for
  /// again by the next call, whatever has entered since.
  bool WaitIdle(std::chrono::milliseconds timeout) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock<std::timed_mutex> writer(writers_, std::defer_lock);
    if (!writer.try_lock_until(deadline)) return false;
    if (undrained_ >= 0) {
      if (!Drained(static_cast<unsigned>(undrained_), deadline)) return false;
      undrained_ = -1;
    }
    const unsigned ended = static_cast<unsigned>(epoch_.fetch_add(1, std::memory_order_seq_cst) & 1u);
    if (!Drained(ended, deadline)) {
      undrained_ = static_cast<int>(ended);
      return false;
    }
    return true;
  }

  /// Readers currently inside a Scope; for diagnostics (a reader that left its Scope by a longjmp
  /// never decrements, so a count that stays non-zero names that).
  int Readers() const {
    return counts_[0].load(std::memory_order_seq_cst) + counts_[1].load(std::memory_order_seq_cst);
  }

  template <class T>
  static void Publish(std::atomic<T*>& slot, T* value) {
    slot.store(value, std::memory_order_seq_cst);
  }

  template <class T>
  static T* Load(const std::atomic<T*>& slot) {
    return slot.load(std::memory_order_seq_cst);
  }

 private:
  // Registers in the counter of the current epoch. The epoch is read again after the increment: a
  // writer that advanced it in between may already have looked at this counter, so the registration is
  // withdrawn and made again in the new epoch.
  template <class Between>
  unsigned Enter(Between&& between) {
    for (;;) {
      const std::uint64_t epoch = epoch_.load(std::memory_order_seq_cst);
      const unsigned slot = static_cast<unsigned>(epoch & 1u);
      between();
      counts_[slot].fetch_add(1, std::memory_order_seq_cst);
      if (epoch_.load(std::memory_order_seq_cst) == epoch) return slot;
      counts_[slot].fetch_sub(1, std::memory_order_seq_cst);
    }
  }

  // Waits for one counter to reach zero. Yield first: a reader that is in and out of the gate in
  // microseconds is missed by a 1 ms poll.
  bool Drained(unsigned slot, std::chrono::steady_clock::time_point deadline) const {
    for (int spin = 0; counts_[slot].load(std::memory_order_seq_cst) != 0; spin++) {
      if (std::chrono::steady_clock::now() >= deadline) return false;
      if (spin < 1000) {
        std::this_thread::yield();
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    return true;
  }

  mutable std::atomic<std::uint64_t> epoch_{0};  // WaitIdle() is const: it only reads what the readers use
  std::atomic<int> counts_[2]{{0}, {0}};
  mutable std::timed_mutex writers_;  // one WaitIdle at a time; guards undrained_
  mutable int undrained_ = -1;        // counter a timed-out WaitIdle left a reader in, or -1
};

}  // namespace nevr
