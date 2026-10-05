// nevr.dll stand-in for the attach-time proof-gate harness. Implements the v1
// descriptor from core/runtime_abi.h and records everything it is called with.
//
// Variants are compile definitions (one DLL per fixture directory):
//   BOOTSTRAP_PROBE_BAD_VERSION   descriptor reports abi_major + 1
//   BOOTSTRAP_PROBE_INIT_FAILURE  initialize returns nonzero
//   BOOTSTRAP_PROBE_REENTER_INIT  initialize calls the hooked PreprocessCommandLine
//   BOOTSTRAP_PROBE_SLOW_INIT     initialize holds the host's InitOnce for 200 ms
//
// The default fixture also installs its OWN MinHook hook (its own static
// MinHook copy, separate from the host's) on a disjoint target, to exercise the
// design's "separate MinHook state/instances ... with disjoint targets" rule. It
// never touches the rendezvous address.

#include "runtime/tests/bootstrap_attach/abi.h"

#include "core/runtime_abi.h"

#include <MinHook.h>

#include <atomic>
#include <cstdint>

namespace {

using namespace BootstrapProbe;

RecordEventFn g_record = nullptr;
NevrPreprocessCommandLineFn g_hooked_entry = nullptr;
std::atomic<LONG> g_nested_issued{0};
SecondTargetFn g_second_original = nullptr;

void Record(Event event, std::uint64_t payload = 0) {
  if (g_record != nullptr) g_record(event, payload);
}

std::uint64_t AsPayload(const void* pointer) {
  return reinterpret_cast<std::uintptr_t>(pointer);
}

// A caller of the hooked entry, recorded exactly like the executable's callers.
void CallHookedEntry(std::uintptr_t raw_game) {
  void* game = reinterpret_cast<void*>(raw_game);
  Record(kCallIssued, raw_game);
  if (g_hooked_entry(game) != ExpectedResult(game)) Record(kBadResult, raw_game);
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

std::int32_t Initialize(const NevrHostContext* host) {
  if (NevrAbi::ValidateHostContext(host) != NevrAbi::Status::kOk) return 1;
  g_hooked_entry = reinterpret_cast<NevrPreprocessCommandLineFn>(
      reinterpret_cast<std::uintptr_t>(host->game_module) + kPreprocessRva);
  Record(kRuntimeInitialized, AsPayload(reinterpret_cast<const void*>(host->original_preprocess)));
#if defined(BOOTSTRAP_PROBE_SLOW_INIT)
  // Test-only race widening: concurrent first callers must be parked in the
  // host's InitOnce for this whole window, never dispatched early or twice.
  Sleep(200);
#endif
#if defined(BOOTSTRAP_PROBE_REENTER_INIT)
  CallHookedEntry(kInitReentryGame);
#endif
  const bool hooked = InstallDisjointHook();
#if defined(BOOTSTRAP_PROBE_INIT_FAILURE)
  // Fail AFTER patching something: the host must still run degraded, with the
  // failed runtime left mapped rather than unloaded under its own live hook.
  return hooked ? 7 : 2;
#else
  return hooked ? 0 : 2;
#endif
}

void PreprocessBefore(void* game) {
  Record(kRuntimeBefore, AsPayload(game));
  // One nested call through the live detour from inside the dispatch.
  if (g_nested_issued.exchange(1, std::memory_order_acq_rel) == 0) {
    CallHookedEntry(kNestedGame);
  }
}

void PreprocessAfter(void* game, std::uint64_t result) {
  Record(kRuntimeAfter, result);
  if (result != ExpectedResult(game)) Record(kBadResult, AsPayload(game));
}

const NevrRuntimeApi kApi = {
    {
        sizeof(NevrRuntimeApi),
#if defined(BOOTSTRAP_PROBE_BAD_VERSION)
        NevrAbi::kMajor + 1,
#else
        NevrAbi::kMajor,
#endif
        NevrAbi::kMinor,
        0,
    },
    &Initialize,
    &PreprocessBefore,
    &PreprocessAfter,
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
