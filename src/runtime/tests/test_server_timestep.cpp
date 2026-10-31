// #511: a dedicated server's fixed time step.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "runtime/lifecycle/server_timestep.h"

namespace {

using nevr_server_timestep::ApplyFlag;
using nevr_server_timestep::ApplyStep;
using nevr_server_timestep::FlagOutcome;
using nevr_server_timestep::StepOutcome;
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
  EXPECT_EQ(w.after, 8333u);
  EXPECT_EQ(field, 8333u);
}

TEST(ServerTimestep, TheSlotIsReadAsAQwordSoDataInItsUpperHalfIsNoStep) {
  // The game stores the step as a qword (echovr.exe 0x140127a04). A slot with anything above bit 31 is not a
  // step, even when its low half looks like one.
  uint64_t field = (1ULL << 40) | 33333ULL;
  const auto w = ApplyStep(true, 120, &field);
  EXPECT_EQ(w.outcome, StepOutcome::kUnexpectedOldValue);
  EXPECT_EQ(field, (1ULL << 40) | 33333ULL) << "nothing is written";
}

TEST(ServerTimestep, AClientsStepIsUntouched) {
  uint64_t field = 33333;
  const auto w = ApplyStep(false, 120, &field);
  EXPECT_EQ(w.outcome, StepOutcome::kNotServer);
  EXPECT_EQ(field, 33333u);
}

TEST(ServerTimestep, ARateOfZeroOrMissingStateWritesNothingAndSaysSo) {
  uint64_t field = 33333;
  EXPECT_EQ(ApplyStep(true, 0, &field).outcome, StepOutcome::kKeepGameStep);
  EXPECT_EQ(ApplyStep(true, 9, &field).outcome, StepOutcome::kRateRefused);
  EXPECT_EQ(field, 33333u);
  EXPECT_EQ(ApplyStep(true, 120, nullptr).outcome, StepOutcome::kStateMissing);
}

