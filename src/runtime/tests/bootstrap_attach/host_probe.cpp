#include "abi.h"

#include <MinHook.h>

#include <cwchar>

namespace {

HMODULE g_host_module = nullptr;
ProbeTargetFunction g_original_target = nullptr;
const ProbeRuntimeApi* g_runtime_api = nullptr;
HMODULE g_runtime_module = nullptr;
INIT_ONCE g_runtime_once = INIT_ONCE_STATIC_INIT;
using RecordEventFunction = void(WINAPI*)(LONG);
RecordEventFunction g_record_event = nullptr;

void Record(ProbeEvent event) {
  if (g_record_event != nullptr) {
    g_record_event(static_cast<LONG>(event));
  }
}

bool RuntimeApiPath(wchar_t* path, std::size_t capacity) {
  DWORD length = GetModuleFileNameW(g_host_module, path, static_cast<DWORD>(capacity));
  if (length == 0 || length >= capacity) {
    return false;
  }
  wchar_t* separator = wcsrchr(path, L'\\');
  if (separator == nullptr) {
    return false;
  }
  constexpr wchar_t runtime_name[] = L"nevr.dll";
  const std::size_t directory_length = static_cast<std::size_t>(separator - path + 1);
  if (directory_length + (sizeof(runtime_name) / sizeof(runtime_name[0])) > capacity) {
    return false;
  }
  wcscpy(path + directory_length, runtime_name);
  return true;
}

BOOL CALLBACK InitializeRuntime(PINIT_ONCE, PVOID, PVOID*) {
  wchar_t runtime_path[MAX_PATH] = {};
  if (!RuntimeApiPath(runtime_path, MAX_PATH)) {
    Record(kRuntimeFailure);
    return TRUE;
  }

  g_runtime_module = LoadLibraryExW(runtime_path, nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                        LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (g_runtime_module == nullptr) {
    Record(kRuntimeFailure);
    return TRUE;
  }

  const auto get_api = reinterpret_cast<ProbeGetRuntimeApi>(
      GetProcAddress(g_runtime_module, "NEVR_GetRuntimeApi"));
  if (get_api == nullptr) {
    Record(kRuntimeFailure);
    return TRUE;
  }

  const ProbeRuntimeApi* api = get_api();
  if (api == nullptr || api->abi_major != kProbeAbiMajor ||
      api->struct_size < sizeof(ProbeRuntimeApi) || api->initialize == nullptr ||
      api->preprocess_before == nullptr || api->preprocess_after == nullptr) {
    Record(kRuntimeFailure);
    return TRUE;
  }
  if (!api->initialize()) {
    Record(kRuntimeFailure);
    return TRUE;
  }

  g_runtime_api = api;
  return TRUE;
}

std::uintptr_t WINAPI TargetDetour(std::uintptr_t first, std::uintptr_t second) {
  InitOnceExecuteOnce(&g_runtime_once, InitializeRuntime, nullptr, nullptr);
  const ProbeRuntimeApi* api = g_runtime_api;
  if (api != nullptr) {
    api->preprocess_before();
  }
  const std::uintptr_t result = g_original_target(first, second);
  if (api != nullptr) {
    api->preprocess_after(result);
  }
  return result;
}

bool InstallProbeHook() {
  HMODULE exe = GetModuleHandleW(nullptr);
  if (exe == nullptr) {
    return false;
  }
  const auto target = reinterpret_cast<ProbeTargetFunction>(
      GetProcAddress(exe, "BootstrapProbeTarget"));
  if (target == nullptr || MH_Initialize() != MH_OK) {
    return false;
  }

  if (MH_CreateHook(reinterpret_cast<LPVOID>(target),
                    reinterpret_cast<LPVOID>(&TargetDetour),
                    reinterpret_cast<LPVOID*>(&g_original_target)) != MH_OK) {
    return false;
  }
  if (MH_EnableHook(reinterpret_cast<LPVOID>(target)) != MH_OK) {
    MH_RemoveHook(reinterpret_cast<LPVOID>(target));
    g_original_target = nullptr;
    return false;
  }
  Record(kHostHookEnabled);
  return true;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_host_module = module;
    HMODULE exe = GetModuleHandleW(nullptr);
    g_record_event = exe == nullptr
                         ? nullptr
                         : reinterpret_cast<RecordEventFunction>(
                               GetProcAddress(exe, "BootstrapProbeRecordEvent"));
    Record(kHostAttachEnter);
    if (!InstallProbeHook()) {
      return FALSE;
    }
    Record(kHostAttachExit);
  }
  return TRUE;
}
