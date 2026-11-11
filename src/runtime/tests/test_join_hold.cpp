// Join hold on the PC dedicated server (server/join_hold_rules.h): the gate's decisions on a fake clock, the config
// value and the install plan. Pure: no game image.

#include "runtime/server/join_hold_rules.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

using namespace nevr_join_hold;

Held Accept(std::uint64_t at, std::uint8_t tag, std::size_t entrants = 1) {
  Held h;
  h.bytes = {tag};
  h.entrants = entrants;
  h.arrivedMs = at;
  return h;
}

Gate Armed(std::uint32_t timeoutMs = 5000) {
  Gate g;
  g.Configure(timeoutMs);
  g.Arm(0);
  return g;
}

// A session whose single load ran 100..1000 ms and whose set was enabled at `enableAt`.
void LoadThenEnable(Gate& g, std::uint64_t enableAt) {
  g.OnLoadBegin(100);
  g.OnLoadEnd(1000);
  ASSERT_TRUE(g.OnSetEnabled(enableAt));
}

TEST(JoinHold, OffPassesEverything) {
  Gate g;  // never configured, never armed
  EXPECT_FALSE(g.Enabled());
  EXPECT_FALSE(g.Admit(Accept(10, 1)));
  EXPECT_EQ(g.QueueSize(), 0u);
  EXPECT_TRUE(g.TakeDue(20).accepts.empty());
}

TEST(JoinHold, IdleServerPasses) {
  Gate g;
  g.Configure(5000);  // on, but no session was created: not armed
  EXPECT_TRUE(g.Ready(0));
  EXPECT_FALSE(g.Admit(Accept(10, 1)));
}

TEST(JoinHold, HeldWhileLoadPending) {
  Gate g = Armed();
  g.OnLoadBegin(100);
  EXPECT_TRUE(g.Admit(Accept(200, 1)));
  EXPECT_EQ(g.Pending(), 1u);
  EXPECT_TRUE(g.TakeDue(2000).accepts.empty());  // still loading, timeout not reached
}

TEST(JoinHold, HeldAfterArmBeforeAnyLoad) {
  Gate g = Armed();
  EXPECT_FALSE(g.Ready(10000));
  EXPECT_TRUE(g.Admit(Accept(50, 1)));
}

TEST(JoinHold, LobbyEnableBeforeSessionLoadDoesNotOpen) {
  Gate g = Armed();
  EXPECT_FALSE(g.OnSetEnabled(50));  // an EnableGameSpaces left over from the lobby, no load seen since the arm
  EXPECT_FALSE(g.Ready(5000));
  g.OnLoadBegin(100);
  EXPECT_FALSE(g.OnSetEnabled(150));  // a load is in flight: not this session's set
  EXPECT_FALSE(g.Ready(5000));
}

TEST(JoinHold, ReleasedInOrderAfterSettle) {
  Gate g = Armed();
  g.OnLoadBegin(100);
  EXPECT_TRUE(g.Admit(Accept(200, 1)));
  EXPECT_TRUE(g.Admit(Accept(300, 2)));
  EXPECT_TRUE(g.Admit(Accept(400, 3)));
  g.OnLoadEnd(1000);
  ASSERT_TRUE(g.OnSetEnabled(1100));
  EXPECT_TRUE(g.TakeDue(1100 + kSettleMs - 1).accepts.empty());
  const Release r = g.TakeDue(1100 + kSettleMs);
  EXPECT_EQ(r.reason, ReleaseReason::kSettled);
  ASSERT_EQ(r.accepts.size(), 3u);
  EXPECT_EQ(r.accepts[0].bytes[0], 1);
  EXPECT_EQ(r.accepts[1].bytes[0], 2);
  EXPECT_EQ(r.accepts[2].bytes[0], 3);
  EXPECT_EQ(g.QueueSize(), 0u);
}

TEST(JoinHold, HeldAcceptNeverOvertaken) {
  Gate g = Armed();
  g.OnLoadBegin(100);
  EXPECT_TRUE(g.Admit(Accept(200, 1)));
  g.OnLoadEnd(1000);
  ASSERT_TRUE(g.OnSetEnabled(1100));
  // The gate is open now, but an accept is still queued: the new one queues behind it.
  EXPECT_TRUE(g.Admit(Accept(1400, 2)));
  const Release r = g.TakeDue(1400);
  ASSERT_EQ(r.accepts.size(), 2u);
  EXPECT_EQ(r.accepts[0].bytes[0], 1);
  EXPECT_EQ(r.accepts[1].bytes[0], 2);
  // With the queue empty and the gate open, the next one goes straight through.
  EXPECT_FALSE(g.Admit(Accept(1500, 3)));
}

TEST(JoinHold, ReleasedAtTimeoutFailsOpen) {
  Gate g = Armed(5000);
  g.OnLoadBegin(100);  // a load that never returns (a longjmp out of it)
  EXPECT_TRUE(g.Admit(Accept(1000, 1)));
  EXPECT_TRUE(g.TakeDue(5999).accepts.empty());
  const Release r = g.TakeDue(6000);
  EXPECT_EQ(r.reason, ReleaseReason::kTimeout);
  ASSERT_EQ(r.accepts.size(), 1u);
  EXPECT_EQ(g.Pending(), 0u);
  EXPECT_FALSE(g.Armed());
  EXPECT_FALSE(g.Admit(Accept(6100, 2)));  // disarmed: everything passes
}

