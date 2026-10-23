// ServerContext entrant access (issue #38).
//
// The entrant accessors must answer from the lobby's live entrant array, not
// from a copy taken at IServerLib::Initialize. The game calls Initialize from
// CNSLobby LoadServerSupport (echovr.exe 0x14060bb70) while the server is still
// booting, before any player has joined, so a copy taken there is empty for
// the life of the process. Every test below mutates the fake lobby AFTER
// Initialize and asserts the accessors see the mutation.
#include "runtime/server/server_context.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

using Entrant = EchoVR::Lobby::EntrantData;

// Points the fake lobby's entrant HeapArray at `entrants`.
void Attach(EchoVR::Lobby& lobby, std::vector<Entrant>& entrants) {
  lobby.entrantData.items = entrants.empty() ? nullptr : entrants.data();
  lobby.entrantData.count = entrants.size();
}

Entrant MakeEntrant(uint64_t accountId) {
  Entrant entrant{};
  entrant.userId.accountId = accountId;
  return entrant;
}

}  // namespace

TEST(ServerContextEntrants, JoinAfterInitializeIsVisible) {
  EchoVR::Lobby lobby{};
  nevr_game_server::ServerContext context;
  context.Initialize(&lobby, nullptr);
  context.FinalizeInitialization();
  ASSERT_EQ(context.GetEntrantCount(), 0U);

  std::vector<Entrant> entrants = {MakeEntrant(11), MakeEntrant(22), MakeEntrant(33)};
  Attach(lobby, entrants);

  EXPECT_EQ(context.GetEntrantCount(), 3U);
  ASSERT_NE(context.GetEntrant(2), nullptr);
  EXPECT_EQ(context.GetEntrant(2)->userId.accountId, 33U);
}

TEST(ServerContextEntrants, LeaveAfterInitializeIsVisible) {
  EchoVR::Lobby lobby{};
  std::vector<Entrant> entrants = {MakeEntrant(11), MakeEntrant(22), MakeEntrant(33)};
  Attach(lobby, entrants);
  nevr_game_server::ServerContext context;
  context.Initialize(&lobby, nullptr);
  context.FinalizeInitialization();
  ASSERT_EQ(context.GetEntrantCount(), 3U);

  lobby.entrantData.count = 1;

  EXPECT_EQ(context.GetEntrantCount(), 1U);
  EXPECT_EQ(context.GetEntrant(1), nullptr);
  EXPECT_EQ(context.GetEntrant(2), nullptr);
}

TEST(ServerContextEntrants, SlotReusedByAnotherPlayerReportsTheNewPlayer) {
  // The smite path resolves an entrant to a slot index and hands that index to
  // the game. A stale copy here would name the slot's previous occupant.
  EchoVR::Lobby lobby{};
  std::vector<Entrant> entrants = {MakeEntrant(11)};
  Attach(lobby, entrants);
  nevr_game_server::ServerContext context;
  context.Initialize(&lobby, nullptr);
  context.FinalizeInitialization();

  entrants[0].userId.accountId = 99;

  ASSERT_NE(context.GetEntrant(0), nullptr);
  EXPECT_EQ(context.GetEntrant(0)->userId.accountId, 99U);
}

TEST(ServerContextEntrants, ReturnsTheGameElementNotACopy) {
  EchoVR::Lobby lobby{};
  std::vector<Entrant> entrants = {MakeEntrant(11), MakeEntrant(22)};
  Attach(lobby, entrants);
  nevr_game_server::ServerContext context;
  context.Initialize(&lobby, nullptr);
  context.FinalizeInitialization();

  EXPECT_EQ(context.GetEntrant(0), &entrants[0]);
  EXPECT_EQ(context.GetEntrant(1), &entrants[1]);
}

TEST(ServerContextEntrants, ElementStrideMatchesTheBinary) {
  // echovr.exe indexes [lobby+0x360] with a 0xD8 stride: CNSLobby::SmiteEntrant
  // 0x14061665d IMUL RAX,RDX,0xd8; fcn_1406082b0 0x1406082c9 IMUL RDX,RDX,0xd8;
  // 0x14061698f ADD R8,0xd8.
  EchoVR::Lobby lobby{};
  std::vector<Entrant> entrants = {MakeEntrant(1), MakeEntrant(2)};
  Attach(lobby, entrants);
  nevr_game_server::ServerContext context;
  context.Initialize(&lobby, nullptr);
  context.FinalizeInitialization();

  const auto* first = reinterpret_cast<const std::byte*>(context.GetEntrant(0));
  const auto* second = reinterpret_cast<const std::byte*>(context.GetEntrant(1));
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(second - first, 0xD8);
}

