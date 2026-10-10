// The scenario build's early quit fire actions (scenario_control.cpp FireEarlyQuit): the commands a test
// sends to drive the game client's early quit lockout state. The game-side effect is checked in game; these
// pin which commands the control endpoint accepts.
#include <gtest/gtest.h>

#include <string>

#include "runtime/scenario/scenario_protocol.h"

namespace {

bool Parses(const char* line, nevr_scenario_protocol::Command* cmd, std::string* error) {
  error->clear();
  return nevr_scenario_protocol::ParseCommand(line, cmd, error);
}

}  // namespace

// early_quit_lockout sets the lockout expiry `seconds` from now; 0 clears it. A week is the cap, so a typo
// cannot lock the client out for years.
TEST(ScenarioEarlyQuit, LockoutTakesSecondsUpToAWeek) {
  nevr_scenario_protocol::Command cmd;
  std::string error;
  ASSERT_TRUE(Parses(R"({"op":"fire","action":"early_quit_lockout","seconds":300})", &cmd, &error)) << error;
  EXPECT_EQ(cmd.op, nevr_scenario_protocol::Op::kFireAction);
  EXPECT_EQ(cmd.action, "early_quit_lockout");
  EXPECT_EQ(cmd.number, 300u);
  ASSERT_TRUE(Parses(R"({"op":"fire","action":"early_quit_lockout","seconds":0})", &cmd, &error)) << error;
  EXPECT_EQ(cmd.number, 0u);
  ASSERT_TRUE(Parses(R"({"op":"fire","action":"early_quit_lockout","seconds":604800})", &cmd, &error)) << error;
  EXPECT_FALSE(Parses(R"({"op":"fire","action":"early_quit_lockout","seconds":604801})", &cmd, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(Parses(R"({"op":"fire","action":"early_quit_lockout"})", &cmd, &error));
}

// early_quit_countdown_active switches the penalty expression's lockoutcountdownactive output, which the
// game hard-codes to false, on or off. It needs an explicit boolean.
TEST(ScenarioEarlyQuit, CountdownActiveTakesABoolean) {
  nevr_scenario_protocol::Command cmd;
  std::string error;
  ASSERT_TRUE(Parses(R"({"op":"fire","action":"early_quit_countdown_active","active":true})", &cmd, &error)) << error;
  EXPECT_TRUE(cmd.flag);
  ASSERT_TRUE(Parses(R"({"op":"fire","action":"early_quit_countdown_active","active":false})", &cmd, &error)) << error;
  EXPECT_FALSE(cmd.flag);
  EXPECT_FALSE(Parses(R"({"op":"fire","action":"early_quit_countdown_active"})", &cmd, &error));
  EXPECT_FALSE(Parses(R"({"op":"fire","action":"early_quit_countdown_active","active":1})", &cmd, &error));
}

// early_quit_warning sets or clears the showearlyquitwarning flag (bit 46).
TEST(ScenarioEarlyQuit, WarningTakesABoolean) {
  nevr_scenario_protocol::Command cmd;
  std::string error;
  ASSERT_TRUE(Parses(R"({"op":"fire","action":"early_quit_warning","show":true})", &cmd, &error)) << error;
  EXPECT_TRUE(cmd.flag);
  EXPECT_FALSE(Parses(R"({"op":"fire","action":"early_quit_warning","show":"yes"})", &cmd, &error));
  EXPECT_FALSE(Parses(R"({"op":"fire","action":"early_quit_warning"})", &cmd, &error));
}

// early_quit_feature_flags sets the early quit feature-flag byte, & 0xdd as the game stores it, or as sent
// with raw (to test bits 1 and 5, which the game's mask clears); a byte at most.
TEST(ScenarioEarlyQuit, FeatureFlagsTakeAByte) {
  nevr_scenario_protocol::Command cmd;
  std::string error;
  ASSERT_TRUE(Parses(R"({"op":"fire","action":"early_quit_feature_flags","flags":1})", &cmd, &error)) << error;
  EXPECT_EQ(cmd.number, 1u);
  ASSERT_TRUE(Parses(R"({"op":"fire","action":"early_quit_feature_flags","flags":255})", &cmd, &error)) << error;
  EXPECT_FALSE(cmd.flag) << "masked unless raw is asked for";
  ASSERT_TRUE(Parses(R"({"op":"fire","action":"early_quit_feature_flags","flags":3,"raw":true})", &cmd, &error)) << error;
  EXPECT_TRUE(cmd.flag);
  EXPECT_FALSE(Parses(R"({"op":"fire","action":"early_quit_feature_flags","flags":3,"raw":"yes"})", &cmd, &error));
  EXPECT_FALSE(Parses(R"({"op":"fire","action":"early_quit_feature_flags","flags":256})", &cmd, &error));
  EXPECT_FALSE(Parses(R"({"op":"fire","action":"early_quit_feature_flags"})", &cmd, &error));
}

// dispatch_event raises a session event by its 64-bit symbol, given as a hex string so no JSON number
// precision is lost; anything else is refused.
TEST(ScenarioEarlyQuit, DispatchEventTakesAHexSymbol) {
  nevr_scenario_protocol::Command cmd;
  std::string error;
  ASSERT_TRUE(Parses(R"({"op":"fire","action":"dispatch_event","event":"0xd8a114aa7515d439"})", &cmd, &error)) << error;
  EXPECT_EQ(cmd.number, 0xd8a114aa7515d439ULL);
  EXPECT_FALSE(Parses(R"({"op":"fire","action":"dispatch_event","event":"d8a114aa7515d439"})", &cmd, &error));
  EXPECT_FALSE(Parses(R"({"op":"fire","action":"dispatch_event","event":"0x"})", &cmd, &error));
  EXPECT_FALSE(Parses(R"({"op":"fire","action":"dispatch_event","event":"0x1d8a114aa7515d439"})", &cmd, &error));
  EXPECT_FALSE(Parses(R"({"op":"fire","action":"dispatch_event","event":"0xzz"})", &cmd, &error));
  EXPECT_FALSE(Parses(R"({"op":"fire","action":"dispatch_event","event":12})", &cmd, &error));
}
