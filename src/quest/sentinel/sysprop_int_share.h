/* Sharing libr15's vrapi_GetSystemPropertyInt slot (JUMP_SLOT ELF 0x36fa728) between two users.
 *
 * GotHook allows one owner per slot (got_hook.h, kAlreadyInstalled), and two things want this one: the texture-quality
 * hook (texture_quality_hook.h, #943), which answers two property ids, and the hardware dump (src/quest/diag, #335),
 * which records every answer. The texture-quality hook owns the slot when it installs; the hardware dump then asks to
 * be told each query's native answer instead of installing a second hook:
 *
 *   - the owner calls MarkSysPropIntSlotOwned(true) once its install took, and (false) if it ever gives the slot up;
 *   - the hardware dump, when SysPropIntSlotOwned(), registers its observer here and installs nothing on the slot;
 *   - the owner's handler calls the observer, if one is registered, with the java argument, the id and the runtime's
 *     own answer (before any override), after it called the original.
 *
 * Header-only inline state, so the sentinel and the hardware dump library need no link-time dependency on each other.
 * The observer runs on the game's thread inside the owner's handler, so it follows the hook-frame rule
 * (callback_thunk.h): personality-free, no logging, no allocation. It is `noexcept` and takes `const void*` for the
 * ovrJava pointer so this header names no game type.
 */
#pragma once

#include <atomic>

namespace sentinel {

using SysPropIntObserver = void (*)(const void* java, int id, int nativeResult) noexcept;

inline std::atomic<bool>& SysPropIntSlotOwnedFlag() noexcept {
  static std::atomic<bool> owned{false};
  return owned;
}
inline std::atomic<SysPropIntObserver>& SysPropIntObserverSlot() noexcept {
  static std::atomic<SysPropIntObserver> observer{nullptr};
  return observer;
}

inline void MarkSysPropIntSlotOwned(bool owned) noexcept {
  SysPropIntSlotOwnedFlag().store(owned, std::memory_order_release);
}
inline bool SysPropIntSlotOwned() noexcept { return SysPropIntSlotOwnedFlag().load(std::memory_order_acquire); }

inline void SetSysPropIntObserver(SysPropIntObserver observer) noexcept {
  SysPropIntObserverSlot().store(observer, std::memory_order_release);
}
inline SysPropIntObserver GetSysPropIntObserver() noexcept {
  return SysPropIntObserverSlot().load(std::memory_order_acquire);
}

}  // namespace sentinel
