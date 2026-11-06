/* SYNTHESIS -- custom tool code, not from binary */

// The stream's capture header and frames built from a snapshot, and the session values the snapshot takes from
// the game (#2100), against a net game laid out as echovr.exe 34.4.631547.1's /session writer (0x1401b2ea0)
// reads it. The offsets and keys below are written out from the disassembly, not taken from the shipped
// headers:
//   - map_name is the `const char*` at *(netGame + 0x40) + 0x470 (0x1401b30bd, 0x1401b30c6), "" when the
//     object at +0x40 is null;
//   - total_round_count is the int getter (0x140d05730) on the net game with the CSymbol64 of
//     "net_total_round_count" (0x1401b31c0-0x1401b31d2);
//   - round_number is the int getter with "round_counter" (game_info writer 0x1401af253-0x1401af28a);
//   - the disc owner is the symbol getter (0x140d5ced0) with "game_frisbee_ownerid", whose low 32 bits are a
//     handle { u16 slot, u16 generation } compared with each player's (0x140155d1d, 0x140156939-0x14015697c);
//   - the restart requests are the bool getter (0x140ce4460) with "blue_team_restart_request" and
//     "orange_team_restart_request" (0x140155d3f-0x140155d90);
//   - the pause state is the symbol getter with "net_game_paused_state" and the float getter (0x140d36750)
//     with "net_game_pause_time"; "unpaused" with a time above 0 is "unpausing" (0x1401b3245-0x1401b3345).

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "nevr/symbol_hash.h"
#include "runtime/server/telemetry_frame.h"
#include "runtime/server/telemetry_session_read.h"

