#pragma once
// Hooking abstraction layer - supports MinHook or Detours
// Define USE_MINHOOK to use MinHook, otherwise uses Detours

#ifdef USE_MINHOOK
#include <MinHook.h>
#else
#include <detours/detours.h>
#endif

#include <windows.h>

#include <mutex>
#include <unordered_map>

#include "core/hook_lifecycle.h"

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

// The create/publish/enable ordering lives in core/hook_lifecycle.h so the Quest
// GOT backend runs the same contract. Re-exported here for existing callers.
using nevr::hook::CreatePublishEnable;

#ifdef USE_MINHOOK
// MinHook identifies a hook by its target address, which Attach() overwrites in
// *ppOriginal with the trampoline. Detach() is handed only the trampoline, so each
// successful Attach records trampoline -> target here. The trampoline is unique per
// MinHook hook, which a detour pointer is not (one detour may serve several targets).
struct MinHookTargets {
  std::mutex mutex;
  std::unordered_map<PVOID, PVOID> targetByTrampoline;
};
inline MinHookTargets& MinHookTargetsRef() {
  static MinHookTargets s;
  return s;
}

template <typename Create, typename Enable, typename Remove>
inline BOOL AttachMinHookWith(PVOID* ppOriginal, PVOID pDetour,
                              Create&& create, Enable&& enable, Remove&& remove) {
  LastAttachErrorRef() = "";
  const PVOID target = *ppOriginal;
  MH_STATUS createStatus = MH_OK;
  MH_STATUS enableStatus = MH_OK;
  const nevr::hook::AttachStage stage = nevr::hook::AttachPublished(
      ppOriginal,
      [&](PVOID* trampoline) {
        createStatus = create(target, pDetour, trampoline);
        return createStatus == MH_OK;
      },
      [&] {
        enableStatus = enable(target);
        return enableStatus == MH_OK;
      },
      [&] { remove(target); });
  switch (stage) {
    case nevr::hook::AttachStage::kAttached: {
      MinHookTargets& targets = MinHookTargetsRef();
      std::lock_guard<std::mutex> lock(targets.mutex);
      targets.targetByTrampoline[*ppOriginal] = target;
      return TRUE;
    }
    case nevr::hook::AttachStage::kCreateFailed:
      LastAttachErrorRef() = MH_StatusToString(createStatus);
      break;
    case nevr::hook::AttachStage::kNullTrampoline:
      LastAttachErrorRef() = "MH_CreateHook returned null trampoline";
      break;
    case nevr::hook::AttachStage::kEnableFailed:
      LastAttachErrorRef() = MH_StatusToString(enableStatus);
      break;
  }
  return FALSE;
}

// Disable only the hook whose trampoline *ppOriginal holds. A trampoline this layer did
// not attach is refused: never fall back to MH_ALL_HOOKS, which would switch off every
// other runtime patch in the process.
template <typename Disable>
inline BOOL DetachMinHookWith(PVOID* ppOriginal, Disable&& disable) {
  MinHookTargets& targets = MinHookTargetsRef();
  PVOID target = nullptr;
  {
    std::lock_guard<std::mutex> lock(targets.mutex);
    const auto it = targets.targetByTrampoline.find(*ppOriginal);
    if (it == targets.targetByTrampoline.end()) return FALSE;
    target = it->second;
  }
  if (disable(target) != MH_OK) return FALSE;
  std::lock_guard<std::mutex> lock(targets.mutex);
  targets.targetByTrampoline.erase(*ppOriginal);
  return TRUE;
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
// pDetour: The hook function (Detours only; MinHook finds the hook by its trampoline)
inline BOOL Detach(PVOID* ppOriginal, PVOID pDetour) {
#ifdef USE_MINHOOK
  static_cast<void>(pDetour);
  return DetachMinHookWith(ppOriginal, MH_DisableHook);
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
