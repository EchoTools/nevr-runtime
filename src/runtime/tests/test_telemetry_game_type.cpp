// #990: the capture header's game type. A private Arena session and a public one carry the engine's symbol
// for their mode (capture.proto, CaptureHeader.game_type), and the value survives the wire.

#include <gtest/gtest.h>

#include <string>

#include "runtime/server/telemetry_game_type.h"
#include "telemetry/v2/capture.pb.h"

TEST(TelemetryGameType, APrivateArenaSessionIsEchoArenaPrivate) {
  telemetry::v2::CaptureHeader header;
  nevr_telemetry_header::SetCaptureGameType(&header, /*isPrivateMatch=*/true);
  EXPECT_EQ(header.game_type(), "echo_arena_private");
}

TEST(TelemetryGameType, APublicArenaSessionIsEchoArena) {
  telemetry::v2::CaptureHeader header;
  nevr_telemetry_header::SetCaptureGameType(&header, /*isPrivateMatch=*/false);
  EXPECT_EQ(header.game_type(), "echo_arena");
}

TEST(TelemetryGameType, TheGameTypeSurvivesTheWire) {
  for (const bool isPrivate : {true, false}) {
    telemetry::v2::CaptureHeader sent;
    nevr_telemetry_header::SetCaptureGameType(&sent, isPrivate);
    const std::string bytes = sent.SerializeAsString();
    telemetry::v2::CaptureHeader received;
    ASSERT_TRUE(received.ParseFromString(bytes));
    EXPECT_EQ(received.game_type(), nevr_telemetry_header::GameTypeForLobby(isPrivate));
  }
}
