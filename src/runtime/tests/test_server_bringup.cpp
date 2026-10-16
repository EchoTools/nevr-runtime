// #45: when a dedicated server begins multiplayer.

#include <gtest/gtest.h>

#include <cstdint>

#include "runtime/lifecycle/server_bringup.h"

namespace {

using nevr_server_bringup::BeginsMultiplayer;
using nevr_server_bringup::kLoggedIn;
using nevr_server_bringup::kLoggingIn;

constexpr uint32_t kLoggedOut = 0;
constexpr uint32_t kLoadingRoot = 1;
constexpr uint32_t kLoadingGlobal = 4;
constexpr uint32_t kLobby = 5;
constexpr uint32_t kLoadingLevel = 7;
constexpr uint32_t kInGame = 9;

TEST(ServerBringup, BeginsWhenTheServersLoginCompletes) {
  EXPECT_TRUE(BeginsMultiplayer(true, kLoggingIn, kLoggedIn));
}

TEST(ServerBringup, AClientLeavesItToItsMenu) {
  EXPECT_FALSE(BeginsMultiplayer(false, kLoggingIn, kLoggedIn));
}

TEST(ServerBringup, LoggedInReachedAnotherWayDoesNotBeginAgain) {
  EXPECT_FALSE(BeginsMultiplayer(true, kInGame, kLoggedIn));
  EXPECT_FALSE(BeginsMultiplayer(true, kLobby, kLoggedIn));
  EXPECT_FALSE(BeginsMultiplayer(true, kLoggedIn, kLoggedIn));
  EXPECT_FALSE(BeginsMultiplayer(true, kLoggedOut, kLoggedIn));
}

TEST(ServerBringup, NoOtherSwitchBegins) {
  const uint32_t states[] = {kLoggedOut, kLoadingRoot, kLoggingIn, kLoggedIn,
                             kLoadingGlobal, kLobby, kLoadingLevel, kInGame};
  for (uint32_t from : states) {
    for (uint32_t to : states) {
      if (from == kLoggingIn && to == kLoggedIn) continue;
      EXPECT_FALSE(BeginsMultiplayer(true, from, to)) << from << " -> " << to;
    }
  }
}

}  // namespace
