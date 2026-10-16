// nevr.dll stand-in for the rendezvous harness. Implements the v1
// descriptor from core/runtime_abi.h and records everything it is called with.
//
// It emulates the runtime-side semantics the ADR pins for preprocess_before /
// preprocess_after (docs/adr/0008-bugsplat-host-and-nevr-runtime.md):
//   - preprocess_before runs the preflight once, parsing -server and recording
//     whether it was detected BEFORE the original runs (PreflightRuntimeBootstrap
//     sets the NEVR server globals before the original call);
//   - preprocess_after rechecks -server and runs the deferred server bootstrap
//     exactly once (RunDeferredRuntimeBootstrap is idempotent and server-only;
//     clients skip it). The recheck re-parses the command line rather than
//     trusting the preflight flag.
//
// Compile-definition variants (one DLL per fixture directory):
//   RENDEZVOUS_PROBE_REENTER_INIT  initialize() re-enters the live detour on the
//                                  same thread, exercising the host's bypass.

#include "runtime/tests/rendezvous/abi.h"

#include "core/runtime_abi.h"

#include <atomic>
#include <cstdint>
#include <windows.h>

namespace {

using namespace nevr_rendezvous_probe;

RecordEventFn g_record = nullptr;
NevrPreprocessCommandLineFn g_hooked_entry = nullptr;

void Record(Event event, std::uint64_t payload = 0) {
  if (g_record != nullptr) g_record(event, payload);
}

std::uint64_t AsPayload(const void* pointer) {
  return reinterpret_cast<std::uintptr_t>(pointer);
}

bool CommandLineHasServer() {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv == nullptr) return false;
  bool server = false;
  for (int i = 0; i < argc; ++i) {
    if (lstrcmpW(argv[i], L"-server") == 0) {
      server = true;
      break;
    }
  }
  LocalFree(argv);
  return server;
}

std::int32_t Initialize(const NevrHostContext* host) {
  if (nevr_runtime_abi::ValidateHostContext(host) != nevr_runtime_abi::Status::kOk) return 1;
  g_hooked_entry = reinterpret_cast<NevrPreprocessCommandLineFn>(
      reinterpret_cast<std::uintptr_t>(host->game_module) + nevr_host::kPreprocessRva);
  Record(kRuntimeInitialized, AsPayload(reinterpret_cast<const void*>(host->original_preprocess)));
#if defined(RENDEZVOUS_PROBE_REENTER_INIT)
  // Re-enter the live detour on the loading thread, the way a runtime that calls
  // back into the game from its own initialize would. The host must bypass to the
  // original rather than deadlock on its InitOnce.
  Record(kCallIssued, kReentryGame);
  const std::uint64_t nested = g_hooked_entry(reinterpret_cast<void*>(kReentryGame));
  if (nested != ExpectedResult(reinterpret_cast<const void*>(kReentryGame))) {
    Record(kBadResult, kReentryGame);
  }
#endif
  return 0;
}

void PreprocessBefore(void* game) {
  Record(kRuntimeBefore, AsPayload(game));
  static bool s_preflighted = false;
  if (!s_preflighted) {
    s_preflighted = true;
    const bool server = CommandLineHasServer();
    Record(server ? kRuntimeServerDetected : kRuntimeClientDetected, server ? 1 : 0);
  }
}

void PreprocessAfter(void* game, std::uint64_t result) {
  Record(kRuntimeAfter, result);
  if (result != ExpectedResult(game)) Record(kBadResult, AsPayload(game));
  static bool s_deferred = false;
  if (!s_deferred && CommandLineHasServer()) {  // the post-phase -server recheck
    s_deferred = true;
    Record(kRuntimeDeferred, 0);
  }
}

const NevrRuntimeApi kApi = {
    {
        sizeof(NevrRuntimeApi),
        nevr_runtime_abi::kMajor,
        nevr_runtime_abi::kMinor,
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

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    HMODULE exe = GetModuleHandleW(nullptr);
    g_record = exe == nullptr ? nullptr
                             : reinterpret_cast<RecordEventFn>(
                                   GetProcAddress(exe, kRecordEventExport));
    Record(kRuntimeDllAttach);
  }
  return TRUE;
}
