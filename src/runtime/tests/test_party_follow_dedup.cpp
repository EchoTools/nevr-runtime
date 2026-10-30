// The party-follow dedup (#1413): the state the bridge keeps about the lobby this client is in or joining, stamped with
// time, and the decision the JoinPartyLobby hook takes from it (patch/lobby_full_backoff.cpp). Both are pure; time comes
// from an injected fake clock, so these tests drive the real code with no game, no network and no sleep.
#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "runtime/compat/lobby_join_state.h"
#include "runtime/patch/party_follow_dedup_rules.h"

namespace {

using nevr_lobby_join_state::LobbyJoinState;
using nevr_lobby_join_state::Stamp;
using nevr_party_follow_dedup::Decide;
using nevr_party_follow_dedup::kFollowDedupWindowMs;
using nevr_party_follow_dedup::Skip;

using Guid = std::array<uint8_t, 16>;

Guid MakeGuid(uint8_t seed) {
  Guid g{};
  for (std::size_t i = 0; i < g.size(); ++i) g[i] = static_cast<uint8_t>(seed + i);
  return g;
}

const Guid kNil{};
const Guid kX = MakeGuid(0x10);
const Guid kY = MakeGuid(0x90);

std::uint64_t g_fakeNowMs = 0;
std::uint64_t FakeNowMs() { return g_fakeNowMs; }

// Snapshot then decide, at the fake clock's current time.
nevr_party_follow_dedup::Verdict DecideNow(LobbyJoinState& state, const Guid& leader) {
  Stamp current{}, joining{};
  state.Snapshot(&current, &joining);
  return Decide(leader, current, joining, g_fakeNowMs);
}

}  // namespace

TEST(PartyFollowDedupState, ASuccessSetsTheCurrentLobbyAndAJoinRequestSetsTheJoiningOne) {
  g_fakeNowMs = 1000;
  LobbyJoinState state(&FakeNowMs);
  state.OnSuccess(kX);
  Stamp current{}, joining{};
  state.Snapshot(&current, &joining);
  EXPECT_EQ(current.lobby, kX);
  EXPECT_EQ(current.atMs, 1000u);
  EXPECT_EQ(joining.lobby, kNil);

  g_fakeNowMs = 1500;
  state.OnJoinRequested(kY);
  state.Snapshot(&current, &joining);
  EXPECT_EQ(joining.lobby, kY);
  EXPECT_EQ(joining.atMs, 1500u);
}

TEST(PartyFollowDedupState, ASuccessClearsTheJoiningLobbyItAnswers) {
  LobbyJoinState state(&FakeNowMs);
  state.OnJoinRequested(kX);
  state.OnSuccess(kX);
  Stamp current{}, joining{};
  state.Snapshot(&current, &joining);
  EXPECT_EQ(current.lobby, kX);
  EXPECT_EQ(joining.lobby, kNil);
}

TEST(PartyFollowDedupState, AFailureThatAnswersAJoinClearsTheJoiningLobby) {
  LobbyJoinState state(&FakeNowMs);
  state.OnJoinRequested(kX);
  state.OnFailure(/*answersJoin=*/true);
  Stamp current{}, joining{};
  state.Snapshot(&current, &joining);
  EXPECT_EQ(joining.lobby, kNil);
}

TEST(PartyFollowDedupState, AFailureClearsTheCurrentLobby) {
  LobbyJoinState state(&FakeNowMs);
  state.OnSuccess(kX);
  state.OnFailure(/*answersJoin=*/false);
  Stamp current{}, joining{};
  state.Snapshot(&current, &joining);
  EXPECT_EQ(current.lobby, kNil);
}

TEST(PartyFollowDedupState, AFailureThatAnswersAFindLeavesTheJoiningLobbyAlone) {
  LobbyJoinState state(&FakeNowMs);
  state.OnJoinRequested(kX);
  state.OnFailure(/*answersJoin=*/false);
  Stamp current{}, joining{};
  state.Snapshot(&current, &joining);
  EXPECT_EQ(joining.lobby, kX);
}

// Inside the window: a follow of the lobby the client is in is skipped, and the age is reported.
TEST(PartyFollowDedupDecide, ASuccessThenAFollowInsideTheWindowIsSkippedAsAlreadyIn) {
  g_fakeNowMs = 10000;
  LobbyJoinState state(&FakeNowMs);
  state.OnSuccess(kX);
  g_fakeNowMs = 10000 + kFollowDedupWindowMs - 1;
  const auto verdict = DecideNow(state, kX);
  EXPECT_EQ(verdict.skip, Skip::AlreadyIn);
  EXPECT_EQ(verdict.ageMs, kFollowDedupWindowMs - 1);
}

