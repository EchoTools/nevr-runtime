// The module-loaded observers (post_load.h): a fixed table of function pointers the dlopen hook calls for every non-null handle.
// Atomics only, no lock and no exception path: AfterDlopen calls it on the game's thread, and an observer is added from an install
// step that may run beside a dlopen on another thread.
#include <atomic>

#include "quest/integration/post_load.h"

namespace nevr_quest::integration {

namespace {
std::atomic<ModuleLoadedFn> g_observers[kModuleLoadedObservers] = {};
}  // namespace

void SetModuleLoadedObserver(ModuleLoadedFn fn) noexcept { g_observers[0].store(fn, std::memory_order_release); }

bool AddModuleLoadedObserver(ModuleLoadedFn fn) noexcept {
  if (fn == nullptr) return false;
  for (int i = 1; i < kModuleLoadedObservers; ++i) {
    ModuleLoadedFn expected = nullptr;
    if (g_observers[i].compare_exchange_strong(expected, fn, std::memory_order_acq_rel)) return true;
    if (expected == fn) return true;  // already there
  }
  return false;
}

void NotifyModuleLoaded(const char* name, void* handle) noexcept {
  for (const std::atomic<ModuleLoadedFn>& slot : g_observers) {
    const ModuleLoadedFn fn = slot.load(std::memory_order_acquire);
    if (fn != nullptr) fn(name, handle);
  }
}

void ClearModuleLoadedObserversForTest() noexcept {
  for (std::atomic<ModuleLoadedFn>& slot : g_observers) slot.store(nullptr, std::memory_order_release);
}

}  // namespace nevr_quest::integration
