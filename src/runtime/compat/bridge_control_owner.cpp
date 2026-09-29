#include "runtime/compat/bridge_control_owner.h"

#include <exception>
#include <future>
#include <system_error>
#include <utility>

#include "core/logging.h"

namespace BridgeControl {

ConnectionLifetime::Lease::Lease(std::shared_ptr<ConnectionLifetime> owner)
    : owner_(std::move(owner)) {}

ConnectionLifetime::Lease::~Lease() {
  owner_->Release();
}

std::shared_ptr<ConnectionLifetime::Lease> ConnectionLifetime::TryAcquire() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!accepting_) return {};
  ++activeLeases_;
  return std::shared_ptr<Lease>(new Lease(shared_from_this()));
}

bool ConnectionLifetime::PostIfAccepting(const std::function<bool()>& post) {
  std::lock_guard<std::mutex> lock(mutex_);
  return accepting_ && post();
}

bool ConnectionLifetime::PostIfBothAccepting(
    const std::shared_ptr<ConnectionLifetime>& first,
    const std::shared_ptr<ConnectionLifetime>& second,
    const std::function<bool()>& post) {
  if (!first || !second) return false;
  if (first == second) return first->PostIfAccepting(post);
  ConnectionLifetime* lower = first.get();
  ConnectionLifetime* upper = second.get();
  if (std::less<ConnectionLifetime*>{}(upper, lower)) std::swap(lower, upper);
  std::unique_lock<std::mutex> lowerLock(lower->mutex_);
  std::unique_lock<std::mutex> upperLock(upper->mutex_);
  return first->accepting_ && second->accepting_ && post();
}

ConnectionLifetime::CloseRequest ConnectionLifetime::CloseAndPost(
    bool retireNow,
    const std::function<bool(const std::shared_ptr<std::promise<bool>>&)>& post) {
  auto candidatePromise = std::make_shared<std::promise<bool>>();
  std::shared_future<bool> candidateCompletion = candidatePromise->get_future().share();
  std::lock_guard<std::mutex> lock(mutex_);
  if (closePromise_) {
    if (retireNow) retired_ = true;
    return CloseRequest{false, closeQueued_, closeCompletion_};
  }
  accepting_ = false;
  retired_ = retired_ || retireNow;
  closePromise_ = candidatePromise;
  closeCompletion_ = candidateCompletion;
  try {
    closeQueued_ = post(candidatePromise);
  } catch (const std::exception&) {
    closeQueued_ = false;
  }
  if (!closeQueued_) {
    try {
      candidatePromise->set_value(false);
    } catch (const std::future_error&) {
    }
  }
  return CloseRequest{true, closeQueued_, closeCompletion_};
}

void ConnectionLifetime::CloseAdmission() {
  std::lock_guard<std::mutex> lock(mutex_);
  accepting_ = false;
  if (activeLeases_ == 0) condition_.notify_all();
}

void ConnectionLifetime::Retire() {
  std::lock_guard<std::mutex> lock(mutex_);
  accepting_ = false;
  retired_ = true;
}

void ConnectionLifetime::WaitForLeases() {
  std::unique_lock<std::mutex> lock(mutex_);
  condition_.wait(lock, [this] { return activeLeases_ == 0; });
}

bool ConnectionLifetime::IsOpen() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return accepting_;
}

bool ConnectionLifetime::IsRetired() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return retired_;
}

void ConnectionLifetime::Release() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (activeLeases_ > 0) --activeLeases_;
  if (!accepting_ && activeLeases_ == 0) condition_.notify_all();
}

OwnerQueue::OwnerQueue(ThreadFactory threadFactory)
    : threadFactory_(std::move(threadFactory)) {}

OwnerQueue::~OwnerQueue() {
  (void)StopAndJoin();
}

bool OwnerQueue::Start() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (worker_.joinable()) return started_;
  stopping_ = false;
  started_ = false;
  ownerThreadId_ = std::thread::id{};
  try {
    Task task = [this] { Run(); };
    worker_ = threadFactory_ ? threadFactory_(std::move(task))
                             : std::thread([this] { Run(); });
  } catch (const std::system_error&) {
    return false;
  }
  if (!worker_.joinable()) return false;
  started_ = true;
  return true;
}

OwnerQueue::PostResult OwnerQueue::PostData(size_t payloadBytes, Task task,
                                            uint64_t* sequence) {
  return PostDataWithSequence(payloadBytes, [task = std::move(task)](uint64_t) { task(); },
                              sequence);
}

OwnerQueue::PostResult OwnerQueue::PostDataWithSequence(
    size_t payloadBytes, std::function<void(uint64_t)> task, uint64_t* sequence) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!started_ || stopping_) return PostResult::Stopped;
  if (payloadBytes > kMaxDataBytes - pendingDataBytes_) return PostResult::DataByteLimit;
  if (pendingDataCount_ >= kMaxDataEvents) return PostResult::DataCountLimit;
  const uint64_t assigned = nextSequence_;
  try {
    events_.push_back(Event{[task = std::move(task), assigned] { task(assigned); },
                            payloadBytes, false, false, false, assigned});
  } catch (const std::exception&) {
    return PostResult::AllocationFailure;
  }
  ++nextSequence_;
  ++pendingDataCount_;
  pendingDataBytes_ += payloadBytes;
  if (sequence) *sequence = assigned;
  condition_.notify_one();
  return PostResult::Queued;
}

