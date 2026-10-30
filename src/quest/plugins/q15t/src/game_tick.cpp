#include "game_tick.h"

#include <atomic>

#include "plugin_log.h"
#include "safe_read.h"

#include <cstdio>

namespace q15t {
namespace {

constexpr int kMaxTicks = kMaxGameTicks;
std::atomic<std::uintptr_t> g_real{0};
std::atomic<std::uintptr_t> g_vptr{0};
std::atomic<TickFn> g_ticks[kMaxTicks] = {};
std::atomic<int> g_count{0};
std::atomic<std::uint32_t> g_calls{0};
std::atomic<std::uint32_t> g_vptr_misses{0};

void Wrapper(void* game) {
  const auto real = reinterpret_cast<void (*)(void*)>(g_real.load(std::memory_order_acquire));
  if (real != nullptr) real(game);
  g_calls.fetch_add(1, std::memory_order_relaxed);
  const std::uintptr_t g = reinterpret_cast<std::uintptr_t>(game);
  bool ok = true;
  const auto vptr = Rd<std::uintptr_t>(g, &ok);
  if (vptr != g_vptr.load(std::memory_order_relaxed) || !ok) {
    // loud once: a wrong vptr means the tick never runs, and the arm silently does nothing
    if (g_vptr_misses.fetch_add(1, std::memory_order_relaxed) == 0) {
      char seen[24], want[24];
      std::snprintf(seen, sizeof seen, "0x%llx", static_cast<unsigned long long>(ok ? vptr : 0));
      std::snprintf(want, sizeof want, "0x%llx", static_cast<unsigned long long>(g_vptr.load()));
      const NevrLogField f[] = {{"seen_vptr", seen}, {"want_vptr", want}, {"readable", ok ? "1" : "0"}};
      Log(NEVR_LOG_ERROR, "game tick: object is not the CR15Game; tick callbacks skipped", f, 3);
    }
    return;
  }
  const int n = g_count.load(std::memory_order_acquire);
  for (int i = 0; i < n; ++i) {
    TickFn fn = g_ticks[i].load(std::memory_order_acquire);
    if (fn != nullptr) fn(g);
  }
}

}  // namespace

bool AddGameTick(TickFn fn) {
  const int i = g_count.load();
  if (i >= kMaxTicks) return false;
  g_ticks[i].store(fn, std::memory_order_release);
  g_count.store(i + 1, std::memory_order_release);
  return true;
}

int GameTickCount() { return g_count.load(); }

std::uint32_t GameTickCalls() { return g_calls.load(); }

InstallResult InstallGameTick(std::uintptr_t base, std::uintptr_t slot_vaddr, std::uintptr_t real_vaddr,
                              std::uintptr_t game_vptr_vaddr) {
  g_vptr.store(base + game_vptr_vaddr);
  return InstallImportHook("CncaGame::ExclusiveUpdateGame", base, slot_vaddr, reinterpret_cast<std::uintptr_t>(&Wrapper),
                           &g_real, real_vaddr);
}

}  // namespace q15t
