// When a dedicated server begins multiplayer, and what it does when it cannot (#45, #514, #592).

#include <gtest/gtest.h>

#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

#include "runtime/hook/addresses.h"
#include "runtime/lifecycle/server_bringup.h"

namespace {

using EchoVR::NetGameState;
using nevr_server_bringup::BeginsMultiplayer;
using nevr_server_bringup::kBeginMultiplayerLobbyPrologue;
using nevr_server_bringup::kQueueGameThreadCallPrologue;
using nevr_server_bringup::RunQueuedBegin;
using nevr_server_bringup::ValidatePrologues;

TEST(ServerBringup, BeginsWhenTheServersLoginCompletes) {
  EXPECT_TRUE(BeginsMultiplayer(true, NetGameState::LoggingIn, NetGameState::LoggedIn));
}

TEST(ServerBringup, AClientLeavesItToItsMenu) {
  EXPECT_FALSE(BeginsMultiplayer(false, NetGameState::LoggingIn, NetGameState::LoggedIn));
}

TEST(ServerBringup, LoggedInReachedAnotherWayDoesNotBeginAgain) {
  EXPECT_FALSE(BeginsMultiplayer(true, NetGameState::InGame, NetGameState::LoggedIn));
  EXPECT_FALSE(BeginsMultiplayer(true, NetGameState::Lobby, NetGameState::LoggedIn));
  EXPECT_FALSE(BeginsMultiplayer(true, NetGameState::LoggedIn, NetGameState::LoggedIn));
  EXPECT_FALSE(BeginsMultiplayer(true, NetGameState::LoggedOut, NetGameState::LoggedIn));
}

TEST(ServerBringup, NoOtherSwitchBegins) {
  const NetGameState states[] = {NetGameState::LoggedOut,    NetGameState::LoadingRoot, NetGameState::LoggingIn,
                                 NetGameState::LoggedIn,     NetGameState::LoadingGlobal, NetGameState::Lobby,
                                 NetGameState::LoadingLevel, NetGameState::InGame,      NetGameState::NoNetwork};
  for (NetGameState from : states) {
    for (NetGameState to : states) {
      if (from == NetGameState::LoggingIn && to == NetGameState::LoggedIn) continue;
      EXPECT_FALSE(BeginsMultiplayer(true, from, to)) << static_cast<int>(from) << " -> " << static_cast<int>(to);
    }
  }
}

// The layout the bring-up reads is named in addresses.h, not written at each use.
TEST(ServerBringup, TheNetGameAndGameOffsetsAreNamedInAddresses) {
  EXPECT_EQ(nevr_patch_addresses::NETGAME_STATE_OFFSET, 0u);
  EXPECT_EQ(nevr_patch_addresses::NETGAME_GAME_OFFSET, 8u);
  EXPECT_EQ(nevr_patch_addresses::GAME_CALL_QUEUE_OFFSET, 8u);
}

// The boot-time check: both functions, through the one ValidatePrologue.
struct Sites {
  std::vector<uint8_t> queue{std::begin(kQueueGameThreadCallPrologue), std::end(kQueueGameThreadCallPrologue)};
  std::vector<uint8_t> begin{std::begin(kBeginMultiplayerLobbyPrologue), std::end(kBeginMultiplayerLobbyPrologue)};
};

TEST(ServerBringupPrologues, MatchingBytesPassAndFailNothing) {
  Sites s;
  std::vector<std::string> causes;
  EXPECT_TRUE(ValidatePrologues(s.queue.data(), s.begin.data(), [&](const char* c) { causes.push_back(c); }));
  EXPECT_TRUE(causes.empty());
}

TEST(ServerBringupPrologues, AChangedQueuePrologueFailsAndNamesTheQueue) {
  Sites s;
  s.queue[2] ^= 0xFF;
  std::vector<std::string> causes;
  EXPECT_FALSE(ValidatePrologues(s.queue.data(), s.begin.data(), [&](const char* c) { causes.push_back(c); }));
  ASSERT_EQ(causes.size(), 1u);
  EXPECT_NE(causes[0].find("QueueGameThreadCall"), std::string::npos) << causes[0];
}

TEST(ServerBringupPrologues, AChangedBeginPrologueFailsAndNamesTheBegin) {
  Sites s;
  s.begin[7] ^= 0xFF;
  std::vector<std::string> causes;
  EXPECT_FALSE(ValidatePrologues(s.queue.data(), s.begin.data(), [&](const char* c) { causes.push_back(c); }));
  ASSERT_EQ(causes.size(), 1u);
  EXPECT_NE(causes[0].find("BeginMultiplayerLobby"), std::string::npos) << causes[0];
}

TEST(ServerBringupPrologues, ANullSiteIsAMismatchNotACrash) {
  Sites s;
  std::vector<std::string> causes;
  EXPECT_FALSE(ValidatePrologues(nullptr, s.begin.data(), [&](const char* c) { causes.push_back(c); }));
  EXPECT_FALSE(ValidatePrologues(s.queue.data(), nullptr, [&](const char* c) { causes.push_back(c); }));
  EXPECT_EQ(causes.size(), 2u);
}

// The queued call, on the game's thread.
TEST(ServerBringupQueuedBegin, LoggedInBeginsAndDoesNotFail) {
  int begins = 0;
  int fails = 0;
  RunQueuedBegin(NetGameState::LoggedIn, [&] { ++begins; }, [&](NetGameState) { ++fails; });
  EXPECT_EQ(begins, 1);
  EXPECT_EQ(fails, 0);
}

TEST(ServerBringupQueuedBegin, AnyOtherStateFailsTheServerRatherThanIdling) {
  const NetGameState others[] = {NetGameState::LoggedOut,    NetGameState::LoadingRoot, NetGameState::LoggingIn,
                                 NetGameState::LoadingGlobal, NetGameState::Lobby,      NetGameState::InGame,
                                 NetGameState::NoNetwork,    NetGameState::LoginFailed};
  for (NetGameState state : others) {
    int begins = 0;
    int fails = 0;
    NetGameState failedIn = NetGameState::LoggedIn;
    RunQueuedBegin(state, [&] { ++begins; }, [&](NetGameState s) { ++fails; failedIn = s; });
    EXPECT_EQ(begins, 0) << static_cast<int>(state);
    EXPECT_EQ(fails, 1) << static_cast<int>(state);
    EXPECT_EQ(failedIn, state);
  }
}

}  // namespace
