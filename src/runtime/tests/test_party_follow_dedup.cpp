// The party-follow dedup (#1413): the state the bridge keeps about the lobby this client is in or joining, stamped with
// time, and the decision the JoinPartyLobby hook takes from it (patch/lobby_full_backoff.cpp). Both are pure; time comes
// from an injected fake clock, so these tests drive the real code with no game, no network and no sleep.
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdio>

#include "runtime/compat/lobby_frame.h"
#include "runtime/compat/lobby_join_state.h"
#include "runtime/patch/party_follow_dedup_rules.h"

namespace {

using nevr_lobby_join_state::LobbyJoinState;
using nevr_lobby_join_state::Stamp;
using nevr_party_follow_dedup::Decide;
using nevr_party_follow_dedup::kFollowDedupWindowMs;
using nevr_party_follow_dedup::NoteNoSkip;
using nevr_party_follow_dedup::NoteSkip;
using nevr_party_follow_dedup::Skip;
using nevr_party_follow_dedup::SkipLogState;

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

// The leader's GUID comes from the game's own text parse (the JSON "lobbyid" path of GetPartyLobbyId, 0x14017c6a0), which
// scans the text with "%08X-%04hX-%04hX-%02hhX%02hhX-%02hhX...". The lobby on the wire is the SUuid copied byte for byte
// (RequestJoinSession, 0x140612600). Both must be the Windows binary GUID layout for the skip to match (#1413).
Guid ParseGameGuidText(const char* text) {
  unsigned int d1 = 0, d2 = 0, d3 = 0;
  unsigned int d4[8] = {};
  const int n = std::sscanf(text, "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", &d1, &d2, &d3, &d4[0], &d4[1],
                            &d4[2], &d4[3], &d4[4], &d4[5], &d4[6], &d4[7]);
  EXPECT_EQ(n, 11);
  // The game's struct is native (x86, little endian): Data1, Data2, Data3 little endian, Data4 as bytes.
  std::array<uint8_t, 16> out{};
  out[0] = static_cast<uint8_t>(d1);
  out[1] = static_cast<uint8_t>(d1 >> 8);
  out[2] = static_cast<uint8_t>(d1 >> 16);
  out[3] = static_cast<uint8_t>(d1 >> 24);
  out[4] = static_cast<uint8_t>(d2);
  out[5] = static_cast<uint8_t>(d2 >> 8);
  out[6] = static_cast<uint8_t>(d3);
  out[7] = static_cast<uint8_t>(d3 >> 8);
  for (std::size_t i = 0; i < 8; ++i) out[8 + i] = static_cast<uint8_t>(d4[i]);
  return out;
}

TEST(PartyFollowDedupGuidLayout, TheGamesTextAndTheWireBytesAreTheSameGuid) {
  const char* text = "3D2C1B0A-5F4E-7160-8293-A4B5C6D7E8F9";
  const Guid leader = ParseGameGuidText(text);
  const std::array<uint8_t, 16> expected = {0x0A, 0x1B, 0x2C, 0x3D, 0x4E, 0x5F, 0x60, 0x71,
                                            0x82, 0x93, 0xA4, 0xB5, 0xC6, 0xD7, 0xE8, 0xF9};
  EXPECT_EQ(leader, expected);
  // The wire carries the same 16 bytes; the bridge reads them with ReadGuid and prints them with FormatGuid.
  const Guid wire = nevr_lobby_frame::detail::ReadGuid(expected.data());
  EXPECT_EQ(wire, leader);
  EXPECT_EQ(nevr_lobby_frame::FormatGuid(wire), std::string(text));
}

TEST(PartyFollowDedupGuidLayout, ASecondKnownGuidReadsTheSameWayOnBothSides) {
  const char* text = "00112233-4455-6677-8899-AABBCCDDEEFF";
  const Guid leader = ParseGameGuidText(text);
  const std::array<uint8_t, 16> expected = {0x33, 0x22, 0x11, 0x00, 0x55, 0x44, 0x77, 0x66,
                                            0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  EXPECT_EQ(leader, expected);
  EXPECT_EQ(nevr_lobby_frame::detail::ReadGuid(expected.data()), leader);
}

// The layouts agree, so a lobby the bridge recorded from a success frame matches the leader's lobby in the same layout.
TEST(PartyFollowDedupGuidLayout, ASuccessFrameLobbyMatchesTheLeadersLobbyParsedFromText) {
  const char* text = "3D2C1B0A-5F4E-7160-8293-A4B5C6D7E8F9";
  const std::array<uint8_t, 16> wireBytes = {0x0A, 0x1B, 0x2C, 0x3D, 0x4E, 0x5F, 0x60, 0x71,
                                             0x82, 0x93, 0xA4, 0xB5, 0xC6, 0xD7, 0xE8, 0xF9};
  LobbyJoinState state(&FakeNowMs);
  g_fakeNowMs = 70000;
  state.OnSuccess(nevr_lobby_frame::detail::ReadGuid(wireBytes.data()));
  Stamp current{}, joining{};
  state.Snapshot(&current, &joining);
  EXPECT_EQ(Decide(ParseGameGuidText(text), current, joining, g_fakeNowMs).skip, Skip::AlreadyIn);
}

// The Info line: one per run of identical skips, with the repeats counted (FollowParty queues the join every tick).
TEST(PartyFollowDedupLogRun, TwoConsecutiveSkipsOfTheSameLobbyWriteOneInfoLine) {
  SkipLogState log;
  EXPECT_TRUE(NoteSkip(log, kX, Skip::AlreadyIn).writeInfo);
  EXPECT_FALSE(NoteSkip(log, kX, Skip::AlreadyIn).writeInfo);
}

TEST(PartyFollowDedupLogRun, ADifferentLobbyWritesAgainAndCarriesTheRepeats) {
  SkipLogState log;
  NoteSkip(log, kX, Skip::AlreadyIn);
  NoteSkip(log, kX, Skip::AlreadyIn);
  NoteSkip(log, kX, Skip::AlreadyIn);
  const auto decision = NoteSkip(log, kY, Skip::AlreadyIn);
  EXPECT_TRUE(decision.writeInfo);
  EXPECT_EQ(decision.repeats, 2u);
}

TEST(PartyFollowDedupLogRun, ADifferentReasonForTheSameLobbyWritesAgain) {
  SkipLogState log;
  NoteSkip(log, kX, Skip::AlreadyIn);
  EXPECT_TRUE(NoteSkip(log, kX, Skip::AlreadyJoining).writeInfo);
}

TEST(PartyFollowDedupLogRun, AFollowThatIsNotSkippedEndsTheRunSoTheNextSkipWrites) {
  SkipLogState log;
  NoteSkip(log, kX, Skip::AlreadyIn);
  NoteSkip(log, kX, Skip::AlreadyIn);
  NoteNoSkip(log);
  const auto decision = NoteSkip(log, kX, Skip::AlreadyIn);
  EXPECT_TRUE(decision.writeInfo);
  EXPECT_EQ(decision.repeats, 1u);
}
