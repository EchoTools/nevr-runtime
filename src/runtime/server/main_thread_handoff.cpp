/* SYNTHESIS -- custom tool code, not from binary */

#include "runtime/server/main_thread_handoff.h"

#include <exception>
#include <utility>

namespace GameServer {

bool MainThreadHandoff::IsFinishedLocked() const {
  return state_ == State::kDone || state_ == State::kThrew || state_ == State::kCancelled;
}

MainThreadHandoff::Outcome MainThreadHandoff::RunOnServicingThread(Task task,
                                                                   std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (state_ != State::kIdle) return Outcome::kBusy;

  task_ = std::move(task);
  state_ = State::kPending;
  pending_.store(true, std::memory_order_release);

  const auto finished = [this]() { return IsFinishedLocked(); };

  if (!finished_.wait_for(lock, timeout, finished)) {
    if (state_ == State::kPending) {
      // Nobody picked it up. Withdraw it so a late Service() cannot run it
      // after the caller has moved on to its fallback.
      task_ = nullptr;
      pending_.store(false, std::memory_order_release);
      state_ = State::kIdle;
      return Outcome::kTimedOut;
    }
    // kRunning: the servicing thread owns the task now. Wait for it.
    finished_.wait(lock, finished);
  }

  Outcome outcome = Outcome::kCancelled;
  if (state_ == State::kDone) outcome = Outcome::kRan;
  if (state_ == State::kThrew) outcome = Outcome::kTaskThrew;
  state_ = State::kIdle;
  return outcome;
}

bool MainThreadHandoff::Service() {
  if (!pending_.load(std::memory_order_acquire)) return false;

  Task task;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::kPending) return false;
    task = std::move(task_);
    task_ = nullptr;
    pending_.store(false, std::memory_order_release);
    state_ = State::kRunning;
  }

  bool threw = false;
  try {
    if (task) task();
  } catch (const std::exception&) {
    threw = true;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = threw ? State::kThrew : State::kDone;
  }
  finished_.notify_all();
  return true;
}

void MainThreadHandoff::Cancel() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::kPending) return;
    task_ = nullptr;
    pending_.store(false, std::memory_order_release);
    state_ = State::kCancelled;
  }
  finished_.notify_all();
}

const char* MainThreadHandoffOutcomeName(MainThreadHandoff::Outcome outcome) {
  switch (outcome) {
    case MainThreadHandoff::Outcome::kRan:
      return "ran";
    case MainThreadHandoff::Outcome::kTaskThrew:
      return "task_threw";
    case MainThreadHandoff::Outcome::kTimedOut:
      return "timed_out";
    case MainThreadHandoff::Outcome::kCancelled:
      return "cancelled";
    case MainThreadHandoff::Outcome::kBusy:
      return "busy";
  }
  return "unknown";
}

}  // namespace GameServer
