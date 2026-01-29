#pragma once
// Hooking abstraction layer - supports MinHook or Detours
// Define USE_MINHOOK to use MinHook, otherwise uses Detours

#ifdef USE_MINHOOK
#include <MinHook.h>
#else
#include <detours/detours.h>
#endif

#include <windows.h>

namespace Hooking {

// Initialize the hooking library (call once at startup)
inline BOOL Initialize() {
#ifdef USE_MINHOOK
  return MH_Initialize() == MH_OK;
#else
  return TRUE;  // Detours doesn't need global initialization
#endif
}

// Shutdown the hooking library (call once at cleanup)
inline VOID Shutdown() {
#ifdef USE_MINHOOK
  MH_Uninitialize();
#endif
}

// Attach a hook to a function
// ppOriginal: Pointer to the original function pointer (will be updated to trampoline)
// pDetour: The hook function
inline BOOL Attach(PVOID* ppOriginal, PVOID pDetour) {
#ifdef USE_MINHOOK
  // MinHook needs the target address, then gives us the trampoline
  PVOID pTarget = *ppOriginal;
  PVOID pTrampoline = nullptr;

  if (MH_CreateHook(pTarget, pDetour, &pTrampoline) != MH_OK) {
    return FALSE;
  }

  if (MH_EnableHook(pTarget) != MH_OK) {
    return FALSE;
  }

  // Update the original pointer to point to the trampoline
  *ppOriginal = pTrampoline;
  return TRUE;
#else
  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());
  LONG result = DetourAttach(ppOriginal, pDetour);
  DetourTransactionCommit();
  return result == NO_ERROR;
#endif
}

// Detach a hook from a function
// ppOriginal: Pointer to the trampoline (will be restored to original)
// pDetour: The hook function
inline BOOL Detach(PVOID* ppOriginal, PVOID pDetour) {
#ifdef USE_MINHOOK
  // MinHook uses the original target to identify the hook
  // We need to disable the hook - but we don't have the original target anymore
  // This is a limitation - we'd need to track the mapping
  // For now, just disable all hooks (not ideal but works for cleanup)
  return MH_DisableHook(MH_ALL_HOOKS) == MH_OK;
#else
  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());
  LONG result = DetourDetach(ppOriginal, pDetour);
  DetourTransactionCommit();
  return result == NO_ERROR;
#endif
}

// Helper macro for the common pattern of hooking a function
#define HOOK_FUNCTION(original, hook) Hooking::Attach(&(PVOID&)(original), (PVOID)(hook))

#define UNHOOK_FUNCTION(original, hook) Hooking::Detach(&(PVOID&)(original), (PVOID)(hook))

}  // namespace Hooking
