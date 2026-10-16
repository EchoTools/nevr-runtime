// #511: a dedicated server's fixed time step.

#include <gtest/gtest.h>

#include <cstdint>

#include "runtime/lifecycle/server_timestep.h"

namespace {

using nevr_server_timestep::ApplyFlag;
using nevr_server_timestep::ApplyStep;
using nevr_server_timestep::kFixedTimestepFlag;
using nevr_server_timestep::StepOutcome;
using nevr_server_timestep::StepMicroseconds;
using nevr_server_timestep::WithFixedTimestep;

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


// --- #515: the two writes the server makes, with the client left alone ---

TEST(ServerTimestep, AServerGetsTheFlagAndKeepsTheOtherBits) {
  uint64_t flags = 0x8000000000000001ULL | 0x100000ULL;
  const uint64_t before = flags;
  EXPECT_TRUE(ApplyFlag(true, &flags));
  EXPECT_EQ(flags, before | 0x2000000ULL);
}

TEST(ServerTimestep, AClientsFlagIsUntouched) {
  uint64_t flags = 0x100000ULL;
  EXPECT_FALSE(ApplyFlag(false, &flags));
  EXPECT_EQ(flags, 0x100000ULL);
}

TEST(ServerTimestep, ANullFlagFieldIsNotWritten) {
  EXPECT_FALSE(ApplyFlag(true, nullptr));
}

TEST(ServerTimestep, AServerGetsTheStepAndTheOldValueIsReported) {
  uint64_t field = 33333;  // the game's own step
  const auto w = ApplyStep(true, 120, &field);
  EXPECT_EQ(w.outcome, StepOutcome::kSet);
  EXPECT_EQ(w.stepUs, 8333u);
  EXPECT_EQ(w.was, 33333u);
  EXPECT_EQ(field, 8333u) << "the whole 64-bit field holds the step";
  uint64_t wide = 0xFFFFFFFF00000000ULL;
  ApplyStep(true, 120, &wide);
  EXPECT_EQ(wide, 8333u) << "the upper half of the qword is written too";
}

TEST(ServerTimestep, AClientsStepIsUntouched) {
  uint64_t field = 33333;
  const auto w = ApplyStep(false, 120, &field);
  EXPECT_EQ(w.outcome, StepOutcome::kNotServer);
  EXPECT_EQ(field, 33333u);
}

TEST(ServerTimestep, ARateOfZeroOrMissingStateWritesNothingAndSaysSo) {
  uint64_t field = 33333;
  EXPECT_EQ(ApplyStep(true, 0, &field).outcome, StepOutcome::kRateZero);
  EXPECT_EQ(field, 33333u);
  EXPECT_EQ(ApplyStep(true, 120, nullptr).outcome, StepOutcome::kStateMissing);
}

TEST(ServerTimestep, EachOutcomeHasAName) {
  EXPECT_STREQ(nevr_server_timestep::OutcomeName(StepOutcome::kSet), "set");
  EXPECT_STREQ(nevr_server_timestep::OutcomeName(StepOutcome::kStateMissing), "state_missing");
  EXPECT_STREQ(nevr_server_timestep::OutcomeName(StepOutcome::kNotApplied), "pending");
}

}  // namespace
