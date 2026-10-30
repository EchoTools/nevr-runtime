// #990, #996: the capture header's game type. The engine's gametype symbol from the session settings JSON is the
// value (capture.proto, CaptureHeader.game_type); the lobby type is only the fallback. The value survives the wire.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "nevr/symbol_hash.h"
#include "runtime/server/game_type_table.h"
#include "runtime/server/telemetry_game_type.h"
#include "telemetry/v2/capture.pb.h"

namespace {

// The community service's gametype: the mode's CSymbol64Hash as a JSON integer. Signed is the int64 the service
// marshals; unsigned is the same bits spelled as a uint64.
std::string SettingsWithMode(const char* mode, bool asUnsigned) {
  const uint64_t hash = EchoVR::CSymbol64Hash(mode);
  const std::string number = asUnsigned ? std::to_string(hash) : std::to_string(static_cast<int64_t>(hash));
  return "{\"gametype\":" + number + "}";
}

}  // namespace

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

// #1493: the service sends gametype as a number. A combat or social session in a public lobby used to read as
// echo_arena (the lobby fallback), which applied the public Arena round default and clock to it.
TEST(TelemetryGameType, ASignedNumberNamesTheCombatModeInAPublicLobby) {
  auto game = nevr_telemetry_header::GameTypeForSession(SettingsWithMode("echo_combat", false), false);
  EXPECT_EQ(game.gameType, "echo_combat");
  EXPECT_TRUE(game.fromSettings);
}

TEST(TelemetryGameType, AnUnsignedNumberNamesTheCombatModeInAPublicLobby) {
  auto game = nevr_telemetry_header::GameTypeForSession(SettingsWithMode("echo_combat", true), false);
  EXPECT_EQ(game.gameType, "echo_combat");
  EXPECT_TRUE(game.fromSettings);
}

TEST(TelemetryGameType, ANumberNamesEachOfTheServiceModes) {
  for (const auto& mode : nevr_game_type_table::kGameTypes) {
    auto game = nevr_telemetry_header::GameTypeForSession(SettingsWithMode(mode.name, false), true);
    EXPECT_EQ(game.gameType, mode.name);
    EXPECT_TRUE(game.fromSettings);
  }
}

TEST(TelemetryGameType, ANumberNamingPublicArenaIsEchoArena) {
  auto game = nevr_telemetry_header::GameTypeForSession(SettingsWithMode("echo_arena", false), false);
  EXPECT_EQ(game.gameType, "echo_arena");
  EXPECT_TRUE(game.fromSettings);
}

TEST(TelemetryGameType, ASocialNumberIsNotArena) {
  auto game = nevr_telemetry_header::GameTypeForSession(SettingsWithMode("social_2.0", false), false);
  EXPECT_EQ(game.gameType, "social_2.0");
  EXPECT_TRUE(game.fromSettings);
}

TEST(TelemetryGameType, ANumberNoServiceModeNamesFallsBackToTheLobbyType) {
  auto game = nevr_telemetry_header::GameTypeForSession("{\"gametype\":1}", false);
  EXPECT_EQ(game.gameType, "echo_arena");
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
