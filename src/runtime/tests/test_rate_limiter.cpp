#include <gtest/gtest.h>

#include <string>

#include "runtime/log/rate_limiter.h"

using nevr_rate_limiter::RateLimiter;
using nevr_rate_limiter::RateVerdict;

namespace {

constexpr const char* kMsg = "[EVR] peer timed out";

/* Feeds `n` copies of `msg` at time `now`; returns how many were collapsed. */
int Burst(RateLimiter& rl, const char* msg, int n, uint64_t now) {
  char summary[256];
  int collapsed = 0;
  for (int i = 0; i < n; i++) {
    if (rl.Check(msg, static_cast<int>(std::strlen(msg)), now, summary, sizeof(summary)) == RateVerdict::Collapse) {
      collapsed++;
    }
  }
  return collapsed;
}

}  // namespace

TEST(RateLimiter, FirstThreeInAWindowPassAndTheRestCollapse) {
  RateLimiter rl;
  EXPECT_EQ(Burst(rl, kMsg, 20, 100), 17);
}

TEST(RateLimiter, DigitsDoNotChangeALinesIdentity) {
  RateLimiter rl;
  char summary[256];
  int collapsed = 0;
  for (int i = 0; i < 10; i++) {
    const std::string msg = "ExitProcess(" + std::to_string(i) + ") call #" + std::to_string(40 + i);
    if (rl.Check(msg.c_str(), static_cast<int>(msg.size()), 100, summary, sizeof(summary)) == RateVerdict::Collapse) {
      collapsed++;
    }
  }
  EXPECT_EQ(collapsed, 7);
}

TEST(RateLimiter, AFollowingLineAfterTheWindowReportsTheCollapsedCount) {
  RateLimiter rl;
  Burst(rl, kMsg, 20, 100);
  char summary[256];
  EXPECT_EQ(rl.Check(kMsg, static_cast<int>(std::strlen(kMsg)), 106, summary, sizeof(summary)), RateVerdict::Emit);
  EXPECT_STREQ(summary, "[NEVR.LOGFILTER] repeated count=17 window_s=6 msg=\"[EVR] peer timed out\"");
}

TEST(RateLimiter, ABurstThatGoesQuietIsReportedOnceItsWindowHasExpired) {
  RateLimiter rl;
  Burst(rl, kMsg, 20, 100);
  char summary[256];
  EXPECT_FALSE(rl.TakePending(103, false, summary, sizeof(summary))) << "the window is still open";
  EXPECT_STREQ(summary, "");
  ASSERT_TRUE(rl.TakePending(106, false, summary, sizeof(summary)));
  EXPECT_STREQ(summary, "[NEVR.LOGFILTER] repeated count=17 window_s=6 msg=\"[EVR] peer timed out\"");
  EXPECT_FALSE(rl.TakePending(106, false, summary, sizeof(summary))) << "a count is reported once";
}

TEST(RateLimiter, AReportedBurstStartsAFreshWindowOnItsNextLine) {
  RateLimiter rl;
  Burst(rl, kMsg, 20, 100);
  char summary[256];
  ASSERT_TRUE(rl.TakePending(106, false, summary, sizeof(summary)));
  EXPECT_EQ(Burst(rl, kMsg, 4, 107), 1) << "three pass again, one collapses";
  ASSERT_TRUE(rl.TakePending(108, true, summary, sizeof(summary)));
  EXPECT_STREQ(summary, "[NEVR.LOGFILTER] repeated count=1 window_s=1 msg=\"[EVR] peer timed out\"");
}

TEST(RateLimiter, ShutdownReportsAWindowThatIsStillOpen) {
  RateLimiter rl;
  Burst(rl, kMsg, 20, 100);
  char summary[256];
  ASSERT_TRUE(rl.TakePending(101, true, summary, sizeof(summary)));
  EXPECT_STREQ(summary, "[NEVR.LOGFILTER] repeated count=17 window_s=1 msg=\"[EVR] peer timed out\"");
  EXPECT_FALSE(rl.TakePending(101, true, summary, sizeof(summary)));
}

TEST(RateLimiter, NothingIsReportedWhenNothingWasCollapsed) {
  RateLimiter rl;
  Burst(rl, kMsg, 3, 100);
  char summary[256];
  EXPECT_FALSE(rl.TakePending(200, true, summary, sizeof(summary)));
  EXPECT_STREQ(summary, "");
}

TEST(RateLimiter, EveryQuietBurstIsReportedByItsOwnLine) {
  RateLimiter rl;
  Burst(rl, "[EVR] socket reset", 10, 100);
  Burst(rl, "[EVR] voip underrun", 6, 100);
  char summary[256];
  std::string all;
  int reports = 0;
  while (rl.TakePending(110, false, summary, sizeof(summary))) {
    all += summary;
    all += '\n';
    reports++;
  }
  EXPECT_EQ(reports, 2);
  EXPECT_NE(all.find("count=7 window_s=10 msg=\"[EVR] socket reset\""), std::string::npos) << all;
  EXPECT_NE(all.find("count=3 window_s=10 msg=\"[EVR] voip underrun\""), std::string::npos) << all;
}

TEST(RateLimiter, AClockThatMovedBackDoesNotMakeAWindowLookExpired) {
  RateLimiter rl;
  Burst(rl, kMsg, 20, 100);
  char summary[256];
  EXPECT_FALSE(rl.TakePending(90, false, summary, sizeof(summary)));
  ASSERT_TRUE(rl.TakePending(90, true, summary, sizeof(summary)));
  EXPECT_STREQ(summary, "[NEVR.LOGFILTER] repeated count=17 window_s=0 msg=\"[EVR] peer timed out\"");
}
