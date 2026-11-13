// #2110: the yield phase of CSpinWait::WaitForValue. The original yields with Sleep(0) for its first ten rounds, then sleeps
// 1 ms a round (the argument is `counter <= 0`, the counter starting at 10). Plain checks, no gtest.

#include <cstdint>
#include <cstdio>

#include "runtime/patch/spin_wait_rules.h"

namespace {

int g_failures = 0;

void ExpectSleepMs(std::uint32_t round, std::uint32_t want) {
  const std::uint32_t got = nevr_spin_wait::SleepMsForRound(round);
  if (got != want) {
    std::printf("FAIL round %u: SleepMsForRound = %u, want %u\n", round, got, want);
    ++g_failures;
  }
}

void TestFirstTenRoundsYieldAndThenSleepOneMs() {
  for (std::uint32_t round = 0; round < 10; ++round) ExpectSleepMs(round, 0);
  for (std::uint32_t round = 10; round < 100; ++round) ExpectSleepMs(round, 1);
  if (nevr_spin_wait::kYieldRounds != 10) {
    std::printf("FAIL kYieldRounds = %u, want 10\n", nevr_spin_wait::kYieldRounds);
    ++g_failures;
  }
}

}  // namespace

int main() {
  TestFirstTenRoundsYieldAndThenSleepOneMs();
  if (g_failures != 0) {
    std::printf("test_spin_wait_rules: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("test_spin_wait_rules: all checks passed\n");
  return 0;
}
