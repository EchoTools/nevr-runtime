/* SYNTHESIS -- custom tool code, not from binary */
#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

/// One dedicated thread that runs every mic lifecycle transition (GH #51).
///
/// Why it exists: the WASAPI provider's resources are thread-affine. MicCreate
/// calls CoInitializeEx on the thread it runs on and creates the WASAPI objects
/// in that thread's apartment; MicDestroy must make the balancing
/// CoUninitialize call on that same thread. MicCaptureLifecycle enforces this
/// by recording an owner thread and refusing transitions from any other.
///
/// The game does not honour that contract. echovr.exe calls MicCreate and
/// MicStart only from CR15NetVoipBroadcasterCS::UpdateGlobal (0x140d7cfc0),
/// a component-system job, and live client logs show the two calls landing on
/// different OS threads. The owner-thread rule is correct; the game cannot be
/// changed to respect it, so callers are marshalled here instead: Run() hands
/// the task to the owner thread and blocks until it returns, so every
/// transition — and every CoInitializeEx/CoUninitialize pair — happens on one
/// thread no matter which game thread asked.
///
/// Run() calls are serialized: one task runs at a time, in arrival order of
/// the internal control lock. Run() from the owner thread itself (a task that
/// calls back into the provider) executes inline rather than deadlocking.
///
/// Teardown: Shutdown() stops and joins the thread; the next Run() starts a
/// fresh one. Call Shutdown() before destruction. The destructor only detaches
/// a still-running thread: during process exit the OS has already terminated
/// it, and joining under the loader lock (DLL_PROCESS_DETACH) can deadlock.
class MicOwnerThread {
 public:
  using Task = void (*)(void* context);

  MicOwnerThread() = default;
  MicOwnerThread(const MicOwnerThread&) = delete;
  MicOwnerThread& operator=(const MicOwnerThread&) = delete;
  ~MicOwnerThread();

  /// Runs task(context) on the owner thread, starting the thread if needed,
  /// and blocks until the task returns. Returns true when the task ran to
  /// completion; false when the thread could not be started (task not run)
  /// or the task exited by throwing a std::exception.
  bool Run(Task task, void* context);

  /// Stops and joins the owner thread. Returns true when no owner thread
  /// remains afterwards; false when called from the owner thread itself
  /// (a thread cannot join itself — the thread is left running).
  bool Shutdown();

  /// Shutdown(), but only when shouldStop(context) returns true. The predicate
  /// is evaluated under the same lock that serializes Run(), so no Run() can
  /// slip in between the decision and the join (e.g. a MicCreate landing
  /// between "provider is closed" and stopping the thread that would own it).
  /// Returns true when no owner thread remains afterwards.
  bool ShutdownWhen(bool (*shouldStop)(void* context), void* context);

  /// True while an owner thread exists.
  bool IsRunning() const;

  /// True when the calling thread is the owner thread.
  bool IsOwnerThread() const;

 private:
  void ThreadMain();
  bool StartLocked();

  // Serializes whole Run()/Shutdown() calls against each other.
  std::mutex controlMutex_;

  // Guards the hand-off state below.
  mutable std::mutex stateMutex_;
  std::condition_variable taskReady_;
  std::condition_variable taskDone_;
  std::thread thread_;
  std::thread::id threadId_;
  Task task_ = nullptr;
  void* context_ = nullptr;
  uint64_t submitted_ = 0;
  uint64_t completed_ = 0;
  bool lastTaskThrew_ = false;
  bool stopRequested_ = false;
};
