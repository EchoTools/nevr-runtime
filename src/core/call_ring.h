#pragma once
// A bounded, lock-free ring of call records, many producers and one consumer.
//
// The export tracer (runtime/hook/export_trace_thunk.h) pushes one record per traced call from the game's
// own threads and a drain thread formats them, so a traced call never formats, logs or takes a lock: it
// claims a slot with one compare-exchange and writes 88 bytes. A full ring drops the new record and counts
// it; nothing ever blocks. (Dmitry Vyukov's bounded MPMC queue; the consumer side is single-threaded here.)
//
// Header-only and free of windows.h: the unit tests drive it from plain threads.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace nevr {

struct CallRecord {
  // No default member initializers: a ring is a static object, zero-initialized, with no dynamic
  // initializer to run at DLL load.
  std::uint64_t enterTicks;  // rdtsc before the original ran
  std::uint64_t exitTicks;   // rdtsc after it returned
  std::uint64_t args[4];     // rcx, rdx, r8, r9 as the caller left them
  std::uint64_t ret;         // rax on return
  std::uint64_t retXmm0;     // low 64 bits of xmm0 on return (a float or double result)
  std::uint32_t exportId;    // the tracer's id for the export
  std::uint32_t threadId;
};

template <std::size_t N>
class CallRing {
  static_assert(N >= 2 && (N & (N - 1)) == 0, "the ring size is a power of two");

 public:
  CallRing() = default;
  CallRing(const CallRing&) = delete;
  CallRing& operator=(const CallRing&) = delete;

  /// Any thread. False (and counted) when the ring is full; never blocks.
  bool Push(const CallRecord& record) noexcept {
    std::uint64_t pos = enqueue_.load(std::memory_order_relaxed);
    for (;;) {
      Slot& slot = slots_[pos & (N - 1)];
      const std::uint64_t seq = slot.seq.load(std::memory_order_acquire);
      const std::int64_t diff = static_cast<std::int64_t>(seq) - static_cast<std::int64_t>(pos);
      if (diff == 0) {
        if (enqueue_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          slot.record = record;
          slot.seq.store(pos + 1, std::memory_order_release);
          pushed_.fetch_add(1, std::memory_order_relaxed);
          return true;
        }
      } else if (diff < 0) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
      } else {
        pos = enqueue_.load(std::memory_order_relaxed);
      }
    }
  }

  /// The single consumer. False when nothing is ready.
  bool Pop(CallRecord* out) noexcept {
    const std::uint64_t pos = dequeue_.load(std::memory_order_relaxed);
    Slot& slot = slots_[pos & (N - 1)];
    const std::uint64_t seq = slot.seq.load(std::memory_order_acquire);
    if (static_cast<std::int64_t>(seq) - static_cast<std::int64_t>(pos + 1) < 0) return false;
    *out = slot.record;
    slot.seq.store(pos + N, std::memory_order_release);
    dequeue_.store(pos + 1, std::memory_order_relaxed);
    return true;
  }

  std::uint64_t Pushed() const noexcept { return pushed_.load(std::memory_order_relaxed); }
  std::uint64_t Dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

 private:
  struct Slot {
    std::atomic<std::uint64_t> seq;
    CallRecord record;
  };

  // The slots' sequence numbers start at their own index; a static initializer cannot do that for an
  // array member, so the first use seeds them (Seed), once, before any Push or Pop.
 public:
  /// Call once, before the first Push or Pop (the tracer does it when it is switched on).
  void Seed() noexcept {
    for (std::size_t i = 0; i < N; ++i) slots_[i].seq.store(i, std::memory_order_relaxed);
    enqueue_.store(0, std::memory_order_relaxed);
    dequeue_.store(0, std::memory_order_relaxed);
    pushed_.store(0, std::memory_order_relaxed);
    dropped_.store(0, std::memory_order_relaxed);
  }

 private:
  Slot slots_[N];
  alignas(64) std::atomic<std::uint64_t> enqueue_;
  alignas(64) std::atomic<std::uint64_t> dequeue_;
  std::atomic<std::uint64_t> pushed_;
  std::atomic<std::uint64_t> dropped_;
};

}  // namespace nevr
