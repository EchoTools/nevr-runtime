// The co-op AI diagnostic's decisions (patch/coop_ai_trace_rules.h, issue #63): when a bot's line is
// written, and the fixed bot table the per-tick hook uses.
#include <gtest/gtest.h>

#include "runtime/patch/coop_ai_trace_rules.h"

using nevr_coop_ai_trace::BotState;
using nevr_coop_ai_trace::BotTable;

// Every gate the trace reports is part of the comparison: a change in any one of them is a line.
TEST(CoopAiTrace, AnyGateChangeIsAChange) {
  const BotState base;
  EXPECT_TRUE(nevr_coop_ai_trace::SameState(base, base));
  BotState s = base;
  s.setupPending = 1;
  EXPECT_FALSE(nevr_coop_ai_trace::SameState(base, s)) << "setup_pending";
  s = base;
  s.setupOk = 1;
  EXPECT_FALSE(nevr_coop_ai_trace::SameState(base, s)) << "setup_ok";
  s = base;
  s.goalsOk = 0;
  EXPECT_FALSE(nevr_coop_ai_trace::SameState(base, s)) << "goals";
  s = base;
  s.waypointsOk = 1;
  EXPECT_FALSE(nevr_coop_ai_trace::SameState(base, s)) << "waypoints";
  s = base;
  s.lookupActor = -1;
  EXPECT_FALSE(nevr_coop_ai_trace::SameState(base, s)) << "lookup_actor";
  s = base;
  s.phase = 4;
  EXPECT_FALSE(nevr_coop_ai_trace::SameState(base, s)) << "phase";
}

// The heartbeat is due once the interval has passed since the bot's last line, not before.
TEST(CoopAiTrace, HeartbeatAfterTheInterval) {
  EXPECT_FALSE(nevr_coop_ai_trace::HeartbeatDue(1'000, 30'999, 30'000));
  EXPECT_TRUE(nevr_coop_ai_trace::HeartbeatDue(1'000, 31'000, 30'000));
  EXPECT_TRUE(nevr_coop_ai_trace::HeartbeatDue(0, 0, 0));
}

// A bot keeps its record across updates; a new bot gets a fresh one; a full table refuses, and the
// records already there are unaffected.
TEST(CoopAiTrace, TableKeepsEachBotsRecord) {
  BotTable<2> table;
  int a = 0, b = 0, c = 0;
  nevr_coop_ai_trace::BotRecord* ra = table.Find(&a);
  ASSERT_NE(ra, nullptr);
  ra->state.phase = 3;
  EXPECT_EQ(table.Find(&a), ra);
  EXPECT_EQ(table.Find(&a)->state.phase, 3);
  nevr_coop_ai_trace::BotRecord* rb = table.Find(&b);
  ASSERT_NE(rb, nullptr);
  EXPECT_NE(rb, ra);
  EXPECT_EQ(rb->state.phase, 0) << "a new bot starts from a fresh record";
  EXPECT_FALSE(rb->everLogged);
  EXPECT_EQ(table.Find(&c), nullptr) << "full";
  EXPECT_EQ(table.Find(&a)->state.phase, 3);
}

// Until the setup lookups run, the record says so rather than reporting a failure.
TEST(CoopAiTrace, UnrunLookupsAreDistinctFromFailures) {
  const BotState s;
  EXPECT_EQ(s.goalsOk, nevr_coop_ai_trace::kNotRun);
  EXPECT_EQ(s.waypointsOk, nevr_coop_ai_trace::kNotRun);
  EXPECT_EQ(s.lookupActor, nevr_coop_ai_trace::kActorNotRead);
  EXPECT_NE(nevr_coop_ai_trace::kActorNotRead, -1) << "-1 is the game's 'no actor'";
}