// Inside the window: a follow of the lobby the client is joining is skipped.
TEST(PartyFollowDedupDecide, AJoinRequestInsideTheWindowIsSkippedAsAlreadyJoining) {
  g_fakeNowMs = 20000;
  LobbyJoinState state(&FakeNowMs);
  state.OnJoinRequested(kX);
  g_fakeNowMs = 20000 + 12000;
  const auto verdict = DecideNow(state, kX);
  EXPECT_EQ(verdict.skip, Skip::AlreadyJoining);
  EXPECT_EQ(verdict.ageMs, 12000u);
}

// The double join in the logs follows a success within seconds: a join request, then its success, then the follow.
TEST(PartyFollowDedupDecide, AJoinThenItsSuccessThenAFollowInsideTheWindowIsSkipped) {
  g_fakeNowMs = 30000;
  LobbyJoinState state(&FakeNowMs);
  state.OnJoinRequested(kX);
  g_fakeNowMs = 30007;
  state.OnSuccess(kX);
  g_fakeNowMs = 30012;
  EXPECT_EQ(DecideNow(state, kX).skip, Skip::AlreadyIn);
}

// At and after the window: the same lobby goes through, so a player who left it can rejoin.
TEST(PartyFollowDedupDecide, ASuccessThenAFollowAtTheWindowGoesThrough) {
  g_fakeNowMs = 10000;
  LobbyJoinState state(&FakeNowMs);
  state.OnSuccess(kX);
  g_fakeNowMs = 10000 + kFollowDedupWindowMs;
  EXPECT_EQ(DecideNow(state, kX).skip, Skip::None);
}

TEST(PartyFollowDedupDecide, AJoinRequestAfterTheWindowGoesThrough) {
  g_fakeNowMs = 10000;
  LobbyJoinState state(&FakeNowMs);
  state.OnJoinRequested(kX);
  g_fakeNowMs = 10000 + kFollowDedupWindowMs + 5000;
  EXPECT_EQ(DecideNow(state, kX).skip, Skip::None);
}

// A failure clears the state at once: the follow right after it goes through, with no window to wait out.
TEST(PartyFollowDedupDecide, ASuccessThenAFailureThenAFollowGoesThroughAtOnce) {
  g_fakeNowMs = 40000;
  LobbyJoinState state(&FakeNowMs);
  state.OnSuccess(kX);
  g_fakeNowMs = 40001;
  state.OnFailure(/*answersJoin=*/false);
  EXPECT_EQ(DecideNow(state, kX).skip, Skip::None);
}

TEST(PartyFollowDedupDecide, AJoinThenAFailureThatAnswersItThenAFollowGoesThroughAtOnce) {
  g_fakeNowMs = 50000;
  LobbyJoinState state(&FakeNowMs);
  state.OnJoinRequested(kX);
  g_fakeNowMs = 50001;
  state.OnFailure(/*answersJoin=*/true);
  EXPECT_EQ(DecideNow(state, kX).skip, Skip::None);
}

TEST(PartyFollowDedupDecide, ADifferentLobbyGoesThroughInsideTheWindow) {
  g_fakeNowMs = 60000;
  LobbyJoinState state(&FakeNowMs);
  state.OnSuccess(kX);
  state.OnJoinRequested(kX);
  EXPECT_EQ(DecideNow(state, kY).skip, Skip::None);
}

TEST(PartyFollowDedupDecide, ANilLeaderLobbyGoesThroughToTheGamesOwnLogic) {
  LobbyJoinState state(&FakeNowMs);
  state.OnSuccess(kX);
  EXPECT_EQ(DecideNow(state, kNil).skip, Skip::None);
}

TEST(PartyFollowDedupDecide, ANilCurrentOrJoiningNeverMatchesAProperLeader) {
  const Stamp nilStamp{};
  EXPECT_EQ(Decide(kX, nilStamp, nilStamp, 0).skip, Skip::None);
}

// A clock that reads earlier than a stamp (a clock that was reset) must not wrap the age into a skip.
TEST(PartyFollowDedupDecide, AStampFromTheFutureIsTreatedAsAgeZero) {
  const Stamp future{kX, 5000};
  const Stamp nilStamp{};
  const auto verdict = Decide(kX, future, nilStamp, 1000);
  EXPECT_EQ(verdict.skip, Skip::AlreadyIn);
  EXPECT_EQ(verdict.ageMs, 0u);
}
