#pragma once

// The runtime's update worker (docs/adr/0024-pc-self-update.md, sections 3 and 6): decides once per launch whether to check, runs the
// check on a thread that is not the game's, and confirms a swapped release once the per-frame dispatcher has run for 10 s.
//
//   * Start() is called once, from the deferred runtime bootstrap (the graphics device exists on a client; on a server the first
//     original call has returned), never from DllMain and never before the boot-state limit of N146. The game never waits for it.
//   * The check thread runs RunCheck (update_run.h) once and ends. A failure ends it too; the next launch tries again.
//   * The confirm thread waits on an event the per-frame dispatcher (frame/tick.cpp, OnFrame) sets 10 s after its first call, then
//     runs UpdateConfirm once. It runs whether or not updates are enabled: a release the host swapped in has to be confirmed
//     even when the operator has since switched checks off, or the host rolls it back after two starts.
//   * No thread is started when the host cannot swap (no capability bit) or the build is a dev build: a pending release exists
//     only after a host's swap, and a dev runtime is never replaced.
//   * Every decision is one `[NEVR.UPDATE]` line (`update disabled`, `update check`, `update verify`, `update published`,
//     `update confirmed`, `update confirm skipped`).

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "runtime/lifecycle/update_policy.h"
#include "runtime/lifecycle/update_run.h"

namespace nevr_update {

struct Deps {
  Switches switches;
  std::optional<std::string> config_channel;  ///< update.channel
  bool server = false;
  std::wstring root;
  /// Read once on the worker's own thread, so no file is hashed on the game thread: the version and the sha256 of the file this
  /// process loaded. Empty when it could not be read, which stops the check and the confirmation with a logged reason.
  std::function<bool(std::string* version, std::string* sha256, std::string* error)> read_running;
  Core core;
  std::function<VerifyOutcome(const std::wstring& path)> verify;
  LogFn log;
  std::function<std::uint64_t()> now_ms;
};

class Worker {
 public:
  explicit Worker(Deps deps);
  Worker(const Worker&) = delete;
  Worker& operator=(const Worker&) = delete;
  ~Worker();

  /// Decides and starts the threads. Returns what was decided; a second call does nothing and returns the same.
  Disabled Start();
  /// The per-frame dispatcher's call, every dispatch, from any thread. Cheap: an atomic compare until the mark.
  void OnFrame(std::uint64_t now_ms);
  /// True once the check thread has ended (a test waits on it).
  bool WaitCheckDone(DWORD timeout_ms);
  bool WaitConfirmDone(DWORD timeout_ms);
  /// Sets the stop event and joins both threads, waiting at most `timeout_ms` for each; a thread still inside the core is detached and
  /// left to the process's exit. Called by the destructor with a short bound.
  void Stop(DWORD timeout_ms);

 /// The state the two threads share with the Worker (update_worker.cpp). The threads own a reference, so a thread still inside the
  /// core when the Worker is destroyed (the core cannot be cancelled) is detached safely and ends with the process.
  struct Shared;

 private:
  std::shared_ptr<Shared> shared_;
};

/// The process's worker, built from the host's capability word (nevr_runtime_abi::HostCanSwap), the command line (`-no-update`),
/// config.yaml (`update.enabled`, `update.channel`) and the build (nevr_plugin_trust::IsDevBuild). Call once from
/// RunDeferredRuntimeBootstrap after the command line is parsed.
void StartProcessWorker(bool host_can_swap, bool flag_no_update, bool server);
/// The per-frame dispatcher's hook (frame/tick.cpp, DispatchPerFrameWork). A no-op until StartProcessWorker ran.
void OnFrame(std::uint64_t now_us);

}  // namespace nevr_update
