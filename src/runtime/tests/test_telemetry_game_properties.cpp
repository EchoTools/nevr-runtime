/* SYNTHESIS -- custom tool code, not from binary */

// The telemetry property keys and how the three game getters are called (#915).
//
// Each key is the CSymbol64 of a replicated variable's name, and the game's getters take the net
// game as the owner: FUN_140d5ced0(out, netGame, key) for the status symbol, FUN_140d36750(netGame,
// key) for the clock and FUN_140d05730(netGame, key) for the scores. The fakes below record what
// ReadGameProperties passes them.
//
// GameStatusFromSymbol (#921) maps the game's state symbols as the game's own state-to-string
// function does (echovr.exe 0x14016d510): every state name it knows, hashed, to its group.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "abi/symbol_hash.h"
#include "runtime/server/telemetry_game_status.h"
#include "runtime/server/telemetry_snapshot.h"

namespace {

TEST(TelemetryPropertyKeys, EachKeyIsTheCSymbol64OfItsName) {
  EXPECT_EQ(nevr_property_hash::GAME_STATUS, EchoVR::CSymbol64Hash("game_state"));
  EXPECT_EQ(nevr_property_hash::GAME_CLOCK, EchoVR::CSymbol64Hash("game_timer"));
  EXPECT_EQ(nevr_property_hash::BLUE_POINTS, EchoVR::CSymbol64Hash("blue_score"));
  EXPECT_EQ(nevr_property_hash::ORANGE_POINTS, EchoVR::CSymbol64Hash("red_score"));
}

struct Call {
  void* owner;
  uint64_t key;
};

std::vector<Call> g_symbolCalls;
std::vector<Call> g_floatCalls;
std::vector<Call> g_intCalls;
uint64_t* g_symbolOut = nullptr;

constexpr uint64_t kStatusSymbol = 0x3B529C356A118CC6;  // nevr_game_status_hash::PLAYING_1

uint64_t* __fastcall FakeSymbol(uint64_t* out, void* owner, uint64_t key) {
  g_symbolCalls.push_back({owner, key});
  g_symbolOut = out;
  *out = kStatusSymbol;
  return out;
}

float __fastcall FakeFloat(void* owner, uint64_t key) {
  g_floatCalls.push_back({owner, key});
  return 123.5f;
}

int32_t __fastcall FakeInt(void* owner, uint64_t key) {
  g_intCalls.push_back({owner, key});
  return key == nevr_property_hash::BLUE_POINTS ? 7 : key == nevr_property_hash::ORANGE_POINTS ? 4 : -99;
}

class ReadGamePropertiesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    g_symbolCalls.clear();
    g_floatCalls.clear();
    g_intCalls.clear();
    g_symbolOut = nullptr;
  }
  int netGame_ = 0;
};

TEST_F(ReadGamePropertiesTest, EveryGetterGetsTheNetGameAndItsKey) {
  const GameProperties props = ReadGameProperties({FakeSymbol, FakeFloat, FakeInt}, &netGame_);

  ASSERT_EQ(g_symbolCalls.size(), 1u);
  EXPECT_EQ(g_symbolCalls[0].owner, &netGame_);
  EXPECT_EQ(g_symbolCalls[0].key, nevr_property_hash::GAME_STATUS);
  EXPECT_NE(g_symbolOut, nullptr);
  EXPECT_NE(static_cast<void*>(g_symbolOut), static_cast<void*>(&netGame_));

  ASSERT_EQ(g_floatCalls.size(), 1u);
  EXPECT_EQ(g_floatCalls[0].owner, &netGame_);
  EXPECT_EQ(g_floatCalls[0].key, nevr_property_hash::GAME_CLOCK);

  ASSERT_EQ(g_intCalls.size(), 2u);
  EXPECT_EQ(g_intCalls[0].owner, &netGame_);
  EXPECT_EQ(g_intCalls[0].key, nevr_property_hash::BLUE_POINTS);
  EXPECT_EQ(g_intCalls[1].owner, &netGame_);
  EXPECT_EQ(g_intCalls[1].key, nevr_property_hash::ORANGE_POINTS);

  EXPECT_EQ(props.statusHash, kStatusSymbol);
  EXPECT_FLOAT_EQ(props.clock, 123.5f);
  EXPECT_EQ(props.bluePoints, 7);
  EXPECT_EQ(props.orangePoints, 4);
}

