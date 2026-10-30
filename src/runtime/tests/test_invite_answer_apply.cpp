// #1415, docs/adr/0017: the per-frame applier against the real invite service, its real worker thread and a scripted loopback
// server. The writer, the join and the log are stubs (tests/invite_apply_stubs.cpp); everything else runs.
//
// Prediction, written before the change. With the service on and nothing submitted, a frame takes no service lock. A frame
// after an answer has arrived takes the answer once and posts its text once, and the frame after that posts nothing. A
// ServiceLockTakesForTest that counted nothing would pass the first claim, so the test also takes an answer and checks that
// the count moves by one.
// Falsifier: the lock count moves on a frame with nothing waiting; AnswersPending is false while an answer waits; an answer
// that arrives between two frames is not applied by the next one, or is applied twice.
#include <winsock2.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "runtime/patch/invite_answer_apply.h"
#include "runtime/patch/invite_service.h"
#include "runtime/patch/invite_wizard.h"
#include "runtime/tests/invite_apply_stubs.h"
#include "runtime/tests/loopback_http_server.h"

namespace {

using loopback::LoopbackServer;
using loopback::Script;
using nevr_invite::LogLevel;
using nevr_invite::Submit;
using nevr_invite::Switch;

constexpr auto kWait = std::chrono::seconds(10);

nevr_invite_service::Settings Settings(const std::string& uri) {
  nevr_invite_service::Settings s;
  s.invite_codes = Switch::On;
  s.http_uri = uri;
  s.token = [] { return std::string("tok-secret-1"); };
  s.log = [](LogLevel, const std::string&) {};
  s.salt = "run-salt";
  s.transport.timeout_s = 20;
  return s;
}

nevr_invite::Plan Resolve() {
  nevr_invite::Metric metric;
  metric.kind = nevr_invite::Kind::Join;
  metric.join_code = "abcd-2345";
  return nevr_invite::PlanRequest(metric, {});
}

// The service is a process-wide singleton: every test ends with Shutdown, which is also what the process does.
struct Guard {
  ~Guard() { nevr_invite_service::Shutdown(); }
};

}  // namespace

TEST(InviteAnswerApply, AFrameWithNothingWaitingTakesNoServiceLock) {
  Guard guard;
  nevr_invite_apply_test::Reset();
  LoopbackServer server({Script{}});
  nevr_invite_service::Start(Settings(server.Base()));

  const std::uint64_t before = nevr_invite_service::ServiceLockTakesForTest();
  for (int frame = 0; frame < 5; ++frame) nevr_invite_apply::OnFrame();
  EXPECT_EQ(nevr_invite_service::ServiceLockTakesForTest(), before);
  EXPECT_FALSE(nevr_invite_service::AnswersPending());
  EXPECT_TRUE(nevr_invite_apply_test::Get().posted.empty());

  // The count is live: taking the answers takes the lock once, so a seam that counted nothing fails the line above too.
  EXPECT_TRUE(nevr_invite_service::TakeAnswers().empty());
  EXPECT_EQ(nevr_invite_service::ServiceLockTakesForTest(), before + 1);
}

TEST(InviteAnswerApply, AnswersPendingReadsWithoutTheServiceLockAndSeesAWaitingAnswer) {
  Guard guard;
  nevr_invite_apply_test::Reset();
  LoopbackServer server({Script{Script::Kind::Reply, 404, R"({"message":"invalid"})", ""}});
  nevr_invite_service::Start(Settings(server.Base()));
  ASSERT_EQ(nevr_invite_service::Submit(Resolve()), Submit::Queued);
  ASSERT_TRUE(nevr_invite_service::WaitForAnswerPendingForTest(kWait));

  const std::uint64_t before = nevr_invite_service::ServiceLockTakesForTest();
  EXPECT_TRUE(nevr_invite_service::AnswersPending());
  EXPECT_EQ(nevr_invite_service::ServiceLockTakesForTest(), before);
  EXPECT_EQ(nevr_invite_service::TakeAnswers().size(), 1u);  // not lost by the read
  EXPECT_FALSE(nevr_invite_service::AnswersPending());
}

TEST(InviteAnswerApply, ShutdownLeavesNothingPending) {
  Guard guard;
  nevr_invite_apply_test::Reset();
  LoopbackServer server({Script{Script::Kind::Reply, 404, R"({"message":"invalid"})", ""}});
  nevr_invite_service::Start(Settings(server.Base()));
  ASSERT_EQ(nevr_invite_service::Submit(Resolve()), Submit::Queued);
  ASSERT_TRUE(nevr_invite_service::WaitForAnswerPendingForTest(kWait));  // an answer waits and nobody takes it

  nevr_invite_service::Shutdown();
  EXPECT_FALSE(nevr_invite_service::AnswersPending());  // the stopped worker's answer is not waiting for anyone
}

