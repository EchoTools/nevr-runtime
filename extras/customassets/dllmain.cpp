// dllmain.cpp : Defines the entry point for the DLL application.
#include <windows.h>

#include "custom_assets.h"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
  switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
      CustomAssets::Initialize();
      break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
      break;
    case DLL_PROCESS_DETACH:
      CustomAssets::Shutdown();
      break;
  }
  return TRUE;
}

// Export with ordinal 1 for Detours compatibility
extern "C" __declspec(dllexport) void DetoursExportPlaceholder() {
  // Empty placeholder export
}
