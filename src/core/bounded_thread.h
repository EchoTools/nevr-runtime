#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace nevr {

/// A worker thread that can never reach std::terminate through its destructor.
///
/// A namespace-scope std::thread that is still joinable when the process exits has its destructor
/// run from the DLL's atexit chain during DLL_PROCESS_DETACH, which calls std::terminate and ends
/// the process with exit code 3 (#340). Joining there is not an option either: the loader lock is
/// held and the OS may already have stopped the thread. This type therefore has two exits:
///   - JoinFor(): wait up to a bound for the thread to finish, join it if it did, detach it if not.
///   - ~BoundedThread(): detach a thread nobody joined; never terminates.
/// The thread's own state is shared (not owned by this object), so a detached thread that outlives
/// the object stays valid.
class BoundedThread {
 public:
  BoundedThread() = default;
  BoundedThread(const BoundedThread&) = delete;
  BoundedThread& operator=(const BoundedThread&) = delete;
  ~BoundedThread() { Abandon(); }

  /// Starts fn on a new thread. A previous thread that is still attached is abandoned first.
  void Start(std::function<void()> fn) {
    Abandon();
    auto state = std::make_shared<State>();
    state_ = state;
    thread_ = std::thread([state, fn = std::move(fn)]() {
      fn();
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->done = true;
      }
      state->cv.notify_all();
    });
  }

  bool Joinable() const { return thread_.joinable(); }

  /// Waits up to `timeout` for the thread function to return. Returns true when it returned (the
  /// thread is joined), false when the wait timed out (the thread is detached and keeps running).
  /// Returns true when there is no thread.
  bool JoinFor(std::chrono::milliseconds timeout) {
    if (!thread_.joinable()) return true;
    bool finished = false;
    {
      std::unique_lock<std::mutex> lock(state_->mutex);
      finished = state_->cv.wait_for(lock, timeout, [this] { return state_->done; });
    }
    if (finished) {
      thread_.join();
    } else {
      thread_.detach();
    }
    state_.reset();
    return finished;
  }

  /// JoinFor(), then release() only when the thread has finished. Whatever the thread reads (its
  /// buffers, maps, handles) must be freed through this: a thread that was detached is still running
  /// and still dereferencing them. Returns the JoinFor() result.
  template <class Release>
  bool JoinForThenRelease(std::chrono::milliseconds timeout, Release&& release) {
    const bool finished = JoinFor(timeout);
    if (finished) release();
    return finished;
  }

 private:
  struct State {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
  };

  void Abandon() {
    if (thread_.joinable()) thread_.detach();
    state_.reset();
  }

  std::thread thread_;
  std::shared_ptr<State> state_;
};

}  // namespace nevr
