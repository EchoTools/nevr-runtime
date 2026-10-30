#include "clock_levels.h"

#include <atomic>
#include "plugin_log.h"

#ifndef Q15T_CPU_LEVEL
#define Q15T_CPU_LEVEL 4
#endif
#ifndef Q15T_GPU_LEVEL
#define Q15T_GPU_LEVEL 4
#endif

namespace q15t {
namespace {

using SetClockLevelsFn = std::int32_t (*)(void* java, std::int32_t cpu, std::int32_t gpu);

std::atomic<bool> g_raise{false};
std::atomic<std::uintptr_t> g_real{0};
std::atomic<std::uint32_t> g_calls{0};
std::atomic<std::int32_t> g_asked_cpu{-1}, g_asked_gpu{-1}, g_sent_cpu{-1}, g_sent_gpu{-1};

constexpr std::uint32_t kLogFirstCalls = 16;
constexpr std::uint32_t kSummaryEvery = 256;

std::int32_t Raised(std::int32_t asked, std::int32_t floor) { return asked < floor ? floor : asked; }

std::int32_t Wrapper(void* java, std::int32_t cpu, std::int32_t gpu) {
  const std::uint32_t n = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  std::int32_t send_cpu = cpu, send_gpu = gpu;
  const bool raise = g_raise.load(std::memory_order_relaxed);
  if (raise) {
    send_cpu = Raised(cpu, Q15T_CPU_LEVEL);
    send_gpu = Raised(gpu, Q15T_GPU_LEVEL);
  }
  const auto real = reinterpret_cast<SetClockLevelsFn>(g_real.load(std::memory_order_acquire));
  const std::int32_t rc = real != nullptr ? real(java, send_cpu, send_gpu) : -1;
  const bool changed = cpu != g_asked_cpu.load(std::memory_order_relaxed) || gpu != g_asked_gpu.load(std::memory_order_relaxed);
  g_asked_cpu.store(cpu, std::memory_order_relaxed);
  g_asked_gpu.store(gpu, std::memory_order_relaxed);
  g_sent_cpu.store(send_cpu, std::memory_order_relaxed);
  g_sent_gpu.store(send_gpu, std::memory_order_relaxed);
  if (n <= kLogFirstCalls || changed || n % kSummaryEvery == 0) {
    const KV kv[] = {{"call", n},          {"asked_cpu", cpu},  {"asked_gpu", gpu},
                     {"sent_cpu", send_cpu}, {"sent_gpu", send_gpu}, {"rc", rc},
                     {"candidate", raise ? 1 : 0}};
    LogNums(NEVR_LOG_INFO, "vrapi_SetClockLevels", kv, 7);
  }
  return rc;
}

}  // namespace

void SetClockRaise(bool raise) { g_raise.store(raise, std::memory_order_relaxed); }
bool ClockRaise() { return g_raise.load(std::memory_order_relaxed); }

ClockStats GetClockStats() {
  return {g_calls.load(), g_asked_cpu.load(), g_asked_gpu.load(), g_sent_cpu.load(), g_sent_gpu.load()};
}

InstallResult InstallClockLevels(std::uintptr_t base, std::uintptr_t slot_vaddr) {
  return InstallImportHook("vrapi_SetClockLevels", base, slot_vaddr, reinterpret_cast<std::uintptr_t>(&Wrapper), &g_real);
}

}  // namespace q15t
