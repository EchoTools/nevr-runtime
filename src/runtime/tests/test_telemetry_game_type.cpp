// #990, #996: the capture header's game type. The engine's gametype symbol from the session settings JSON is the
// value (capture.proto, CaptureHeader.game_type); the lobby type is only the fallback. The value survives the wire.

#include <gtest/gtest.h>

#include <string>

#include "runtime/server/telemetry_game_type.h"
#include "telemetry/v2/capture.pb.h"

TEST(TelemetryGameType, APrivateArenaSessionFallsBackToEchoArenaPrivate) {
  telemetry::v2::CaptureHeader header;
  auto game = nevr_telemetry_header::GameTypeForSession("{}", /*isPrivateMatch=*/true);
  nevr_telemetry_header::SetCaptureGameType(&header, game.gameType);
  EXPECT_EQ(header.game_type(), "echo_arena_private");
  EXPECT_FALSE(game.fromSettings);
}

TEST(TelemetryGameType, APublicArenaSessionFallsBackToEchoArena) {
  telemetry::v2::CaptureHeader header;
  auto game = nevr_telemetry_header::GameTypeForSession("{}", /*isPrivateMatch=*/false);
  nevr_telemetry_header::SetCaptureGameType(&header, game.gameType);
  EXPECT_EQ(header.game_type(), "echo_arena");
  EXPECT_FALSE(game.fromSettings);
}

TEST(TelemetryGameType, ThePrivateArenaSymbolFromSettingsIsKeptWhenThePublicLobbyFlagIsSet) {
  auto game = nevr_telemetry_header::GameTypeForSession("{\"gametype\":\"echo_arena_private\"}", false);
  EXPECT_EQ(game.gameType, "echo_arena_private");
  EXPECT_TRUE(game.fromSettings);
}

TEST(TelemetryGameType, ANonArenaModeKeepsItsOwnSymbolEvenInAPublicLobby) {
  auto game = nevr_telemetry_header::GameTypeForSession("{\"gametype\":\"echo_combat\",\"appid\":\"7\"}", false);
  EXPECT_EQ(game.gameType, "echo_combat");
  EXPECT_TRUE(game.fromSettings);
}

TEST(TelemetryGameType, ANonSymbolValueFallsBackToTheLobbyType) {
  EXPECT_EQ(nevr_telemetry_header::GameTypeForSession("{\"gametype\":\"Echo Arena\"}", true).gameType,
            "echo_arena_private");
  EXPECT_EQ(nevr_telemetry_header::GameTypeForSession("{\"gametype\":22}", false).gameType, "echo_arena");
  EXPECT_EQ(nevr_telemetry_header::GameTypeForSession("{\"gametype\":\"\"}", false).gameType, "echo_arena");
  EXPECT_FALSE(nevr_telemetry_header::GameTypeForSession("{\"gametype\":22}", false).fromSettings);
}

TEST(TelemetryGameType, MalformedSettingsFallBackToTheLobbyType) {
  auto game = nevr_telemetry_header::GameTypeForSession("{\"gametype\":", true);
  EXPECT_EQ(game.gameType, "echo_arena_private");
  EXPECT_FALSE(game.fromSettings);
}

TEST(TelemetryGameType, TheGameTypeSurvivesTheWire) {
  for (const char* symbol : {"echo_arena", "echo_arena_private", "echo_combat"}) {
    telemetry::v2::CaptureHeader sent;
    nevr_telemetry_header::SetCaptureGameType(&sent, symbol);
    std::string bytes;
    ASSERT_TRUE(sent.SerializeToString(&bytes));
    telemetry::v2::CaptureHeader received;
    ASSERT_TRUE(received.ParseFromString(bytes));
    EXPECT_EQ(received.game_type(), symbol);
  }
}
