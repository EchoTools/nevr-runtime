// #1156 (PR-D, the live call): the scenario build's way to drive the invite service without the wizard UI. Parse only; the
// request itself is checked against a loopback server (test_invite_service) and in a real run.
//
// Prediction, written before the code. The control endpoint takes three commands:
//   {"op":"fire","action":"invite_resolve","code":"ZZZZZZ"}
//   {"op":"fire","action":"invite_create","region":"US WEST","expires":"3 HOURS","experience":"arena","mode":"custom","map":1}
//   {"op":"invite_answers"}
// and refuses a resolve with no code, a create with no region, expiry, experience or mode, and a map above 255.
// Falsifier: any of those parses to a command the control endpoint would act on with a field missing.
#include <gtest/gtest.h>

#include <string>

#include "runtime/scenario/scenario_protocol.h"

namespace {

bool Parses(const char* line, nevr_scenario_protocol::Command* cmd, std::string* error) {
  error->clear();
  return nevr_scenario_protocol::ParseCommand(line, cmd, error);
}

}  // namespace

TEST(ScenarioInvite, AResolveTakesACode) {
  nevr_scenario_protocol::Command cmd;
  std::string error;
  ASSERT_TRUE(Parses(R"({"op":"fire","action":"invite_resolve","code":"ZZZZZZ"})", &cmd, &error)) << error;
  EXPECT_EQ(cmd.op, nevr_scenario_protocol::Op::kFireAction);
  EXPECT_EQ(cmd.action, "invite_resolve");
  EXPECT_EQ(cmd.value, "ZZZZZZ");
}

TEST(ScenarioInvite, AResolveWithoutACodeIsRefused) {
  nevr_scenario_protocol::Command cmd;
  std::string error;
  EXPECT_FALSE(Parses(R"({"op":"fire","action":"invite_resolve"})", &cmd, &error));
  EXPECT_NE(error.find("code"), std::string::npos) << error;
  EXPECT_FALSE(Parses(R"({"op":"fire","action":"invite_resolve","code":""})", &cmd, &error));
  EXPECT_FALSE(Parses(R"({"op":"fire","action":"invite_resolve","code":5})", &cmd, &error));
}

TEST(ScenarioInvite, ACreateTakesTheMetricsFieldsAndTheCardChoices) {
  nevr_scenario_protocol::Command cmd;
  std::string error;
  ASSERT_TRUE(Parses(
      R"({"op":"fire","action":"invite_create","region":"US WEST","expires":"3 HOURS","experience":"arena","mode":"custom","map":1})",
      &cmd, &error))
      << error;
  EXPECT_EQ(cmd.action, "invite_create");
  EXPECT_EQ(cmd.value, "US WEST");
  EXPECT_EQ(cmd.key, "3 HOURS");
  EXPECT_EQ(cmd.inviteExperience, "arena");
  EXPECT_EQ(cmd.inviteMode, "custom");
  EXPECT_EQ(cmd.number, 1u);
}

TEST(ScenarioInvite, ACreateNeedsEveryFieldButTheMap) {
  nevr_scenario_protocol::Command cmd;
  std::string error;
  EXPECT_TRUE(Parses(R"({"op":"fire","action":"invite_create","region":"US WEST","expires":"3 HOURS","experience":"lobby","mode":""})",
                     &cmd, &error))
      << error;
  EXPECT_EQ(cmd.number, 0u);
  EXPECT_FALSE(Parses(R"({"op":"fire","action":"invite_create","expires":"3 HOURS","experience":"arena","mode":"custom"})", &cmd, &error));
  EXPECT_FALSE(Parses(R"({"op":"fire","action":"invite_create","region":"US WEST","experience":"arena","mode":"custom"})", &cmd, &error));
  EXPECT_FALSE(Parses(R"({"op":"fire","action":"invite_create","region":"US WEST","expires":"3 HOURS","mode":"custom"})", &cmd, &error));
  EXPECT_FALSE(Parses(R"({"op":"fire","action":"invite_create","region":"US WEST","expires":"3 HOURS","experience":"arena"})", &cmd, &error));
  EXPECT_FALSE(Parses(
      R"({"op":"fire","action":"invite_create","region":"US WEST","expires":"3 HOURS","experience":"arena","mode":"custom","map":256})",
      &cmd, &error));
}

TEST(ScenarioInvite, TheAnswersAreAskedForByTheirOwnOp) {
  nevr_scenario_protocol::Command cmd;
  std::string error;
  ASSERT_TRUE(Parses(R"({"op":"invite_answers"})", &cmd, &error)) << error;
  EXPECT_EQ(cmd.op, nevr_scenario_protocol::Op::kInviteAnswers);
}