TEST(ServerTimestep, AFieldHoldingNoStepIsNotWritten) {
  // N31 (main's PlanStepWrite): a value no step can have means the address is not the step slot.
  for (const uint64_t other : {0ULL, 999ULL, 100001ULL}) {
    uint64_t field = other;
    const auto w = ApplyStep(true, 120, &field);
    EXPECT_EQ(w.outcome, StepOutcome::kUnexpectedOldValue) << other;
    EXPECT_EQ(w.was, other);
    EXPECT_EQ(w.stepUs, 0u);
    EXPECT_EQ(field, other) << "nothing is written";
    EXPECT_TRUE(nevr_server_timestep::StepFailed(w.outcome));
  }
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



// --- #626 review: the read-back decision, the readable guard and the outcome records ---

namespace {
nevr_server_timestep::ClockReadBack ClockHolding(uint64_t step) {
  nevr_server_timestep::ClockReadBack rb;
  rb.present = true;
  rb.stepUs = step;
  return rb;
}
}  // namespace

TEST(ServerTimestep, AfterAWriteTheClockShouldHoldTheStepWrittenAndAResetIsNotLive) {
  nevr_server_timestep::StepWrite w;
  w.outcome = StepOutcome::kSet;
  w.stepUs = 8333;
  w.was = 33333;
  const auto live = nevr_server_timestep::DecideReadBack(w, 120, ClockHolding(8333), 125);
  EXPECT_TRUE(live.live);
  EXPECT_EQ(live.expectedStepUs, 8333u);
  const auto reset = nevr_server_timestep::DecideReadBack(w, 120, ClockHolding(33333), 125);
  EXPECT_FALSE(reset.live) << "the clock was reset to the game's step after the write";
  EXPECT_NE(reset.line.find("live=false"), std::string::npos) << reset.line;
}

TEST(ServerTimestep, ARateOfZeroExpectsTheGamesOwnStepAndIsLiveWhenTheClockHoldsIt) {
  // network.tick_rate_hz: 0 keeps the game's step on purpose; the read-back must not call that a failure.
  nevr_server_timestep::StepWrite w;
  w.outcome = StepOutcome::kKeepGameStep;
  w.was = 33333;
  const auto d = nevr_server_timestep::DecideReadBack(w, 0, ClockHolding(33333), 125);
  EXPECT_EQ(d.expectedStepUs, 33333u);
  EXPECT_TRUE(d.live);
  EXPECT_NE(d.line.find("expected_step_us=33333"), std::string::npos) << d.line;
  EXPECT_NE(d.line.find("step_state=keep_game_step"), std::string::npos) << d.line;
  EXPECT_FALSE(nevr_server_timestep::DecideReadBack(w, 0, ClockHolding(8333), 125).live)
      << "a clock holding some other step is still not live";
}

TEST(ServerTimestep, AFailedOrPendingStepExpectsTheConfiguredRateAndIsNotLiveUnlessTheClockHoldsIt) {
  for (const StepOutcome o : {StepOutcome::kStateMissing, StepOutcome::kRateRefused, StepOutcome::kUnexpectedOldValue,
                              StepOutcome::kWriteDidNotHold, StepOutcome::kNotApplied}) {
    nevr_server_timestep::StepWrite w;
    w.outcome = o;
    const auto d = nevr_server_timestep::DecideReadBack(w, 120, ClockHolding(33333), 125);
    EXPECT_EQ(d.expectedStepUs, 8333u) << nevr_server_timestep::OutcomeName(o);
    EXPECT_FALSE(d.live) << nevr_server_timestep::OutcomeName(o);
  }
  nevr_server_timestep::StepWrite w;
  w.outcome = StepOutcome::kNotApplied;
  EXPECT_FALSE(nevr_server_timestep::DecideReadBack(w, 120, nevr_server_timestep::ReadClock(nullptr), 125).live)
      << "an absent clock is never live";
}

TEST(ServerTimestep, AnUnreadableClockIsReportedAbsentAndNeverDereferenced) {
  alignas(8) unsigned char clock[0x100] = {};
  *reinterpret_cast<uint64_t*>(clock + nevr_server_timestep::kClockStepOffset) = 8333;
  EXPECT_TRUE(nevr_server_timestep::ReadClockIf(true, clock).present);
  EXPECT_FALSE(nevr_server_timestep::ReadClockIf(false, clock).present) << "the guard result decides";
  EXPECT_FALSE(nevr_server_timestep::ReadClockIf(true, nullptr).present);
}

TEST(ServerTimestep, TheRecordsKeepWhatWasStoredForTheOtherThread) {
  nevr_server_timestep::StepRecord record;
  EXPECT_EQ(record.Load().outcome, StepOutcome::kNotApplied) << "nothing recorded yet is pending";
  nevr_server_timestep::StepWrite w;
  w.outcome = StepOutcome::kUnexpectedOldValue;
  w.stepUs = 7;
  w.was = 123456789012ULL;
  w.after = 9;
  record.Store(w);
  const auto r = record.Load();
  EXPECT_EQ(r.outcome, StepOutcome::kUnexpectedOldValue);
  EXPECT_EQ(r.stepUs, 7u);
  EXPECT_EQ(r.was, 123456789012ULL);
  EXPECT_EQ(r.after, 9u);
  nevr_server_timestep::FlagRecord flag;
  EXPECT_EQ(flag.Load(), FlagOutcome::kNotApplied);
  flag.Store(FlagOutcome::kFieldMissing);
  EXPECT_EQ(flag.Load(), FlagOutcome::kFieldMissing);
}

// #1785: the step follows the session's game type.
using nevr_server_timestep::RateSource;
using nevr_server_timestep::TickRateForGameType;
using nevr_server_timestep::kCombatTickRateHz;

TEST(SessionTickRate, CombatSessionsGetTheRateTheirPhysicsWasTunedFor) {
  for (const char* type : {"echo_combat", "echo_combat_private", "echo_combat_tournament"}) {
    const auto rate = TickRateForGameType(type, kDefaultTickRateHz, false);
    EXPECT_EQ(rate.hz, 30u) << type;
    EXPECT_EQ(rate.source, RateSource::kCombatDefault) << type;
    EXPECT_EQ(StepMicroseconds(rate.hz), kGameDefaultStepUs) << type << ": the game's own 30 a second step";
  }
}

TEST(SessionTickRate, ArenaAndEveryOtherTypeKeepTheDefaultRate) {
  for (const char* type : {"echo_arena", "echo_arena_private", "social_2.0", "unknown", ""}) {
    const auto rate = TickRateForGameType(type, kDefaultTickRateHz, false);
    EXPECT_EQ(rate.hz, 120u) << type;
    EXPECT_EQ(rate.source, RateSource::kDefault) << type;
    EXPECT_EQ(StepMicroseconds(rate.hz), 8333u) << type;
  }
}

TEST(SessionTickRate, AnExplicitConfiguredRateWinsForEveryType) {
  for (const char* type : {"echo_combat", "echo_combat_private", "echo_arena", "social_2.0"}) {
    const auto rate = TickRateForGameType(type, 60, true);
    EXPECT_EQ(rate.hz, 60u) << type;
    EXPECT_EQ(rate.source, RateSource::kConfig) << type;
  }
  EXPECT_EQ(TickRateForGameType("echo_combat", 0, true).hz, 0u) << "0 keeps the game's own step, for Combat too";
  EXPECT_EQ(TickRateForGameType("echo_arena", 0, true).hz, 0u);
}

TEST(SessionTickRate, ACombatTypeNeedsTheWholePrefix) {
  EXPECT_FALSE(nevr_server_timestep::IsCombatGameType("echo_comba"));
  EXPECT_FALSE(nevr_server_timestep::IsCombatGameType("xecho_combat"));
  EXPECT_TRUE(nevr_server_timestep::IsCombatGameType("echo_combat"));
}

TEST(SessionTickRate, TheSessionRateIsWrittenToTheStepSlotAndReadBack) {
  // The write ApplySessionTimestep makes (config.cpp), driven on a buffer: a boot-time Arena step, then a Combat
  // session, then an Arena session again, each read back.
  uint64_t slot = kGameDefaultStepUs;
  auto session = [&](const char* type) {
    return ApplyStep(true, TickRateForGameType(type, kDefaultTickRateHz, false).hz, &slot);
  };
  EXPECT_EQ(session("echo_arena").outcome, StepOutcome::kSet);
  EXPECT_EQ(slot, 8333u);
  const auto combat = session("echo_combat");
  EXPECT_EQ(combat.outcome, StepOutcome::kSet);
  EXPECT_EQ(combat.was, 8333u);
  EXPECT_EQ(slot, kGameDefaultStepUs);
  EXPECT_EQ(session("echo_arena").outcome, StepOutcome::kSet);
  EXPECT_EQ(slot, 8333u) << "the next Arena session on the same process gets 120 again";
}

}  // namespace
