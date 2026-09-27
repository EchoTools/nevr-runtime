/* SYNTHESIS -- custom tool code, not from binary */
#pragma once

#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <thread>

enum class MicWorkerWaitResult {
  Signaled,
  Timeout,
  Failed,
};

enum class MicWorkerCreateResult {
  Started,
  FailedWithoutWorker,
  FailedWithWorker,
};

enum class MicLifecycleState {
  Closed,
  Ready,
  Running,
  Stopping,
  FaultedWorker,
  FaultedNoWorker,
};

struct MicLifecycleOperations {
  void* context = nullptr;
  bool (*createResources)(void*) = nullptr;
  bool (*startAudio)(void*) = nullptr;
  MicWorkerCreateResult (*createWorker)(void*) = nullptr;
  bool (*requestStop)(void*) = nullptr;
  MicWorkerWaitResult (*waitWorker)(void*, uint32_t timeoutMs) = nullptr;
  void (*closeWorker)(void*) = nullptr;
  bool (*stopAudio)(void*) = nullptr;
  void (*releaseResources)(void*) = nullptr;
  void (*resetStream)(void*) = nullptr;
};

/// COM initialization returns S_FALSE when the calling thread was already
/// initialized in the same apartment. It is still a successful increment and
/// must be balanced with CoUninitialize; RPC_E_CHANGED_MODE is a failure.
bool MicComInitializationRequiresUninitialize(int32_t hresult);

/// Serialized lifecycle for the WASAPI owner and capture worker. The state
/// mutex is released while external operations (including bounded joins) run;
/// `transitionBusy_` keeps competing lifecycle calls serialized meanwhile.
class MicCaptureLifecycle {
 public:
  bool Create(uint32_t ownerThread, const MicLifecycleOperations& ops);
  bool Start(uint32_t callerThread, const MicLifecycleOperations& ops);
  bool Stop(uint32_t callerThread, const MicLifecycleOperations& ops, uint32_t timeoutMs);
  bool Destroy(uint32_t callerThread, const MicLifecycleOperations& ops, uint32_t timeoutMs);

  MicLifecycleState State() const;
  bool IsCreated() const;
  bool HasWorker() const;
  uint32_t OwnerThread() const;

 private:
  bool BeginTransition(std::unique_lock<std::mutex>& lock);
  void EndTransition(std::unique_lock<std::mutex>& lock);
  bool StopWorkerOutsideLock(const MicLifecycleOperations& ops, uint32_t timeoutMs);

  mutable std::mutex mutex_;
  std::condition_variable changed_;
  bool transitionBusy_ = false;
  std::thread::id transitionThread_;
  MicLifecycleState state_ = MicLifecycleState::Closed;
  uint32_t ownerThread_ = 0;
  bool workerExists_ = false;
  bool audioStarted_ = false;
};