namespace {

constexpr uint64_t kSessionObject = 0x40;
constexpr uint64_t kLevelName = 0x470;

template <typename T>
void Put(std::vector<uint8_t>& buf, uint64_t off, T value) {
  ASSERT_LE(off + sizeof(T), buf.size());
  std::memcpy(buf.data() + off, &value, sizeof(T));
}

// A net game with the object at +0x40 whose +0x470 names the level.
struct Session {
  Session() : netGame(0x3000, 0), object(0x500, 0) {
    Put<const uint8_t*>(netGame, kSessionObject, object.data());
  }
  void SetLevel(const char* name) { Put<const char*>(object, kLevelName, name); }
  void* ng() { return netGame.data(); }
  std::vector<uint8_t> netGame;
  std::vector<uint8_t> object;
};

struct IntCall {
  void* owner;
  uint64_t key;
};
std::vector<IntCall> g_intCalls;

int32_t __fastcall FakeInt(void* owner, uint64_t key) {
  g_intCalls.push_back({owner, key});
  return key == EchoVR::CSymbol64Hash("net_total_round_count") ? 1 : -99;
}

// The other three getters, each answering its own keys and anything else with a value no key gives.
std::vector<IntCall> g_otherCalls;
uint64_t g_discOwner = ~0ULL;
uint64_t g_pausedState = ~0ULL;
float g_pauseTime = 0.0f;
bool g_blueRestart = false;
bool g_orangeRestart = false;

int32_t __fastcall FakeRounds(void* owner, uint64_t key) {
  g_intCalls.push_back({owner, key});
  if (key == EchoVR::CSymbol64Hash("net_total_round_count")) return 3;
  if (key == EchoVR::CSymbol64Hash("round_counter")) return 2;
  return -99;
}
uint64_t* __fastcall FakeSymbol(uint64_t* out, void* owner, uint64_t key) {
  g_otherCalls.push_back({owner, key});
  if (key == EchoVR::CSymbol64Hash("game_frisbee_ownerid")) {
    *out = g_discOwner;
  } else if (key == EchoVR::CSymbol64Hash("net_game_paused_state")) {
    *out = g_pausedState;
  } else {
    *out = 0x5555;
  }
  return out;
}
float __fastcall FakeFloat(void* owner, uint64_t key) {
  g_otherCalls.push_back({owner, key});
  return key == EchoVR::CSymbol64Hash("net_game_pause_time") ? g_pauseTime : -7.0f;
}
bool __fastcall FakeBool(void* owner, uint64_t key) {
  g_otherCalls.push_back({owner, key});
  if (key == EchoVR::CSymbol64Hash("blue_team_restart_request")) return g_blueRestart;
  if (key == EchoVR::CSymbol64Hash("orange_team_restart_request")) return g_orangeRestart;
  return true;
}

constexpr nevr_telemetry_session::SessionPropertyGetters kNoGetters{nullptr, nullptr, nullptr, nullptr};

TelemetrySnapshot Cleared() {
  TelemetrySnapshot s;
  s.Clear();
  return s;
}

TEST(TelemetrySessionKeys, EachKeyIsTheCSymbol64OfItsName) {
  EXPECT_EQ(nevr_property_hash::TOTAL_ROUND_COUNT, EchoVR::CSymbol64Hash("net_total_round_count"));
  EXPECT_EQ(nevr_property_hash::ROUND_COUNTER, EchoVR::CSymbol64Hash("round_counter"));
  EXPECT_EQ(nevr_property_hash::DISC_OWNER, EchoVR::CSymbol64Hash("game_frisbee_ownerid"));
  EXPECT_EQ(nevr_property_hash::BLUE_RESTART_REQUEST, EchoVR::CSymbol64Hash("blue_team_restart_request"));
  EXPECT_EQ(nevr_property_hash::ORANGE_RESTART_REQUEST, EchoVR::CSymbol64Hash("orange_team_restart_request"));
  EXPECT_EQ(nevr_property_hash::PAUSED_STATE, EchoVR::CSymbol64Hash("net_game_paused_state"));
  EXPECT_EQ(nevr_property_hash::PAUSE_TIME, EchoVR::CSymbol64Hash("net_game_pause_time"));
  EXPECT_EQ(nevr_pause_symbol::UNPAUSED, EchoVR::CSymbol64Hash("unpaused"));
  EXPECT_EQ(nevr_pause_symbol::PAUSED, EchoVR::CSymbol64Hash("paused"));
  EXPECT_EQ(nevr_pause_symbol::PAUSE_REQUESTED, EchoVR::CSymbol64Hash("pause_requested"));
}

TEST(TelemetrySessionRead, TheLevelIsTheNameTheSessionObjectPointsTo) {
  Session s;
  s.SetLevel("mpl_combat_dyson");
  TelemetrySnapshot snap = Cleared();
  nevr_telemetry_session::ReadSessionState(kNoGetters, s.ng(), &snap);
  EXPECT_STREQ(snap.mapName, "mpl_combat_dyson");
}

TEST(TelemetrySessionRead, NoSessionObjectOrNoNameIsAnEmptyLevel) {
  Session s;
  TelemetrySnapshot snap = Cleared();
  std::memcpy(snap.mapName, "stale", 6);
  nevr_telemetry_session::ReadSessionState(kNoGetters, s.ng(), &snap);  // the name pointer is null
  EXPECT_STREQ(snap.mapName, "");

  s.SetLevel("mpl_arena_a");
  Put<const uint8_t*>(s.netGame, kSessionObject, nullptr);
  std::memcpy(snap.mapName, "stale", 6);
  nevr_telemetry_session::ReadSessionState(kNoGetters, s.ng(), &snap);
  EXPECT_STREQ(snap.mapName, "");
}

TEST(TelemetrySessionRead, ALongLevelNameIsCutAndEnded) {
  Session s;
  const std::string longName(100, 'm');
  s.SetLevel(longName.c_str());
  TelemetrySnapshot snap = Cleared();
  nevr_telemetry_session::ReadSessionState(kNoGetters, s.ng(), &snap);
  EXPECT_EQ(std::string(snap.mapName), std::string(63, 'm'));
}

TEST(TelemetrySessionRead, TheRoundCountIsTheGamesOwnOnTheNetGame) {
  Session s;
  g_intCalls.clear();
  TelemetrySnapshot snap = Cleared();
  nevr_telemetry_session::ReadSessionState({FakeInt, nullptr, nullptr, nullptr}, s.ng(), &snap);
  EXPECT_EQ(snap.totalRoundCount, 1);
  bool asked = false;
  for (const auto& c : g_intCalls) {
    EXPECT_EQ(c.owner, s.ng());
    asked = asked || c.key == EchoVR::CSymbol64Hash("net_total_round_count");
  }
  EXPECT_TRUE(asked);

  TelemetrySnapshot none = Cleared();
  nevr_telemetry_session::ReadSessionState(kNoGetters, s.ng(), &none);
  EXPECT_EQ(none.totalRoundCount, 0);
}

TEST(TelemetrySessionRead, RoundPauseRestartAndDiscOwnerAreTheGamesOwnOnTheNetGame) {
  Session s;
  g_intCalls.clear();
  g_otherCalls.clear();
  g_discOwner = 0xABCD000000070004ULL;  // the handle is the low 32 bits: slot 4, generation 7
  g_pausedState = EchoVR::CSymbol64Hash("paused");
  g_pauseTime = 1.5f;
  g_blueRestart = true;
  g_orangeRestart = false;
  TelemetrySnapshot snap = Cleared();
  nevr_telemetry_session::ReadSessionState({FakeRounds, FakeSymbol, FakeFloat, FakeBool}, s.ng(), &snap);
  EXPECT_EQ(snap.totalRoundCount, 3);
  EXPECT_EQ(snap.roundNumber, 2);
  EXPECT_EQ(snap.discOwnerHandle, 0x00070004u);
  EXPECT_EQ(snap.pauseStateSymbol, EchoVR::CSymbol64Hash("paused"));
  EXPECT_FLOAT_EQ(snap.pauseTime, 1.5f);
  EXPECT_TRUE(snap.blueRestartRequest);
  EXPECT_FALSE(snap.orangeRestartRequest);
  for (const auto& c : g_intCalls) EXPECT_EQ(c.owner, s.ng());
  for (const auto& c : g_otherCalls) EXPECT_EQ(c.owner, s.ng());

  g_blueRestart = false;
  g_orangeRestart = true;
  TelemetrySnapshot again = Cleared();
  nevr_telemetry_session::ReadSessionState({FakeRounds, FakeSymbol, FakeFloat, FakeBool}, s.ng(), &again);
  EXPECT_FALSE(again.blueRestartRequest);
  EXPECT_TRUE(again.orangeRestartRequest);
}

TEST(TelemetrySessionRead, WithNoGettersTheSymbolsAreMinusOneAndTheRestZero) {
  Session s;
  TelemetrySnapshot snap = Cleared();
  nevr_telemetry_session::ReadSessionState(kNoGetters, s.ng(), &snap);
  EXPECT_EQ(snap.roundNumber, 0);
  EXPECT_EQ(snap.discOwnerHandle, 0xFFFFFFFFu);
  EXPECT_EQ(snap.pauseStateSymbol, ~0ULL);
  EXPECT_FLOAT_EQ(snap.pauseTime, 0.0f);
  EXPECT_FALSE(snap.blueRestartRequest);
  EXPECT_FALSE(snap.orangeRestartRequest);
}

TEST(TelemetryPauseState, EachGameStateHasTheNameSessionGivesIt) {
  using nevr_telemetry_session::PauseStateFromGame;
  const uint64_t unpaused = EchoVR::CSymbol64Hash("unpaused");
  EXPECT_EQ(PauseStateFromGame(unpaused, 0.0f), telemetry::v2::PAUSE_STATE_NOT_PAUSED);
  EXPECT_EQ(PauseStateFromGame(unpaused, -1.0f), telemetry::v2::PAUSE_STATE_NOT_PAUSED);
  EXPECT_EQ(PauseStateFromGame(unpaused, std::numeric_limits<float>::quiet_NaN()),
            telemetry::v2::PAUSE_STATE_NOT_PAUSED);
  EXPECT_EQ(PauseStateFromGame(unpaused, 0.25f), telemetry::v2::PAUSE_STATE_UNPAUSING);
  EXPECT_EQ(PauseStateFromGame(EchoVR::CSymbol64Hash("paused"), 0.0f), telemetry::v2::PAUSE_STATE_PAUSED);
  EXPECT_EQ(PauseStateFromGame(EchoVR::CSymbol64Hash("paused"), 3.0f), telemetry::v2::PAUSE_STATE_PAUSED);
  EXPECT_EQ(PauseStateFromGame(EchoVR::CSymbol64Hash("pause_requested"), 0.0f),
            telemetry::v2::PAUSE_STATE_PAUSED_REQUESTED);
  EXPECT_EQ(PauseStateFromGame(~0ULL, 0.0f), telemetry::v2::PAUSE_STATE_NONE);
  EXPECT_EQ(PauseStateFromGame(EchoVR::CSymbol64Hash("orange"), 0.0f), telemetry::v2::PAUSE_STATE_NONE);
}

TelemetrySnapshot WithPlayer(TelemetrySnapshot snap, uint8_t slot, uint16_t generation) {
  snap.playerInfo[slot].present = true;
  snap.playerInfo[slot].handleGeneration = generation;
  return snap;
}

TEST(TelemetryDiscHolder, ItIsThePlayerWhoseHandleIsTheOwners) {
  TelemetrySnapshot snap = WithPlayer(WithPlayer(Cleared(), 2, 1), 9, 3);
  snap.discOwnerHandle = 0x00030009u;
  EXPECT_EQ(nevr_telemetry_frame::DiscHolderSlot(snap), 9);
  snap.discOwnerHandle = 0x00010002u;
  EXPECT_EQ(nevr_telemetry_frame::DiscHolderSlot(snap), 2);
}

TEST(TelemetryDiscHolder, ASlotWithAnotherGenerationOrNoPlayerOrNoOwnerHoldsNothing) {
  TelemetrySnapshot snap = WithPlayer(Cleared(), 9, 3);
  snap.discOwnerHandle = 0x00040009u;  // slot 9 at an older generation: another player's handle
  EXPECT_EQ(nevr_telemetry_frame::DiscHolderSlot(snap), -1);
  snap.discOwnerHandle = 0x00030005u;  // slot 5 holds no player
  EXPECT_EQ(nevr_telemetry_frame::DiscHolderSlot(snap), -1);
  snap.discOwnerHandle = 0xFFFFFFFFu;  // no owner
  EXPECT_EQ(nevr_telemetry_frame::DiscHolderSlot(snap), -1);
}

TEST(TelemetryFrame, RoundPauseRestartAndHolderAreTheSnapshots) {
  TelemetrySnapshot snap = WithPlayer(Cleared(), 6, 2);
  snap.gameRead = true;
  snap.roundNumber = 2;
  snap.pauseStateSymbol = EchoVR::CSymbol64Hash("pause_requested");
  snap.blueRestartRequest = false;
  snap.orangeRestartRequest = true;
  snap.discOwnerHandle = 0x00020006u;
  telemetry::v2::EchoArenaFrame frame;
  nevr_telemetry_frame::BuildArenaFrame(snap, &frame);
  EXPECT_EQ(frame.round_number(), 2);
  EXPECT_EQ(frame.pause_state(), telemetry::v2::PAUSE_STATE_PAUSED_REQUESTED);
  EXPECT_EQ(frame.blue_team_restart_request(), 0);
  EXPECT_EQ(frame.orange_team_restart_request(), 1);
  ASSERT_TRUE(frame.has_disc_holder_slot());
  EXPECT_EQ(frame.disc_holder_slot(), 6);

  snap.blueRestartRequest = true;
  snap.orangeRestartRequest = false;
  snap.discOwnerHandle = 0xFFFFFFFFu;
  telemetry::v2::EchoArenaFrame again;
  nevr_telemetry_frame::BuildArenaFrame(snap, &again);
  EXPECT_EQ(again.blue_team_restart_request(), 1);
  EXPECT_EQ(again.orange_team_restart_request(), 0);
  EXPECT_FALSE(again.has_disc_holder_slot());
}

TEST(TelemetryFrame, ASnapshotTheGameDidNotReadSendsNoRoundPauseOrHolder) {
  TelemetrySnapshot snap = WithPlayer(Cleared(), 6, 2);
  snap.gameRead = false;
  snap.roundNumber = 2;
  snap.pauseStateSymbol = EchoVR::CSymbol64Hash("paused");
  snap.blueRestartRequest = true;
  snap.discOwnerHandle = 0x00020006u;
  telemetry::v2::EchoArenaFrame frame;
  nevr_telemetry_frame::BuildArenaFrame(snap, &frame);
  EXPECT_EQ(frame.round_number(), 0);
  EXPECT_EQ(frame.pause_state(), telemetry::v2::PAUSE_STATE_UNSPECIFIED);
  EXPECT_EQ(frame.blue_team_restart_request(), 0);
  EXPECT_FALSE(frame.has_disc_holder_slot());
}

TEST(TelemetryFrame, EachPlayerCarriesItsPossessionTimeAndPacketLoss) {
  TelemetrySnapshot snap = WithPlayer(WithPlayer(Cleared(), 1, 1), 12, 1);
  snap.gameRead = true;
  snap.playerInfo[1].packetLossRatio = 0.125f;
  snap.playerInfo[12].packetLossRatio = 0.5f;
  snap.playerInfo[1].ping = 40;
  snap.stats[1].possessionTime = 7.5f;
  snap.stats[12].possessionTime = 1.25f;
  snap.players[12].headPos[1] = 1.75f;
  telemetry::v2::EchoArenaFrame frame;
  nevr_telemetry_frame::BuildArenaFrame(snap, &frame);
  ASSERT_EQ(frame.players_size(), 2);
  EXPECT_EQ(frame.players(0).slot(), 1);
  EXPECT_FLOAT_EQ(frame.players(0).packet_loss_ratio(), 0.125f);
  EXPECT_FLOAT_EQ(frame.players(0).possession_time(), 7.5f);
  EXPECT_EQ(frame.players(0).ping(), 40u);
  EXPECT_EQ(frame.players(1).slot(), 12);
  EXPECT_FLOAT_EQ(frame.players(1).packet_loss_ratio(), 0.5f);
  EXPECT_FLOAT_EQ(frame.players(1).possession_time(), 1.25f);
  EXPECT_FLOAT_EQ(frame.players(1).head().position().y(), 1.75f);
}

TEST(TelemetryFrame, TheStatusClockScoresAndDiscAreTheSnapshots) {
  TelemetrySnapshot snap = Cleared();
  snap.gameStatusHash = nevr_game_status_hash::PLAYING_1;
  snap.gameClock = 123.5f;
  snap.bluePoints = 4;
  snap.orangePoints = 6;
  snap.discPos[2] = 9.0f;
  snap.discOrient[3] = 1.0f;
  snap.discVel[0] = -2.0f;
  snap.discBounceCount = 3;
  telemetry::v2::EchoArenaFrame frame;
  nevr_telemetry_frame::BuildArenaFrame(snap, &frame);
  EXPECT_EQ(frame.game_status(), telemetry::v2::GAME_STATUS_PLAYING);
  EXPECT_FLOAT_EQ(frame.game_clock(), 123.5f);
  EXPECT_EQ(frame.blue_points(), 4);
  EXPECT_EQ(frame.orange_points(), 6);
  EXPECT_FLOAT_EQ(frame.disc().pose().position().z(), 9.0f);
  EXPECT_FLOAT_EQ(frame.disc().pose().orientation().w(), 1.0f);
  EXPECT_FLOAT_EQ(frame.disc().velocity().x(), -2.0f);
  EXPECT_EQ(frame.disc().bounce_count(), 3u);
}

TEST(TelemetryHeader, TheMapAndRoundCountAreTheSessionsNotConstants) {
  TelemetrySnapshot snap = Cleared();
  std::memcpy(snap.mapName, "mpl_combat_dyson", 17);
  snap.totalRoundCount = 1;
  telemetry::v2::EchoArenaHeader ah;
  nevr_telemetry_frame::BuildArenaHeader(snap, "SESSION-1", &ah);
  EXPECT_EQ(ah.map_name(), "mpl_combat_dyson");
  EXPECT_EQ(ah.total_round_count(), 1);
  EXPECT_EQ(ah.session_id(), "SESSION-1");

  std::memcpy(snap.mapName, "mpl_arena_a", 12);
  snap.totalRoundCount = 5;
  telemetry::v2::EchoArenaHeader again;
  nevr_telemetry_frame::BuildArenaHeader(snap, "SESSION-1", &again);
  EXPECT_EQ(again.map_name(), "mpl_arena_a");
  EXPECT_EQ(again.total_round_count(), 5);
}

TEST(TelemetryHeader, TheRosterHoldsEachPresentSlotWithItsRole) {
  TelemetrySnapshot snap = Cleared();
  snap.playerInfo[4].present = true;
  snap.playerInfo[4].accountId = 77;
  snap.playerInfo[4].teamIndex = 1;
  std::memcpy(snap.playerInfo[4].displayName, "orange4", 8);
  snap.playerInfo[9].accountId = 99;  // not present
  telemetry::v2::EchoArenaHeader ah;
  nevr_telemetry_frame::BuildArenaHeader(snap, "S", &ah);
  ASSERT_EQ(ah.initial_roster_size(), 1);
  EXPECT_EQ(ah.initial_roster(0).slot(), 4);
  EXPECT_EQ(ah.initial_roster(0).account_number(), 77u);
  EXPECT_EQ(ah.initial_roster(0).display_name(), "orange4");
  EXPECT_EQ(ah.initial_roster(0).role(), telemetry::v2::ROLE_ORANGE_TEAM);
}

TEST(TelemetryHeader, TheRosterCarriesEachPlayersJerseyAndLevel) {
  TelemetrySnapshot snap = Cleared();
  snap.playerInfo[3].present = true;
  snap.playerInfo[3].jerseyNumber = 42;
  snap.playerInfo[3].level = 17;
  telemetry::v2::EchoArenaHeader ah;
  nevr_telemetry_frame::BuildArenaHeader(snap, "S", &ah);
  ASSERT_EQ(ah.initial_roster_size(), 1);
  EXPECT_EQ(ah.initial_roster(0).jersey_number(), 42);
  EXPECT_EQ(ah.initial_roster(0).level(), 17);
}

}  // namespace