TEST(ServerContextEntrants, FreedArrayReadsAsEmpty) {
  // The game frees the array and zeroes items/count together (CNSLobby.cpp:473
  // in echovr-reconstruction). A null items pointer must never be indexed even
  // if count is stale.
  EchoVR::Lobby lobby{};
  std::vector<Entrant> entrants = {MakeEntrant(11)};
  Attach(lobby, entrants);
  nevr_game_server::ServerContext context;
  context.Initialize(&lobby, nullptr);
  context.FinalizeInitialization();

  lobby.entrantData.items = nullptr;

  EXPECT_EQ(context.GetEntrantCount(), 0U);
  EXPECT_EQ(context.GetEntrant(0), nullptr);
}

TEST(ServerContextEntrants, UninitializedAndTerminatedReadAsEmpty) {
  EchoVR::Lobby lobby{};
  std::vector<Entrant> entrants = {MakeEntrant(11)};
  Attach(lobby, entrants);
  nevr_game_server::ServerContext context;

  EXPECT_EQ(context.GetEntrantCount(), 0U);
  EXPECT_EQ(context.GetEntrant(0), nullptr);

  context.Initialize(&lobby, nullptr);
  context.FinalizeInitialization();
  EXPECT_EQ(context.GetEntrantCount(), 1U);

  context.Terminate();
  EXPECT_EQ(context.GetEntrantCount(), 0U);
  EXPECT_EQ(context.GetEntrant(0), nullptr);
}

// Smite entrant resolution (issue #119). The game maps a GUID to an entrant slot through the
// lobby's player-session array (lobby+0xC8, stride 0x28, GUID at +8), not through
// entrant->userId (echovr.exe AcceptPlayersSuccessCBHost 0x140603e20).
namespace {

using Slot = EchoVR::Lobby::PlayerSessionSlot;

GUID MakeGuid(uint8_t seed) {
  GUID guid{};
  guid.Data1 = 0x11110000U + seed;
  guid.Data2 = static_cast<uint16_t>(0x2200 + seed);
  guid.Data3 = static_cast<uint16_t>(0x3300 + seed);
  for (int i = 0; i < 8; i++) guid.Data4[i] = static_cast<uint8_t>(seed + i);
  return guid;
}

constexpr uint64_t kJoinStateAccepted = 4;

// A live (accepted) player session.
Slot MakeSlot(const GUID& guid) {
  Slot slot{};
  slot.guid = guid;
  slot.joinState = kJoinStateAccepted;
  return slot;
}

// What RemoveEntrant leaves behind: the slot reset in place, join state 0.
Slot MakeDepartedSlot(const GUID& guid) {
  Slot slot{};
  slot.guid = guid;
  slot.joinState = 0;
  return slot;
}

void AttachSessions(EchoVR::Lobby& lobby, std::vector<Slot>& sessions) {
  lobby.playerSessions = sessions.empty() ? nullptr : sessions.data();
  lobby.playerSessionCount = sessions.size();
}

}  // namespace

TEST(ServerContextSmite, ResolvesGuidToSessionSlotIndex) {
  EchoVR::Lobby lobby{};
  std::vector<Entrant> entrants = {MakeEntrant(11), MakeEntrant(22), MakeEntrant(33)};
  std::vector<Slot> sessions = {MakeSlot(MakeGuid(1)), MakeSlot(MakeGuid(2)), MakeSlot(MakeGuid(3))};
  Attach(lobby, entrants);
  AttachSessions(lobby, sessions);
  nevr_game_server::ServerContext context;
  context.Initialize(&lobby, nullptr);
  context.FinalizeInitialization();

  uint64_t slot = 99;
  ASSERT_TRUE(context.FindEntrantSlotBySession(MakeGuid(3), slot));
  EXPECT_EQ(slot, 2U);
  ASSERT_TRUE(context.FindEntrantSlotBySession(MakeGuid(1), slot));
  EXPECT_EQ(slot, 0U);
}

