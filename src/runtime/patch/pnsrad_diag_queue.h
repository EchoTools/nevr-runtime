#pragma once

#include <atomic>
#include <cstdint>

// The DLL notification only records the module address. Optional MinHook
// diagnostics are installed by the runtime tick after the load callback ends.
class PnsradDiagQueue {
 public:
  void NoteLoaded(uintptr_t base) { pending_.store(base, std::memory_order_release); }

  template <typename Install>
  bool Drain(Install&& install) {
    const uintptr_t base = pending_.exchange(0, std::memory_order_acq_rel);
    if (base == 0) return false;
    install(base);
    return true;
  }

 private:
  std::atomic<uintptr_t> pending_{0};
};
