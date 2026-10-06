// runtime_entry.cpp : nevr.dll's versioned C entry point and no-op DllMain
// (docs/design/2026-10-05-bugsplat-nevr-split.md step 5).
//
// nevr.dll is the changing feature runtime. The stable host (BugSplat64.dll)
// loads it lazily on the first PreprocessCommandLine rendezvous, resolves
// NEVR_GetRuntimeApi, validates the descriptor, and then calls:
//   initialize        -> validate the host context, set the game base, resolve
//                        function pointers, install the runtime's hooks (its own
//                        MinHook copy, disjoint targets — never the rendezvous
//                        address, which the host owns);
//   preprocess_before -> set the game instance and run PreflightRuntimeBootstrap
//                        (-server etc. parsed BEFORE the original call);
//   preprocess_after  -> run the idempotent RunDeferredRuntimeBootstrap only
//                        when -server (clients skip this server-only path).
//
// The host supplies the original PreprocessCommandLine trampoline through
// NevrHostContext and calls it itself exactly once between before and after, so
// the runtime never installs a second hook on that address.

#include "core/runtime_abi.h"
#include "core/globals.h"
#include "runtime/lifecycle/boot.h"
#include "runtime/lifecycle/cli.h"
#include "runtime/lifecycle/config.h"
#include "runtime/lifecycle/initialize.h"

namespace {

std::int32_t Initialize(const NevrHostContext* host) {
  if (NevrAbi::ValidateHostContext(host) != NevrAbi::Status::kOk) return 1;
  // The host has already validated the game image; here we record the game base
  // and run the full runtime initialization (function pointers + hooks).
  InitializeValidatedRuntime(host->game_module);
  return 0;
}

void PreprocessBefore(void* game) {
  g_pGame = game;
  PreflightRuntimeBootstrap();
}

void PreprocessAfter(void* game, std::uint64_t result) {
  (void)result;
  if (g_isServer) {
    RunDeferredRuntimeBootstrap(game, "Preprocess first-call server bootstrap");
  }
}

const NevrRuntimeApi kApi = {
    {
        sizeof(NevrRuntimeApi),
        NevrAbi::kMajor,
        NevrAbi::kMinor,
        0,
    },
    &Initialize,
    &PreprocessBefore,
    &PreprocessAfter,
};

}  // namespace

extern "C" __declspec(dllexport) const NevrRuntimeApi* NEVR_GetRuntimeApi() {
  return &kApi;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) {
  // No-op DllMain (design step 5): the runtime is loaded and initialized through
  // NEVR_GetRuntimeApi on the first PreprocessCommandLine call, not from attach.
  // The module is kept loaded for process lifetime; no FreeLibrary or
  // thread-joining shutdown runs here.
  return TRUE;
}
