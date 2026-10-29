// #1156 (PR-D slice D1b), docs/adr/0017: the invite service of a client (patch/invite_service.h): the config key, the address
// check, the libcurl transport and the worker together, against a scripted loopback HTTP server.
//
// Prediction, written before the service was tested. With the key on, the address a loopback server and a token held:
//   Start writes `[NEVR.INVITE] service on`; a Resolve of the typed code "ABCD2345" writes `request sent op=resolve code_len=8
//   code_fp=<8 hex>` then `answer op=resolve status=404 ... result=invalid`, and the server sees exactly one request:
//   POST /v2/rpc/invite/resolve?unwrap with `Authorization: Bearer <token>`. No line carries the code or the token.
//   With the key absent: `feature off reason=not_configured` at Info, and the server never sees a connection. A misspelt value:
//   `feature off reason=unrecognised_value` at Warning. An address that may not receive a token (plain http to a remote host):
//   `feature off reason=service_address_refused detail=plain_http_to_remote` at Warning, nothing sent.
//   Shutdown with a request held by the server returns in far less than the transport timeout, and a Submit after it is
//   `request not sent reason=stopped`.
// Falsifier: a connection with the key off or the address refused, a token or code in a line, a Shutdown that waits out the
// timeout, a second Start that changes the first one's settings.
#include <gtest/gtest.h>

#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "runtime/patch/invite_config.h"
#include "runtime/patch/invite_service.h"
#include "runtime/patch/invite_wizard.h"
#include "runtime/tests/loopback_http_server.h"

