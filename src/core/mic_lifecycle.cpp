/* SYNTHESIS -- custom tool code, not from binary */

#include "core/mic_lifecycle.h"

bool MicComInitializationRequiresUninitialize(int32_t hresult) {
  return hresult >= 0;
}

bool MicCaptureLifecycle::BeginTransition(std::unique_lock<std::mutex>& lock) {
  while (transitionBusy_) {
    if (transitionThread_ == std::this_thread::get_id()) return false;
    changed_.wait(lock);
  }
  transitionBusy_ = true;
  transitionThread_ = std::this_thread::get_id();
  return true;
}

void MicCaptureLifecycle::EndTransition(std::unique_lock<std::mutex>& lock) {
  transitionBusy_ = false;
  transitionThread_ = std::thread::id{};
  lock.unlock();
  changed_.notify_all();
}

bool MicCaptureLifecycle::Create(uint32_t ownerThread, const MicLifecycleOperations& ops) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (!BeginTransition(lock)) return false;
  if (state_ == MicLifecycleState::Ready || state_ == MicLifecycleState::Running) {
    const bool sameOwner = ownerThread == ownerThread_;
    EndTransition(lock);
    return sameOwner;
  }
  if (state_ != MicLifecycleState::Closed || ownerThread == 0 || ops.createResources == nullptr) {
    EndTransition(lock);
    return false;
  }
  lock.unlock();

  const bool created = ops.createResources(ops.context);

  lock.lock();
  if (created) {
    ownerThread_ = ownerThread;
    workerExists_ = false;
    audioStarted_ = false;
    if (ops.resetStream != nullptr) ops.resetStream(ops.context);
    state_ = MicLifecycleState::Ready;
  }
  EndTransition(lock);
  return created;
}

bool MicCaptureLifecycle::Start(uint32_t callerThread, const MicLifecycleOperations& ops) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (!BeginTransition(lock)) return false;
  if (state_ != MicLifecycleState::Ready || callerThread != ownerThread_ ||
      ops.startAudio == nullptr || ops.createWorker == nullptr || ops.stopAudio == nullptr) {
    EndTransition(lock);
    return false;
  }
  lock.unlock();

  // Every capture starts from an empty stream: audio read or polled while stopped must not make
  // the new capture look like it already has a listening reader (#95).
  if (ops.resetStream != nullptr) ops.resetStream(ops.context);
  // A client that cannot start may be dead (its device was invalidated): get a fresh one once and retry.
  bool started = ops.startAudio(ops.context);
  if (!started && ops.recoverAudio != nullptr && ops.recoverAudio(ops.context)) {
    started = ops.startAudio(ops.context);
  }
  if (!started) {
    lock.lock();
    EndTransition(lock);
    return false;
  }

  lock.lock();
  audioStarted_ = true;
  lock.unlock();
  const MicWorkerCreateResult createResult = ops.createWorker(ops.context);

  lock.lock();
  if (createResult == MicWorkerCreateResult::Started) {
    workerExists_ = true;
    state_ = MicLifecycleState::Running;
    EndTransition(lock);
    return true;
  }
  workerExists_ = createResult == MicWorkerCreateResult::FailedWithWorker;
  state_ = workerExists_ ? MicLifecycleState::Stopping : MicLifecycleState::Ready;
  lock.unlock();

  bool workerJoined = true;
  if (createResult == MicWorkerCreateResult::FailedWithWorker) {
    workerJoined = StopWorkerOutsideLock(ops, 2000);
  }
  if (!workerJoined) {
    lock.lock();
    state_ = MicLifecycleState::FaultedWorker;
    EndTransition(lock);
    return false;
  }
  if (createResult == MicWorkerCreateResult::FailedWithWorker) {
    lock.lock();
    workerExists_ = false;
    lock.unlock();
  }

  const bool stopped = ops.stopAudio(ops.context);

  lock.lock();
  audioStarted_ = !stopped;
  if (stopped) {
    if (ops.resetStream != nullptr) ops.resetStream(ops.context);
    state_ = MicLifecycleState::Ready;
  } else {
    state_ = MicLifecycleState::FaultedNoWorker;
  }
  EndTransition(lock);
  return false;
}

bool MicCaptureLifecycle::StopWorkerOutsideLock(const MicLifecycleOperations& ops, uint32_t timeoutMs) {
  if (ops.requestStop != nullptr) {
    // A failed wake is diagnostic only: cancellation is already published by
    // the operation, so still attempt the bounded wait.
    (void)ops.requestStop(ops.context);
  }
  if (ops.waitWorker == nullptr) return false;
  const MicWorkerWaitResult waitResult = ops.waitWorker(ops.context, timeoutMs);
  if (waitResult != MicWorkerWaitResult::Signaled) return false;
  if (ops.closeWorker != nullptr) ops.closeWorker(ops.context);
  return true;
}

