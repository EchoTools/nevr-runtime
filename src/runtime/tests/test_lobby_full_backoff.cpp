// The party-follow join refused as "lobby full" (#1011): the decisions of the backoff, apart from the hooks that
// apply them (patch/lobby_full_backoff.cpp). The game re-queues JoinPartyLobby every frame while the follow data
// names a lobby, so the wait sequence, the give-up and the cooldown are what bound the sends.
#include <gtest/gtest.h>

#include <cstdint>

#include "runtime/patch/lobby_full_backoff_rules.h"

namespace {

using nevr_lobby_full_backoff::Policy;
using nevr_lobby_full_backoff::Refusal;

// Simulates a player who keeps following: the game asks every 100 ms, and each send that is allowed is refused.
// Returns the number of sends in [0, windowMs].
int SendsRefusedEveryTime(std::uint64_t windowMs) {
  Policy policy;
  int sends = 0;
  for (std::uint64_t now = 0; now <= windowMs; now += 100) {
    if (policy.MaySend(now)) {
      ++sends;
      policy.OnLobbyFull(now);
    }
  }
  return sends;
}

}  // namespace

TEST(LobbyFullBackoff, FirstJoinIsSentAtOnce) {
  Policy policy;
  EXPECT_TRUE(policy.MaySend(0));
  EXPECT_EQ(policy.refusals(), 0);
}

TEST(LobbyFullBackoff, WaitDoublesFromOneSecondAndStopsAtThirtySeconds) {
  Policy policy;
  const std::uint64_t expectedMs[] = {1000, 2000, 4000, 8000, 16000, 30000};
  std::uint64_t now = 0;
  for (int k = 1; k <= 6; ++k) {
    const Refusal r = policy.OnLobbyFull(now);
    EXPECT_EQ(r.count, k);
    EXPECT_FALSE(r.gaveUp);
    EXPECT_EQ(r.delayMs, expectedMs[k - 1]) << "refusal " << k;
    EXPECT_FALSE(policy.MaySend(now + r.delayMs - 1)) << "refusal " << k << " must hold its full wait";
    EXPECT_TRUE(policy.MaySend(now + r.delayMs)) << "refusal " << k << " must release at its wait";
    now += r.delayMs;
  }
}

TEST(LobbyFullBackoff, SeventhRefusalGivesUpForACooldown) {
  Policy policy;
  std::uint64_t now = 0;
  for (int k = 1; k <= 6; ++k) now += policy.OnLobbyFull(now).delayMs;
  const Refusal r = policy.OnLobbyFull(now);
  EXPECT_TRUE(r.gaveUp);
  EXPECT_EQ(r.count, 7);
  EXPECT_EQ(r.delayMs, 60000u);
  EXPECT_FALSE(policy.MaySend(now + 59999));
  EXPECT_TRUE(policy.MaySend(now + 60000));
}

TEST(LobbyFullBackoff, SendsInTheFirstSixtyOneSecondsAreSeven) {
  // Refusals at 0, 1, 3, 7, 15, 31 and 61 s: the seventh is the give-up.
  EXPECT_EQ(SendsRefusedEveryTime(61000), 7);
}

TEST(LobbyFullBackoff, TheCycleRestartsAfterTheCooldown) {
  Policy policy;
  std::uint64_t now = 0;
  for (int k = 1; k <= 7; ++k) {
    const Refusal r = policy.OnLobbyFull(now);
    if (k < 7) now += r.delayMs;
  }
  now += 60000;
  const Refusal again = policy.OnLobbyFull(now);
  EXPECT_EQ(again.count, 1);
  EXPECT_EQ(again.delayMs, 1000u);
  EXPECT_FALSE(again.gaveUp);
}

TEST(LobbyFullBackoff, ASuccessClearsTheWait) {
  Policy policy;
  policy.OnLobbyFull(0);
  policy.OnLobbyFull(1000);
  EXPECT_EQ(policy.refusals(), 2);
  policy.OnSuccess();
  EXPECT_EQ(policy.refusals(), 0);
  EXPECT_TRUE(policy.MaySend(1001));
}
