// #511: a dedicated server's fixed time step.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "runtime/lifecycle/server_timestep.h"

namespace {

using nevr_server_timestep::ApplyFlag;
using nevr_server_timestep::FlagOutcome;
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
  EXPECT_EQ(ApplyFlag(true, &flags), FlagOutcome::kSet);
  EXPECT_EQ(flags, before | 0x2000000ULL);
}

TEST(ServerTimestep, AClientsFlagIsUntouched) {
  uint64_t flags = 0x100000ULL;
  EXPECT_EQ(ApplyFlag(false, &flags), FlagOutcome::kNotServer);
  EXPECT_EQ(flags, 0x100000ULL);
}

TEST(ServerTimestep, ANullFlagFieldIsNotWritten) {
  EXPECT_EQ(ApplyFlag(true, nullptr), FlagOutcome::kFieldMissing) << "a missing field is reported, not silent";
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

TEST(ServerTimestep, AFieldHoldingSomethingElseIsNotWritten) {
  // N31: only the game's default step (33333) or our own is overwritten; any other value is a layout or a
  // version the code does not know.
  uint64_t other = 12345;
  const auto w = ApplyStep(true, 120, &other);
  EXPECT_EQ(w.outcome, StepOutcome::kUnexpectedOldValue);
  EXPECT_EQ(w.was, 12345u);
  EXPECT_EQ(w.stepUs, 0u);
  EXPECT_EQ(other, 12345u) << "nothing is written";
  EXPECT_TRUE(nevr_server_timestep::StepFailed(w.outcome));
  uint64_t ours = 8333;  // a second run over our own value is still a set
  EXPECT_EQ(ApplyStep(true, 120, &ours).outcome, StepOutcome::kSet);
  EXPECT_EQ(ours, 8333u);
}

TEST(ServerTimestep, FieldAtIsTheBasePlusTheOffsetAndNullForNoBase) {
  alignas(8) unsigned char block[0x100] = {};
  EXPECT_EQ(nevr_server_timestep::FieldAt(block, 0x90), reinterpret_cast<uint64_t*>(block + 0x90));
  EXPECT_EQ(nevr_server_timestep::FieldAt(nullptr, 0x90), nullptr);
}

TEST(ServerTimestep, TheClockReadBackReportsStepAndModeBits) {
  alignas(8) unsigned char clock[0x100] = {};
  *reinterpret_cast<uint64_t*>(clock + nevr_server_timestep::kClockStepOffset) = 8333;
  *reinterpret_cast<uint64_t*>(clock + nevr_server_timestep::kClockModeOffset) = 0xFFFFFFFFFFFFFF18ULL;
  const auto rb = nevr_server_timestep::ReadClock(clock);
  EXPECT_TRUE(rb.present);
  EXPECT_EQ(rb.stepUs, 8333u);
  EXPECT_EQ(rb.modeBits, 0x18u) << "only the fixed and paced bits";
  EXPECT_TRUE(nevr_server_timestep::ReadBackIsLive(rb, 8333));
  EXPECT_FALSE(nevr_server_timestep::ReadBackIsLive(rb, 33333));
  EXPECT_FALSE(nevr_server_timestep::ReadClock(nullptr).present);
  EXPECT_FALSE(nevr_server_timestep::ReadBackIsLive(nevr_server_timestep::ReadClock(nullptr), 8333));
}

TEST(ServerTimestep, TheReadBackLineSaysWhetherTheStepIsLive) {
  alignas(8) unsigned char clock[0x100] = {};
  *reinterpret_cast<uint64_t*>(clock + nevr_server_timestep::kClockStepOffset) = 33333;
  const auto reset = nevr_server_timestep::ReadClock(clock);
  const std::string line = nevr_server_timestep::ReadBackLine(reset, 8333, StepOutcome::kSet, 125);
  EXPECT_NE(line.find("step_us=33333"), std::string::npos) << line;
  EXPECT_NE(line.find("expected_step_us=8333"), std::string::npos) << line;
  EXPECT_NE(line.find("live=false"), std::string::npos) << "a clock that was reset is not live: " << line;
  EXPECT_NE(line.find("step_state=set"), std::string::npos) << line;
  *reinterpret_cast<uint64_t*>(clock + nevr_server_timestep::kClockStepOffset) = 8333;
  const std::string live =
      nevr_server_timestep::ReadBackLine(nevr_server_timestep::ReadClock(clock), 8333, StepOutcome::kSet, 125);
  EXPECT_NE(live.find("live=true"), std::string::npos) << live;
  EXPECT_NE(nevr_server_timestep::ReadBackLine(nevr_server_timestep::ReadClock(nullptr), 8333,
                                               StepOutcome::kSet, 125).find("clock=absent"),
            std::string::npos);
}

TEST(ServerTimestep, TheReadBackHappensOnceOnAServerOnTheOneTick) {
  using nevr_server_timestep::kReadBackTick;
  using nevr_server_timestep::ShouldReadBack;
  EXPECT_TRUE(ShouldReadBack(true, kReadBackTick));
  EXPECT_FALSE(ShouldReadBack(false, kReadBackTick)) << "a client never reads back";
  EXPECT_FALSE(ShouldReadBack(true, kReadBackTick - 1));
  EXPECT_FALSE(ShouldReadBack(true, kReadBackTick + 1)) << "one shot, not every tick after";
}

TEST(ServerTimestep, EachOutcomeHasAName) {
  EXPECT_STREQ(nevr_server_timestep::OutcomeName(StepOutcome::kSet), "set");
  EXPECT_STREQ(nevr_server_timestep::OutcomeName(StepOutcome::kStateMissing), "state_missing");
  EXPECT_STREQ(nevr_server_timestep::OutcomeName(StepOutcome::kNotApplied), "pending");
  EXPECT_STREQ(nevr_server_timestep::OutcomeName(StepOutcome::kUnexpectedOldValue), "unexpected_old_value");
  EXPECT_STREQ(nevr_server_timestep::FlagOutcomeName(FlagOutcome::kFieldMissing), "field_missing");
}

}  // namespace
