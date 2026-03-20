// GameServer DLL entry point
#include "echovr.h"
#include "gameserver.h"
#include "pch.h"

// Global server library instance (game expects this as singleton)
EchoVR::IServerLib* g_ServerLib;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
  switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
      break;
  }
  return TRUE;
}

// RadEngine plugin hooks (required exports, mostly no-op)
extern "C" {
HRESULT RadPluginInit() { return ERROR_SUCCESS; }
HRESULT RadPluginInitMemoryStatics(HMODULE) { return ERROR_SUCCESS; }
HRESULT RadPluginInitNonMemoryStatics(HMODULE) { return ERROR_SUCCESS; }
HRESULT RadPluginMain(CHAR*) { return ERROR_SUCCESS; }
HRESULT RadPluginSetAllocator(VOID*) { return ERROR_SUCCESS; }
HRESULT RadPluginSetEnvironment(VOID*) { return ERROR_SUCCESS; }
HRESULT RadPluginSetEnvironmentMethods(VOID*, VOID*) { return ERROR_SUCCESS; }
HRESULT RadPluginSetFileTypes(VOID*) { return ERROR_SUCCESS; }
HRESULT RadPluginSetPresenceFactory(VOID*) { return ERROR_SUCCESS; }
HRESULT RadPluginSetSymbolDebugMethodsMethod(VOID*, VOID*, VOID*, VOID*) { return ERROR_SUCCESS; }

HRESULT RadPluginShutdown() {
  delete g_ServerLib;
  g_ServerLib = nullptr;
  return ERROR_SUCCESS;
}

// Main entry point - game calls this to get IServerLib instance
EchoVR::IServerLib* ServerLib() {
  if (!g_ServerLib) {
    g_ServerLib = new GameServerLib();
  }
  return g_ServerLib;
}
}  // extern "C"
