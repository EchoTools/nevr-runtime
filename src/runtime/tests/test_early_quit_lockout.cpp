// The early quit lockout's decisions (patch/early_quit_lockout_rules.h): what the countdown becomes after the
// game's setter has looked at an update, and when the scripts' lockoutcountdownactive reads true. The hooks
// that apply these are proven live (echovr-reconstruction docs/earlyquit_field_analysis.md).
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "runtime/patch/early_quit_lockout_rules.h"

namespace {

using nevr_early_quit_lockout::Countdown;

constexpr std::int64_t kNow = 1'790'975'000;

// CR15NetGame::SetEarlyQuitPenaltyLevel (echovr.exe 0x1401ae2e0) as its disassembly reads, written here apart
// from the rules header so the tests do not check the rules against themselves: the stamp is stored at
// +0x64820 only when nothing is stored (-1) or the stored one is smaller (signed); otherwise the function
// returns before storing anything.
struct GameSetter {
  std::int64_t stored = -1;

  // One update as the hook sees it: the stamp and expiry the game service sent, then the hook's decision.
  // Mirrors SetPenaltyHook: a legacy-shaped update opens the gate (-1) before the setter's own gate runs.
  Countdown Receive(std::int64_t stamp, std::int64_t expiry, std::int64_t now) {
    const nevr_early_quit_lockout::Intake intake = nevr_early_quit_lockout::ReadIntake(stamp, expiry, now);
    if (intake.legacy) stored = -1;
    const std::int64_t before = stored;
    if (stored == -1 || stored < stamp) stored = stamp;
    return nevr_early_quit_lockout::AfterSetPenalty(before, stored, stamp, intake.expiry, now);
  }
};

std::vector<std::uint8_t> FromHex(const std::string& hex) {
  std::vector<std::uint8_t> bytes;
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) bytes.push_back(static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
  return bytes;
}

}  // namespace

// A penalty that runs into the future starts the countdown to its expiry.
TEST(EarlyQuitLockout, AFutureExpiryStartsTheCountdownToIt) {
  GameSetter game;
  const Countdown c = game.Receive(kNow, kNow + 600, kNow);
  EXPECT_TRUE(c.applied);
  EXPECT_EQ(c.expiry, kNow + 600);
  EXPECT_TRUE(c.active);
}

// The latest message from the game service wins, a shorter lockout included: the stamp only orders the
// messages, the expiry is what the countdown runs to.
TEST(EarlyQuitLockout, ALaterMessageWithAnEarlierExpiryShortensTheCountdown) {
  GameSetter game;
  ASSERT_EQ(game.Receive(kNow, kNow + 600, kNow).expiry, kNow + 600);
  const Countdown c = game.Receive(kNow + 1, kNow + 60, kNow + 1);
  EXPECT_TRUE(c.applied);
  EXPECT_EQ(c.expiry, kNow + 60);
  EXPECT_TRUE(c.active);
}

// A later message that changes only the counters and levels carries the same expiry under a newer stamp. The
// setter takes it (the stamp is newer), and the countdown keeps running to the same end.
TEST(EarlyQuitLockout, ACountersOnlyUpdateAppliesAndKeepsTheCountdown) {
  GameSetter game;
  ASSERT_EQ(game.Receive(kNow, kNow + 600, kNow).expiry, kNow + 600);
  const Countdown c = game.Receive(kNow + 5, kNow + 600, kNow + 5);
  EXPECT_TRUE(c.applied);
  EXPECT_EQ(c.expiry, kNow + 600);
  EXPECT_TRUE(c.active);
}

// A cleared expiry (-1 or 0) under a newer stamp ends the lockout.
TEST(EarlyQuitLockout, AClearedExpiryUnderANewerStampEndsTheLockout) {
  for (const std::int64_t cleared : {std::int64_t{-1}, std::int64_t{0}}) {
    GameSetter game;
    ASSERT_TRUE(game.Receive(kNow, kNow + 600, kNow).active);
    const Countdown c = game.Receive(kNow + 1, cleared, kNow + 1);
    EXPECT_TRUE(c.applied) << cleared;
    EXPECT_EQ(c.expiry, -1) << cleared;
    EXPECT_FALSE(c.active) << cleared;
  }
}

