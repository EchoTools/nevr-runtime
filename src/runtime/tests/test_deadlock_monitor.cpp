// The game's CDeadlockMonitor thread loop as the hook runs it (patch/deadlock_monitor_loop.h, driven by
// patch/deadlock_monitor.cpp). The stock loop reads the clock again on every pass to test for a stall and writes null
// when the test passes; this build must refresh the heartbeat each pass and never evaluate the stall.
#include <gtest/gtest.h>

#include <cstdint>

#include "runtime/patch/deadlock_monitor_loop.h"

namespace {

// Runs the loop until the stand-in sleep has been called `passes` times. The clock returns 100, 101, ...
struct Harness {
  std::int32_t running = 1;
  std::uint32_t lastBeat = 5;  // old enough that a stock stall check (timeout 10) would fire on the first pass
  int sleeps = 0;
  int clockCalls = 0;
  std::uint64_t nextClock = 100;

  void Run(int passes) {
    nevr_deadlock_monitor::RunLoop(
        &running, &lastBeat,
        [&] {
          ++sleeps;
          if (sleeps >= passes) running = 0;
        },
        [&]() -> std::uint64_t {
          ++clockCalls;
          return nextClock++;
        });
  }
};

TEST(DeadlockMonitorLoop, EachPassSleepsOnceAndStoresTheClock) {
  Harness h;
  h.Run(3);
  EXPECT_EQ(h.sleeps, 3);
  EXPECT_EQ(h.lastBeat, 102U);  // the third clock value: 100, 101, 102
}

TEST(DeadlockMonitorLoop, StallIsNotEvaluatedSoTheClockIsReadOncePerPass) {
  // Mutant: restore the stock stall check (`if (lastBeat != 0 && clock() > lastBeat + 10) { crash }`) before the
  // store. The clock is then read twice on each pass, and the loop is not one read per second.
  Harness h;
  h.lastBeat = 5;
  h.Run(3);
  EXPECT_EQ(h.clockCalls, 3);
}

TEST(DeadlockMonitorLoop, ClearedRunningFlagEndsTheLoopWithoutAPass) {
  Harness h;
  h.running = 0;
  h.Run(3);
  EXPECT_EQ(h.sleeps, 0);
  EXPECT_EQ(h.clockCalls, 0);
  EXPECT_EQ(h.lastBeat, 5U);
}

}  // namespace
