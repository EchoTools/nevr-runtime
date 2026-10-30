// The party-follow dedup (#1413): the state the bridge keeps about the lobby this client is in or joining, and the
// decision the JoinPartyLobby hook takes from it (patch/lobby_full_backoff.cpp). Both are pure, so these tests
// drive the real code with no game and no network.
#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "runtime/compat/lobby_join_state.h"
#include "runtime/patch/party_follow_dedup_rules.h"

namespace {

using nevr_lobby_join_state::LobbyJoinState;
using nevr_party_follow_dedup::Decide;
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

}  // namespace

TEST(PartyFollowDedupState, ASuccessSetsTheCurrentLobbyAndAJoinRequestSetsTheJoiningOne) {
  LobbyJoinState state;
  state.OnSuccess(kX);
  Guid current{}, joining{};
  state.Snapshot(&current, &joining);
  EXPECT_EQ(current, kX);
  EXPECT_EQ(joining, kNil);

  state.OnJoinRequested(kY);
  state.Snapshot(&current, &joining);
  EXPECT_EQ(joining, kY);
}

TEST(PartyFollowDedupState, ASuccessClearsTheJoiningLobbyItAnswers) {
  LobbyJoinState state;
  state.OnJoinRequested(kX);
  state.OnSuccess(kX);
  Guid current{}, joining{};
  state.Snapshot(&current, &joining);
  EXPECT_EQ(current, kX);
  EXPECT_EQ(joining, kNil);
}

TEST(PartyFollowDedupState, AFailureThatAnswersAJoinClearsTheJoiningLobby) {
  LobbyJoinState state;
  state.OnJoinRequested(kX);
  state.OnFailure(/*answersJoin=*/true);
  Guid current{}, joining{};
  state.Snapshot(&current, &joining);
  EXPECT_EQ(joining, kNil);
}

TEST(PartyFollowDedupState, AFailureClearsTheCurrentLobby) {
  LobbyJoinState state;
  state.OnSuccess(kX);
  state.OnFailure(/*answersJoin=*/false);
  Guid current{}, joining{};
  state.Snapshot(&current, &joining);
  EXPECT_EQ(current, kNil);
}

TEST(PartyFollowDedupState, AFailureThatAnswersAFindLeavesTheJoiningLobbyAlone) {
  LobbyJoinState state;
  state.OnJoinRequested(kX);
  state.OnFailure(/*answersJoin=*/false);
  Guid current{}, joining{};
  state.Snapshot(&current, &joining);
  EXPECT_EQ(joining, kX);
}

TEST(PartyFollowDedupDecide, TheLeadersLobbyThatIsCurrentIsSkippedAsAlreadyIn) {
  LobbyJoinState state;
  state.OnSuccess(kX);
  Guid current{}, joining{};
  state.Snapshot(&current, &joining);
  EXPECT_EQ(Decide(kX, current, joining), Skip::AlreadyIn);
}

TEST(PartyFollowDedupDecide, TheLeadersLobbyThatIsBeingJoinedIsSkippedAsAlreadyJoining) {
  LobbyJoinState state;
  state.OnJoinRequested(kX);
  Guid current{}, joining{};
  state.Snapshot(&current, &joining);
  EXPECT_EQ(Decide(kX, current, joining), Skip::AlreadyJoining);
}

TEST(PartyFollowDedupDecide, ASuccessThenAFollowOfTheSameLobbyIsSkipped) {
  LobbyJoinState state;
  state.OnJoinRequested(kX);
  state.OnSuccess(kX);
  Guid current{}, joining{};
  state.Snapshot(&current, &joining);
  EXPECT_EQ(Decide(kX, current, joining), Skip::AlreadyIn);
}

TEST(PartyFollowDedupDecide, AFailureThenAFollowOfTheSameLobbyGoesThrough) {
  LobbyJoinState state;
  state.OnJoinRequested(kX);
  state.OnFailure(/*answersJoin=*/true);
  Guid current{}, joining{};
  state.Snapshot(&current, &joining);
  EXPECT_EQ(Decide(kX, current, joining), Skip::None);
}

TEST(PartyFollowDedupDecide, ADifferentLobbyGoesThrough) {
  LobbyJoinState state;
  state.OnSuccess(kX);
  state.OnJoinRequested(kX);
  Guid current{}, joining{};
  state.Snapshot(&current, &joining);
  EXPECT_EQ(Decide(kY, current, joining), Skip::None);
}

TEST(PartyFollowDedupDecide, ANilLeaderLobbyGoesThroughToTheGamesOwnLogic) {
  LobbyJoinState state;
  state.OnSuccess(kX);
  Guid current{}, joining{};
  state.Snapshot(&current, &joining);
  EXPECT_EQ(Decide(kNil, current, joining), Skip::None);
}

TEST(PartyFollowDedupDecide, ANilCurrentOrJoiningNeverMatchesAProperLeader) {
  EXPECT_EQ(Decide(kX, kNil, kNil), Skip::None);
}
