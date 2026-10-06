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
