// nevr-runtime.dll stand-in for the rendezvous harness. Implements the v1 descriptor from
// core/runtime_abi.h and records everything it is called with
// (docs/adr/0008-bugsplat-host-and-nevr-runtime.md).
//
//   - initialize validates the host context, records the trampoline and the host's
//     record of its wrapper hook (target and the first bytes found after the host
//     enabled it), and checks that record against the live bytes at the target;
//   - game_main records its argument, runs the original wrapper through the host's
//     trampoline (the stand-in for GameMain, which the real runtime calls directly) and
//     records the result it returns.
//
// Compile-definition variants (one DLL per fixture directory):
//   RENDEZVOUS_PROBE_REENTER_INIT  initialize() re-enters the live detour on the
//                                  same thread, exercising the host's bypass.

#include "runtime/tests/rendezvous/abi.h"

#include "core/runtime_abi.h"

#include <cstdint>
#include <cstring>
#include <windows.h>

namespace {

using namespace nevr_rendezvous_probe;

RecordEventFn g_record = nullptr;
NevrGameMainFn g_original = nullptr;
NevrGameMainFn g_hooked_entry = nullptr;

void Record(Event event, std::uint64_t payload = 0) {
  if (g_record != nullptr) g_record(event, payload);
}

std::uint64_t AsPayload(const void* pointer) {
  return reinterpret_cast<std::uintptr_t>(pointer);
}

std::int32_t Initialize(const NevrHostContext* host) {
  if (nevr_runtime_abi::ValidateHostContext(host) != nevr_runtime_abi::Status::kOk) return 1;
  g_original = host->original_game_main_wrapper;
  // An unread or malformed hook record is not one the runtime may register: counted as a mismatch.
  if (!nevr_runtime_abi::HookRecordUsable(host)) Record(kRuntimeHookMismatch);
  g_hooked_entry = reinterpret_cast<NevrGameMainFn>(
      reinterpret_cast<std::uintptr_t>(host->game_module) + nevr_host::kGameMainWrapperRva);
  Record(kRuntimeInitialized, AsPayload(reinterpret_cast<const void*>(host->original_game_main_wrapper)));
  Record(kRuntimeHookTarget, AsPayload(host->wrapper_hook_target));
  std::uint64_t first_bytes = 0;
  std::memcpy(&first_bytes, host->wrapper_hook_bytes, sizeof(first_bytes));
  Record(kRuntimeHookBytes, first_bytes);
  // The record is the bytes found at the target after the host enabled its hook. Nothing has
  // touched the target since, so the live bytes are the record.
  if (std::memcmp(host->wrapper_hook_target, host->wrapper_hook_bytes,
                  nevr_runtime_abi::kHookBytesSize) != 0) {
    Record(kRuntimeHookMismatch);
  }
#if defined(RENDEZVOUS_PROBE_REENTER_INIT)
  // Re-enter the live detour on the loading thread, the way a runtime that calls
  // back into the game from its own initialize would. The host must bypass to the
  // original rather than deadlock on its InitOnce.
  Record(kCallIssued, static_cast<std::uint64_t>(kReentryArgument));
  const std::int64_t nested = g_hooked_entry(kReentryArgument);
  if (nested != ExpectedResult(kReentryArgument)) {
    Record(kBadResult, static_cast<std::uint64_t>(kReentryArgument));
  }
#endif
  return 0;
}

std::int64_t GameMain(std::int64_t command_line) {
  Record(kRuntimeGameMainEnter, static_cast<std::uint64_t>(command_line));
  const std::int64_t result = g_original(command_line);
  Record(kRuntimeGameMainExit, static_cast<std::uint64_t>(result));
  return result;
}

// The identity literal a real runtime carries: NUL-bounded "NEVR-BUILD <version> <commit40>" (src/core and
// host_identity.cpp). The host reads the version out of the file for its `versions` record
// (test_host_runtime_verify), and this stand-in has to say one.
extern "C" __attribute__((used)) const char kNevrBuildLiteral[] =
    "\0NEVR-BUILD 7.7.7 0123456789abcdef0123456789abcdef01234567";

const NevrRuntimeApi kApi = {
    {
        sizeof(NevrRuntimeApi),
        nevr_runtime_abi::kMajor,
        nevr_runtime_abi::kMinor,
        0,
    },
    &Initialize,
    &GameMain,
    {},  // no export table: the host's thunks return their failure values
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
