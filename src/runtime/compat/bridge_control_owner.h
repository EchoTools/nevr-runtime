#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace BridgeControl {

class ConnectionLifetime : public std::enable_shared_from_this<ConnectionLifetime> {
 public:
  class Lease {
   public:
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    ~Lease();

   private:
    friend class ConnectionLifetime;
    explicit Lease(std::shared_ptr<ConnectionLifetime> owner);
    std::shared_ptr<ConnectionLifetime> owner_;
  };

  std::shared_ptr<Lease> TryAcquire();
  std::shared_ptr<Lease> TryAcquireClose();
  void RequestClose();
  void CancelCloseRequest();
  void CloseAdmission();
  void WaitForLeases();
  bool IsOpen() const;

 private:
  void Release();

  mutable std::mutex mutex_;
  std::condition_variable condition_;
  size_t activeLeases_ = 0;
  bool accepting_ = true;
  bool closeRequested_ = false;
  bool closeLeaseTaken_ = false;
};

class OwnerQueue {
 public:
  using Task = std::function<void()>;
  using ThreadFactory = std::function<std::thread(Task)>;

  enum class PostResult {
    Queued,
    Stopped,
    DataCountLimit,
    DataByteLimit,
    ControlCountLimit,
  };

  static constexpr size_t kMaxDataEvents = 256;
  static constexpr size_t kMaxDataBytes = 4 * 1024 * 1024;
  static constexpr size_t kReservedControlEvents = 64;

  explicit OwnerQueue(ThreadFactory threadFactory = {});
  ~OwnerQueue();
  OwnerQueue(const OwnerQueue&) = delete;
  OwnerQueue& operator=(const OwnerQueue&) = delete;

  bool Start();
  PostResult PostData(size_t payloadBytes, Task task);
  PostResult PostControl(Task task);
  bool PostFence(Task task);
  bool InvokeControlAndWait(Task task);
  bool StopAndJoin();
  bool IsOwnerThread() const;

 private:
  struct Event {
    Task task;
    size_t payloadBytes = 0;
    bool control = false;
    bool fence = false;
  };

  void Run();

  ThreadFactory threadFactory_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<Event> fenceEvents_;
  std::deque<Event> controlEvents_;
  std::deque<Event> dataEvents_;
  std::thread worker_;
  std::thread::id ownerThreadId_;
  size_t pendingDataCount_ = 0;
  size_t pendingDataBytes_ = 0;
  size_t pendingControlCount_ = 0;
  bool started_ = false;
  bool stopping_ = false;
};

}  // namespace BridgeControl