TEST(ServerContextSmite, UnknownGuidIsNotFound) {
  EchoVR::Lobby lobby{};
  std::vector<Entrant> entrants = {MakeEntrant(11)};
  std::vector<Slot> sessions = {MakeSlot(MakeGuid(1))};
  Attach(lobby, entrants);
  AttachSessions(lobby, sessions);
  nevr_game_server::ServerContext context;
  context.Initialize(&lobby, nullptr);
  context.FinalizeInitialization();

  uint64_t slot = 99;
  EXPECT_FALSE(context.FindEntrantSlotBySession(MakeGuid(9), slot));
  EXPECT_EQ(slot, 99U);
}

TEST(ServerContextSmite, EntrantUserIdBytesDoNotMatch) {
  // A session GUID equal to the entrant's userId bytes must not resolve through the entrant array.
  EchoVR::Lobby lobby{};
  std::vector<Entrant> entrants = {MakeEntrant(11)};
  std::vector<Slot> sessions = {MakeSlot(MakeGuid(1))};
  Attach(lobby, entrants);
  AttachSessions(lobby, sessions);
  GUID asUserId{};
  std::memcpy(&asUserId, &entrants[0].userId, sizeof(GUID));
  nevr_game_server::ServerContext context;
  context.Initialize(&lobby, nullptr);
  context.FinalizeInitialization();

  uint64_t slot = 0;
  EXPECT_FALSE(context.FindEntrantSlotBySession(asUserId, slot));
}

TEST(ServerContextSmite, SlotBeyondEntrantArrayIsNotReturned) {
  // entrantData.count is the array's capacity in the game. A session slot past it must not name a
  // slot the game would index out of range.
  EchoVR::Lobby lobby{};
  std::vector<Entrant> entrants = {MakeEntrant(11)};
  std::vector<Slot> sessions = {MakeSlot(MakeGuid(1)), MakeSlot(MakeGuid(2))};
  Attach(lobby, entrants);
  AttachSessions(lobby, sessions);
  nevr_game_server::ServerContext context;
  context.Initialize(&lobby, nullptr);
  context.FinalizeInitialization();

  uint64_t slot = 0;
  EXPECT_FALSE(context.FindEntrantSlotBySession(MakeGuid(2), slot));
}

TEST(ServerContextSmite, NilGuidDoesNotMatchADepartedSlot) {
  // RemoveEntrant resets a departed slot in place with join state 0; its GUID must not resolve.
  EchoVR::Lobby lobby{};
  std::vector<Entrant> entrants = {MakeEntrant(11), MakeEntrant(12)};
  std::vector<Slot> sessions = {MakeDepartedSlot(GUID{}), MakeSlot(MakeGuid(2))};
  Attach(lobby, entrants);
  AttachSessions(lobby, sessions);
  nevr_game_server::ServerContext context;
  context.Initialize(&lobby, nullptr);
  context.FinalizeInitialization();

  uint64_t slot = 99;
  EXPECT_FALSE(context.FindEntrantSlotBySession(GUID{}, slot));
  EXPECT_EQ(slot, 99U);
  EXPECT_TRUE(context.FindEntrantSlotBySession(MakeGuid(2), slot));
  EXPECT_EQ(slot, 1U);
}

TEST(ServerContextSmite, SlotThatIsNotAcceptedIsNotReturned) {
  EchoVR::Lobby lobby{};
  std::vector<Entrant> entrants = {MakeEntrant(11)};
  std::vector<Slot> sessions = {MakeSlot(MakeGuid(1))};
  sessions[0].joinState = 2;  // add pending
  Attach(lobby, entrants);
  AttachSessions(lobby, sessions);
  nevr_game_server::ServerContext context;
  context.Initialize(&lobby, nullptr);
  context.FinalizeInitialization();

  uint64_t slot = 0;
  EXPECT_FALSE(context.FindEntrantSlotBySession(MakeGuid(1), slot));
}

