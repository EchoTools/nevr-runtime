#include <windows.h>

#include "runtime/lifecycle/stable_string_pool.h"

namespace {
bool* g_detachObserved = nullptr;
}

extern "C" __declspec(dllexport) void StableStringPoolFixtureObserveDetach(bool* observed) {
  g_detachObserved = observed;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_DETACH && g_detachObserved != nullptr) *g_detachObserved = true;
  return TRUE;
}

extern "C" __declspec(dllexport) const char* StableStringPoolFixtureIntern(const char* value) {
  if (value == nullptr) return nullptr;
  const auto result = nevr_runtime::lifecycle::InternStableCStr(value);
  return result.status == nevr_runtime::lifecycle::InternStatus::kSuccess ? result.pointer : nullptr;
}
