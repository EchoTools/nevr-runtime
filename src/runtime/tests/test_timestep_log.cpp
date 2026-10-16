// #515: what boot.cpp logs about a server's time-step arguments is a pure decision (timestep_log.h): the
// level and text for every combination of what the hook, ApplyStep and ApplyFlag recorded.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "runtime/lifecycle/timestep_log.h"

namespace {

using nevr_server_timestep::FlagOutcome;
using nevr_server_timestep::StepOutcome;
using nevr_timestep_log::Decide;
using nevr_timestep_log::Inputs;
using nevr_timestep_log::Level;
using nevr_timestep_log::Line;

Inputs ServerRemoved() {
  Inputs in;
  in.report.tokensRemoved = 3;
  in.report.options = "-fixedtimestep,-timestep";
  in.bootSaysServer = true;
  in.cmdHasTimestepOption = true;
  in.step = StepOutcome::kSet;
  in.stepUs = 8333;
  in.rateHz = 120;
  in.flag = FlagOutcome::kSet;
  return in;
}

bool Contains(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

}  // namespace

TEST(TimestepLog, AServerThatLostItsArgumentsAndGotTheStepSaysSoAtInfo) {
  const auto lines = Decide(ServerRemoved());
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].level, Level::kInfo);
  EXPECT_TRUE(Contains(lines[0].text, "options=-fixedtimestep,-timestep")) << lines[0].text;
  EXPECT_TRUE(Contains(lines[0].text, "tokens_removed=3"));
  EXPECT_TRUE(Contains(lines[0].text, "the server step is 8333 us (120 Hz), set by NEVR")) << lines[0].text;
  EXPECT_TRUE(Contains(lines[0].text, "flag_state=set"));
  EXPECT_TRUE(Contains(lines[0].text, "CL Args")) << "the game's own line still shows the original";
}

TEST(TimestepLog, TheStepIsReportedFromTheRecordedOutcomeNotTheRate) {
  for (const StepOutcome failed : {StepOutcome::kStateMissing, StepOutcome::kRateZero, StepOutcome::kUnexpectedOldValue}) {
    Inputs in = ServerRemoved();
    in.step = failed;
    in.stepUs = 0;
    const auto lines = Decide(in);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].level, Level::kWarning) << nevr_server_timestep::OutcomeName(failed);
    EXPECT_TRUE(Contains(lines[0].text, "NOT set")) << lines[0].text;
    EXPECT_FALSE(Contains(lines[0].text, "8333")) << "no step is claimed when none was written: " << lines[0].text;
    EXPECT_TRUE(Contains(lines[0].text, nevr_server_timestep::OutcomeName(failed)));
  }
}

TEST(TimestepLog, AStepNotYetWrittenIsPendingNotSet) {
  Inputs in = ServerRemoved();
  in.step = StepOutcome::kNotApplied;
  const auto lines = Decide(in);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].level, Level::kInfo);
  EXPECT_TRUE(Contains(lines[0].text, "step_state=pending")) << lines[0].text;
  EXPECT_FALSE(Contains(lines[0].text, "8333"));
}

TEST(TimestepLog, AMissingFlagFieldOnAServerIsAWarning) {
  Inputs in = ServerRemoved();
  in.flag = FlagOutcome::kFieldMissing;
  const auto lines = Decide(in);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].level, Level::kWarning);
  EXPECT_TRUE(Contains(lines[0].text, "flag_state=field_missing")) << lines[0].text;
}

TEST(TimestepLog, ArgumentsRemovedFromALineBootTookForAClientIsAWarningThatNoStepWasSet) {
  // The game's parser read -server; boot.cpp's CommandLineToArgvW did not. Nothing sets a step or flag.
  Inputs in = ServerRemoved();
  in.bootSaysServer = false;
  in.step = StepOutcome::kNotServer;
  in.stepUs = 0;
  in.flag = FlagOutcome::kNotServer;
  const auto lines = Decide(in);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].level, Level::kWarning);
  EXPECT_TRUE(Contains(lines[0].text, "runs this process as a client")) << lines[0].text;
  EXPECT_TRUE(Contains(lines[0].text, "no fixed-time-step flag or step was set"));
  EXPECT_TRUE(Contains(lines[0].text, "options=-fixedtimestep,-timestep"));
  EXPECT_FALSE(Contains(lines[0].text, "sets the server step when the local config loads"))
      << "the old message claimed a step would still be set";
  EXPECT_FALSE(Contains(lines[0].text, "unchanged"));
}

TEST(TimestepLog, AResidualOptionAddsAWarningLine) {
  Inputs in = ServerRemoved();
  in.report.residual = true;
  const auto lines = Decide(in);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[1].level, Level::kWarning);
  EXPECT_TRUE(Contains(lines[1].text, "residual=true"));
  Inputs none = ServerRemoved();
  EXPECT_EQ(Decide(none).size(), 1u) << "no residual line when none is left";
}

TEST(TimestepLog, ATimestepArgumentThatWasNotRemovedOnAServerIsAWarning) {
  Inputs in;
  in.bootSaysServer = true;
  in.cmdHasTimestepOption = true;
  in.report.failed = true;
  const auto lines = Decide(in);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].level, Level::kWarning);
  EXPECT_TRUE(Contains(lines[0].text, "NOT removed"));
  EXPECT_TRUE(Contains(lines[0].text, "hook_failed=true"));
}

TEST(TimestepLog, AClientWithTheArgumentsPassesThemUnchangedAtInfo) {
  Inputs in;
  in.bootSaysServer = false;
  in.cmdHasTimestepOption = true;
  const auto lines = Decide(in);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].level, Level::kInfo);
  EXPECT_TRUE(Contains(lines[0].text, "passed to the game unchanged"));
}

TEST(TimestepLog, NothingToSayWithoutATimestepArgument) {
  Inputs server;
  server.bootSaysServer = true;
  EXPECT_TRUE(Decide(server).empty());
  Inputs client;
  EXPECT_TRUE(Decide(client).empty());
}
