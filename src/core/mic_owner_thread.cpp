/* SYNTHESIS -- custom tool code, not from binary */

#include "core/mic_owner_thread.h"

#include <exception>
#include <system_error>

MicOwnerThread::~MicOwnerThread() {
  // Normal teardown is Shutdown(). Reaching here with a live thread means
  // process exit (DLL_PROCESS_DETACH): the OS has already terminated the
  // thread, and a join under the loader lock can deadlock. Detach so the
  // std::thread destructor does not call std::terminate.
  if (thread_.joinable()) thread_.detach();
}

bool MicOwnerThread::IsRunning() const {
  std::lock_guard<std::mutex> lock(stateMutex_);
  return thread_.joinable();
}

bool MicOwnerThread::IsOwnerThread() const {
  std::lock_guard<std::mutex> lock(stateMutex_);
  return thread_.joinable() && threadId_ == std::this_thread::get_id();
}

bool MicOwnerThread::StartLocked() {
  if (thread_.joinable()) return true;
  stopRequested_ = false;
  try {
    thread_ = std::thread(&MicOwnerThread::ThreadMain, this);
  } catch (const std::system_error&) {
    return false;
  }
  threadId_ = thread_.get_id();
  return true;
}

bool MicOwnerThread::Run(Task task, void* context) {
  if (task == nullptr) return false;

  // A task already running on the owner thread that re-enters the provider
  // must not wait for itself: run it inline. The caller holds controlMutex_.
  if (IsOwnerThread()) {
    try {
      task(context);
    } catch (const std::exception&) {
      return false;
    }
    return true;
  }

  std::lock_guard<std::mutex> control(controlMutex_);
  std::unique_lock<std::mutex> lock(stateMutex_);
  if (!StartLocked()) return false;

  task_ = task;
  context_ = context;
  const uint64_t ticket = ++submitted_;
  taskReady_.notify_one();
  taskDone_.wait(lock, [this, ticket]() { return completed_ >= ticket; });
  return !lastTaskThrew_;
}

bool MicOwnerThread::Shutdown() { return ShutdownWhen(nullptr, nullptr); }

bool MicOwnerThread::ShutdownWhen(bool (*shouldStop)(void*), void* context) {
  if (IsOwnerThread()) return false;

  std::lock_guard<std::mutex> control(controlMutex_);
  if (shouldStop != nullptr && !shouldStop(context)) return !IsRunning();
  std::thread worker;
  {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (!thread_.joinable()) return true;
    stopRequested_ = true;
    worker = std::move(thread_);
  }
  taskReady_.notify_one();
  worker.join();

  std::lock_guard<std::mutex> lock(stateMutex_);
  threadId_ = std::thread::id{};
  stopRequested_ = false;
  return true;
}

void MicOwnerThread::ThreadMain() {
  std::unique_lock<std::mutex> lock(stateMutex_);
  for (;;) {
    taskReady_.wait(lock, [this]() { return task_ != nullptr || stopRequested_; });
    if (task_ == nullptr) return;  // stop requested and nothing pending

    const Task task = task_;
    void* const context = context_;
    task_ = nullptr;
    context_ = nullptr;
    lock.unlock();

    bool threw = false;
    try {
      task(context);
    } catch (const std::exception&) {
      threw = true;
    }

    lock.lock();
    lastTaskThrew_ = threw;
    ++completed_;
    taskDone_.notify_all();
  }
}
