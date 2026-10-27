#include "clock_levels_hook.h"

#include <atomic>
#include <cstdint>

#include "hook_install.h"
#include "hook_log.h"
#include "hook_report.h"

namespace nevr_quest::clock_levels {

namespace {

using sentinel::pinned::OvrMobileOpaque;

sentinel::GotHook g_hook;

std::atomic<std::uint64_t> g_calls{0};
std::atomic<std::uint64_t> g_overridden{0};
// Set once by Install() before the thunk is installed; read by every call.
std::atomic<bool> g_override{false};

// vrapi_SetClockLevels. The original always runs, with the game's levels or the override's, and its result is returned.
std::int32_t Handler(Thunk::Fn original, OvrMobileOpaque* ovr, std::int32_t cpu, std::int32_t gpu) noexcept {
  const bool overrideOn = g_override.load(std::memory_order_acquire);
  const bool rewrite = overrideOn && cpu == kGameCpuLevel && gpu == kGameGpuLevel;
  const std::int32_t appliedCpu = rewrite ? kOverrideCpuLevel : cpu;
  const std::int32_t appliedGpu = rewrite ? kOverrideGpuLevel : gpu;
  const std::int32_t result = original(ovr, appliedCpu, appliedGpu);

  const std::uint64_t call = g_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (rewrite) g_overridden.fetch_add(1, std::memory_order_relaxed);
  if (call <= kMaxLoggedCalls) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "clock_levels",
                        {{"requested_cpu", cpu},
                         {"requested_gpu", gpu},
                         {"applied_cpu", appliedCpu},
                         {"applied_gpu", appliedGpu},
                         {"result", result},
                         {"override", overrideOn ? "on" : "off"}});
  }
  return result;
}

NEVR_HOOK_RECORD(kHook, Thunk, &Handler);

}  // namespace

bool RegisterCounters() noexcept {
  struct Entry {
    const char* name;
    const std::atomic<std::uint64_t>* value;
    sentinel::ReportKind kind;
  };
  const Entry entries[kCounterCount] = {
      {"clock_levels_calls", &g_calls, sentinel::ReportKind::kCalls},
      {"clock_levels_overridden", &g_overridden, sentinel::ReportKind::kCalls},
      {"clock_levels_thunk_faults", &Thunk::FaultCounter(), sentinel::ReportKind::kFaults},
  };
  int registered = 0;
  for (const Entry& e : entries) registered += sentinel::RegisterReportCounter(e.name, e.value, e.kind) ? 1 : 0;
  if (registered != kCounterCount) {
    sentinel::LogFields(sentinel::LogLevel::kError, "clock_levels_counters",
                        {{"result", "refused"}, {"registered", registered}, {"wanted", kCounterCount}});
    return false;
  }
  return true;
}

bool Install(bool overrideOn) noexcept {
  g_override.store(overrideOn, std::memory_order_release);
  Thunk::Arm(kHook);
  const sentinel::GotStatus status = sentinel::InstallThunk<Thunk>(g_hook, sentinel::pinned::LibR15SetClockLevels());
  const bool ok = status == sentinel::GotStatus::kOk;
  sentinel::LogFields(ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "clock_levels_install",
                      {{"result", ok ? "installed" : "failed"},
                       {"override", overrideOn ? "on" : "off"},
                       {"slot", sentinel::GotStatusName(status)}});
  return ok;
}

bool InstallIfCounted(bool countersRegistered, bool overrideOn) noexcept {
  if (!countersRegistered) {
    sentinel::LogFields(sentinel::LogLevel::kError, "clock_levels_install",
                        {{"result", "skipped"}, {"why", "counters_refused"}, {"override", overrideOn ? "on" : "off"}});
    return false;
  }
  return Install(overrideOn);
}

void ArmForTest(bool overrideOn) noexcept {
  g_override.store(overrideOn, std::memory_order_release);
  Thunk::Arm(kHook);
}

void ResetForTest() noexcept {
  g_override.store(false, std::memory_order_relaxed);
  g_calls.store(0, std::memory_order_relaxed);
  g_overridden.store(0, std::memory_order_relaxed);
}

Counts CurrentCounts() noexcept {
  return {g_calls.load(std::memory_order_relaxed), g_overridden.load(std::memory_order_relaxed)};
}

}  // namespace nevr_quest::clock_levels
