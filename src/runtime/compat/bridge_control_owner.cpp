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
  if (!accepting_ || closeRequested_) return {};
  ++activeLeases_;
  return std::shared_ptr<Lease>(new Lease(shared_from_this()));
}

std::shared_ptr<ConnectionLifetime::Lease> ConnectionLifetime::TryAcquireClose() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (closeLeaseTaken_) return {};
  closeLeaseTaken_ = true;
  accepting_ = false;
  closeRequested_ = true;
  ++activeLeases_;
  return std::shared_ptr<Lease>(new Lease(shared_from_this()));
}

void ConnectionLifetime::RequestClose() {
  std::lock_guard<std::mutex> lock(mutex_);
  closeRequested_ = true;
}

void ConnectionLifetime::CancelCloseRequest() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (accepting_) closeRequested_ = false;
}

void ConnectionLifetime::CloseAdmission() {
  std::lock_guard<std::mutex> lock(mutex_);
  accepting_ = false;
  if (activeLeases_ == 0) condition_.notify_all();
}

void ConnectionLifetime::WaitForLeases() {
  std::unique_lock<std::mutex> lock(mutex_);
  condition_.wait(lock, [this] { return activeLeases_ == 0; });
}

bool ConnectionLifetime::IsOpen() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return accepting_ && !closeRequested_;
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

OwnerQueue::PostResult OwnerQueue::PostData(size_t payloadBytes, Task task) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!started_ || stopping_) return PostResult::Stopped;
  if (payloadBytes > kMaxDataBytes - pendingDataBytes_) return PostResult::DataByteLimit;
  if (pendingDataCount_ >= kMaxDataEvents) return PostResult::DataCountLimit;
  ++pendingDataCount_;
  pendingDataBytes_ += payloadBytes;
  dataEvents_.push_back(Event{std::move(task), payloadBytes, false});
  condition_.notify_one();
  return PostResult::Queued;
}

OwnerQueue::PostResult OwnerQueue::PostControl(Task task) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!started_ || stopping_) return PostResult::Stopped;
  if (pendingControlCount_ >= kReservedControlEvents) return PostResult::ControlCountLimit;
  ++pendingControlCount_;
  controlEvents_.push_back(Event{std::move(task), 0, true});
  condition_.notify_one();
  return PostResult::Queued;
}

bool OwnerQueue::PostFence(Task task) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_ || stopping_) return false;
    fenceEvents_.push_back(Event{std::move(task), 0, true, true});
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
  if (!PostFence([task = std::move(task), completion] {
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
        return stopping_ || !fenceEvents_.empty() || !controlEvents_.empty() || !dataEvents_.empty();
      });
      if (fenceEvents_.empty() && controlEvents_.empty() && dataEvents_.empty() && stopping_) break;
      if (!fenceEvents_.empty()) {
        event = std::move(fenceEvents_.front());
        fenceEvents_.pop_front();
      } else if (!controlEvents_.empty()) {
        event = std::move(controlEvents_.front());
        controlEvents_.pop_front();
      } else {
        event = std::move(dataEvents_.front());
        dataEvents_.pop_front();
      }
      if (event.fence) {
        // Fences bypass the bounded control lane and have their own queue.
      } else if (event.control) {
        --pendingControlCount_;
      } else {
        --pendingDataCount_;
        pendingDataBytes_ -= event.payloadBytes;
      }
    }
    try {
      event.task();
    } catch (const std::exception&) {
      Log(EchoVR::LogLevel::Error, "[NEVR.WS] Bridge owner task failed");
    }
  }
}

}  // namespace BridgeControl
