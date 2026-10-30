// #1419: the GPU score rule (patch/gpu_score.h). Pure: no game image, no Windows call.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>

#include "runtime/patch/gpu_score.h"

namespace {

using nevr_gpu_score::DecideScore;
using nevr_gpu_score::NvidiaMeasure;
using nevr_gpu_score::Outcome;

NvidiaMeasure Measured(std::uint32_t cores, std::uint32_t clockKhz) {
  NvidiaMeasure m;
  m.haveCores = true;
  m.cores = cores;
  m.haveClockKhz = true;
  m.clockKhz = clockKhz;
  return m;
}

// The game's arithmetic: 3584 cores at 1,500,000 kHz: 3584 x 1500000 x 2 = 10752000000, x 1e-9 = 10.752.
TEST(GpuScore, NvidiaScoreIsCoresTimesKhzTimesTwoTimesOneE9) {
  EXPECT_DOUBLE_EQ(nevr_gpu_score::NvidiaScore(3584, 1500000), 10.752);
  EXPECT_DOUBLE_EQ(nevr_gpu_score::NvidiaScore(1, 1), 2e-9);
}

TEST(GpuScore, AnAbsurdProductIsRefusedNotStored) {
  // 3584 cores at 1,000,000,000 kHz (a clock of 1 THz): 3584 x 10^9 x 2 x 1e-9 = 7168, over the 1000 ceiling.
  const auto d = DecideScore(nevr_gpu_score::kVendorNvidia, 0.0, Measured(3584, 1000000000U));
  EXPECT_EQ(d.outcome, Outcome::Unrated);
  EXPECT_STREQ(d.reason, "score_implausible");
}

TEST(GpuScore, ANonZeroGameScoreIsKeptWhateverTheVendorOrTheQuery) {
  const auto d = DecideScore(nevr_gpu_score::kVendorNvidia, 6.0, Measured(3584, 1500000));
  EXPECT_EQ(d.outcome, Outcome::Kept);
  EXPECT_DOUBLE_EQ(d.score, 6.0);
  EXPECT_STREQ(d.source, "game");
  EXPECT_STREQ(d.reason, "");

  const auto amd = DecideScore(2, 10.0, NvidiaMeasure());
  EXPECT_EQ(amd.outcome, Outcome::Kept);
  EXPECT_DOUBLE_EQ(amd.score, 10.0);
}

TEST(GpuScore, ANvidiaZeroScoreWithBothValuesIsFilledWithTheFormula) {
  const auto d = DecideScore(nevr_gpu_score::kVendorNvidia, 0.0, Measured(3584, 1500000));
  EXPECT_EQ(d.outcome, Outcome::Filled);
  EXPECT_DOUBLE_EQ(d.score, 10.752);
  EXPECT_STREQ(d.source, "nvidia_nvapi");
  EXPECT_STREQ(d.reason, "");
}

TEST(GpuScore, AnAmdZeroScoreStaysUnratedWithTheReason) {
  const auto d = DecideScore(2, 0.0, Measured(3584, 1500000));
  EXPECT_EQ(d.outcome, Outcome::Unrated);
  EXPECT_DOUBLE_EQ(d.score, 0.0);
  EXPECT_STREQ(d.reason, "vendor_unsupported");
}

TEST(GpuScore, AMissingNvapiValueNamesTheStepThatFailed) {
  NvidiaMeasure noCores;
  noCores.haveClockKhz = true;
  noCores.clockKhz = 1500000;
  noCores.failure = "nvapi_get_gpu_core_count_failed";
  const auto a = DecideScore(nevr_gpu_score::kVendorNvidia, 0.0, noCores);
  EXPECT_EQ(a.outcome, Outcome::Unrated);
  EXPECT_STREQ(a.reason, "nvapi_get_gpu_core_count_failed");

  NvidiaMeasure noClock;
  noClock.haveCores = true;
  noClock.cores = 3584;
  const auto b = DecideScore(nevr_gpu_score::kVendorNvidia, 0.0, noClock);
  EXPECT_EQ(b.outcome, Outcome::Unrated);
  EXPECT_STREQ(b.reason, "nvapi_clock_unavailable");
}

TEST(GpuScore, AZeroOrAbsurdNvidiaResultStaysUnrated) {
  const auto zero = DecideScore(nevr_gpu_score::kVendorNvidia, 0.0, Measured(0, 1500000));
  EXPECT_EQ(zero.outcome, Outcome::Unrated);
  EXPECT_STREQ(zero.reason, "score_implausible");

  const auto absurd = DecideScore(nevr_gpu_score::kVendorNvidia, 0.0, Measured(3584, 1000000000U));
  EXPECT_EQ(absurd.outcome, Outcome::Unrated);
  EXPECT_STREQ(absurd.reason, "score_implausible");
}

TEST(GpuScore, ANanGameScoreIsKeptAsItIs) {
  // A NaN is not zero, so the game's own value is never overwritten by the runtime.
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const auto d = DecideScore(nevr_gpu_score::kVendorNvidia, nan, Measured(3584, 1500000));
  EXPECT_EQ(d.outcome, Outcome::Kept);
  EXPECT_TRUE(std::isnan(d.score));
}

}  // namespace
