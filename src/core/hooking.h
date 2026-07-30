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

// N127: the reason the most recent Attach() failed — which MinHook stage and its
// MH_STATUS — so the caller (PatchDetour) can name it in one Warning line instead
// of leaving "undetermined". MH_StatusToString returns a static string literal, so
// storing the pointer is safe. Written only on the init thread, where hooks
// install one at a time and PatchDetour reads this immediately after each; not for
// cross-thread use.
inline const char*& LastAttachErrorRef() {
  static const char* s = "";
  return s;
}
inline const char* LastAttachError() { return LastAttachErrorRef(); }

// Attach a hook to a function
// ppOriginal: Pointer to the original function pointer (will be updated to trampoline)
// pDetour: The hook function
inline BOOL Attach(PVOID* ppOriginal, PVOID pDetour) {
  LastAttachErrorRef() = "";
#ifdef USE_MINHOOK
  // MinHook needs the target address, then gives us the trampoline
  PVOID pTarget = *ppOriginal;
  PVOID pTrampoline = nullptr;

  MH_STATUS st = MH_CreateHook(pTarget, pDetour, &pTrampoline);
  if (st != MH_OK) {
    LastAttachErrorRef() = MH_StatusToString(st);  // e.g. MH_ERROR_UNSUPPORTED_FUNCTION
    return FALSE;
  }

  st = MH_EnableHook(pTarget);
  if (st != MH_OK) {
    LastAttachErrorRef() = MH_StatusToString(st);  // e.g. MH_ERROR_NOT_EXECUTABLE
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
  if (result != NO_ERROR) LastAttachErrorRef() = "DetourAttach failed";
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