OwnerQueue::PostResult OwnerQueue::PostControl(Task task, uint64_t* sequence) {
  return PostControlWithSequence([task = std::move(task)](uint64_t) { task(); }, sequence);
}

OwnerQueue::PostResult OwnerQueue::PostControlWithSequence(
    std::function<void(uint64_t)> task, uint64_t* sequence) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!started_ || stopping_) return PostResult::Stopped;
  if (pendingControlCount_ >= kReservedControlEvents) return PostResult::ControlCountLimit;
  const uint64_t assigned = nextSequence_;
  try {
    events_.push_back(Event{[task = std::move(task), assigned] { task(assigned); },
                            0, true, false, false, assigned});
  } catch (const std::exception&) {
    return PostResult::AllocationFailure;
  }
  ++nextSequence_;
  ++pendingControlCount_;
  if (sequence) *sequence = assigned;
  condition_.notify_one();
  return PostResult::Queued;
}

bool OwnerQueue::ReserveCloseSlot() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (reservedCloseSlots_ >= kMaxConnectionCloseSlots) return false;
  ++reservedCloseSlots_;
  return true;
}

void OwnerQueue::ReleaseCloseSlot() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (reservedCloseSlots_ > 0) --reservedCloseSlots_;
  condition_.notify_all();
}

OwnerQueue::PostResult OwnerQueue::PostTerminalClose(Task task, uint64_t* sequence) {
  return PostTerminalCloseWithSequence(
      [task = std::move(task)](uint64_t) { task(); }, sequence);
}

OwnerQueue::PostResult OwnerQueue::PostTerminalCloseWithSequence(
    std::function<void(uint64_t)> task, uint64_t* sequence) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!started_ || stopping_) return PostResult::Stopped;
  if (pendingCloseCount_ >= reservedCloseSlots_) return PostResult::CloseCapacityLimit;
  const uint64_t assigned = nextSequence_;
  try {
    events_.push_back(Event{[task = std::move(task), assigned] { task(assigned); },
                            0, false, false, true, assigned});
  } catch (const std::exception&) {
    return PostResult::AllocationFailure;
  }
  ++nextSequence_;
  ++pendingCloseCount_;
  if (sequence) *sequence = assigned;
  condition_.notify_one();
  return PostResult::Queued;
}

bool OwnerQueue::PostFence(Task task) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!started_ || stopping_) return false;
  try {
    events_.push_back(Event{std::move(task), 0, true, true, false, nextSequence_++});
  } catch (const std::exception&) {
    return false;
  }
  condition_.notify_one();
  return true;
}

bool OwnerQueue::InvokeControlAndWait(Task task) {
  if (IsOwnerThread()) {
    task();
    return true;
  }
  auto completion = std::make_shared<std::promise<void>>();
  std::future<void> result = completion->get_future();
  if (!PostControlAndWait([task = std::move(task), completion] {
    try {
      task();
      completion->set_value();
    } catch (const std::exception&) {
      completion->set_exception(std::current_exception());
    }
  })) return false;
  result.get();
  return true;
}

bool OwnerQueue::PostControlAndWait(Task task) {
  std::unique_lock<std::mutex> lock(mutex_);
  condition_.wait(lock, [this] {
    return !started_ || stopping_ || pendingControlCount_ < kReservedControlEvents;
  });
  if (!started_ || stopping_) return false;
  try {
    events_.push_back(Event{std::move(task), 0, true, false, false, nextSequence_});
  } catch (const std::exception&) {
    return false;
  }
  ++nextSequence_;
  ++pendingControlCount_;
  condition_.notify_one();
  return true;
}

bool OwnerQueue::StopAndJoin() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!worker_.joinable()) return true;
    if (ownerThreadId_ == std::this_thread::get_id()) return false;
    stopping_ = true;
  }
  condition_.notify_all();
  worker_.join();
  std::lock_guard<std::mutex> lock(mutex_);
  started_ = false;
  stopping_ = false;
  return true;
}

bool OwnerQueue::IsOwnerThread() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return ownerThreadId_ == std::this_thread::get_id();
}

void OwnerQueue::Run() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ownerThreadId_ = std::this_thread::get_id();
  }
  for (;;) {
    Event event;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      condition_.wait(lock, [this] {
        return stopping_ || !events_.empty();
      });
      if (events_.empty() && stopping_) break;
      event = std::move(events_.front());
      events_.pop_front();
      if (event.fence) {
        // Fences bypass the bounded control lane and have their own queue.
      } else if (event.close) {
        --pendingCloseCount_;
      } else if (event.control) {
        --pendingControlCount_;
      } else {
        --pendingDataCount_;
        pendingDataBytes_ -= event.payloadBytes;
      }
      condition_.notify_all();
    }
    try {
      event.task();
    } catch (const std::exception&) {
      Log(EchoVR::LogLevel::Error, "[NEVR.WS] Bridge owner task failed");
    }
  }
}

}  // namespace BridgeControl
