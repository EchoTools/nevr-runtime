#include "clock_levels_hook.h"

#include <atomic>
#include <cstdint>

#include "hook_log.h"
#include "hook_report.h"

namespace nevr_quest::clock_levels {

namespace {

std::atomic<std::uint64_t> g_calls{0};
std::atomic<std::uint64_t> g_overridden{0};
// Set once by Arm() before the first call can happen (the game calls at VR-mode entry); read by every call.
std::atomic<bool> g_override{false};

}  // namespace

Levels Choose(std::int32_t cpu, std::int32_t gpu) noexcept {
  const bool rewrite = g_override.load(std::memory_order_acquire) && cpu == kGameCpuLevel && gpu == kGameGpuLevel;
  g_calls.fetch_add(1, std::memory_order_relaxed);
  if (rewrite) {
    g_overridden.fetch_add(1, std::memory_order_relaxed);
    return {kOverrideCpuLevel, kOverrideGpuLevel};
  }
  return {cpu, gpu};
}

bool RegisterCounters() noexcept {
  struct Entry {
    const char* name;
    const std::atomic<std::uint64_t>* value;
    sentinel::ReportKind kind;
  };
  const Entry entries[kCounterCount] = {
      {"clock_levels_calls", &g_calls, sentinel::ReportKind::kCalls},
      {"clock_levels_overridden", &g_overridden, sentinel::ReportKind::kCalls},
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

bool Arm(bool overrideOn) noexcept {
  g_override.store(overrideOn, std::memory_order_release);
  sentinel::LogFields(sentinel::LogLevel::kInfo, "clock_levels_arm",
                      {{"result", "armed"}, {"override", overrideOn ? "on" : "off"}});
  return true;
}

bool ArmIfCounted(bool countersRegistered, bool overrideOn) noexcept {
  if (!countersRegistered) {
    sentinel::LogFields(sentinel::LogLevel::kError, "clock_levels_arm",
                        {{"result", "skipped"}, {"why", "counters_refused"}, {"override", overrideOn ? "on" : "off"}});
    return false;
  }
  return Arm(overrideOn);
}

void ArmForTest(bool overrideOn) noexcept { g_override.store(overrideOn, std::memory_order_release); }

void ResetForTest() noexcept {
  g_override.store(false, std::memory_order_relaxed);
  g_calls.store(0, std::memory_order_relaxed);
  g_overridden.store(0, std::memory_order_relaxed);
}

Counts CurrentCounts() noexcept {
  return {g_calls.load(std::memory_order_relaxed), g_overridden.load(std::memory_order_relaxed)};
}

}  // namespace nevr_quest::clock_levels
