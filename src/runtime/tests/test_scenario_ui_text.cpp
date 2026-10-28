// The scenario build's ui_text fire action (scenario_protocol.h): the command a test sends to write a text into the
// menu through the UI text writer (patch/ui_text.h). Parse only; the write is checked in game.
#include <gtest/gtest.h>

#include <string>

#include "runtime/scenario/scenario_protocol.h"

namespace {

bool Parses(const char* line, nevr_scenario_protocol::Command* cmd, std::string* error) {
  error->clear();
  return nevr_scenario_protocol::ParseCommand(line, cmd, error);
}

}  // namespace

TEST(ScenarioUiText, TakesAnActorAnElementAndAText) {
  nevr_scenario_protocol::Command cmd;
  std::string error;
  ASSERT_TRUE(Parses(R"({"op":"fire","action":"ui_text","actor":"0xad97e58a7b13425f","element":"0xa8452de53657bd6e","text":"PR-C TEST"})",
                     &cmd, &error))
      << error;
  EXPECT_EQ(cmd.op, nevr_scenario_protocol::Op::kFireAction);
  EXPECT_EQ(cmd.action, "ui_text");
  EXPECT_EQ(cmd.uiActor, 0xad97e58a7b13425fULL);
  EXPECT_EQ(cmd.uiElement, 0xa8452de53657bd6eULL);
  EXPECT_EQ(cmd.value, "PR-C TEST");
}

TEST(ScenarioUiText, AnEmptyTextClearsTheElement) {
  nevr_scenario_protocol::Command cmd;
  std::string error;
  ASSERT_TRUE(Parses(R"({"op":"fire","action":"ui_text","actor":"0x1","element":"0x2","text":""})", &cmd, &error)) << error;
  EXPECT_EQ(cmd.value, "");
}

TEST(ScenarioUiText, RefusesWhatItCannotRead) {
  nevr_scenario_protocol::Command cmd;
  std::string error;
  EXPECT_FALSE(Parses(R"({"op":"fire","action":"ui_text","element":"0x2","text":"x"})", &cmd, &error));
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(Parses(R"({"op":"fire","action":"ui_text","actor":"0x1","text":"x"})", &cmd, &error));
  EXPECT_FALSE(Parses(R"({"op":"fire","action":"ui_text","actor":"0x1","element":"0x2"})", &cmd, &error));
  EXPECT_FALSE(Parses(R"({"op":"fire","action":"ui_text","actor":"1","element":"0x2","text":"x"})", &cmd, &error));
  EXPECT_FALSE(Parses(R"({"op":"fire","action":"ui_text","actor":"0x","element":"0x2","text":"x"})", &cmd, &error));
  EXPECT_FALSE(Parses(R"({"op":"fire","action":"ui_text","actor":"0x1ffffffffffffffff","element":"0x2","text":"x"})", &cmd, &error));
  EXPECT_FALSE(Parses(R"({"op":"fire","action":"ui_text","actor":"0xzz","element":"0x2","text":"x"})", &cmd, &error));
  EXPECT_FALSE(Parses(R"({"op":"fire","action":"ui_text","actor":5,"element":"0x2","text":"x"})", &cmd, &error));
}

// The 63-byte text is the writer's own limit; the protocol's string cap is 64 characters, so the writer is what refuses
// the 64th (patch/ui_text.h CheckRequest).
TEST(ScenarioUiText, ATextOfSixtyFourOrMoreCharactersIsRefusedAtTheProtocol) {
  nevr_scenario_protocol::Command cmd;
  std::string error;
  const std::string sixtyThree(63, 'a');
  const std::string sixtyFour(64, 'a');
  EXPECT_TRUE(Parses(("{\"op\":\"fire\",\"action\":\"ui_text\",\"actor\":\"0x1\",\"element\":\"0x2\",\"text\":\"" + sixtyThree + "\"}").c_str(),
                     &cmd, &error))
      << error;
  EXPECT_FALSE(Parses(("{\"op\":\"fire\",\"action\":\"ui_text\",\"actor\":\"0x1\",\"element\":\"0x2\",\"text\":\"" + sixtyFour + "\"}").c_str(),
                      &cmd, &error));
}
