/* Which libr15 slots the render-configuration hooks own (render_config.h), for the hardware dump (src/quest/diag).
 *
 * GotHook allows one owner per slot (got_hook.h, kAlreadyInstalled). Four of the five slots the render-configuration log
 * hooks are slots the hardware dump (docs/adr/0006) hooks as well. The render-configuration hooks are installed first
 * (the constructor sequence runs them before the dump), mark each slot they own here, and the dump installs nothing on a
 * marked slot and reports it as owned_by_render_config. Header-only inline state, like sysprop_int_share.h, so the
 * sentinel and the dump library need no link-time dependency on each other.
 */
#pragma once

#include <atomic>
#include <cstddef>

namespace sentinel {

enum class RenderConfigSlot : std::size_t {
  kSetDisplayRefreshRate,
  kSetClockLevels,
  kSetPropertyInt,
  kGetSystemPropertyFloat,
  kCount,
};

inline std::atomic<bool>& RenderConfigSlotFlag(RenderConfigSlot slot) noexcept {
  static std::atomic<bool> owned[static_cast<std::size_t>(RenderConfigSlot::kCount)] = {};
  return owned[static_cast<std::size_t>(slot)];
}

inline void MarkRenderConfigSlotOwned(RenderConfigSlot slot, bool owned) noexcept {
  RenderConfigSlotFlag(slot).store(owned, std::memory_order_release);
}
inline bool RenderConfigSlotOwned(RenderConfigSlot slot) noexcept {
  return RenderConfigSlotFlag(slot).load(std::memory_order_acquire);
}

}  // namespace sentinel