// #58: the live entrant count is the accepted player sessions (join state 4), not the array capacity.
TEST(ServerContextSmite, AcceptedEntrantCountIgnoresEmptyAndPendingSlots) {
  EchoVR::Lobby lobby{};
  std::vector<Entrant> entrants = {MakeEntrant(11), MakeEntrant(12), MakeEntrant(13), MakeEntrant(14)};
  std::vector<Slot> sessions = {MakeSlot(MakeGuid(1)), MakeDepartedSlot(GUID{}), MakeSlot(MakeGuid(3)),
                                MakeSlot(MakeGuid(4))};
  sessions[3].joinState = 2;  // add pending
  Attach(lobby, entrants);
  AttachSessions(lobby, sessions);
  nevr_game_server::ServerContext context;
  EXPECT_EQ(context.CountAcceptedEntrants(), 0U) << "not initialized";
  context.Initialize(&lobby, nullptr);
  context.FinalizeInitialization();
  EXPECT_EQ(context.CountAcceptedEntrants(), 2U);

  sessions[1].joinState = 4;
  EXPECT_EQ(context.CountAcceptedEntrants(), 3U);
}

TEST(ServerContextSmite, AbsentSessionArrayIsNotFound) {
  EchoVR::Lobby lobby{};
  std::vector<Entrant> entrants = {MakeEntrant(11)};
  Attach(lobby, entrants);
  nevr_game_server::ServerContext context;
  context.Initialize(&lobby, nullptr);
  context.FinalizeInitialization();

  uint64_t slot = 0;
  EXPECT_FALSE(context.FindEntrantSlotBySession(MakeGuid(1), slot));
}

TEST(ServerContextEntrants, EntrantSessionGuidIsTheAcceptedSlotsClientGuid) {
  EchoVR::Lobby lobby{};
  std::vector<Entrant> entrants = {MakeEntrant(11), MakeEntrant(22)};
  Attach(lobby, entrants);

  EchoVR::Lobby::PlayerSessionSlot sessions[2]{};
  sessions[1].guid.Data1 = 0x11223344;
  sessions[1].guid.Data2 = 0x5566;
  sessions[1].guid.Data3 = 0x7788;
  sessions[1].guid.Data4[0] = 0x99;
  sessions[1].joinState = 4;
  sessions[0].joinState = 4;
  lobby.playerSessions = sessions;
  lobby.playerSessionCount = 2;

  nevr_game_server::ServerContext context;
  context.Initialize(&lobby, nullptr);
  context.FinalizeInitialization();

  const GUID slotOne = context.GetEntrantSessionGuid(1);
  EXPECT_EQ(slotOne.Data1, 0x11223344U);
  EXPECT_EQ(slotOne.Data2, 0x5566U);
  EXPECT_EQ(slotOne.Data3, 0x7788U);
  EXPECT_EQ(slotOne.Data4[0], 0x99U);

  const GUID slotZero = context.GetEntrantSessionGuid(0);
  EXPECT_EQ(slotZero.Data1, 0U);
  EXPECT_EQ(slotZero.Data2, 0U);
  EXPECT_EQ(slotZero.Data3, 0U);
}

TEST(ServerContextEntrants, EntrantSessionGuidIgnoresUnacceptedAndOutOfRangeSlots) {
  EchoVR::Lobby lobby{};
  std::vector<Entrant> entrants = {MakeEntrant(11)};
  Attach(lobby, entrants);

  EchoVR::Lobby::PlayerSessionSlot sessions[1]{};
  sessions[0].guid.Data1 = 0xDEADBEEF;
  sessions[0].joinState = 3;  // pending, not accepted
  lobby.playerSessions = sessions;
  lobby.playerSessionCount = 1;

  nevr_game_server::ServerContext context;
  context.Initialize(&lobby, nullptr);
  context.FinalizeInitialization();

  EXPECT_EQ(context.GetEntrantSessionGuid(0).Data1, 0U);
  EXPECT_EQ(context.GetEntrantSessionGuid(1).Data1, 0U);
}

TEST(ServerContextEntrants, TryGetEntrantSessionGuidReturnsTheAcceptedSlotsGuid) {
  EchoVR::Lobby lobby{};
  std::vector<Entrant> entrants = {MakeEntrant(11)};
  Attach(lobby, entrants);

  EchoVR::Lobby::PlayerSessionSlot sessions[1]{};
  sessions[0].guid.Data1 = 0x11223344;
  sessions[0].joinState = 4;  // accepted
  lobby.playerSessions = sessions;
  lobby.playerSessionCount = 1;

  nevr_game_server::ServerContext context;
  context.Initialize(&lobby, nullptr);
  context.FinalizeInitialization();

  GUID out{};
  ASSERT_TRUE(context.TryGetEntrantSessionGuid(0, &out));
  EXPECT_EQ(out.Data1, 0x11223344U);
}

