#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

namespace BridgeControl {

class ConnectionLifetime : public std::enable_shared_from_this<ConnectionLifetime> {
 public:
  struct CloseRequest {
    bool first = false;
    bool queued = false;
    std::shared_future<bool> completion;
  };

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
  bool PostIfAccepting(const std::function<bool()>& post);
  static bool PostIfBothAccepting(
      const std::shared_ptr<ConnectionLifetime>& first,
      const std::shared_ptr<ConnectionLifetime>& second,
      const std::function<bool()>& post);
  CloseRequest CloseAndPost(
      bool retireNow,
      const std::function<bool(const std::shared_ptr<std::promise<bool>>&)>& post);
  void CloseAdmission();
  void Retire();
  void WaitForLeases();
  bool IsOpen() const;
  bool IsRetired() const;

 private:
  void Release();

  mutable std::mutex mutex_;
  std::condition_variable condition_;
  size_t activeLeases_ = 0;
  bool accepting_ = true;
  bool retired_ = false;
  bool closeQueued_ = false;
  std::shared_ptr<std::promise<bool>> closePromise_;
  std::shared_future<bool> closeCompletion_;
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
    CloseCapacityLimit,
    AllocationFailure,
  };

  static constexpr size_t kMaxDataEvents = 256;
  static constexpr size_t kMaxDataBytes = 4 * 1024 * 1024;
  static constexpr size_t kReservedControlEvents = 64;
  static constexpr size_t kMaxConnectionCloseSlots = 64;

  explicit OwnerQueue(ThreadFactory threadFactory = {});
  ~OwnerQueue();
  OwnerQueue(const OwnerQueue&) = delete;
  OwnerQueue& operator=(const OwnerQueue&) = delete;

  bool Start();
  PostResult PostData(size_t payloadBytes, Task task, uint64_t* sequence = nullptr);
  PostResult PostDataWithSequence(size_t payloadBytes,
                                  std::function<void(uint64_t)> task,
                                  uint64_t* sequence = nullptr);
  PostResult PostControl(Task task, uint64_t* sequence = nullptr);
  PostResult PostControlWithSequence(std::function<void(uint64_t)> task,
                                     uint64_t* sequence = nullptr);
  bool ReserveCloseSlot();
  void ReleaseCloseSlot();
  PostResult PostTerminalClose(Task task, uint64_t* sequence = nullptr);
  PostResult PostTerminalCloseWithSequence(std::function<void(uint64_t)> task,
                                           uint64_t* sequence = nullptr);
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
    bool close = false;
    uint64_t sequence = 0;
  };

  void Run();
  bool PostControlAndWait(Task task);

  ThreadFactory threadFactory_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<Event> events_;
  std::thread worker_;
  std::thread::id ownerThreadId_;
  size_t pendingDataCount_ = 0;
  size_t pendingDataBytes_ = 0;
  size_t pendingControlCount_ = 0;
  size_t reservedCloseSlots_ = 0;
  size_t pendingCloseCount_ = 0;
  uint64_t nextSequence_ = 1;
  bool started_ = false;
  bool stopping_ = false;
};

}  // namespace BridgeControl
