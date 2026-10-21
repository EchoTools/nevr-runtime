#include "runtime/server/telemetry_header.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <string>

#include "telemetry/v2/capture.pb.h"
#include "telemetry/v2/echo_arena.pb.h"

namespace {

using nevr_game_server::BuildCaptureHeaderEnvelope;

TelemetrySnapshot SnapshotWithPlayers() {
  static TelemetrySnapshot snap;  // ~40 KB: off the stack
  std::memset(&snap, 0, sizeof(snap));
  snap.playerCount = 3;
  snap.playerInfo[0].accountId = 1001;
  std::strcpy(snap.playerInfo[0].displayName, "blue one");
  snap.playerInfo[0].teamIndex = 0;
  snap.playerInfo[1].accountId = 0;  // empty slot: skipped
  snap.playerInfo[2].accountId = 1003;
  std::strcpy(snap.playerInfo[2].displayName, "spectator \"quoted\"");
  snap.playerInfo[2].teamIndex = 2;
  return snap;
}

// Serializes the envelope and parses it back, so every assertion reads what goes on the wire.
telemetry::v2::Envelope RoundTrip(const telemetry::v2::Envelope& envelope) {
  std::string wire;
  EXPECT_TRUE(envelope.SerializeToString(&wire));
  telemetry::v2::Envelope parsed;
  EXPECT_TRUE(parsed.ParseFromString(wire));
  return parsed;
}

const std::chrono::system_clock::time_point kNow{std::chrono::seconds(1760000000) + std::chrono::nanoseconds(250)};

TEST(TelemetryHeader, PublicMatchRecordsTheEchoArenaGameType) {
  const auto parsed = RoundTrip(BuildCaptureHeaderEnvelope("sess-1", false, SnapshotWithPlayers(), kNow));
  ASSERT_TRUE(parsed.has_header());
  EXPECT_EQ(parsed.header().game_type(), "echo_arena");
}

TEST(TelemetryHeader, PrivateMatchRecordsThePrivateGameType) {
  const auto parsed = RoundTrip(BuildCaptureHeaderEnvelope("sess-1", true, SnapshotWithPlayers(), kNow));
  ASSERT_TRUE(parsed.has_header());
  EXPECT_EQ(parsed.header().game_type(), "echo_arena_private");
}

TEST(TelemetryHeader, CarriesTheCaptureIdentityAndTime) {
  const auto parsed = RoundTrip(BuildCaptureHeaderEnvelope("sess-42", false, SnapshotWithPlayers(), kNow));
  const auto& header = parsed.header();
  EXPECT_EQ(header.capture_id(), "sess-42");
  EXPECT_EQ(header.format_version(), 2u);
  EXPECT_EQ(header.created_at().seconds(), 1760000000);
  EXPECT_EQ(header.created_at().nanos(), 250);
  const auto& arena = header.echo_arena();
  EXPECT_EQ(arena.session_id(), "sess-42");
  EXPECT_EQ(arena.client_name(), "nevr-runtime");
  EXPECT_EQ(arena.map_name(), "mpl_arena_a");
  EXPECT_EQ(arena.total_round_count(), 3);
  EXPECT_EQ(arena.skeleton().bone_count(), 23u);
  EXPECT_EQ(arena.skeleton().transform_stride(), 12u);
  EXPECT_EQ(arena.skeleton().orientation_stride(), 16u);
}

TEST(TelemetryHeader, RosterSkipsEmptySlotsAndMapsTeams) {
  const auto parsed = RoundTrip(BuildCaptureHeaderEnvelope("sess-1", false, SnapshotWithPlayers(), kNow));
  const auto& roster = parsed.header().echo_arena().initial_roster();
  ASSERT_EQ(roster.size(), 2);
  EXPECT_EQ(roster[0].slot(), 0);
  EXPECT_EQ(roster[0].account_number(), 1001u);
  EXPECT_EQ(roster[0].display_name(), "blue one");
  EXPECT_EQ(roster[0].role(), telemetry::v2::ROLE_BLUE_TEAM);
  EXPECT_EQ(roster[1].slot(), 2);
  EXPECT_EQ(roster[1].account_number(), 1003u);
  EXPECT_EQ(roster[1].display_name(), "spectator \"quoted\"");
  EXPECT_EQ(roster[1].role(), telemetry::v2::ROLE_SPECTATOR);
}

}  // namespace
