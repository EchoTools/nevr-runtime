#include "abi.h"

#include <atomic>

namespace {

ProbeTargetFunction g_target = nullptr;
std::atomic<LONG> g_reentered{0};
thread_local bool g_inside_reentry = false;
using RecordEventFunction = void(WINAPI*)(LONG);

void Record(ProbeEvent event) {
  HMODULE exe = GetModuleHandleW(nullptr);
  if (exe == nullptr) {
    return;
  }
  const auto record = reinterpret_cast<RecordEventFunction>(
      GetProcAddress(exe, "BootstrapProbeRecordEvent"));
  if (record != nullptr) {
    record(static_cast<LONG>(event));
  }
}

BOOL WINAPI Initialize() {
  Record(kRuntimeInitialized);
  wchar_t fail[2] = {};
  if (GetEnvironmentVariableW(L"NEVR_PROBE_FAIL_INIT", fail, 2) != 0 &&
      fail[0] == L'1') {
    return FALSE;
  }
  HMODULE exe = GetModuleHandleW(nullptr);
  g_target = exe == nullptr
                 ? nullptr
                 : reinterpret_cast<ProbeTargetFunction>(
                       GetProcAddress(exe, "BootstrapProbeTarget"));
  return g_target != nullptr;
}

void WINAPI PreprocessBefore() {
  Record(kRuntimePreprocessBefore);
  if (!g_inside_reentry && g_target != nullptr &&
      g_reentered.exchange(1, std::memory_order_acq_rel) == 0) {
    g_inside_reentry = true;
    static_cast<void>(g_target(3, 7));
    g_inside_reentry = false;
  }
}

void WINAPI PreprocessAfter(std::uintptr_t) {
  Record(kRuntimePreprocessAfter);
}

const ProbeRuntimeApi kApi = {
    sizeof(ProbeRuntimeApi),
#ifdef BOOTSTRAP_PROBE_BAD_VERSION
    kProbeAbiMajor + 1,
#else
    kProbeAbiMajor,
#endif
    0,
    &Initialize,
    &PreprocessBefore,
    &PreprocessAfter,
};

}  // namespace

extern "C" __declspec(dllexport) const ProbeRuntimeApi* WINAPI
NEVR_GetRuntimeApi() {
  return &kApi;
}
