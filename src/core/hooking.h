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

// A detour may run on another thread as soon as it is enabled. Publish its
// trampoline first; the callbacks also let the ordering be tested without
// patching executable memory.
template <typename Create, typename Publish, typename Enable>
inline bool CreatePublishEnable(Create&& create, Publish&& publish, Enable&& enable) {
  PVOID trampoline = nullptr;
  if (!create(&trampoline) || trampoline == nullptr) return false;
  publish(trampoline);
  return enable();
}

#ifdef USE_MINHOOK
template <typename Create, typename Enable, typename Remove>
inline BOOL AttachMinHookWith(PVOID* ppOriginal, PVOID pDetour,
                              Create&& create, Enable&& enable, Remove&& remove) {
  LastAttachErrorRef() = "";
  const PVOID target = *ppOriginal;
  MH_STATUS createStatus = MH_OK;
  MH_STATUS enableStatus = MH_OK;
  bool created = false;
  const bool enabled = CreatePublishEnable(
      [&](PVOID* trampoline) {
        createStatus = create(target, pDetour, trampoline);
        created = createStatus == MH_OK;
        return created;
      },
      [&](PVOID trampoline) { *ppOriginal = trampoline; },
      [&] {
        enableStatus = enable(target);
        return enableStatus == MH_OK;
      });
  if (enabled) return TRUE;
  if (created) {
    remove(target);
    *ppOriginal = target;
  }
  LastAttachErrorRef() = createStatus != MH_OK ? MH_StatusToString(createStatus)
      : enableStatus != MH_OK ? MH_StatusToString(enableStatus)
      : "MH_CreateHook returned null trampoline";
  return FALSE;
}
#endif

// Attach a hook to a function
// ppOriginal: Pointer to the original function pointer (will be updated to trampoline)
// pDetour: The hook function
inline BOOL Attach(PVOID* ppOriginal, PVOID pDetour) {
  LastAttachErrorRef() = "";
#ifdef USE_MINHOOK
  return AttachMinHookWith(ppOriginal, pDetour, MH_CreateHook, MH_EnableHook, MH_RemoveHook);
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
