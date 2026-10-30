#pragma once

// Keeping our crash handler first, on both platforms (#1605). Echo VR is one codebase built for echovr.exe (PC) and
// libr15.so (Quest): the game installs its own crash handler after ours, and on both it would take our place. The rule
// is the same on both, so its bookkeeping is written once, here:
//
//   - a request by the game to install a crash handler is REMEMBERED, not carried out (DisplacedSlot): ours stays
//     the one the system calls;
//   - when a fatal event arrives, ours runs once (FatalClaim), then the game's remembered handler runs.
//
// What differs is only the hook that intercepts the game's request and the call that invokes the remembered handler:
// Quest hooks libr15's `sigaction` and `signal` (quest/sentinel/crash_first_hook.h), PC hooks
// `SetUnhandledExceptionFilter` (runtime/lifecycle/crash_first_win.h).
//
// Crash-safe: nothing here allocates, throws or takes a lock on the read side. The reader runs inside a signal handler
// or an exception filter, so it never waits for a writer: a write in progress makes it report kBusy and the caller
// chains to nothing for that one event, which is better than hanging a process that is already dying.
//
// The header includes no platform header.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace nevr_crash_first {

enum class ReadResult : std::uint8_t {
  kEmpty,  // the game never asked for a handler in this slot
  kRead,   // *out holds the game's latest request
  kBusy,   // a write was in progress (or raced the copy); *out is unspecified
};

// The game's latest request for one handler slot (one POSIX signal, or the one top-level filter). A seqlock: Store
// calls must be serialized by the caller (a mutex in the hook, which runs in an ordinary thread); Read may run in any
// context, including a signal handler that interrupted a Store on the same thread, and never blocks.
template <typename Record>
class DisplacedSlot {
  static_assert(std::is_trivially_copyable_v<Record>, "a record is copied byte for byte inside a signal handler");

 public:
  // Remembers `next` as the game's request.
  void Store(const Record& next) noexcept {
    const std::uint32_t begin = sequence_.load(std::memory_order_relaxed);
    sequence_.store(begin + 1, std::memory_order_relaxed);  // odd: a write is in progress
    std::atomic_thread_fence(std::memory_order_release);
    std::memcpy(&record_, &next, sizeof(Record));
    std::atomic_thread_fence(std::memory_order_release);
    sequence_.store(begin + 2, std::memory_order_release);  // even: the record is whole
  }

  ReadResult Read(Record* out) const noexcept {
    const std::uint32_t before = sequence_.load(std::memory_order_acquire);
    if (before == 0) return ReadResult::kEmpty;
    if ((before & 1U) != 0) return ReadResult::kBusy;
    std::memcpy(out, &record_, sizeof(Record));
    std::atomic_thread_fence(std::memory_order_acquire);
    const std::uint32_t after = sequence_.load(std::memory_order_relaxed);
    return after == before ? ReadResult::kRead : ReadResult::kBusy;
  }

  bool HasBeenSet() const noexcept { return sequence_.load(std::memory_order_acquire) != 0; }

  // Test support: forgets the request. Not for a slot a signal handler can read at the same moment.
  void ResetForTest() noexcept {
    sequence_.store(0, std::memory_order_relaxed);
    record_ = Record{};
  }

 private:
  std::atomic<std::uint32_t> sequence_{0};
  Record record_{};
};

// The fatal event in progress. The first thread to claim it runs ours (the report and the dump); a thread that
// arrives while that runs waits for it through Done, so the game's handler (which may end the process) never races
// the dump. A thread that arrives after it finished finds Done true at once and goes straight to the game's handler.
class FatalClaim {
 public:
  // True for exactly one caller per process (until ResetForTest).
  bool TryClaim() noexcept { return !claimed_.exchange(true, std::memory_order_acq_rel); }
  // The claimant calls this when ours has finished, whatever it achieved.
  void MarkDone() noexcept { done_.store(true, std::memory_order_release); }
  bool Done() const noexcept { return done_.load(std::memory_order_acquire); }
  void ResetForTest() noexcept {
    claimed_.store(false, std::memory_order_relaxed);
    done_.store(false, std::memory_order_relaxed);
  }

 private:
  std::atomic<bool> claimed_{false};
  std::atomic<bool> done_{false};
};

}  // namespace nevr_crash_first
