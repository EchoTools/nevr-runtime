// The hook registry's self-check (runtime/compat/self_check.h) and its start-up summary.
//
// A hook the registry refused for want of room (sentinel::GotStatus::kRegistryFull, got_hook.h) is not installed and the game
// runs its own code in that slot. The refusal is already an Error line at the moment it happens; this is the other two
// places that say so:
//
//   - the self-check `hook_registry_has_room`: the probe fails, naming the hooks that lost, the first time Flush polls it
//     after a refusal and again whenever the count changes, so a run card shows it from the service side as well as the log;
//     a run with no refusal passes once, with the counts it ran on;
//   - the summary line the constructor writes when its sequence is done (production_steps.cpp, which builds this text with
//     DescribeRegistry): Error with the same names when anything was refused, Info with the counts otherwise.
//
// Header only on top of got_hook.h (the registry's snapshot) and the self-check unit's Observation, and free of exception
// handling, so the host test that drives the registry (built with -fno-exceptions) covers it without another translation unit.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "quest/sentinel/got_hook.h"
#include "runtime/compat/self_check.h"

namespace nevr_quest::integration {

inline constexpr const char* kHookRegistryCheckName = "hook_registry_has_room";
inline constexpr const char* kHookRegistryCheckExpected =
    "no hook was refused for want of room in the hook registry (refused=0)";

// "capacity=256 used=61 high_water=61 refused=0", then, for each hook that lost (the first few), ` lost=<module>:<symbol>`
// with `!` after it for a critical one. The same words in the check's result and in the constructor's line.
inline std::string DescribeRegistry(const sentinel::RegistryStats& stats) {
  std::string text = "capacity=" + std::to_string(stats.capacity) + " used=" + std::to_string(stats.used) +
                     " high_water=" + std::to_string(stats.highWater) + " refused=" + std::to_string(stats.refused);
  for (std::size_t i = 0; i < stats.ledgerCount; ++i) {
    const sentinel::RegistryRefusal& lost = stats.lost[i];
    text += std::string(" lost=") + lost.module + ":" + lost.symbol + (lost.critical ? "!" : "");
  }
  if (stats.refused > stats.ledgerCount) {
    text += " more=" + std::to_string(stats.refused - stats.ledgerCount);
  }
  return text;
}

namespace detail {
inline constexpr std::uint64_t kNothingReported = ~std::uint64_t{0};
inline std::atomic<std::uint64_t> g_reportedRefusals{kNothingReported};
}  // namespace detail

// Polled by Flush on whatever thread calls it: reads the registry's counters under the registry's own short lock and
// builds one string. True when there is a result: the first poll, then again whenever the refusal count moved.
inline bool HookRegistryProbe(nevr_self_check::Observation* out) {
  const sentinel::RegistryStats stats = sentinel::RegistrySnapshot();
  if (detail::g_reportedRefusals.exchange(stats.refused) == stats.refused) return false;
  out->observed = DescribeRegistry(stats);
  if (out->observed.size() > nevr_self_check::kMaxTextBytes) out->observed.resize(nevr_self_check::kMaxTextBytes);
  out->pass = stats.refused == 0;
  return true;
}

// Test seam: the next poll reports again.
inline void ResetHookRegistryCheckForTest() { detail::g_reportedRefusals.store(detail::kNothingReported); }

}  // namespace nevr_quest::integration