namespace {

using loopback::LoopbackServer;
using loopback::Script;
using nevr_invite::LogLevel;
using nevr_invite::Submit;
using nevr_invite::Switch;

struct Line {
  LogLevel level;
  std::string text;
};

struct Rig {
  std::mutex m;
  std::vector<Line> lines;
  nevr_invite_service::Settings Settings(Switch sw, const std::string& uri) {
    nevr_invite_service::Settings s;
    s.invite_codes = sw;
    s.http_uri = uri;
    s.token = [] { return std::string("tok-secret-1"); };
    s.log = [this](LogLevel level, const std::string& text) {
      std::lock_guard<std::mutex> lock(m);
      lines.push_back({level, text});
    };
    s.salt = "run-salt";
    s.transport.timeout_s = 20;
    return s;
  }
  std::vector<Line> Lines() {
    std::lock_guard<std::mutex> lock(m);
    return lines;
  }
};

bool Has(const std::vector<Line>& lines, LogLevel level, const std::string& needle) {
  for (const Line& l : lines) {
    if (l.level == level && l.text.find(needle) != std::string::npos) return true;
  }
  return false;
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

// Declared first on purpose: the service is a process-wide singleton and this is the state before any Start of the process.
TEST(InviteService, ASubmitBeforeAnyStartIsNotConfigured) {
  EXPECT_EQ(nevr_invite_service::Submit(Resolve()), Submit::NotConfigured);
  EXPECT_TRUE(nevr_invite_service::TakeAnswers().empty());
}

TEST(InviteService, WithTheKeyOnAResolveGoesToTheServerWithTheBearerAndTheAnswerComesBack) {
  Guard guard;
  LoopbackServer server({Script{Script::Kind::Reply, 404, R"({"message":"invalid"})", ""}});
  Rig rig;
  nevr_invite_service::Start(rig.Settings(Switch::On, server.Base()));

  EXPECT_EQ(nevr_invite_service::Submit(Resolve()), Submit::Queued);
  const auto answers = nevr_invite_service::WaitForAnswers(std::chrono::seconds(10));
  ASSERT_EQ(answers.size(), 1u);
  EXPECT_EQ(answers[0].status, 404);
  const std::string seenCode = "ABCD2345";
  EXPECT_EQ(answers[0].result, nevr_invite::Result::Invalid);

  const auto seen = server.Requests();
  ASSERT_EQ(seen.size(), 1u);
  EXPECT_EQ(seen[0].request_line, "POST /v2/rpc/invite/resolve?unwrap HTTP/1.1");
  EXPECT_EQ(seen[0].authorization, "Bearer tok-secret-1");

  const auto lines = rig.Lines();
  EXPECT_TRUE(Has(lines, LogLevel::Info, "[NEVR.INVITE] service on"));
  EXPECT_TRUE(Has(lines, LogLevel::Info, "request sent op=resolve code_len=8 code_fp=" +
                                              nevr_invite::CodeFingerprint(seenCode, "run-salt")));  // the run's salt, not none
  EXPECT_TRUE(Has(lines, LogLevel::Warning, "answer op=resolve status=404"));
  for (const Line& l : lines) {
    EXPECT_EQ(l.text.find("tok-secret-1"), std::string::npos) << l.text;
    EXPECT_EQ(l.text.find("ABCD2345"), std::string::npos) << l.text;
    EXPECT_EQ(l.text.find("abcd-2345"), std::string::npos) << l.text;
  }
}

TEST(InviteService, WithTheKeyAbsentNothingIsSentAndTheStartSaysWhy) {
  Guard guard;
  LoopbackServer server({Script{}});
  Rig rig;
  nevr_invite_service::Start(rig.Settings(Switch::Off, server.Base()));

  EXPECT_EQ(nevr_invite_service::Submit(Resolve()), Submit::NotConfigured);
  EXPECT_TRUE(Has(rig.Lines(), LogLevel::Info, "[NEVR.INVITE] feature off reason=not_configured"));
  EXPECT_TRUE(Has(rig.Lines(), LogLevel::Info, "request not sent reason=not_configured code_len=8"));
  EXPECT_TRUE(server.Requests().empty());
}

TEST(InviteService, AValueNobodyMeantIsOffAndWarned) {
  Guard guard;
  Rig rig;
  nevr_invite_service::Start(rig.Settings(Switch::Unrecognised, "https://svc.example"));
  EXPECT_TRUE(Has(rig.Lines(), LogLevel::Warning, "[NEVR.INVITE] feature off reason=unrecognised_value"));
  EXPECT_EQ(nevr_invite_service::Submit(Resolve()), Submit::NotConfigured);
}

TEST(InviteService, NoServiceAddressIsOffAndWarned) {
  Guard guard;
  Rig rig;
  nevr_invite_service::Start(rig.Settings(Switch::On, ""));
  EXPECT_TRUE(Has(rig.Lines(), LogLevel::Warning, "[NEVR.INVITE] feature off reason=no_service_address"));
  EXPECT_EQ(nevr_invite_service::Submit(Resolve()), Submit::NotConfigured);
}

TEST(InviteService, AnAddressThatMayNotReceiveTheTokenIsRefusedAtStart) {
  Guard guard;
  Rig rig;
  nevr_invite_service::Start(rig.Settings(Switch::On, "http://svc.example"));
  EXPECT_TRUE(Has(rig.Lines(), LogLevel::Warning,
                  "[NEVR.INVITE] feature off reason=service_address_refused detail=plain_http_to_remote"));
  EXPECT_EQ(nevr_invite_service::Submit(Resolve()), Submit::NotConfigured);
}

TEST(InviteService, ASecondStartChangesNothing) {
  Guard guard;
  LoopbackServer server({Script{Script::Kind::Reply, 404, R"({"message":"invalid"})", ""}});
  Rig rig;
  nevr_invite_service::Start(rig.Settings(Switch::On, server.Base()));
  nevr_invite_service::Start(rig.Settings(Switch::Off, ""));
  EXPECT_TRUE(Has(rig.Lines(), LogLevel::Warning, "service start ignored reason=already_started"));

  EXPECT_EQ(nevr_invite_service::Submit(Resolve()), Submit::Queued);
  EXPECT_EQ(nevr_invite_service::WaitForAnswers(std::chrono::seconds(10)).size(), 1u);
}

TEST(InviteService, ShutdownDoesNotWaitOutARequestTheServerHolds) {
  Guard guard;
  LoopbackServer server({Script{Script::Kind::Hold, 0, "", ""}});
  Rig rig;
  nevr_invite_service::Start(rig.Settings(Switch::On, server.Base()));
  ASSERT_EQ(nevr_invite_service::Submit(Resolve()), Submit::Queued);
  ASSERT_TRUE(server.WaitForRequests(1));  // the request is in flight and the server will not answer it

  const auto started = std::chrono::steady_clock::now();
  nevr_invite_service::Shutdown();
  const auto took = std::chrono::steady_clock::now() - started;
  EXPECT_LT(took, std::chrono::seconds(5)) << "the transport timeout is 20 s";
}

// The per-frame applier (patch/invite_answer_apply.cpp) reads Active() first: off costs it one atomic load, on takes the answers.
TEST(InviteService, ActiveIsTrueOnlyWhileTheFeatureIsOnAndNotShutDown) {
  Guard guard;
  EXPECT_FALSE(nevr_invite_service::Active());  // before any Start
  LoopbackServer server({Script{}});
  Rig rig;
  nevr_invite_service::Start(rig.Settings(Switch::Off, server.Base()));
  EXPECT_FALSE(nevr_invite_service::Active());  // started, but the key is absent
  nevr_invite_service::Shutdown();
  nevr_invite_service::Start(rig.Settings(Switch::On, ""));
  EXPECT_FALSE(nevr_invite_service::Active());  // on, with no address to send to
  nevr_invite_service::Shutdown();
  nevr_invite_service::Start(rig.Settings(Switch::On, server.Base()));
  EXPECT_TRUE(nevr_invite_service::Active());
  nevr_invite_service::Shutdown();
  EXPECT_FALSE(nevr_invite_service::Active());
}

TEST(InviteService, ASubmitAfterShutdownIsRefusedWithALine) {
  Guard guard;
  LoopbackServer server({Script{}});
  Rig rig;
  nevr_invite_service::Start(rig.Settings(Switch::On, server.Base()));
  nevr_invite_service::Shutdown();
  EXPECT_EQ(nevr_invite_service::Submit(Resolve()), Submit::Stopped);
  EXPECT_TRUE(Has(rig.Lines(), LogLevel::Info, "request not sent reason=stopped code_len=8"));
}

TEST(InviteService, ATransportFailureIsALineFromTheService) {
  Guard guard;
  Rig rig;
  nevr_invite_service::Start(rig.Settings(Switch::On, "http://127.0.0.1:1"));  // nothing listens on port 1
  ASSERT_EQ(nevr_invite_service::Submit(Resolve()), Submit::Queued);
  const auto answers = nevr_invite_service::WaitForAnswers(std::chrono::seconds(15));
  ASSERT_EQ(answers.size(), 1u);
  EXPECT_EQ(answers[0].status, 0);
  EXPECT_TRUE(Has(rig.Lines(), LogLevel::Warning, "[NEVR.INVITE] transport failed curl_code="));
}

TEST(InviteService, TakeAnswersHandsOverAnAnswerOnceWithoutWaiting) {
  Guard guard;
  LoopbackServer server({Script{Script::Kind::Reply, 404, R"({"message":"invalid"})", ""}});
  Rig rig;
  nevr_invite_service::Start(rig.Settings(Switch::On, server.Base()));
  ASSERT_EQ(nevr_invite_service::Submit(Resolve()), Submit::Queued);
  ASSERT_TRUE(server.WaitForRequests(1));
  std::vector<nevr_invite::Answer> got;
  for (int i = 0; i < 200 && got.empty(); ++i) {
    got = nevr_invite_service::TakeAnswers();
    if (got.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].status, 404);
  EXPECT_TRUE(nevr_invite_service::TakeAnswers().empty());
}
