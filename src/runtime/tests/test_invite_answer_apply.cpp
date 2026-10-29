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
