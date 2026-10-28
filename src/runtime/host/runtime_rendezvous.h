#pragma once

// Host-side early GameMainWrapper rendezvous
// (docs/adr/0008-bugsplat-host-and-nevr-runtime.md, "Decision: the rendezvous is the
// game's main wrapper").
//
// The stable host's DllMain validates the game image and installs ONE MinHook
// detour on the game's GameMainWrapper (echovr.exe 0x1400CD510); it loads nothing and
// does nothing else there. On the first call after attach returns, the detour loads the
// runtime through RuntimeLoader::Load (the load/validate state machine, which calls the
// runtime's initialize), then returns the runtime's game_main(command_line). When no
// runtime is published it stops the game (StopGame). The original trampoline
// is published through NevrHostContext BEFORE MH_EnableHook, so the runtime never
// installs a second hook on the wrapper and a racing thread can never read a null
// trampoline; the first bytes found at the target after the hook was enabled are added to
// the context just before the load. A same-thread re-entry during runtime load bypasses to
// the original (InitOnce recursion on the loading thread deadlocks; the attach proof gate
// in tests/bootstrap_attach measures it). A failed load is a stopped game, not a degraded one
// (docs/adr/0012-static-install-root.md), and never falls back to loading the runtime from DllMain.

#include "core/runtime_abi.h"
#include "runtime/host/game_targets.h"
#include "runtime/host/runtime_load.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <windows.h>

namespace nevr_host {

class RuntimeRendezvous {
 public:
  // Record the modules and resolve the runtime path (via the loader). No detour,
  // no I/O, no allocation: safe under the loader lock. `diagnostics` must outlive
  // this object; the attach steps are recorded in it, and the load diagnostic is
  // written through it on the first call, after attach returns.
  void Prepare(HMODULE host_module, HMODULE game_module, BootstrapDiagnostics& diagnostics);

  // Validate the pinned prologue at `target` and install the GameMainWrapper
  // detour, recording each decision in the diagnostics journal (prologue accepted
  // or mismatched with the bytes found, the MinHook stage and status). Returns the
  // original trampoline on success (published before MH_EnableHook), or nullptr on
  // failure: the game is left unhooked and a degraded run of the original is the only
  // consequence. Idempotent: a second call after a successful install returns the
  // same trampoline.
  NevrGameMainFn Install(void* target);

  // Read the kGameMainWrapperPrologueSize bytes at `target` with ReadProcessMemory
  // (never a direct read, which would fault). False when they cannot be read.
  static bool ReadPrologue(const void* target, unsigned char (&out)[kGameMainWrapperPrologueSize]);

  // True when the bytes at `target` match the pinned prologue.
  static bool PrologueMatches(const void* target);

  // The detour body (MinHook calls it). Loads nevr-runtime.dll once under InitOnce and
  // returns the runtime's game_main(command_line); when no runtime is published the game is
  // stopped (StopGame) and this does not return. A same-thread re-entry during load bypasses
  // to the original.
  static std::int64_t GameMainWrapperDetour(std::int64_t command_line);

  // The loaded runtime's descriptor: set once the load succeeded, which includes the runtime's
  // initialize(). Null before the rendezvous completed and after a failed load. The host's export
  // thunks call into the runtime only through this.
  const NevrRuntimeApi* RuntimeApi() const { return api_.load(std::memory_order_acquire); }

  // How the wait for the install ended.
  enum class InstallWait { kSignalled, kUnavailable, kTimeout };

  // Wait until Install has journalled its last decision, and journal how the wait ended
  // (install_wait: signalled | unavailable | timeout). Unavailable (no event could be created)
  // and timeout both let the load proceed unsynchronized; the caller flushes the journal again
  // after the load so a late install record is still written. A detour thread calls this before
  // it touches the host context, which Install finishes writing before it signals.
  InstallWait AwaitInstall();

