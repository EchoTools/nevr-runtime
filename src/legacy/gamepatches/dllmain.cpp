// DLL entry point
#include "common/pch.h"
#include "patches.h"
#include "plugin_loader.h"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
  switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
      Initialize();
      break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
      break;
    case DLL_PROCESS_DETACH:
      UnloadPlugins();
      break;
  }
  return TRUE;
}

extern "C" __declspec(dllexport) void DetoursExportPlaceholder() {
  // An export with ordinal number 1 is needed for this library to be loaded as a suspended process.
}