bool MicCaptureLifecycle::Stop(uint32_t callerThread, const MicLifecycleOperations& ops,
                               uint32_t timeoutMs) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (!BeginTransition(lock)) return false;
  if (state_ == MicLifecycleState::Closed) {
    EndTransition(lock);
    return true;
  }
  if (callerThread != ownerThread_) {
    EndTransition(lock);
    return false;
  }

  if (workerExists_) {
    state_ = MicLifecycleState::Stopping;
    lock.unlock();
    const bool joined = StopWorkerOutsideLock(ops, timeoutMs);
    lock.lock();
    if (!joined) {
      state_ = MicLifecycleState::FaultedWorker;
      EndTransition(lock);
      return false;
    }
    workerExists_ = false;
  }

  if (audioStarted_) {
    lock.unlock();
    const bool stopped = ops.stopAudio != nullptr && ops.stopAudio(ops.context);
    lock.lock();
    if (!stopped) {
      state_ = MicLifecycleState::FaultedNoWorker;
      EndTransition(lock);
      return false;
    }
    audioStarted_ = false;
  }

  if (ops.resetStream != nullptr) ops.resetStream(ops.context);
  state_ = MicLifecycleState::Ready;
  EndTransition(lock);
  return true;
}

bool MicCaptureLifecycle::Destroy(uint32_t callerThread, const MicLifecycleOperations& ops,
                                  uint32_t timeoutMs) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (!BeginTransition(lock)) return false;
  if (state_ == MicLifecycleState::Closed) {
    EndTransition(lock);
    return true;
  }
  if (callerThread != ownerThread_) {
    EndTransition(lock);
    return false;
  }

  if (workerExists_) {
    state_ = MicLifecycleState::Stopping;
    lock.unlock();
    const bool joined = StopWorkerOutsideLock(ops, timeoutMs);
    lock.lock();
    if (!joined) {
      state_ = MicLifecycleState::FaultedWorker;
      EndTransition(lock);
      return false;
    }
    workerExists_ = false;
  }

  if (audioStarted_) {
    lock.unlock();
    const bool stopped = ops.stopAudio != nullptr && ops.stopAudio(ops.context);
    lock.lock();
    if (!stopped) {
      state_ = MicLifecycleState::FaultedNoWorker;
      EndTransition(lock);
      return false;
    }
    audioStarted_ = false;
  }

  lock.unlock();
  if (ops.releaseResources != nullptr) ops.releaseResources(ops.context);
  if (ops.resetStream != nullptr) ops.resetStream(ops.context);

  lock.lock();
  workerExists_ = false;
  audioStarted_ = false;
  ownerThread_ = 0;
  state_ = MicLifecycleState::Closed;
  EndTransition(lock);
  return true;
}

MicLifecycleState MicCaptureLifecycle::State() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

bool MicCaptureLifecycle::IsCreated() const {
  const MicLifecycleState state = State();
  return state != MicLifecycleState::Closed;
}

bool MicCaptureLifecycle::HasWorker() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return workerExists_;
}

uint32_t MicCaptureLifecycle::OwnerThread() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return ownerThread_;
}

bool MicCaptureLifecycle::Recover(uint32_t callerThread, const MicLifecycleOperations& ops,
                                  uint32_t timeoutMs) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (!BeginTransition(lock)) return false;
  if (callerThread != ownerThread_ || ops.recoverAudio == nullptr ||
      (state_ != MicLifecycleState::Running && state_ != MicLifecycleState::Ready)) {
    EndTransition(lock);
    return false;
  }
  if (workerExists_) {
    state_ = MicLifecycleState::Stopping;
    lock.unlock();
    const bool joined = StopWorkerOutsideLock(ops, timeoutMs);
    lock.lock();
    if (!joined) {
      state_ = MicLifecycleState::FaultedWorker;
      EndTransition(lock);
      return false;
    }
    workerExists_ = false;
  }
  // The invalidated client cannot be stopped; recoverAudio releases it.
  audioStarted_ = false;
  state_ = MicLifecycleState::Ready;
  lock.unlock();

  const bool recovered = ops.recoverAudio(ops.context);

  lock.lock();
  EndTransition(lock);
  if (!recovered) return false;
  lock.unlock();
  return Start(callerThread, ops);
}