  // Test seam: whether Install has already published a trampoline.
  bool Installed() const { return original_ != nullptr; }

#if defined(NEVR_HOST_TESTING)
  // Test seams, compiled only into the test targets (NEVR_HOST_TESTING): the context Install built, and the
  // things a test must be able to make fail or run short. All default to production behaviour.
  struct TestSeams {
    bool fail_event_create = false;     // CreateEventW "fails": no install event
    bool fail_hook_bytes_read = false;  // the read of the hook's first bytes "fails"
    bool fail_hook_create = false;      // MH_CreateHook "fails" (the hook it made is removed)
    DWORD install_wait_ms = 5000;       // how long AwaitInstall waits (kInstallWaitMs in production)
    // StopGame calls this instead of ending the process, then the detour runs the original wrapper so a harness
    // can go on observing. Null in production: the process ends and nothing runs after.
    void (*stop_game)(std::uint32_t exit_code) = nullptr;
    // StopGame shows the player's notice through this instead of a message box; null: no dialog is ever opened
    // in a test. The text is StopNoticeTextFor(outcome, reason) (stop_notice.h).
    void (*show_stop_notice)(const wchar_t* text) = nullptr;
    // The command line StopGame reads to tell a client from a server; null: the process's own.
    const char* (*command_line)() = nullptr;
  };
  static TestSeams& Seams();
  const NevrHostContext& Context() const;
  void SetInstallEventForTest(HANDLE event);
  void SetRuntimeApiForTest(const NevrRuntimeApi* api) { api_.store(api, std::memory_order_release); }
  static void ResetActiveForTest();
#endif

  // How long a detour thread waits for the install to finish journalling.
  static constexpr DWORD kInstallWaitMs = 5000;

 private:
  // The InitOnce body: load/validate/initialize the runtime exactly once. A
  // failed load is final (never retried) and the failed runtime stays mapped. The
  // loading thread id is published for the duration so a same-thread re-entry from
  // the runtime's initialize() bypasses to the original instead of deadlocking.
  static BOOL CALLBACK LoadRuntimeOnce(PINIT_ONCE init_once, PVOID parameter, PVOID* context);

  // The runtime did not load: write the game_stop record (once, however many threads arrive), then end the
  // process with kRuntimeStopExitCode by TerminateProcess. The host's own call, never the runtime's
  // ForceFatalExit, which belongs to a runtime that is not there. Does not return in production.
  // With the test seam set it returns the original wrapper's result instead.
  std::int64_t StopGame(std::int64_t command_line);

  void Journal(AttachStep step, AttachOutcome outcome, std::uint32_t detail = 0,
               const unsigned char* actual = nullptr, HookStage stage = HookStage::kNone,
               AttachNote note = AttachNote::kNone);

  HMODULE host_module_ = nullptr;
  HMODULE game_module_ = nullptr;
  BootstrapDiagnostics* diagnostics_ = nullptr;
  RuntimeLoader loader_;
  NevrHostContext context_{};
  NevrGameMainFn original_ = nullptr;
  INIT_ONCE load_once_ = INIT_ONCE_STATIC_INIT;
  std::atomic<const NevrRuntimeApi*> api_{nullptr};
  std::atomic<DWORD> loading_thread_{0};
  std::atomic<bool> stop_recorded_{false};
  // StopGame runs one thread at a time: the first records the stop and tells the player, the others wait behind it,
  // so the process is not ended under the dialog. SRWLOCK is kernel32; the host takes no thread library.
  SRWLOCK stop_lock_ = SRWLOCK_INIT;
  // Set when Install has journalled its last decision. A thread can enter the detour while
  // MH_EnableHook is still returning, so the load waits for this before it reports the
  // journal; otherwise the install record could be recorded after the report and never written.
  // The handle is never closed: a detour thread can be waiting on it at any moment of the
  // process's life, and one event is the whole cost.
  std::atomic<HANDLE> install_done_{nullptr};
};

}  // namespace nevr_host