TEST(JoinHold, ClearedOnSessionEnd) {
  Gate g = Armed();
  EXPECT_TRUE(g.Admit(Accept(10, 1)));
  EXPECT_TRUE(g.Admit(Accept(20, 2)));
  EXPECT_EQ(g.Clear(), 2u);
  EXPECT_EQ(g.QueueSize(), 0u);
  EXPECT_FALSE(g.Armed());
  EXPECT_TRUE(g.TakeDue(100000).accepts.empty());
  EXPECT_FALSE(g.Admit(Accept(100001, 3)));
}

TEST(JoinHold, ArmDropsAnEarlierSessionsAccepts) {
  Gate g = Armed();
  EXPECT_TRUE(g.Admit(Accept(10, 1)));
  EXPECT_EQ(g.Arm(500), 1u);
  EXPECT_EQ(g.QueueSize(), 0u);
}

TEST(JoinHold, ArmKeepsInFlightLoad) {
  Gate g;
  g.Configure(5000);
  g.OnLoadBegin(10);  // a load started before the session was created
  g.Arm(20);
  EXPECT_EQ(g.Pending(), 1u);
  EXPECT_FALSE(g.OnSetEnabled(30));  // still in flight
  g.OnLoadEnd(40);
  EXPECT_TRUE(g.OnSetEnabled(50));  // the in-flight load counts as the load seen since the arm
  EXPECT_TRUE(g.Ready(50 + kSettleMs));
}

TEST(JoinHold, ALoadAfterOpeningClosesItAgain) {
  Gate g = Armed();
  LoadThenEnable(g, 1100);
  EXPECT_TRUE(g.Ready(1100 + kSettleMs));
  g.OnLoadBegin(2000);
  EXPECT_FALSE(g.Ready(9000));
}

TEST(JoinHold, LoadEndNeverGoesBelowZero) {
  Gate g = Armed();
  g.OnLoadEnd(10);
  g.OnLoadEnd(20);
  EXPECT_EQ(g.Pending(), 0u);
}

TEST(JoinHold, ParseRange) {
  std::string problem = "stale";
  EXPECT_EQ(ParseJoinHoldMs("0", &problem), 0u);
  EXPECT_TRUE(problem.empty());
  EXPECT_EQ(ParseJoinHoldMs("100", &problem), 500u);
  EXPECT_FALSE(problem.empty());
  EXPECT_EQ(ParseJoinHoldMs("500", &problem), 500u);
  EXPECT_TRUE(problem.empty());
  EXPECT_EQ(ParseJoinHoldMs("15000", &problem), 15000u);
  EXPECT_TRUE(problem.empty());
  EXPECT_EQ(ParseJoinHoldMs("20000", &problem), 15000u);
  EXPECT_FALSE(problem.empty());
  EXPECT_EQ(ParseJoinHoldMs("99999999999999999999", &problem), 15000u);
  EXPECT_FALSE(problem.empty());
  EXPECT_EQ(ParseJoinHoldMs("x", &problem), 0u);
  EXPECT_FALSE(problem.empty());
  EXPECT_EQ(ParseJoinHoldMs("-5", &problem), 0u);
  EXPECT_FALSE(problem.empty());
  EXPECT_EQ(ParseJoinHoldMs(nullptr, &problem), kDefaultHoldMs);
  EXPECT_TRUE(problem.empty());
  EXPECT_EQ(ParseJoinHoldMs("", &problem), kDefaultHoldMs);
  EXPECT_TRUE(problem.empty());
  EXPECT_EQ(kDefaultHoldMs, 0u);  // owner decision D4: off unless asked
}

TEST(JoinHoldPlan, ClientLogsItDoesNotApply) {
  const InstallPlan p = PlanInstall(false, "5000");
  EXPECT_EQ(p.timeoutMs, 0u);
  ASSERT_EQ(p.lines.size(), 1u);
  EXPECT_NE(p.lines[0].text.find("join hold off reason=client"), std::string::npos);
  EXPECT_FALSE(p.lines[0].warning);
}

TEST(JoinHoldPlan, AbsentAndZeroAreOffWithTheirReason) {
  InstallPlan p = PlanInstall(true, nullptr);
  EXPECT_EQ(p.timeoutMs, 0u);
  ASSERT_EQ(p.lines.size(), 1u);
  EXPECT_NE(p.lines[0].text.find("reason=absent"), std::string::npos);
  p = PlanInstall(true, "0");
  EXPECT_EQ(p.timeoutMs, 0u);
  ASSERT_EQ(p.lines.size(), 1u);
  EXPECT_NE(p.lines[0].text.find("reason=config_0"), std::string::npos);
}

TEST(JoinHoldPlan, BadValueWarnsAndIsOff) {
  const InstallPlan p = PlanInstall(true, "soon");
  EXPECT_EQ(p.timeoutMs, 0u);
  ASSERT_EQ(p.lines.size(), 2u);
  EXPECT_TRUE(p.lines[0].warning);
  EXPECT_NE(p.lines[0].text.find("network.join_hold_ms is not a whole number"), std::string::npos);
  EXPECT_NE(p.lines[1].text.find("reason=config_invalid"), std::string::npos);
}

TEST(JoinHoldPlan, OutOfRangeWarnsAndClamps) {
  InstallPlan p = PlanInstall(true, "100");
  EXPECT_EQ(p.timeoutMs, 500u);
  ASSERT_EQ(p.lines.size(), 1u);
  EXPECT_TRUE(p.lines[0].warning);
  EXPECT_NE(p.lines[0].text.find("using 500"), std::string::npos);
  p = PlanInstall(true, "5000");
  EXPECT_EQ(p.timeoutMs, 5000u);
  EXPECT_TRUE(p.lines.empty());
}

}  // namespace