TEST_F(ReadGamePropertiesTest, AbsentGettersLeaveTheDefaults) {
  const GameProperties props = ReadGameProperties({nullptr, nullptr, nullptr}, &netGame_);
  EXPECT_EQ(props.statusHash, ~0ULL);
  EXPECT_FLOAT_EQ(props.clock, 0.0f);
  EXPECT_EQ(props.bluePoints, 0);
  EXPECT_EQ(props.orangePoints, 0);
  EXPECT_TRUE(g_symbolCalls.empty());
}

struct NamedState {
  const char* name;
  telemetry::v2::GameStatus status;
};

// The 25 symbols 0x14016d510 compares, by name, with the string it returns for each.
constexpr NamedState kGameStates[] = {
    {"pre_match_enter", telemetry::v2::GAME_STATUS_PRE_MATCH},
    {"pre_match_wait_for_ready_enter", telemetry::v2::GAME_STATUS_PRE_MATCH},
    {"pre_match_wait_for_ready_update", telemetry::v2::GAME_STATUS_PRE_MATCH},
    {"round_start", telemetry::v2::GAME_STATUS_ROUND_START},
    {"pre_launch_enter", telemetry::v2::GAME_STATUS_ROUND_START},
    {"pre_launch_update", telemetry::v2::GAME_STATUS_ROUND_START},
    {"team_room_wait_enter", telemetry::v2::GAME_STATUS_ROUND_START},
    {"team_room_wait_update", telemetry::v2::GAME_STATUS_ROUND_START},
    {"main_loop_enter", telemetry::v2::GAME_STATUS_PLAYING},
    {"main_loop_update", telemetry::v2::GAME_STATUS_PLAYING},
    {"point_score_enter", telemetry::v2::GAME_STATUS_SCORE},
    {"point_score_update", telemetry::v2::GAME_STATUS_SCORE},
    {"round_over_enter", telemetry::v2::GAME_STATUS_ROUND_OVER},
    {"round_over_update", telemetry::v2::GAME_STATUS_ROUND_OVER},
    {"post_match_celebration_enter", telemetry::v2::GAME_STATUS_POST_MATCH},
    {"post_match_celebration_update", telemetry::v2::GAME_STATUS_POST_MATCH},
    {"post_match_celebration_personal_stats_enter", telemetry::v2::GAME_STATUS_POST_MATCH},
    {"post_match_celebration_personal_stats_update", telemetry::v2::GAME_STATUS_POST_MATCH},
    {"waiting_to_return_to_lobby", telemetry::v2::GAME_STATUS_POST_MATCH},
    {"pre_sudden_death_enter", telemetry::v2::GAME_STATUS_PRE_SUDDEN_DEATH},
    {"pre_sudden_death_update", telemetry::v2::GAME_STATUS_PRE_SUDDEN_DEATH},
    {"sudden_death_new_round_enter", telemetry::v2::GAME_STATUS_SUDDEN_DEATH},
    {"sudden_death_new_round_update", telemetry::v2::GAME_STATUS_SUDDEN_DEATH},
    {"post_sudden_death_enter", telemetry::v2::GAME_STATUS_POST_SUDDEN_DEATH},
    {"post_sudden_death_update", telemetry::v2::GAME_STATUS_POST_SUDDEN_DEATH},
};

TEST(GameStatusFromSymbol, EveryStateTheGameNamesMapsToItsGroup) {
  for (const NamedState& state : kGameStates) {
    EXPECT_EQ(GameStatusFromSymbol(EchoVR::CSymbol64Hash(state.name)), state.status) << state.name;
  }
}

TEST(GameStatusFromSymbol, AnUnknownOrAbsentSymbolIsUnspecified) {
  EXPECT_EQ(GameStatusFromSymbol(EchoVR::CSymbol64Hash("game_state")), telemetry::v2::GAME_STATUS_UNSPECIFIED);
  EXPECT_EQ(GameStatusFromSymbol(~0ULL), telemetry::v2::GAME_STATUS_UNSPECIFIED);
  EXPECT_EQ(GameStatusFromSymbol(0), telemetry::v2::GAME_STATUS_UNSPECIFIED);
}

}  // namespace