// An update whose stamp is not newer than the stored one is ignored, as the game ignores it: the hook leaves
// the countdown alone, whatever expiry the update carries.
TEST(EarlyQuitLockout, AnUpdateWhoseStampIsNotNewerIsIgnored) {
  for (const std::int64_t stamp : {kNow, kNow - 30}) {
    GameSetter game;
    ASSERT_TRUE(game.Receive(kNow, kNow + 600, kNow).applied);
    const Countdown c = game.Receive(stamp, kNow + 60, kNow + 1);
    EXPECT_FALSE(c.applied) << stamp;
    EXPECT_EQ(game.stored, kNow) << stamp;
  }
}

// The hook trusts what is stored, not only the gate: when the gate was open but the stamp is not what the setter
// left at +0x64820 (the setter did not store it), the update did not apply and the countdown is left alone.
TEST(EarlyQuitLockout, AnUpdateTheSetterDidNotStoreIsNotApplied) {
  EXPECT_FALSE(nevr_early_quit_lockout::AfterSetPenalty(-1, -1, kNow, kNow + 600, kNow).applied);
  EXPECT_FALSE(nevr_early_quit_lockout::AfterSetPenalty(kNow - 10, kNow - 10, kNow, kNow + 600, kNow).applied);
  EXPECT_TRUE(nevr_early_quit_lockout::AfterSetPenalty(kNow - 10, kNow, kNow, kNow + 600, kNow).applied);
}

// The login path passes -1 as the expiry (CR14LocalPlayerCS_ParseEarlyQuitData, echovr.exe 0x14017e330): it
// carries a stamp and no expiry, so it starts no countdown. The update the game service sends after it does.
TEST(EarlyQuitLockout, TheLoginCallStartsNoCountdown) {
  GameSetter game;
  const Countdown login = game.Receive(kNow - 5, -1, kNow);
  EXPECT_TRUE(login.applied);
  EXPECT_EQ(login.expiry, -1);
  EXPECT_FALSE(login.active);
  const Countdown update = game.Receive(kNow, kNow + 600, kNow);
  EXPECT_TRUE(update.applied) << "the update after the login carries a stamp above the profile's";
  EXPECT_EQ(update.expiry, kNow + 600);
  EXPECT_TRUE(update.active);
}

// An expiry already over (or ending now) leaves no countdown.
TEST(EarlyQuitLockout, AnExpiredPenaltyLeavesNoCountdown) {
  GameSetter game;
  const Countdown c = game.Receive(kNow, kNow - 1, kNow);
  EXPECT_TRUE(c.applied);
  EXPECT_EQ(c.expiry, -1);
  EXPECT_FALSE(c.active);
  GameSetter other;
  EXPECT_FALSE(other.Receive(kNow, kNow, kNow).active) << "a lockout ending now is over";
}

// The setter compares signed values: the first stamp ever (stored -1) is always taken, and so is any stamp
// above a stored one.
TEST(EarlyQuitLockout, TheSetterTakesTheFirstStampAndAnyNewerOne) {
  EXPECT_TRUE(nevr_early_quit_lockout::SetterTakes(-1, 0));
  EXPECT_TRUE(nevr_early_quit_lockout::SetterTakes(-1, -1));
  EXPECT_TRUE(nevr_early_quit_lockout::SetterTakes(kNow, kNow + 1));
  EXPECT_FALSE(nevr_early_quit_lockout::SetterTakes(kNow, kNow));
  EXPECT_FALSE(nevr_early_quit_lockout::SetterTakes(kNow, kNow - 1));
}

