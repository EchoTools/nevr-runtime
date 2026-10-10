// #58: the empty-server TTL decision.

#include <gtest/gtest.h>

#include <string>

#include "runtime/lifecycle/return_to_lobby_hold.h"

namespace {

using nevr_return_to_lobby_hold::PollVerdict;
using nevr_return_to_lobby_hold::Policy;
using nevr_return_to_lobby_hold::RequestVerdict;

constexpr uint64_t kTtl = 20ULL * 60ULL * 1000ULL;

Policy WithTtl(uint64_t ttlMs) {
  Policy policy;
  policy.SetTtlMs(ttlMs);
  return policy;
}

TEST(ReturnToLobbyHold, TtlParsesWholeSecondsAndRejectsTheRest) {
  std::string problem;
  EXPECT_EQ(nevr_return_to_lobby_hold::ParseTtlSeconds(nullptr, &problem), 0U);
  EXPECT_TRUE(problem.empty());
  EXPECT_EQ(nevr_return_to_lobby_hold::ParseTtlSeconds("", &problem), 0U);
  EXPECT_TRUE(problem.empty());
  EXPECT_EQ(nevr_return_to_lobby_hold::ParseTtlSeconds("1200", &problem), 1200U);
  EXPECT_TRUE(problem.empty());
  EXPECT_EQ(nevr_return_to_lobby_hold::ParseTtlSeconds("0", &problem), 0U);
  EXPECT_EQ(nevr_return_to_lobby_hold::ParseTtlSeconds("20m", &problem), 0U);
  EXPECT_FALSE(problem.empty());
  EXPECT_EQ(nevr_return_to_lobby_hold::ParseTtlSeconds("-5", &problem), 0U);
  EXPECT_FALSE(problem.empty());
  EXPECT_EQ(nevr_return_to_lobby_hold::ParseTtlSeconds("1.5", &problem), 0U);
  EXPECT_FALSE(problem.empty());
  EXPECT_EQ(nevr_return_to_lobby_hold::ParseTtlSeconds("86400", &problem), 86400U);
  EXPECT_TRUE(problem.empty());
  EXPECT_EQ(nevr_return_to_lobby_hold::ParseTtlSeconds("86401", &problem), nevr_return_to_lobby_hold::kMaxTtlSeconds);
  EXPECT_FALSE(problem.empty());
  EXPECT_EQ(nevr_return_to_lobby_hold::ParseTtlSeconds("99999999999999999999999", nullptr), nevr_return_to_lobby_hold::kMaxTtlSeconds);
}

TEST(ReturnToLobbyHold, ZeroTtlNeverHolds) {
  Policy policy = WithTtl(0);
  EXPECT_EQ(policy.OnReturnRequested(1000, 0, false), RequestVerdict::Proceed);
  EXPECT_FALSE(policy.Holding());
  EXPECT_EQ(policy.Poll(1000000, 0, false), PollVerdict::Keep);
}

TEST(ReturnToLobbyHold, AnEmptySessionIsHeldUntilTheTtlThenReleased) {
  Policy policy = WithTtl(kTtl);
  EXPECT_EQ(policy.OnReturnRequested(5000, 0, false), RequestVerdict::Hold);
  EXPECT_TRUE(policy.Holding());
  EXPECT_EQ(policy.HeldSinceMs(), 5000U);
  EXPECT_EQ(policy.Poll(5000 + kTtl - 1, 0, false), PollVerdict::Keep);
  EXPECT_EQ(policy.Poll(5000 + kTtl, 0, false), PollVerdict::Release);
  EXPECT_FALSE(policy.Holding());
  EXPECT_EQ(policy.Poll(5000 + kTtl + 1, 0, false), PollVerdict::Keep) << "nothing is held after the release";
}

TEST(ReturnToLobbyHold, RepeatedRequestsDoNotRestartTheClock) {
  Policy policy = WithTtl(kTtl);
  EXPECT_EQ(policy.OnReturnRequested(1000, 0, false), RequestVerdict::Hold);
  EXPECT_EQ(policy.OnReturnRequested(900000, 0, false), RequestVerdict::HoldAgain);
  EXPECT_EQ(policy.HeldSinceMs(), 1000U);
  EXPECT_EQ(policy.Poll(1000 + kTtl, 0, false), PollVerdict::Release);
}

// The game re-issues the request every tick while a session is empty (CR15NetDedicatedLobby
// vslot[1] in state 0xb), so only the first request of a hold may be reported as a new hold.
TEST(ReturnToLobbyHold, OnlyTheFirstRequestOfAHoldIsNew) {
  Policy policy = WithTtl(kTtl);
  EXPECT_EQ(policy.OnReturnRequested(1000, 0, false), RequestVerdict::Hold);
  for (int tick = 1; tick <= 100; ++tick) {
    EXPECT_EQ(policy.OnReturnRequested(1000 + tick, 0, false), RequestVerdict::HoldAgain);
  }
  EXPECT_EQ(policy.HeldRequests(), 101U);
  ASSERT_EQ(policy.Poll(1000 + kTtl, 0, false), PollVerdict::Release);
  EXPECT_EQ(policy.OnReturnRequested(5000000, 0, false), RequestVerdict::Proceed);
}

// After the TTL releases the return, the game's queued callback has not run yet and the next tick
// asks again: that must reach the game, not start a second TTL.
TEST(ReturnToLobbyHold, ARequestAfterTheReleaseProceedsInsteadOfStartingAFreshHold) {
  Policy policy = WithTtl(kTtl);
  ASSERT_EQ(policy.OnReturnRequested(1000, 0, false), RequestVerdict::Hold);
  ASSERT_EQ(policy.Poll(1000 + kTtl, 0, false), PollVerdict::Release);
  EXPECT_EQ(policy.OnReturnRequested(1000 + kTtl + 16, 0, false), RequestVerdict::Proceed);
  EXPECT_FALSE(policy.Holding());
  EXPECT_EQ(policy.OnReturnRequested(1000 + kTtl + 32, 0, false), RequestVerdict::Proceed);
}

TEST(ReturnToLobbyHold, ThePostReleaseLatchEndsOncePlayersAreSeenAgain) {
  Policy policy = WithTtl(kTtl);
  ASSERT_EQ(policy.OnReturnRequested(1000, 0, false), RequestVerdict::Hold);
  ASSERT_EQ(policy.Poll(1000 + kTtl, 0, false), PollVerdict::Release);
  // Latched: a later empty session is not held again while no player has been seen.
  EXPECT_EQ(policy.OnReturnRequested(2000000, 0, false), RequestVerdict::Proceed);
  EXPECT_EQ(policy.Poll(2000010, 0, false), PollVerdict::Keep);
  EXPECT_EQ(policy.OnReturnRequested(3000000, 0, false), RequestVerdict::Proceed) << "still latched";
  // A player is seen (by a Poll, or by a request): the latch ends.
  EXPECT_EQ(policy.Poll(3000010, 2, false), PollVerdict::Keep);
  EXPECT_EQ(policy.OnReturnRequested(9000000, 0, false), RequestVerdict::Hold) << "a new empty session holds again";
  EXPECT_EQ(policy.HeldSinceMs(), 9000000U);
}

TEST(ReturnToLobbyHold, ARequestWithPlayersAlsoEndsTheLatch) {
  Policy policy = WithTtl(kTtl);
  ASSERT_EQ(policy.OnReturnRequested(1000, 0, false), RequestVerdict::Hold);
  ASSERT_EQ(policy.Poll(1000 + kTtl, 0, false), PollVerdict::Release);
  EXPECT_EQ(policy.OnReturnRequested(2000000, 0, false), RequestVerdict::Proceed);
  EXPECT_EQ(policy.OnReturnRequested(2000010, 1, false), RequestVerdict::Proceed);
  EXPECT_EQ(policy.OnReturnRequested(2000020, 0, false), RequestVerdict::Hold);
}

// At TTL 0 (the default) the per-frame poll must not even count entrants.
TEST(ReturnToLobbyHold, PollAtZeroTtlCallsNothing) {
  Policy policy = WithTtl(0);
  int counted = 0;
  int shutdownChecks = 0;
  for (int frame = 0; frame < 1000; ++frame) {
    EXPECT_EQ(nevr_return_to_lobby_hold::PollIfActive(
                  policy, frame, [&] { ++counted; return uint64_t{0}; }, [&] { ++shutdownChecks; return false; }),
              PollVerdict::Keep);
  }
  EXPECT_EQ(counted, 0);
  EXPECT_EQ(shutdownChecks, 0);
}

TEST(ReturnToLobbyHold, PollWithATtlStillDecides) {
  Policy policy = WithTtl(kTtl);
  ASSERT_EQ(policy.OnReturnRequested(1000, 0, false), RequestVerdict::Hold);
  int counted = 0;
  const auto count = [&] { ++counted; return uint64_t{0}; };
  const auto noShutdown = [] { return false; };
  EXPECT_EQ(nevr_return_to_lobby_hold::PollIfActive(policy, 2000, count, noShutdown), PollVerdict::Keep);
  EXPECT_EQ(nevr_return_to_lobby_hold::PollIfActive(policy, 1000 + kTtl, count, noShutdown), PollVerdict::Release);
  EXPECT_EQ(counted, 2);
}

TEST(ReturnToLobbyHold, APlayerJoiningCancelsTheHold) {
  Policy policy = WithTtl(kTtl);
  ASSERT_EQ(policy.OnReturnRequested(1000, 0, false), RequestVerdict::Hold);
  EXPECT_EQ(policy.Poll(2000, 1, false), PollVerdict::Cancel);
  EXPECT_FALSE(policy.Holding());
  // A later empty session starts a fresh hold with a fresh clock.
  EXPECT_EQ(policy.OnReturnRequested(50000, 0, false), RequestVerdict::Hold);
  EXPECT_EQ(policy.HeldSinceMs(), 50000U);
}

TEST(ReturnToLobbyHold, AShutdownIsNeverHeldAndCancelsAHold) {
  Policy policy = WithTtl(kTtl);
  EXPECT_EQ(policy.OnReturnRequested(1000, 0, true), RequestVerdict::Proceed);
  EXPECT_FALSE(policy.Holding());
  ASSERT_EQ(policy.OnReturnRequested(1000, 0, false), RequestVerdict::Hold);
  EXPECT_EQ(policy.Poll(1500, 0, true), PollVerdict::Cancel);
  EXPECT_FALSE(policy.Holding());
}

TEST(ReturnToLobbyHold, ASessionWithPlayersEndsNormally) {
  Policy policy = WithTtl(kTtl);
  EXPECT_EQ(policy.OnReturnRequested(1000, 3, false), RequestVerdict::Proceed);
  EXPECT_FALSE(policy.Holding());
}

}  // namespace
