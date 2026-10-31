#pragma once

#include <cstdint>

namespace nevr_deadlock_monitor {

/// The loop of the game's CDeadlockMonitor thread procedure as this build runs it (deadlock_monitor.cpp supplies the
/// game's Sleep and CTimer::Seconds). Each pass sleeps, then stores the clock in the heartbeat. The stock stall check
/// is not evaluated: its panic is a store through a null pointer, which a dedicated server must not reach on a stall.
/// Returns when the game clears *running.
template <typename SleepFn, typename ClockFn>
void RunLoop(const std::int32_t* running, std::uint32_t* lastBeat, SleepFn&& sleep, ClockFn&& clock) {
  while (*running != 0) {
    sleep();
    *lastBeat = static_cast<std::uint32_t>(clock());
  }
}

}  // namespace nevr_deadlock_monitor
