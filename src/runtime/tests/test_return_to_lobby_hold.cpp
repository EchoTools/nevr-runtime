// #58: the empty-server TTL decision.

#include <gtest/gtest.h>

#include <string>

#include "runtime/lifecycle/return_to_lobby_hold.h"

namespace {

using ReturnToLobbyHold::PollVerdict;
using ReturnToLobbyHold::Policy;
using ReturnToLobbyHold::RequestVerdict;

constexpr uint64_t kTtl = 20ULL * 60ULL * 1000ULL;

Policy WithTtl(uint64_t ttlMs) {
  Policy policy;
  policy.SetTtlMs(ttlMs);
  return policy;
}

TEST(ReturnToLobbyHold, TtlParsesWholeSecondsAndRejectsTheRest) {
  std::string problem;
  EXPECT_EQ(ReturnToLobbyHold::ParseTtlSeconds(nullptr, &problem), 0U);
  EXPECT_TRUE(problem.empty());
  EXPECT_EQ(ReturnToLobbyHold::ParseTtlSeconds("", &problem), 0U);
  EXPECT_TRUE(problem.empty());
  EXPECT_EQ(ReturnToLobbyHold::ParseTtlSeconds("1200", &problem), 1200U);
  EXPECT_TRUE(problem.empty());
  EXPECT_EQ(ReturnToLobbyHold::ParseTtlSeconds("0", &problem), 0U);
  EXPECT_EQ(ReturnToLobbyHold::ParseTtlSeconds("20m", &problem), 0U);
  EXPECT_FALSE(problem.empty());
  EXPECT_EQ(ReturnToLobbyHold::ParseTtlSeconds("-5", &problem), 0U);
  EXPECT_FALSE(problem.empty());
  EXPECT_EQ(ReturnToLobbyHold::ParseTtlSeconds("1.5", &problem), 0U);
  EXPECT_FALSE(problem.empty());
  EXPECT_EQ(ReturnToLobbyHold::ParseTtlSeconds("86400", &problem), 86400U);
  EXPECT_TRUE(problem.empty());
  EXPECT_EQ(ReturnToLobbyHold::ParseTtlSeconds("86401", &problem), ReturnToLobbyHold::kMaxTtlSeconds);
  EXPECT_FALSE(problem.empty());
  EXPECT_EQ(ReturnToLobbyHold::ParseTtlSeconds("99999999999999999999999", nullptr), ReturnToLobbyHold::kMaxTtlSeconds);
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
  EXPECT_EQ(policy.Poll(1000 + kTtl + 5, 2, false), PollVerdict::Keep);
  EXPECT_EQ(policy.OnReturnRequested(9000000, 0, false), RequestVerdict::Hold) << "a new empty session holds again";
  EXPECT_EQ(policy.HeldSinceMs(), 9000000U);
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