TEST(ServerContextEntrants, TryGetEntrantSessionGuidDropsAnUnacceptedSlotWithoutAccountFallback) {
  EchoVR::Lobby lobby{};
  std::vector<Entrant> entrants = {MakeEntrant(42)};  // account id 42 must never be sent
  Attach(lobby, entrants);

  EchoVR::Lobby::PlayerSessionSlot sessions[1]{};
  sessions[0].guid.Data1 = 0xDEADBEEF;
  sessions[0].joinState = 3;  // pending, not accepted
  lobby.playerSessions = sessions;
  lobby.playerSessionCount = 1;

  nevr_game_server::ServerContext context;
  context.Initialize(&lobby, nullptr);
  context.FinalizeInitialization();

  GUID out{};
  out.Data1 = 0xAA;
  EXPECT_FALSE(context.TryGetEntrantSessionGuid(0, &out));
  EXPECT_EQ(out.Data1, 0xAAU) << "a rejected slot must leave the caller's GUID untouched";
  EXPECT_NE(out.Data1, 42U) << "the account id must never stand in for the session UUID";
}

// #772: cases that pin each guard in GetEntrantSessionGuid and each field of the zero-GUID test in
// TryGetEntrantSessionGuid; every one is killed by a single-line mutant of that function (see the PR body).
namespace {

// A lobby with `entrantCount` entrants, `sessionCount` reported player sessions over an array of
// `arraySize` slots, every slot accepted with a distinct non-zero GUID (Data1 = slot index + 1).
struct SessionLobby {
  EchoVR::Lobby lobby{};
  std::vector<Entrant> entrants;
  std::vector<EchoVR::Lobby::PlayerSessionSlot> sessions;

  SessionLobby(size_t entrantCount, size_t sessionCount, size_t arraySize) {
    for (size_t i = 0; i < entrantCount; ++i) {
      entrants.push_back(MakeEntrant(100 + i));
    }
    sessions.resize(arraySize);
    for (size_t i = 0; i < arraySize; ++i) {
      sessions[i].guid.Data1 = static_cast<unsigned long>(i + 1);
      sessions[i].joinState = 4;
    }
    Attach(lobby, entrants);
    lobby.playerSessions = sessions.data();
    lobby.playerSessionCount = sessionCount;
  }
};

}  // namespace

TEST(ServerContextEntrants, EntrantSessionGuidIsEmptyBeforeInitializeAndAfterTerminate) {
  SessionLobby fixture(1, 1, 1);
  nevr_game_server::ServerContext context;

  GUID out{};
  out.Data1 = 0xAA;
  EXPECT_EQ(context.GetEntrantSessionGuid(0).Data1, 0U);
  EXPECT_FALSE(context.TryGetEntrantSessionGuid(0, &out));

  context.Initialize(&fixture.lobby, nullptr);
  context.FinalizeInitialization();
  ASSERT_TRUE(context.TryGetEntrantSessionGuid(0, &out));
  EXPECT_EQ(out.Data1, 1U);

  context.Terminate();
  out.Data1 = 0xAA;
  EXPECT_EQ(context.GetEntrantSessionGuid(0).Data1, 0U);
  EXPECT_FALSE(context.TryGetEntrantSessionGuid(0, &out));
  EXPECT_EQ(out.Data1, 0xAAU);
}

