// nevr-runtime.dll stand-in for the attach-time proof-gate harness. Implements the v1
// descriptor from core/runtime_abi.h and records everything it is called with.
//
// Variants are compile definitions (one DLL per fixture directory):
//   BOOTSTRAP_PROBE_BAD_VERSION     descriptor reports abi_major + 1
//   BOOTSTRAP_PROBE_INIT_FAILURE    initialize returns nonzero
//   BOOTSTRAP_PROBE_REENTER_INIT    initialize calls the hooked GameMainWrapper
//   BOOTSTRAP_PROBE_SLOW_INIT       initialize holds the host's InitOnce for 200 ms
//
// The default fixture also installs its OWN MinHook hook (its own static
// MinHook copy, separate from the host's) on a disjoint target, to exercise the
// ADR's rule that the host and the runtime use separate MinHook state with disjoint targets. It
// never touches the rendezvous address, and it checks that the host's hook record still
// matches the bytes at the wrapper after its own hook is in: the first 16 bytes read after
// initialize equal the host's post-install snapshot.

#include "runtime/tests/bootstrap_attach/abi.h"

#include "core/runtime_abi.h"

#include <MinHook.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace {

using namespace nevr_bootstrap_probe;

RecordEventFn g_record = nullptr;
NevrGameMainFn g_original = nullptr;
NevrGameMainFn g_hooked_entry = nullptr;
const NevrHostContext* g_host = nullptr;
std::atomic<LONG> g_nested_issued{0};
SecondTargetFn g_second_original = nullptr;

void Record(Event event, std::uint64_t payload = 0) {
  if (g_record != nullptr) g_record(event, payload);
}

std::uint64_t AsPayload(const void* pointer) {
  return reinterpret_cast<std::uintptr_t>(pointer);
}

// A caller of the hooked entry, recorded exactly like the executable's callers.
void CallHookedEntry(std::int64_t command_line) {
  Record(kCallIssued, static_cast<std::uint64_t>(command_line));
  if (g_hooked_entry(command_line) != ExpectedResult(command_line)) {
    Record(kBadResult, static_cast<std::uint64_t>(command_line));
  }
}

std::uint64_t SecondTargetDetour(std::uint64_t value) {
  Record(kSecondDetour, value);
  return g_second_original(value);
}

bool InstallDisjointHook() {
  HMODULE exe = GetModuleHandleW(nullptr);
  void* target = exe == nullptr ? nullptr
                                : reinterpret_cast<void*>(GetProcAddress(exe, kSecondTargetExport));
  if (target == nullptr || MH_Initialize() != MH_OK) return false;
  void* trampoline = nullptr;
  if (MH_CreateHook(target, reinterpret_cast<void*>(&SecondTargetDetour), &trampoline) != MH_OK) {
    return false;
  }
  g_second_original = reinterpret_cast<SecondTargetFn>(trampoline);
  if (MH_EnableHook(target) != MH_OK) return false;
  Record(kRuntimeSecondHookEnabled);
  return true;
}

// The host's record of its hook is the bytes at the target after the host enabled it. Nothing
// but the host may change them: the runtime registers the host's hook instead of hooking.
void CheckHostHookRecord() {
  if (std::memcmp(g_host->wrapper_hook_target, g_host->wrapper_hook_bytes,
                  nevr_runtime_abi::kHookBytesSize) != 0) {
    Record(kRuntimeHookMismatch);
  }
}

std::int32_t Initialize(const NevrHostContext* host) {
  if (nevr_runtime_abi::ValidateHostContext(host) != nevr_runtime_abi::Status::kOk) return 1;
  g_host = host;
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
#if defined(BOOTSTRAP_PROBE_SLOW_INIT)
  // Test-only race widening: concurrent first callers must be parked in the
  // host's InitOnce for this whole window, never dispatched early or twice.
  Sleep(200);
#endif
#if defined(BOOTSTRAP_PROBE_REENTER_INIT)
  CallHookedEntry(kInitReentryArgument);
#endif
  const bool hooked = InstallDisjointHook();
  CheckHostHookRecord();
#if defined(BOOTSTRAP_PROBE_INIT_FAILURE)
  // Fail AFTER patching something: the host must still run degraded, with the
  // failed runtime left mapped rather than unloaded under its own live hook.
  return hooked ? 7 : 2;
#else
  return hooked ? 0 : 2;
#endif
}

// The stand-in for GameMain: runs the original wrapper through the host's trampoline.
std::int64_t GameMain(std::int64_t command_line) {
  Record(kRuntimeGameMainEnter, static_cast<std::uint64_t>(command_line));
  // One nested call through the live detour from inside the dispatch.
  if (g_nested_issued.exchange(1, std::memory_order_acq_rel) == 0) {
    CallHookedEntry(kNestedArgument);
  }
  CheckHostHookRecord();
  const std::int64_t result = g_original(command_line);
  Record(kRuntimeGameMainExit, static_cast<std::uint64_t>(result));
  return result;
}

const NevrRuntimeApi kApi = {
    {
        sizeof(NevrRuntimeApi),
#if defined(BOOTSTRAP_PROBE_BAD_VERSION)
        nevr_runtime_abi::kMajor + 1,
#else
        nevr_runtime_abi::kMajor,
#endif
        nevr_runtime_abi::kMinor,
        0,
    },
    &Initialize,
    &GameMain,
    {},  // the export table: this probe publishes none, and the host's thunks then return their failure values
};

}  // namespace

extern "C" __declspec(dllexport) const NevrRuntimeApi* NEVR_GetRuntimeApi() {
  Record(kRuntimeGetApi);
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
