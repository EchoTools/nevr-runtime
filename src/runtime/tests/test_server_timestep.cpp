// #511: a dedicated server's fixed time step.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "runtime/lifecycle/server_timestep.h"

namespace {

using nevr_server_timestep::kFixedTimestepFlag;
using nevr_server_timestep::StepMicroseconds;
using nevr_server_timestep::WithFixedTimestep;
using nevr_server_timestep::kDefaultTickRateHz;
using nevr_server_timestep::kGameDefaultStepUs;
using nevr_server_timestep::kMaxTickRateHz;
using nevr_server_timestep::kMinTickRateHz;
using nevr_server_timestep::ParseTickRateHz;
using nevr_server_timestep::PlanStepWrite;
using nevr_server_timestep::ReadBackMatches;
using nevr_server_timestep::StepAction;

TEST(ServerTimestep, StepIsAMillionMicrosecondsOverTheRate) {
  EXPECT_EQ(StepMicroseconds(120), 8333u);
  EXPECT_EQ(StepMicroseconds(60), 16666u);
  EXPECT_EQ(StepMicroseconds(30), 33333u);  // the game's own default
}

TEST(ServerTimestep, ARateOfZeroLeavesTheGamesStep) {
  EXPECT_EQ(StepMicroseconds(0), 0u);
}

TEST(ServerTimestep, TheFlagIsSetAndTheOtherBitsKept) {
  EXPECT_EQ(WithFixedTimestep(0), kFixedTimestepFlag);
  EXPECT_EQ(kFixedTimestepFlag, 0x2000000u);
  const uint64_t others = 0x8000000000000001ULL | 0x100000ULL;
  EXPECT_EQ(WithFixedTimestep(others), others | 0x2000000ULL);
  EXPECT_EQ(WithFixedTimestep(WithFixedTimestep(others)), WithFixedTimestep(others));
}

// #595: the tick rate is the config key network.tick_rate_hz (flat key nevr_tick_rate_hz).
TEST(ServerTimestep, AnAbsentOrEmptyKeyIsTheDefaultRateWithNoProblem) {
  std::string problem = "stale";
  EXPECT_EQ(ParseTickRateHz(nullptr, &problem), 120u);
  EXPECT_TRUE(problem.empty());
  EXPECT_EQ(ParseTickRateHz("", &problem), 120u);
  EXPECT_TRUE(problem.empty());
  EXPECT_EQ(kDefaultTickRateHz, 120u);
}

TEST(ServerTimestep, AValueInRangeIsTheRate) {
  std::string problem;
  EXPECT_EQ(ParseTickRateHz("60", &problem), 60u);
  EXPECT_TRUE(problem.empty());
  EXPECT_EQ(ParseTickRateHz("240", &problem), 240u);
  EXPECT_EQ(ParseTickRateHz("10", &problem), kMinTickRateHz);
  EXPECT_EQ(ParseTickRateHz("1000", &problem), kMaxTickRateHz);
  EXPECT_TRUE(problem.empty());
}

TEST(ServerTimestep, AValueOutOfRangeIsRefusedForTheDefaultWithAProblem) {
  std::string problem;
  EXPECT_EQ(ParseTickRateHz("9", &problem), kDefaultTickRateHz);
  EXPECT_FALSE(problem.empty());
  EXPECT_EQ(ParseTickRateHz("1001", &problem), kDefaultTickRateHz);
  EXPECT_FALSE(problem.empty());
  // 2^32 + 60 must not wrap into a valid rate.
  EXPECT_EQ(ParseTickRateHz("4294967356", &problem), kDefaultTickRateHz);
  EXPECT_FALSE(problem.empty());
  EXPECT_EQ(ParseTickRateHz("99999999999999999999999", &problem), kDefaultTickRateHz);
  EXPECT_FALSE(problem.empty());
}

TEST(ServerTimestep, AValueThatIsNotAWholeNumberIsRefusedForTheDefaultWithAProblem) {
  for (const char* text : {"abc", "-5", "12.5", "60hz", " 60", "0x3c"}) {
    std::string problem;
    EXPECT_EQ(ParseTickRateHz(text, &problem), kDefaultTickRateHz) << text;
    EXPECT_FALSE(problem.empty()) << text;
  }
}

// Rate 0 is "the game's own step": the fixed-step flag is still set (a server is always fixed-step),
// so 0 is the game's 30 a second, never an unpaced loop.
TEST(ServerTimestep, ARateOfZeroIsTheGamesOwnStepAndIsNotAProblem) {
  std::string problem;
  EXPECT_EQ(ParseTickRateHz("0", &problem), 0u);
  EXPECT_TRUE(problem.empty());
  const auto plan = PlanStepWrite(0, kGameDefaultStepUs);
  EXPECT_EQ(plan.action, StepAction::KeepGameStep);
  EXPECT_EQ(kGameDefaultStepUs, StepMicroseconds(30));
}

TEST(ServerTimestep, TheStepIsWrittenForARateInRangeOverAPlausibleStep) {
  const auto plan = PlanStepWrite(120, kGameDefaultStepUs);
  EXPECT_EQ(plan.action, StepAction::Write);
  EXPECT_EQ(plan.stepUs, 8333u);
}

TEST(ServerTimestep, NothingIsWrittenForARateOutOfRange) {
  EXPECT_EQ(PlanStepWrite(1, kGameDefaultStepUs).action, StepAction::RefuseRate);
  EXPECT_EQ(PlanStepWrite(9, kGameDefaultStepUs).action, StepAction::RefuseRate);
  EXPECT_EQ(PlanStepWrite(1001, kGameDefaultStepUs).action, StepAction::RefuseRate);
  EXPECT_EQ(PlanStepWrite(0xFFFFFFFFu, kGameDefaultStepUs).action, StepAction::RefuseRate);
}

// N31: the current step is read from a fixed game address; a value no step can have means the address
// is not the step any more (a changed binary), so nothing is written over it.
TEST(ServerTimestep, NothingIsWrittenOverAStepNoStepCanBe) {
  EXPECT_EQ(PlanStepWrite(120, 0).action, StepAction::RefuseSlot);
  EXPECT_EQ(PlanStepWrite(120, 999).action, StepAction::RefuseSlot);
  EXPECT_EQ(PlanStepWrite(120, 100001).action, StepAction::RefuseSlot);
  EXPECT_EQ(PlanStepWrite(120, 0xDEADBEEFu).action, StepAction::RefuseSlot);
  EXPECT_EQ(PlanStepWrite(120, 1000).action, StepAction::Write);
  EXPECT_EQ(PlanStepWrite(120, 100000).action, StepAction::Write);
}

TEST(ServerTimestep, TheReadBackMustBeTheStepWritten) {
  EXPECT_TRUE(ReadBackMatches(8333, 8333));
  EXPECT_FALSE(ReadBackMatches(8333, 33333));
  EXPECT_FALSE(ReadBackMatches(8333, 0));
}

}  // namespace
