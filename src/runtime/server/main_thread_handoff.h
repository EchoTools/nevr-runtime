/* SYNTHESIS -- custom tool code, not from binary */
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>

namespace nevr_game_server {

/// Hands one task from a background thread to the thread that calls Service()
/// — in production GameServerLib::Update(), which the game calls on its main
/// thread (GH #44).
///
/// Why it exists: the callback registry (ServerContext::GetCallbackRegistry)
/// and the game's broadcaster listener table are main-thread-only.
/// EchoVR::BroadcasterUnlisten (echovr.exe 0x140f8df20) takes no lock: it
/// unlinks the handle from the broadcaster's listener hash chain and only
/// defers the delete when a plain "dispatch in progress" bit is set — a
/// same-thread reentrancy guard for SBroadcasterData's dispatch loop, not a
/// cross-thread one. The graceful-shutdown thread must therefore not
/// unregister by itself; it asks the game thread to do it and waits.
///
/// The game thread is not ours: it can stop calling Update() (level
/// transitions, teardown). The wait is therefore bounded, and the caller
/// decides what to do on kTimedOut / kCancelled.
///
/// One request at a time. A request the servicing thread has already started
/// is never abandoned: the requester keeps waiting past its timeout until the
/// task finishes, so it never runs the fallback while the task is still
/// running on the other thread.
class MainThreadHandoff {
 public:
  using Task = std::function<void()>;

  enum class Outcome {
    kRan,         // the servicing thread ran the task to completion
    kTaskThrew,   // the servicing thread ran the task and it threw a std::exception
    kTimedOut,    // nobody called Service() in time; the task did not run and never will
    kCancelled,   // Cancel() withdrew the request before it started; the task did not run
    kBusy,        // another request was already in flight; this task did not run
  };

  MainThreadHandoff() = default;
  MainThreadHandoff(const MainThreadHandoff&) = delete;
  MainThreadHandoff& operator=(const MainThreadHandoff&) = delete;

  /// Requester side (any thread except the servicing one). Queues the task and
  /// blocks until it has run, the timeout elapses before it started, or
  /// Cancel() withdraws it.
  Outcome RunOnServicingThread(Task task, std::chrono::milliseconds timeout);

  /// Servicing side. Runs the pending task, if any, on the calling thread.
  /// Returns true when a task ran (successfully or by throwing). Costs one
  /// atomic load when nothing is pending. Never lets an exception escape: this
  /// is called from a game vtable entry point.
  bool Service();

  /// Withdraws a request that has not started yet; its requester returns
  /// kCancelled. A request already running is left to finish.
  void Cancel();

  /// True while a request is waiting to be serviced.
  bool HasPending() const { return pending_.load(std::memory_order_acquire); }

 private:
  enum class State { kIdle, kPending, kRunning, kDone, kThrew, kCancelled };

  bool IsFinishedLocked() const;  // caller holds mutex_

  std::mutex mutex_;
  std::condition_variable finished_;
  std::atomic<bool> pending_{false};
  State state_ = State::kIdle;
  Task task_;
};

const char* MainThreadHandoffOutcomeName(MainThreadHandoff::Outcome outcome);

}  // namespace nevr_game_server
