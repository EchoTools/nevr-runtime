// #982: the CJson_GetFloat detour (lifecycle/arena_rules_hook.cpp) answers round_time with 480 s for a public Arena
// session on a server with no arena.round_time in config.yaml, however the game's reads and the session's messages are
// ordered. The tests call the detour itself; the game's CJson_GetFloat is a stub that answers the game's own 300 s.

#include <gtest/gtest.h>

#include <winsock2.h>
#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "abi/echovr_functions.h"
#include "core/globals.h"
#include "runtime/lifecycle/arena_round_time.h"
#include "runtime/lifecycle/config.h"
#include "runtime/server/game_type_table.h"

BOOL g_isServer = TRUE;  // the runtime defines it in cli.cpp; the detour reads it

namespace {

constexpr FLOAT kGameValue = 300.0f;

std::vector<std::string> g_pathsRead;

FLOAT GameCJsonGetFloat(PVOID, const CHAR* path, FLOAT, INT32) {
  g_pathsRead.emplace_back(path);
  return kGameValue;
}

FLOAT ReadRoundTime(const char* path = "round_time") { return CJsonGetFloatHook(nullptr, path, 0.0f, 0); }

// A GameServerSessionStart frame as the service writes it: the 36-byte header, the settings JSON, a NUL, no entrants.
std::vector<std::uint8_t> SessionStartFrame(const std::string& settingsJson) {
  std::vector<std::uint8_t> frame(nevr_game_type_table::kSessionStartHeaderSize, 0);
  frame.insert(frame.end(), settingsJson.begin(), settingsJson.end());
  frame.push_back(0);
  return frame;
}

class ArenaRulesHook : public ::testing::Test {
 protected:
  void SetUp() override {
    EchoVR::CJsonGetFloat = &GameCJsonGetFloat;
    g_pathsRead.clear();
    g_isServer = TRUE;
    g_arenaRoundTime = 0.0f;
    g_arenaCelebrationTime = 0.0f;
    g_arenaMercyScore = 0.0f;
    nevr_arena_round_time::ResetForTest();
  }
};

TEST_F(ArenaRulesHook, PublicArenaOnAServerReadsEightMinutes) {
  nevr_arena_round_time::SetSessionGameType("echo_arena");
  EXPECT_FLOAT_EQ(ReadRoundTime(), 480.0f);
  EXPECT_FLOAT_EQ(ReadRoundTime("rules|arena|round_time"), 480.0f);
  EXPECT_EQ(g_pathsRead.size(), 2u) << "the game's own read still runs before the answer";
}

TEST_F(ArenaRulesHook, AReadBeforeTheSessionTypeKeepsTheGameValueAndTheNextReadIsEightMinutes) {
  EXPECT_FLOAT_EQ(ReadRoundTime(), kGameValue) << "no session is known: the game's value";
  EXPECT_FLOAT_EQ(ReadRoundTime(), kGameValue);

  nevr_arena_round_time::SetSessionGameType("echo_arena");

  EXPECT_FLOAT_EQ(ReadRoundTime(), 480.0f) << "the session's reads after its type arrives";
}

TEST_F(ArenaRulesHook, ReadsThatFoundNoTypeAreCountedUntilTheSessionStoresIt) {
  ReadRoundTime();
  ReadRoundTime();
  ReadRoundTime("round_time_private");  // never reaches the decision: not counted
  EXPECT_EQ(nevr_arena_round_time::TakeReadsWithoutType(), 2u);
  EXPECT_EQ(nevr_arena_round_time::TakeReadsWithoutType(), 0u) << "taking the count resets it";

  nevr_arena_round_time::SetSessionGameType("echo_arena");
  ReadRoundTime();
  EXPECT_EQ(nevr_arena_round_time::TakeReadsWithoutType(), 0u) << "a read that found a type is not counted";
}

TEST_F(ArenaRulesHook, TheSessionStartFramesTypeIsInPlaceForTheGamesRead) {
  // The previous session was a combat match; this session arrives as the legacy frame alone (no protobuf create).
  nevr_arena_round_time::SetSessionGameType("echo_combat");
  EXPECT_FLOAT_EQ(ReadRoundTime(), kGameValue);

  const auto frame = SessionStartFrame(R"({"gametype":"echo_arena","level":"mpl_arena_a"})");
  const auto type = nevr_game_type_table::GameTypeOfSessionStart(frame.data(), frame.size());
  ASSERT_TRUE(type.has_value());
  const auto previous = nevr_arena_round_time::SetSessionGameType(*type);
  EXPECT_EQ(previous, std::optional<std::string>("echo_combat"));

  EXPECT_FLOAT_EQ(ReadRoundTime(), 480.0f);
}

TEST_F(ArenaRulesHook, ThePreviousPublicArenaSessionDoesNotLeakIntoACombatOrPrivateSession) {
  nevr_arena_round_time::SetSessionGameType("echo_arena");
  EXPECT_FLOAT_EQ(ReadRoundTime(), 480.0f);

  for (const char* settings : {R"({"gametype":"echo_combat"})", R"({"gametype":"echo_arena_private"})",
                               R"({"gametype":"social_2.0"})"}) {
    const auto frame = SessionStartFrame(settings);
    const auto type = nevr_game_type_table::GameTypeOfSessionStart(frame.data(), frame.size());
    ASSERT_TRUE(type.has_value()) << settings;
    nevr_arena_round_time::SetSessionGameType(*type);
    EXPECT_FLOAT_EQ(ReadRoundTime(), kGameValue) << settings;
  }
}

TEST_F(ArenaRulesHook, PrivateCombatAndSocialSessionsKeepTheGameValue) {
  for (const char* type : {"echo_arena_private", "echo_combat", "echo_combat_private", "social_2.0"}) {
    nevr_arena_round_time::SetSessionGameType(type);
    EXPECT_FLOAT_EQ(ReadRoundTime(), kGameValue) << type;
  }
}

TEST_F(ArenaRulesHook, ThePrivateAndTimerPathsKeepTheGameValueInAPublicArenaSession) {
  nevr_arena_round_time::SetSessionGameType("echo_arena");
  for (const char* path : {"round_time_private", "arena_private|round_time", "round_timer", "sudden_death_round_time"}) {
    EXPECT_FLOAT_EQ(ReadRoundTime(path), kGameValue) << path;
  }
}

TEST_F(ArenaRulesHook, AClientKeepsTheGameValue) {
  g_isServer = FALSE;
  nevr_arena_round_time::SetSessionGameType("echo_arena");
  EXPECT_FLOAT_EQ(ReadRoundTime(), kGameValue);
}

TEST_F(ArenaRulesHook, AnArenaRoundTimeInConfigWinsOverTheDefault) {
  g_arenaRoundTime = 240.0f;
  nevr_arena_round_time::SetSessionGameType("echo_arena");
  EXPECT_FLOAT_EQ(ReadRoundTime(), 240.0f);
}

}  // namespace
