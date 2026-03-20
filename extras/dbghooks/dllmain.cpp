// dllmain.cpp : Defines the entry point for the DbgHooks DLL
#include "gun2cr_hook.h"
#include "hash_hooks.h"
#include "pch.h"
#include "static_entry.h"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
  switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
      dbghooks::InitializeStatic(hModule);
      break;

    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
      break;

    case DLL_PROCESS_DETACH:
      dbghooks::ShutdownStatic();
      break;
  }
  return TRUE;
}

extern "C" __declspec(dllexport) void DbgHooksExportPlaceholder() {
  // Export placeholder for DLL loading
}

// ============================================================================
// LAZY INITIALIZATION EXPORTS
// ============================================================================
// Call these from game initialization code when g_GameBaseAddress is available

extern "C" __declspec(dllexport) VOID InitializeGun2CRHookDeferred() {
  try {
    InitializeGun2CRHook();
    printf("[DbgHooks] Gun2CR hook initialized successfully via deferred init\n");
  } catch (...) {
    printf("[DbgHooks] Exception during Gun2CR deferred initialization\n");
  }
}

extern "C" __declspec(dllexport) VOID InitializeHashHooksDeferred() {
  try {
    InitializeHashHooks();
    printf("[DbgHooks] Hash hooks initialized successfully via deferred init\n");
  } catch (...) {
    printf("[DbgHooks] Exception during hash hooks deferred initialization\n");
  }
}

extern "C" __declspec(dllexport) VOID InitializeAllHooks() {
  InitializeGun2CRHookDeferred();
  InitializeHashHooksDeferred();
}