TEST(InviteAnswerApply, AnAnswerPushedBetweenFramesIsAppliedByTheNextFrameOnce) {
  Guard guard;
  nevr_invite_apply_test::Reset();
  LoopbackServer server({Script{Script::Kind::Reply, 404, R"({"message":"invalid"})", ""}});
  nevr_invite_service::Start(Settings(server.Base()));

  nevr_invite_apply::OnFrame();  // nothing submitted: nothing applied
  EXPECT_TRUE(nevr_invite_apply_test::Get().posted.empty());

  ASSERT_EQ(nevr_invite_service::Submit(Resolve()), Submit::Queued);
  ASSERT_TRUE(nevr_invite_service::WaitForAnswerPendingForTest(kWait));  // the answer arrives between the two frames
  nevr_invite_apply::OnFrame();

  const nevr_invite_apply_test::Record& record = nevr_invite_apply_test::Get();
  EXPECT_EQ(record.offered_answers, 1u);
  ASSERT_EQ(record.posted.size(), 1u);
  EXPECT_EQ(record.posted[0].text, nevr_invite::WizardText(nevr_invite::Result::Invalid));

  nevr_invite_apply::OnFrame();  // taken once: the frame after applies nothing more
  EXPECT_EQ(record.posted.size(), 1u);
  EXPECT_EQ(record.offered_answers, 1u);
}

// #1731: the share page's JOIN joins the match the player created.
//
// Prediction, written before the change. After a create answer has been applied, a share-page JOIN request makes the next frame
// offer that answer's match (lower-case UUID as the parser gives it) to the join exactly once, with a `share join requested` line
// naming it in upper case; with no create in the run, the next frame offers nothing and says why.
// Falsifier: the match offered is another (the resolve's, or none); a second frame offers it again; no line when nothing was created.
namespace {

nevr_invite::Plan Create() {
  nevr_invite::Metric metric;
  metric.kind = nevr_invite::Kind::Create;
  metric.expires_text = "3 HOURS";
  nevr_invite::Choices choices;
  choices.experience = "arena";
  choices.mode = "custom";
  return nevr_invite::PlanRequest(metric, choices);
}

constexpr char kCreated[] = R"({"code":"K7M2Q","match_id":"1fa5a0f6-f128-45a7-b1d0-72a5ab63d72f.node","expires_at":"2026-10-30T21:00:00Z"})";

bool Logged(const nevr_invite_apply_test::Record& record, const std::string& needle) {
  for (const std::string& line : record.logged) {
    if (line.find(needle) != std::string::npos) return true;
  }
  return false;
}

}  // namespace

TEST(InviteShareJoin, TheShareJoinOffersTheCreatedMatchOnceAndNamesIt) {
  Guard guard;
  nevr_invite_apply_test::Reset();
  nevr_invite_apply::ResetShareJoinForTest();
  LoopbackServer server({Script{Script::Kind::Reply, 200, kCreated, ""}});
  nevr_invite_service::Start(Settings(server.Base()));
  ASSERT_EQ(nevr_invite_service::Submit(Create()), Submit::Queued);
  ASSERT_TRUE(nevr_invite_service::WaitForAnswerPendingForTest(kWait));
  nevr_invite_apply::OnFrame();  // the create answer is applied: its match is remembered
  const nevr_invite_apply_test::Record& record = nevr_invite_apply_test::Get();
  EXPECT_TRUE(record.matches_offered.empty()) << "a create alone starts no join";

  nevr_invite_apply::RequestShareJoin();
  nevr_invite_apply::OnFrame();
  ASSERT_EQ(record.matches_offered.size(), 1u);
  EXPECT_EQ(record.matches_offered[0], "1fa5a0f6-f128-45a7-b1d0-72a5ab63d72f");
  EXPECT_TRUE(Logged(record, "[NEVR.INVITE] share join requested lobby=1FA5A0F6-F128-45A7-B1D0-72A5AB63D72F"));

  nevr_invite_apply::OnFrame();  // taken once
  EXPECT_EQ(record.matches_offered.size(), 1u);
}

TEST(InviteShareJoin, AShareJoinWithNothingCreatedOffersNothingAndSaysWhy) {
  Guard guard;
  nevr_invite_apply_test::Reset();
  nevr_invite_apply::ResetShareJoinForTest();
  LoopbackServer server({Script{}});
  nevr_invite_service::Start(Settings(server.Base()));
  nevr_invite_apply::RequestShareJoin();
  nevr_invite_apply::OnFrame();
  const nevr_invite_apply_test::Record& record = nevr_invite_apply_test::Get();
  EXPECT_TRUE(record.matches_offered.empty());
  EXPECT_TRUE(Logged(record, "[NEVR.INVITE] share join not started reason=no_created_match"));
}

TEST(InviteShareJoin, ARefusedCreateIsNoMatchToJoin) {
  Guard guard;
  nevr_invite_apply_test::Reset();
  nevr_invite_apply::ResetShareJoinForTest();
  // the refusal carries a body shaped like a created one: only an answer with a result of Ok names a match
  LoopbackServer server({Script{Script::Kind::Reply, 400, kCreated, ""}});
  nevr_invite_service::Start(Settings(server.Base()));
  ASSERT_EQ(nevr_invite_service::Submit(Create()), Submit::Queued);
  ASSERT_TRUE(nevr_invite_service::WaitForAnswerPendingForTest(kWait));
  nevr_invite_apply::OnFrame();
  nevr_invite_apply::RequestShareJoin();
  nevr_invite_apply::OnFrame();
  EXPECT_TRUE(nevr_invite_apply_test::Get().matches_offered.empty());
}