// The update the game service sends, byte for byte: the fixture and its hex are the ones in
// EchoTools/nevr-runtime#907 (issuecomment-6393534889), and the nakama test for EchoTools/nakama#712 encodes the
// same fixture and asserts the same hex. A layout change on either side fails both tests.
TEST(EarlyQuitLockout, TheGoldenUpdateParsesToTheStampAndTheExpiry) {
  const std::vector<std::uint8_t> bytes =
      FromHex("000000000000000000000000000000008877665544332211181cc06a00000000701ec06a0000000003000000110000000200000002010000");
  ASSERT_EQ(bytes.size(), nevr_early_quit_lockout::kUpdateSize);
  nevr_early_quit_lockout::Update update{};
  ASSERT_TRUE(nevr_early_quit_lockout::ParseUpdate(bytes.data(), bytes.size(), &update));
  EXPECT_EQ(update.stamp, 1'790'975'000);
  EXPECT_EQ(update.expiry, 1'790'975'600);
  EXPECT_EQ(update.numEarlyQuits, 3);
  EXPECT_EQ(update.numSteadyMatches, 17);
  EXPECT_EQ(update.numSteadyEarlyQuits, 2);
  EXPECT_EQ(update.penaltyLevel, 2);
  EXPECT_EQ(update.steadyPlayerLevel, 1);
  GameSetter game;
  const Countdown c = game.Receive(update.stamp, update.expiry, update.stamp);
  EXPECT_TRUE(c.applied);
  EXPECT_EQ(c.expiry, 1'790'975'600) << "the lockout ends at the expiry, not at the stamp";
  EXPECT_TRUE(c.active);
}

// A buffer shorter than the message is refused, not read past.
TEST(EarlyQuitLockout, AShortUpdateIsRefused) {
  const std::vector<std::uint8_t> bytes(nevr_early_quit_lockout::kUpdateSize - 1, 0);
  nevr_early_quit_lockout::Update update{};
  EXPECT_FALSE(nevr_early_quit_lockout::ParseUpdate(bytes.data(), bytes.size(), &update));
  EXPECT_FALSE(nevr_early_quit_lockout::ParseUpdate(nullptr, nevr_early_quit_lockout::kUpdateSize, &update));
}

// lockoutcountdownactive (hard-coded 0 in the game) reads true exactly while seconds remain.
TEST(EarlyQuitLockout, CountdownActiveWhileSecondsRemain) {
  EXPECT_TRUE(nevr_early_quit_lockout::CountdownActive(1));
  EXPECT_TRUE(nevr_early_quit_lockout::CountdownActive(900));
  EXPECT_FALSE(nevr_early_quit_lockout::CountdownActive(0));
  EXPECT_FALSE(nevr_early_quit_lockout::CountdownActive(-5));
}

// Legacy layout (nakama main): the end is in the stamp field and +0x20 is zero, so the future stamp is the end.
TEST(EarlyQuitLockout, LegacyFutureEndStartsTheCountdownToIt) {
  GameSetter game;
  const Countdown c = game.Receive(kNow + 600, 0, kNow);
  EXPECT_TRUE(c.applied);
  EXPECT_EQ(c.expiry, kNow + 600);
  EXPECT_TRUE(c.active);
}

// The stamp-and-expiry layout (nakama branch feat/712), with a future expiry at +0x20 and the stamp about now.
TEST(EarlyQuitLockout, Layout712FutureExpiryStartsTheCountdownToIt) {
  GameSetter game;
  const Countdown c = game.Receive(kNow, kNow + 600, kNow);
  EXPECT_TRUE(c.applied);
  EXPECT_EQ(c.expiry, kNow + 600);
  EXPECT_TRUE(c.active);
}

// A legacy end already over starts no countdown, and the expiry the hook writes is -1.
TEST(EarlyQuitLockout, LegacyExpiredEndLeavesNoCountdown) {
  GameSetter game;
  const Countdown c = game.Receive(kNow - 1, 0, kNow);
  EXPECT_TRUE(c.applied);
  EXPECT_EQ(c.expiry, -1);
  EXPECT_FALSE(c.active);
}

// A stamp-and-expiry update whose expiry is already over starts no countdown.
TEST(EarlyQuitLockout, Layout712ExpiredExpiryLeavesNoCountdown) {
  GameSetter game;
  const Countdown c = game.Receive(kNow, kNow - 1, kNow);
  EXPECT_TRUE(c.applied);
  EXPECT_EQ(c.expiry, -1);
  EXPECT_FALSE(c.active);
}

// A legacy clear (stamp 0) ends an active lockout: the gate is opened, so the setter stores 0.
TEST(EarlyQuitLockout, LegacyClearEndsAnActiveLockout) {
  GameSetter game;
  ASSERT_TRUE(game.Receive(kNow + 600, 0, kNow).active);
  const Countdown c = game.Receive(0, 0, kNow + 10);
  EXPECT_TRUE(c.applied);
  EXPECT_EQ(c.expiry, -1);
  EXPECT_FALSE(c.active);
  EXPECT_EQ(game.stored, 0);
}

// A stamp-and-expiry clear (expiry 0 after an active lockout, stamp newer) ends the lockout.
TEST(EarlyQuitLockout, Layout712ClearEndsAnActiveLockout) {
  GameSetter game;
  ASSERT_TRUE(game.Receive(kNow, kNow + 600, kNow).active);
  const Countdown c = game.Receive(kNow + 1, 0, kNow + 1);
  EXPECT_TRUE(c.applied);
  EXPECT_EQ(c.expiry, -1);
  EXPECT_FALSE(c.active);
}

// A shorter legacy lockout replaces a longer one, as the stamp-and-expiry layout's shorter expiry does.
TEST(EarlyQuitLockout, LegacyShorterLockoutReplacesALongerOne) {
  GameSetter game;
  ASSERT_EQ(game.Receive(kNow + 600, 0, kNow).expiry, kNow + 600);
  const Countdown c = game.Receive(kNow + 120, 0, kNow + 1);
  EXPECT_TRUE(c.applied);
  EXPECT_EQ(c.expiry, kNow + 120);
  EXPECT_TRUE(c.active);
}

// Proves a legacy update is not blocked by a stored stamp equal to its end: the login call stores that stamp, and
// the legacy update for the same end still starts the countdown because the hook opens the gate for it.
TEST(EarlyQuitLockout, LegacyUpdateAfterTheLoginCallStartsTheCountdown) {
  GameSetter game;
  EXPECT_FALSE(game.Receive(kNow + 600, -1, kNow).active);
  const Countdown c = game.Receive(kNow + 600, 0, kNow);
  EXPECT_TRUE(c.applied);
  EXPECT_EQ(c.expiry, kNow + 600);
  EXPECT_TRUE(c.active);
}

// A stamp-and-expiry clear whose stamp is 10 s ahead of the client clock is not read as a legacy lockout (the
// slack is 30 s), so it clears the countdown. The login path's -1 is never read as legacy either.
TEST(EarlyQuitLockout, ClockSkewWithinTheSlackIsNotALegacyLockout) {
  GameSetter game;
  ASSERT_TRUE(game.Receive(kNow, kNow + 600, kNow).active);
  const Countdown c = game.Receive(kNow + 10, 0, kNow);
  EXPECT_TRUE(c.applied);
  EXPECT_FALSE(c.active);
  EXPECT_FALSE(nevr_early_quit_lockout::ReadIntake(kNow + 600, -1, kNow).legacy);
}

// The golden legacy update, byte for byte: the end 1'790'975'600 is in the stamp field and +0x20 is zero.
TEST(EarlyQuitLockout, TheGoldenLegacyUpdateParsesToTheEndInTheStamp) {
  const std::vector<std::uint8_t> bytes =
      FromHex("000000000000000000000000000000008877665544332211" "701ec06a00000000" "0000000000000000"
              "03000000110000000200000002010000");
  ASSERT_EQ(bytes.size(), nevr_early_quit_lockout::kUpdateSize);
  nevr_early_quit_lockout::Update update{};
  ASSERT_TRUE(nevr_early_quit_lockout::ParseUpdate(bytes.data(), bytes.size(), &update));
  EXPECT_EQ(update.stamp, 1'790'975'600);
  EXPECT_EQ(update.expiry, 0);
  GameSetter game;
  const Countdown c = game.Receive(update.stamp, update.expiry, kNow);
  EXPECT_TRUE(c.applied);
  EXPECT_EQ(c.expiry, 1'790'975'600);
  EXPECT_TRUE(c.active);
}
