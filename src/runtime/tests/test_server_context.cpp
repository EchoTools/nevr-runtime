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
  GameServer::ServerContext context;
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
  GameServer::ServerContext context;
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
  GameServer::ServerContext context;
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
  GameServer::ServerContext context;
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
  GameServer::ServerContext context;
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
  GameServer::ServerContext context;
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
  GameServer::ServerContext context;

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
  GameServer::ServerContext context;
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
  GameServer::ServerContext context;
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
  GameServer::ServerContext context;
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
  GameServer::ServerContext context;
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
  GameServer::ServerContext context;
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
  GameServer::ServerContext context;
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
  GameServer::ServerContext context;
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
  GameServer::ServerContext context;
  context.Initialize(&lobby, nullptr);
  context.FinalizeInitialization();

  uint64_t slot = 0;
  EXPECT_FALSE(context.FindEntrantSlotBySession(MakeGuid(1), slot));
}
