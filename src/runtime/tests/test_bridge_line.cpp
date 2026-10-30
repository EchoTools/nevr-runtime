#include <gtest/gtest.h>

#include "runtime/log/bridge_line.h"

// #1294: the game's connection lines to the local bridge are demoted; nothing else is.

TEST(BridgeLine, GameConnectionToTheBridgeIsDemoted) {
  EXPECT_TRUE(nevr_bridge_line::IsBridgeConnectionLine(
      "[EVR] [TCP CLIENT] [R14NETCLIENT] connection to ws://127.0.0.1:53748/login"));
}

TEST(BridgeLine, GameConnectionToAnotherAddressIsNotDemoted) {
  EXPECT_FALSE(nevr_bridge_line::IsBridgeConnectionLine(
      "[EVR] [TCP CLIENT] [R14NETCLIENT] connection to ws://g.echovrce.com:80/spr"));
}

TEST(BridgeLine, NevrLineNamingTheBridgeIsNeverDemoted) {
  EXPECT_FALSE(nevr_bridge_line::IsBridgeConnectionLine("[NEVR.WS] Matchmaker listener on ws://127.0.0.1:51234"));
}

TEST(BridgeLine, AddressMustBeWholeNotAPrefixOrSuffixOfALongerAddress) {
  EXPECT_FALSE(nevr_bridge_line::IsBridgeConnectionLine("connection to ws://127.0.0.10:80/spr"));
  EXPECT_FALSE(nevr_bridge_line::IsBridgeConnectionLine("connection to ws://1127.0.0.1:80/spr"));
  EXPECT_FALSE(nevr_bridge_line::IsBridgeConnectionLine("connection to ws://127.0.0.1.example/spr"));
}

TEST(BridgeLine, LaterWholeOccurrenceStillMatches) {
  EXPECT_TRUE(nevr_bridge_line::IsBridgeConnectionLine("from 10.127.0.0.1 to 127.0.0.1"));
}

TEST(BridgeLine, NullAndEmptyAreNotBridgeLines) {
  EXPECT_FALSE(nevr_bridge_line::IsBridgeConnectionLine(nullptr));
  EXPECT_FALSE(nevr_bridge_line::IsBridgeConnectionLine(""));
}