TEST(ServerContextEntrants, EntrantSessionGuidIsEmptyWithoutALobbyOrItsArrays) {
  nevr_game_server::ServerContext noLobby;
  noLobby.Initialize(nullptr, nullptr);
  noLobby.FinalizeInitialization();
  EXPECT_EQ(noLobby.GetEntrantSessionGuid(0).Data1, 0U);

  SessionLobby noSessions(1, 1, 1);
  noSessions.lobby.playerSessions = nullptr;
  nevr_game_server::ServerContext withoutSessions;
  withoutSessions.Initialize(&noSessions.lobby, nullptr);
  withoutSessions.FinalizeInitialization();
  EXPECT_EQ(withoutSessions.GetEntrantSessionGuid(0).Data1, 0U);

  SessionLobby noEntrants(1, 1, 1);
  noEntrants.lobby.entrantData.items = nullptr;  // count stays 1: the array is absent, not empty
  nevr_game_server::ServerContext withoutEntrants;
  withoutEntrants.Initialize(&noEntrants.lobby, nullptr);
  withoutEntrants.FinalizeInitialization();
  EXPECT_EQ(withoutEntrants.GetEntrantSessionGuid(0).Data1, 0U);
}

TEST(ServerContextEntrants, EntrantSessionGuidStopsAtTheSmallerOfSessionAndEntrantCounts) {
  // Two accepted slots in the array, but the lobby reports one session: slot 1 is past the end.
  SessionLobby fewerSessions(2, 1, 2);
  nevr_game_server::ServerContext a;
  a.Initialize(&fewerSessions.lobby, nullptr);
  a.FinalizeInitialization();
  EXPECT_EQ(a.GetEntrantSessionGuid(0).Data1, 1U);
  EXPECT_EQ(a.GetEntrantSessionGuid(1).Data1, 0U) << "slot 1 is past playerSessionCount";

  // Two sessions reported, one entrant: slot 1 has no entrant.
  SessionLobby fewerEntrants(1, 2, 2);
  nevr_game_server::ServerContext b;
  b.Initialize(&fewerEntrants.lobby, nullptr);
  b.FinalizeInitialization();
  EXPECT_EQ(b.GetEntrantSessionGuid(0).Data1, 1U);
  EXPECT_EQ(b.GetEntrantSessionGuid(1).Data1, 0U) << "slot 1 is past the entrant count";

  // The first slot past both counts, in an array that holds it.
  SessionLobby exact(2, 2, 3);
  nevr_game_server::ServerContext c;
  c.Initialize(&exact.lobby, nullptr);
  c.FinalizeInitialization();
  EXPECT_EQ(c.GetEntrantSessionGuid(1).Data1, 2U);
  EXPECT_EQ(c.GetEntrantSessionGuid(2).Data1, 0U) << "slot 2 equals the limit";
}

TEST(ServerContextEntrants, TryGetEntrantSessionGuidRefusesANullDestination) {
  SessionLobby fixture(1, 1, 1);
  nevr_game_server::ServerContext context;
  context.Initialize(&fixture.lobby, nullptr);
  context.FinalizeInitialization();
  EXPECT_FALSE(context.TryGetEntrantSessionGuid(0, nullptr));
}

TEST(ServerContextEntrants, TryGetEntrantSessionGuidAcceptsAGuidWhoseOnlyNonZeroFieldIsAnyOne) {
  // 11 fields: Data1, Data2, Data3 and Data4[0..7]. Each alone must make an accepted slot's GUID count.
  for (size_t field = 0; field < 11; ++field) {
    SessionLobby fixture(1, 1, 1);
    GUID& guid = fixture.sessions[0].guid;
    guid = GUID{};
    if (field == 0) {
      guid.Data1 = 1;
    } else if (field == 1) {
      guid.Data2 = 1;
    } else if (field == 2) {
      guid.Data3 = 1;
    } else {
      guid.Data4[field - 3] = 1;
    }
    nevr_game_server::ServerContext context;
    context.Initialize(&fixture.lobby, nullptr);
    context.FinalizeInitialization();

    GUID out{};
    EXPECT_TRUE(context.TryGetEntrantSessionGuid(0, &out)) << "field " << field << " alone was treated as a zero GUID";
  }
}

TEST(ServerContextEntrants, TryGetEntrantSessionGuidRefusesAnAcceptedSlotWithAZeroGuid) {
  SessionLobby fixture(1, 1, 1);
  fixture.sessions[0].guid = GUID{};
  nevr_game_server::ServerContext context;
  context.Initialize(&fixture.lobby, nullptr);
  context.FinalizeInitialization();

  GUID out{};
  out.Data1 = 0xAA;
  EXPECT_FALSE(context.TryGetEntrantSessionGuid(0, &out));
  EXPECT_EQ(out.Data1, 0xAAU);
}
