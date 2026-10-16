#pragma once

// Host-side early PreprocessCommandLine rendezvous
// (docs/adr/0008-bugsplat-host-and-nevr-runtime.md, "Early bootstrap").
//
// The stable host's DllMain validates the game image and installs ONE MinHook
// detour on the game's PreprocessCommandLine; it loads nothing and does nothing
// else there. On the first call after attach returns, the detour loads the
// runtime through RuntimeLoader::Load (the load/validate state machine), then
// dispatches preprocess_before -> original (exactly once) ->
// preprocess_after, returning the original's result. The original trampoline is
// published through NevrHostContext BEFORE MH_EnableHook, so the runtime never
// installs a second hook on the rendezvous address and a racing thread can never
// read a null trampoline. A same-thread re-entry during runtime load bypasses to
// the original (InitOnce recursion on the loading thread deadlocks; the attach
// proof gate in tests/bootstrap_attach measures it). Any failure degrades to
// original-only and never falls back to loading the runtime from DllMain.

#include "core/runtime_abi.h"
#include "runtime/host/preprocess_target.h"
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

  // Validate the pinned prologue at `target` and install the PreprocessCommandLine
  // detour, recording each decision in the diagnostics journal (prologue accepted
  // or mismatched with the bytes found, MinHook status). Returns the original
  // trampoline on success (published before MH_EnableHook), or nullptr on failure:
  // the game is left unhooked and a degraded run of the original is the only
  // consequence. Idempotent: a second call after a successful install returns the
  // same trampoline.
  NevrPreprocessCommandLineFn Install(void* target);

  // Read the kPreprocessPrologueSize bytes at `target` with ReadProcessMemory
  // (never a direct read, which would fault). False when they cannot be read.
  static bool ReadPrologue(const void* target, unsigned char (&out)[kPreprocessPrologueSize]);

  // True when the bytes at `target` match the pinned prologue.
  static bool PrologueMatches(const void* target);

  // The detour body (MinHook calls it). Loads nevr.dll once under InitOnce and
  // dispatches; a same-thread re-entry during load bypasses to the original.
  static std::uint64_t PreprocessCommandLineDetour(void* game);

  // The runtime module once it has loaded, else null.
  HMODULE RuntimeModule() const { return runtime_module_.load(std::memory_order_acquire); }

  // Test seam: whether Install has already published a trampoline.
  bool Installed() const { return original_ != nullptr; }

 private:
  // The InitOnce body: load/validate/initialize the runtime exactly once. A
  // failed load is final (never retried) and the failed runtime stays mapped. The
  // loading thread id is published for the duration so a same-thread re-entry from
  // the runtime's initialize() bypasses to the original instead of deadlocking.
  static BOOL CALLBACK LoadRuntimeOnce(PINIT_ONCE init_once, PVOID parameter, PVOID* context);

  void Journal(AttachStep step, AttachOutcome outcome, std::uint32_t detail = 0,
               const unsigned char* actual = nullptr);

  HMODULE host_module_ = nullptr;
  HMODULE game_module_ = nullptr;
  BootstrapDiagnostics* diagnostics_ = nullptr;
  RuntimeLoader loader_;
  NevrHostContext context_{};
  NevrPreprocessCommandLineFn original_ = nullptr;
  INIT_ONCE load_once_ = INIT_ONCE_STATIC_INIT;
  std::atomic<const NevrRuntimeApi*> api_{nullptr};
  std::atomic<HMODULE> runtime_module_{nullptr};
  std::atomic<DWORD> loading_thread_{0};
};

}  // namespace nevr_host
