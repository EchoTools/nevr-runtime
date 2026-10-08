#pragma once
// Hook install/remove ordering shared by every hook backend.
//
// Two backends use it today: the Windows runtime (MinHook / Detours, via
// core/hooking.h) and the Quest GOT backend (quest/sentinel/got_hook.cpp). The
// header names no platform: no <windows.h>, no MinHook, no <link.h>. A backend
// supplies three callables and keeps its own status codes; this file owns only
// the ordering contract and the rollback:
//
//   create   build the hook object and produce the original-call pointer
//   publish  make that pointer visible to the detour (*ppOriginal = ...)
//   enable   start redirecting callers
//   remove   undo create (only called if create succeeded)
//
// A detour can run on another thread the moment enable returns, and may run
// earlier on a backend whose enable is a single store. The original-call pointer
// therefore has to be published BEFORE enable. If enable fails, the hook is
// removed and the caller's pointer is put back to its prior value, so a failed
// attach leaves no partial state.

#include <atomic>
#include <cstdint>

namespace nevr::hook {

enum class AttachStage : std::uint8_t {
  kAttached,        // create, publish and enable all succeeded
  kCreateFailed,    // create returned false; nothing was published or removed
  kNullTrampoline,  // create returned true but produced no pointer; removed
  kEnableFailed,    // enable returned false; removed and the pointer restored
};

// Stable tokens for structured log lines.
constexpr const char* AttachStageName(AttachStage stage) {
  switch (stage) {
    case AttachStage::kAttached:       return "attached";
    case AttachStage::kCreateFailed:   return "create_failed";
    case AttachStage::kNullTrampoline: return "null_trampoline";
    case AttachStage::kEnableFailed:   return "enable_failed";
  }
  return "unknown";
}

// Stores the original-call pointer with release semantics. A detour on another
// thread reads it with an acquire load; the release store means everything the
// backend did before publishing is visible to a thread that sees the pointer.
inline void PublishPointer(void** where, void* value) {
#if defined(__GNUC__)
  __atomic_store_n(where, value, __ATOMIC_RELEASE);
#else
  std::atomic_thread_fence(std::memory_order_release);
  *static_cast<void* volatile*>(where) = value;
#endif
}

// Create, publish, enable. `create(void** original)` returns whether the hook
// object now exists; `publish(void* original)` is called only with a non-null
// pointer; `enable()` returns whether callers are now redirected.
template <typename Create, typename Publish, typename Enable>
inline bool CreatePublishEnable(Create&& create, Publish&& publish, Enable&& enable) {
  void* trampoline = nullptr;
  if (!create(&trampoline) || trampoline == nullptr) return false;
  publish(trampoline);
  return enable();
}

// The full attach with rollback. `*ppOriginal` holds the function being hooked
// on entry (or any prior value); on kAttached it holds the original-call
// pointer; on every other result it holds its entry value again. `remove` runs
// after a successful create whenever a later stage fails.
template <typename Create, typename Enable, typename Remove>
inline AttachStage AttachPublished(void** ppOriginal, Create&& create, Enable&& enable,
                                   Remove&& remove) {
  void* const prior = *ppOriginal;
  void* trampoline = nullptr;
  if (!create(&trampoline)) return AttachStage::kCreateFailed;
  if (trampoline == nullptr) {
    remove();
    return AttachStage::kNullTrampoline;
  }
  PublishPointer(ppOriginal, trampoline);
  if (!enable()) {
    remove();
    PublishPointer(ppOriginal, prior);
    return AttachStage::kEnableFailed;
  }
  return AttachStage::kAttached;
}

}  // namespace nevr::hook
