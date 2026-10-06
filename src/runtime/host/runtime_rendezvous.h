#pragma once

// Host-side early PreprocessCommandLine rendezvous
// (docs/design/2026-10-05-bugsplat-nevr-split.md step 4, "Early bootstrap without
// doing the full runtime under the loader lock").
//
// The stable host's DllMain validates the game image and installs ONE MinHook
// detour on the game's PreprocessCommandLine; it loads nothing and does nothing
// else there. On the first call after attach returns, the detour loads the
// sibling nevr.dll through RuntimeLoader::Load (the step-3 load/validate state
// machine), then dispatches preprocess_before -> original (exactly once) ->
// preprocess_after, returning the original's result. The original trampoline is
// published through NevrHostContext BEFORE MH_EnableHook, so the runtime never
// installs a second hook on the rendezvous address and a racing thread can never
// read a null trampoline. A same-thread re-entry during runtime load bypasses to
// the original (InitOnce recursion on the loading thread deadlocks — measured in
// the step-2 proof gate). Any failure degrades to original-only and never falls
// back to loading the runtime from DllMain.

#include "core/runtime_abi.h"
#include "runtime/host/runtime_load.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <windows.h>

namespace NevrHost {

// echovr.exe facts for the rendezvous. The host hooks
// `game_module + kPreprocessRva` and validates the pinned prologue before writing
// the detour (never blind-write). RVA: src/abi/echovr_functions.cpp:112.
// Prologue: the first 18 bytes of echovr.exe 0x140116720 before the first
// position-dependent CALL rel32; tools/hook_identity_manifest.json pins the first
// 8 (0x405557488d6c24b1).
inline constexpr std::uintptr_t kPreprocessRva = 0x116720;
inline constexpr std::size_t kPreprocessPrologueSize = 18;
inline constexpr unsigned char kPreprocessPrologue[kPreprocessPrologueSize] = {
    0x40, 0x55, 0x57, 0x48, 0x8d, 0x6c, 0x24, 0xb1, 0x48,
    0x81, 0xec, 0xa8, 0x00, 0x00, 0x00, 0x48, 0x8b, 0xf9};

class RuntimeRendezvous {
 public:
  // Record the modules and precompute the sibling nevr.dll path (via the loader).
  // No detour, no I/O, no allocation: safe under the loader lock. diagnostics
  // must outlive this object; it is used to write the step-3 load diagnostic on
  // the first call, after attach returns.
  void Prepare(HMODULE shim_module, HMODULE game_module, BootstrapDiagnostics& diagnostics);

  // Validate the pinned prologue at `target` and install the PreprocessCommandLine
  // detour. Returns the original trampoline on success (published before
  // MH_EnableHook), or nullptr on failure — the game is left unhooked and a
  // degraded run of the original is the only consequence. Idempotent: a second
  // call after a successful install returns the same trampoline.
  NevrPreprocessCommandLineFn Install(void* target);

  // The detour body (MinHook calls it). Loads nevr.dll once under InitOnce and
  // dispatches; a same-thread re-entry during load bypasses to the original.
  static std::uint64_t PreprocessCommandLineDetour(void* game);

  // Test seam: whether Install has already published a trampoline.
  bool Installed() const { return original_ != nullptr; }

 private:
  // The InitOnce body: load/validate/initialize the runtime exactly once. A
  // failed load is final (never retried) and the failed runtime stays mapped. The
  // loading thread id is published for the duration so a same-thread re-entry from
  // the runtime's initialize() bypasses to the original instead of deadlocking.
  static BOOL CALLBACK LoadRuntimeOnce(PINIT_ONCE init_once, PVOID parameter, PVOID* context);

  HMODULE shim_module_ = nullptr;
  HMODULE game_module_ = nullptr;
  BootstrapDiagnostics* diagnostics_ = nullptr;
  RuntimeLoader loader_;
  NevrHostContext context_{};
  NevrPreprocessCommandLineFn original_ = nullptr;
  INIT_ONCE load_once_ = INIT_ONCE_STATIC_INIT;
  std::atomic<const NevrRuntimeApi*> api_{nullptr};
  std::atomic<DWORD> loading_thread_{0};
};

}  // namespace NevrHost
