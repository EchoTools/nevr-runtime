// The early quit lockout's decisions (patch/early_quit_lockout_rules.h): what the countdown becomes after the
// game stores a penalty, and when the scripts' lockoutcountdownactive reads true. The hooks that apply these
// are proven live (echovr-reconstruction docs/earlyquit_field_analysis.md).
#include <gtest/gtest.h>

#include "runtime/patch/early_quit_lockout_rules.h"

namespace {

constexpr std::int64_t kNow = 1'790'975'000;

}  // namespace

// A penalty that runs into the future starts the countdown to that time: the game service's penalty
// timestamp is the lockout's end.
TEST(EarlyQuitLockout, AFuturePenaltyStartsTheCountdownToIt) {
  const auto c = nevr_early_quit_lockout::AfterSetPenalty(kNow + 600, kNow + 600, kNow);
  EXPECT_EQ(c.expiry, kNow + 600);
  EXPECT_TRUE(c.active);
  EXPECT_FALSE(c.resetStoredTs);
}

// The game's setter keeps a newer stored timestamp; the countdown follows what is stored, so an older or
// repeated message does not shorten a running lockout.
TEST(EarlyQuitLockout, AnOlderMessageKeepsTheRunningLockout) {
  const auto c = nevr_early_quit_lockout::AfterSetPenalty(kNow + 60, kNow + 600, kNow);
  EXPECT_EQ(c.expiry, kNow + 600);
  EXPECT_TRUE(c.active);
}

// A penalty already over (or the stored one past) leaves no countdown.
TEST(EarlyQuitLockout, AnExpiredPenaltyLeavesNoCountdown) {
  const auto c = nevr_early_quit_lockout::AfterSetPenalty(kNow - 1, kNow - 1, kNow);
  EXPECT_EQ(c.expiry, -1);
  EXPECT_FALSE(c.active);
  EXPECT_FALSE(c.resetStoredTs);
  EXPECT_FALSE(nevr_early_quit_lockout::AfterSetPenalty(kNow, kNow, kNow).active) << "a lockout ending now is over";
}

// The game service clearing a penalty (-1 or 0) ends the lockout, though the game's setter ignores it as
// older than the stored timestamp; the stored timestamp is reset so the next penalty is taken.
TEST(EarlyQuitLockout, AClearedPenaltyEndsTheLockout) {
  for (const std::int64_t cleared : {std::int64_t{-1}, std::int64_t{0}}) {
    const auto c = nevr_early_quit_lockout::AfterSetPenalty(cleared, kNow + 600, kNow);
    EXPECT_EQ(c.expiry, -1) << cleared;
    EXPECT_FALSE(c.active) << cleared;
    EXPECT_TRUE(c.resetStoredTs) << cleared;
  }
  EXPECT_FALSE(nevr_early_quit_lockout::AfterSetPenalty(-1, -1, kNow).resetStoredTs) << "nothing stored, nothing to reset";
}

// lockoutcountdownactive (hard-coded 0 in the game) reads true exactly while seconds remain.
TEST(EarlyQuitLockout, CountdownActiveWhileSecondsRemain) {
  EXPECT_TRUE(nevr_early_quit_lockout::CountdownActive(1));
  EXPECT_TRUE(nevr_early_quit_lockout::CountdownActive(900));
  EXPECT_FALSE(nevr_early_quit_lockout::CountdownActive(0));
  EXPECT_FALSE(nevr_early_quit_lockout::CountdownActive(-5));
}
