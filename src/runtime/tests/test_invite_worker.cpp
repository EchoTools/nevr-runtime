// #1156 (PR-D slice D1), docs/adr/0017: the request worker, on the host with a fake transport and a real worker thread.
//
// Prediction, written before the code. A resolve of the typed code "ABCD2345" with a token held, the transport answering
// 404 {"message":"invalid"}, writes exactly two lines, in this order, from the worker thread:
//   [NEVR.INVITE] request sent op=resolve code_len=8 code_fp=<8 hex>
//   [NEVR.INVITE] answer op=resolve status=404 latency_ms=<n> result=invalid        (Warning)
// and hands the caller one Answer {op=resolve, sent, status=404, result=invalid}. With no token: one line,
// "request not sent reason=no_token code_len=8", no call to the transport. No line carries the code, the token or a body.
// Falsifier: any of those three in a line, a transport call with no token, or a second line for the same request.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "runtime/patch/invite_config.h"
#include "runtime/patch/invite_wizard.h"
#include "runtime/patch/invite_worker.h"

namespace {

using nevr_invite::Answer;
using nevr_invite::LogLevel;
using nevr_invite::Op;
using nevr_invite::Result;
using nevr_invite::Submit;

constexpr auto kWait = std::chrono::seconds(10);

struct Call {
  std::string rpc;
  std::string body;
  std::string bearer;
};

// A transport the test drives: it records each call, can hold the worker inside a call until released, and answers from a
// script.
class FakeTransport final : public nevr_invite::Transport {
 public:
  nevr_invite::HttpAnswer Post(std::string_view rpc, std::string_view body, std::string_view bearer) override {
    std::unique_lock<std::mutex> lock(m_);
    calls_.push_back({std::string(rpc), std::string(body), std::string(bearer)});
    ++inside_;
    cv_.notify_all();
    cv_.wait(lock, [&] { return !hold_; });
    --inside_;
    if (throws_) throw std::runtime_error("socket closed");
    return answer_;
  }

  void Hold() {
    std::lock_guard<std::mutex> lock(m_);
    hold_ = true;
  }
  void Release() {
    std::lock_guard<std::mutex> lock(m_);
    hold_ = false;
    cv_.notify_all();
  }
  void Answers(int status, std::string body) {
    std::lock_guard<std::mutex> lock(m_);
    answer_ = {status, std::move(body)};
  }
  void Throws() {
    std::lock_guard<std::mutex> lock(m_);
    throws_ = true;
  }
  bool WaitUntilInside() {
    std::unique_lock<std::mutex> lock(m_);
    return cv_.wait_for(lock, kWait, [&] { return inside_ > 0; });
  }
  bool WaitForCalls(std::size_t n) {
    std::unique_lock<std::mutex> lock(m_);
    return cv_.wait_for(lock, kWait, [&] { return calls_.size() >= n; });
  }
  std::vector<Call> Calls() {
    std::lock_guard<std::mutex> lock(m_);
    return calls_;
  }

 private:
  std::mutex m_;
  std::condition_variable cv_;
  std::vector<Call> calls_;
  nevr_invite::HttpAnswer answer_{200, "{}"};
  bool hold_ = false;
  bool throws_ = false;
  int inside_ = 0;
};

struct Line {
  LogLevel level;
  std::string text;
};

// The pieces a worker is built from, collected so a test can read what it did.
struct Rig {
  FakeTransport transport;
  std::mutex m;
  std::condition_variable cv;
  std::vector<Line> lines;
  std::string token = "tok-secret-1";
  std::int64_t now = 1000;
  bool configured = true;

  nevr_invite::WorkerParts Parts() {
    nevr_invite::WorkerParts parts;
    parts.transport = &transport;
    parts.token = [this] {
      std::lock_guard<std::mutex> lock(m);
      return token;
    };
    parts.log = [this](LogLevel level, const std::string& text) {
      std::lock_guard<std::mutex> lock(m);
      lines.push_back({level, text});
      cv.notify_all();
    };
    parts.clock_ms = [this] {
      std::lock_guard<std::mutex> lock(m);
      return now += 7;
    };
    parts.salt = "run-salt";
    parts.configured = configured;
    return parts;
  }
  void SetToken(const std::string& t) {
    std::lock_guard<std::mutex> lock(m);
    token = t;
  }
  bool WaitForLines(std::size_t n) {
    std::unique_lock<std::mutex> lock(m);
    return cv.wait_for(lock, kWait, [&] { return lines.size() >= n; });
  }
  std::vector<Line> Lines() {
    std::lock_guard<std::mutex> lock(m);
    return lines;
  }
};

nevr_invite::Plan Resolve(const std::string& typed = "abcd-2345") {
  nevr_invite::Metric metric;
  metric.kind = nevr_invite::Kind::Join;
  metric.join_code = typed;
  return nevr_invite::PlanRequest(metric, {});
}

nevr_invite::Plan Create() {
  nevr_invite::Metric metric;
  metric.kind = nevr_invite::Kind::Create;
  metric.expires_text = "3 HOURS";
  nevr_invite::Choices choices;
  choices.experience = "arena";
  choices.mode = "custom";
  choices.map = 1;
  return nevr_invite::PlanRequest(metric, choices);
}

bool Contains(const std::vector<Line>& lines, const std::string& needle) {
  for (const Line& line : lines) {
    if (line.text.find(needle) != std::string::npos) return true;
  }
  return false;
}

std::vector<Answer> WaitForAnswers(nevr_invite::Worker& worker, std::size_t n) {
  std::vector<Answer> got;
  const auto deadline = std::chrono::steady_clock::now() + kWait;
  while (got.size() < n && std::chrono::steady_clock::now() < deadline) {
    std::vector<Answer> more = worker.WaitForAnswers(std::chrono::milliseconds(100));
    for (Answer& a : more) got.push_back(std::move(a));
  }
  return got;
}

}  // namespace

TEST(InviteWorker, AResolveIsPostedWithItsRpcItsBodyAndTheBearerAndAnswersInvalid) {
  Rig rig;
  rig.transport.Answers(404, R"({"message":"invalid"})");
  nevr_invite::Worker worker(rig.Parts());

  EXPECT_EQ(worker.Enqueue(Resolve()), Submit::Queued);
  const std::vector<Answer> answers = WaitForAnswers(worker, 1);
  ASSERT_EQ(answers.size(), 1u);
  EXPECT_EQ(answers[0].op, Op::Resolve);
  EXPECT_TRUE(answers[0].sent);
  EXPECT_EQ(answers[0].status, 404);
  EXPECT_EQ(answers[0].result, Result::Invalid);

  const std::vector<Call> calls = rig.transport.Calls();
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].rpc, "invite/resolve");
  EXPECT_EQ(calls[0].body, R"({"code":"ABCD2345"})");
  EXPECT_EQ(calls[0].bearer, "tok-secret-1");
}

TEST(InviteWorker, TheRequestLineThenTheAnswerLineAreTheOnlyLinesAndCarryNoSecret) {
  Rig rig;
  rig.transport.Answers(404, R"({"message":"invalid","detail":"body-detail-text"})");
  nevr_invite::Worker worker(rig.Parts());
  ASSERT_EQ(worker.Enqueue(Resolve()), Submit::Queued);
  ASSERT_EQ(WaitForAnswers(worker, 1).size(), 1u);
  worker.Stop();

  const std::vector<Line> lines = rig.Lines();
  ASSERT_EQ(lines.size(), 2u);
  const std::string fp = nevr_invite::CodeFingerprint("ABCD2345", "run-salt");
  EXPECT_EQ(lines[0].text, "[NEVR.INVITE] request sent op=resolve code_len=8 code_fp=" + fp);
  EXPECT_EQ(lines[0].level, LogLevel::Info);
  EXPECT_EQ(lines[1].text, "[NEVR.INVITE] answer op=resolve status=404 latency_ms=7 result=invalid");
  EXPECT_EQ(lines[1].level, LogLevel::Warning);
  EXPECT_FALSE(Contains(lines, "ABCD2345"));
  EXPECT_FALSE(Contains(lines, "tok-secret-1"));
  EXPECT_FALSE(Contains(lines, "body-detail-text"));
}

TEST(InviteWorker, ARefusalTheServiceDidNotNameSaysTheStatusDecidedItAndTheBodyStaysOut) {
  struct Case {
    int status;
    const char* body;
    const char* line;
  };
  const Case cases[] = {
      {404, R"({"code":5,"message":"RPC function not found"})",
       "[NEVR.INVITE] answer op=resolve status=404 latency_ms=7 result=invalid via=status"},
      {401, R"({"code":16,"message":"Auth token invalid"})",
       "[NEVR.INVITE] answer op=resolve status=401 latency_ms=7 result=server_error via=status"},
  };
  for (const Case& c : cases) {
    Rig rig;
    rig.transport.Answers(c.status, c.body);
    nevr_invite::Worker worker(rig.Parts());
    ASSERT_EQ(worker.Enqueue(Resolve()), Submit::Queued);
    const std::vector<Answer> answers = WaitForAnswers(worker, 1);
    ASSERT_EQ(answers.size(), 1u);
    EXPECT_TRUE(answers[0].via_status) << c.status;
    worker.Stop();
    const std::vector<Line> lines = rig.Lines();
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[1].text, c.line);
    EXPECT_FALSE(Contains(lines, "not found"));
    EXPECT_FALSE(Contains(lines, "Auth token"));
  }
}

TEST(InviteWorker, AnUnnamedRefusalHandsItsMessageBackForTheTestEndpointAndNeverLogsIt) {
  Rig rig;
  rig.transport.Answers(503, R"({"code":14,"message":"no servers available"})");
  nevr_invite::Worker worker(rig.Parts());
  ASSERT_EQ(worker.Enqueue(Resolve()), Submit::Queued);
  const std::vector<Answer> answers = WaitForAnswers(worker, 1);
  ASSERT_EQ(answers.size(), 1u);
  EXPECT_EQ(answers[0].refusal_message, "no servers available");
  EXPECT_TRUE(answers[0].body.empty());
  worker.Stop();
  EXPECT_FALSE(Contains(rig.Lines(), "no servers"));
}

TEST(InviteWorker, ARefusalTheServiceNamedCarriesNoMessage) {
  Rig rig;
  rig.transport.Answers(400, R"({"code":3,"message":"invalid"})");
  nevr_invite::Worker worker(rig.Parts());
  ASSERT_EQ(worker.Enqueue(Resolve()), Submit::Queued);
  const std::vector<Answer> answers = WaitForAnswers(worker, 1);
  ASSERT_EQ(answers.size(), 1u);
  EXPECT_TRUE(answers[0].refusal_message.empty());
}

TEST(InviteWorker, AResultTheServiceNamedIsNotViaStatus) {
  Rig rig;
  rig.transport.Answers(404, R"({"message":"invalid"})");
  nevr_invite::Worker worker(rig.Parts());
  ASSERT_EQ(worker.Enqueue(Resolve()), Submit::Queued);
  const std::vector<Answer> answers = WaitForAnswers(worker, 1);
  ASSERT_EQ(answers.size(), 1u);
  EXPECT_FALSE(answers[0].via_status);
}

TEST(InviteWorker, NoAnswerAtAllIsNotViaStatus) {
  Rig rig;
  rig.transport.Answers(0, "");
  nevr_invite::Worker worker(rig.Parts());
  ASSERT_EQ(worker.Enqueue(Resolve()), Submit::Queued);
  const std::vector<Answer> answers = WaitForAnswers(worker, 1);
  ASSERT_EQ(answers.size(), 1u);
  EXPECT_FALSE(answers[0].via_status);
  worker.Stop();
  EXPECT_FALSE(Contains(rig.Lines(), "via=status"));
}

TEST(InviteWorker, ASuccessIsLoggedAtInfoAndItsBodyIsHandedBackNotLogged) {
  Rig rig;
  rig.transport.Answers(200, R"({"code":"KQ7M2X4P","match_id":"2f1c9b0e-6a3d-4c58-9d11-0a7e5b3c4d21"})");
  nevr_invite::Worker worker(rig.Parts());
  ASSERT_EQ(worker.Enqueue(Create()), Submit::Queued);
  const std::vector<Answer> answers = WaitForAnswers(worker, 1);
  ASSERT_EQ(answers.size(), 1u);
  EXPECT_EQ(answers[0].op, Op::Create);
  EXPECT_EQ(answers[0].result, Result::Ok);
  EXPECT_NE(answers[0].body.find("KQ7M2X4P"), std::string::npos);
  worker.Stop();

  const std::vector<Line> lines = rig.Lines();
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0].text, "[NEVR.INVITE] request sent op=create expires_s=10800");
  EXPECT_EQ(lines[1].text, "[NEVR.INVITE] answer op=create status=200 latency_ms=7 result=ok");
  EXPECT_EQ(lines[1].level, LogLevel::Info);
  EXPECT_FALSE(Contains(lines, "KQ7M2X4P"));
}

TEST(InviteWorker, AFailureCarriesNoBodyBack) {
  Rig rig;
  rig.transport.Answers(404, R"({"message":"invalid","detail":"body-detail-text"})");
  nevr_invite::Worker worker(rig.Parts());
  ASSERT_EQ(worker.Enqueue(Resolve()), Submit::Queued);
  const std::vector<Answer> answers = WaitForAnswers(worker, 1);
  ASSERT_EQ(answers.size(), 1u);
  EXPECT_TRUE(answers[0].body.empty());
}

TEST(InviteWorker, TheTokenIsReadWhenTheRequestIsSentNotWhenItWasQueued) {
  Rig rig;
  nevr_invite::Worker worker(rig.Parts());
  rig.transport.Hold();
  ASSERT_EQ(worker.Enqueue(Resolve("AAAA2222")), Submit::Queued);
  ASSERT_TRUE(rig.transport.WaitUntilInside());  // the first request is in flight with the first token
  ASSERT_EQ(worker.Enqueue(Resolve("BBBB3333")), Submit::Queued);
  rig.SetToken("tok-secret-2");  // refreshed while the second waits
  rig.transport.Release();
  ASSERT_EQ(WaitForAnswers(worker, 2).size(), 2u);

  const std::vector<Call> calls = rig.transport.Calls();
  ASSERT_EQ(calls.size(), 2u);
  EXPECT_EQ(calls[0].bearer, "tok-secret-1");
  EXPECT_EQ(calls[1].bearer, "tok-secret-2");
}

TEST(InviteWorker, WithNoTokenNothingIsSentAndTheLineSaysSo) {
  Rig rig;
  rig.SetToken("");
  nevr_invite::Worker worker(rig.Parts());
  ASSERT_EQ(worker.Enqueue(Resolve()), Submit::Queued);
  const std::vector<Answer> answers = WaitForAnswers(worker, 1);
  ASSERT_EQ(answers.size(), 1u);
  EXPECT_FALSE(answers[0].sent);
  EXPECT_EQ(answers[0].result, Result::Unavailable);
  worker.Stop();

  EXPECT_TRUE(rig.transport.Calls().empty());
  const std::vector<Line> lines = rig.Lines();
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].text, "[NEVR.INVITE] request not sent reason=no_token code_len=8");
}

TEST(InviteWorker, WhenTheFeatureIsOffNothingIsQueuedOrSentAndTheLineSaysSo) {
  Rig rig;
  rig.configured = false;
  nevr_invite::Worker worker(rig.Parts());
  EXPECT_EQ(worker.Enqueue(Resolve()), Submit::FeatureOff);
  worker.Stop();

  EXPECT_TRUE(rig.transport.Calls().empty());
  const std::vector<Line> lines = rig.Lines();
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].text, "[NEVR.INVITE] request not sent reason=feature_off code_len=8");
  EXPECT_TRUE(worker.TakeAnswers().empty());
}

TEST(InviteWorker, APlanWithNoRequestIsNeverQueued) {
  Rig rig;
  nevr_invite::Worker worker(rig.Parts());
  nevr_invite::Plan none;  // no request: what PlanRequest returns for a skipped click
  none.skip = nevr_invite::Skip::Empty;
  EXPECT_EQ(worker.Enqueue(none), Submit::NoRequest);
  worker.Stop();
  EXPECT_TRUE(rig.transport.Calls().empty());
  EXPECT_TRUE(rig.Lines().empty());
}

TEST(InviteWorker, AFullQueueRefusesTheNextRequestWithALineAndLosesNoneOfTheQueuedOnes) {
  Rig rig;
  nevr_invite::Worker worker(rig.Parts());
  rig.transport.Hold();
  ASSERT_EQ(worker.Enqueue(Resolve("AAAA2222")), Submit::Queued);
  ASSERT_TRUE(rig.transport.WaitUntilInside());  // one in flight; the queue behind it is empty

  std::size_t queued = 0;
  std::size_t refused = 0;
  for (std::size_t i = 0; i < nevr_invite::kMaxQueued + 3; ++i) {
    const Submit s = worker.Enqueue(Resolve("BBBB3333"));
    if (s == Submit::Queued) ++queued;
    if (s == Submit::Busy) ++refused;
  }
  EXPECT_EQ(queued, nevr_invite::kMaxQueued);
  EXPECT_EQ(refused, 3u);
  rig.transport.Release();
  ASSERT_EQ(WaitForAnswers(worker, 1 + nevr_invite::kMaxQueued).size(), 1 + nevr_invite::kMaxQueued);
  worker.Stop();

  EXPECT_EQ(rig.transport.Calls().size(), 1 + nevr_invite::kMaxQueued);
  std::size_t busyLines = 0;
  for (const Line& line : rig.Lines()) {
    if (line.text == "[NEVR.INVITE] request not sent reason=busy code_len=8") ++busyLines;
  }
  EXPECT_EQ(busyLines, 3u);
}

TEST(InviteWorker, AnswersNobodyTookAreBoundedTheOldestGoAndTheLossIsLogged) {
  Rig rig;
  nevr_invite::Worker worker(rig.Parts());
  const std::size_t total = nevr_invite::kMaxAnswers + 4;
  for (std::size_t done = 1; done <= total; ++done) {
    // One at a time, so the order is known; nothing takes an answer. A request writes two lines, and once the answers are
    // full each further one writes a third (the drop), after its answer line.
    ASSERT_EQ(worker.Enqueue(Resolve("AAAA2222")), Submit::Queued);
    const std::size_t drops = done > nevr_invite::kMaxAnswers ? done - nevr_invite::kMaxAnswers : 0;
    ASSERT_TRUE(rig.WaitForLines(2 * done + drops));
  }
  worker.Stop();

  const std::vector<Answer> kept = worker.TakeAnswers();
  EXPECT_EQ(kept.size(), nevr_invite::kMaxAnswers);
  std::size_t dropLines = 0;
  for (const Line& line : rig.Lines()) {
    if (line.text.find("[NEVR.INVITE] answers dropped") == 0) {
      ++dropLines;
      EXPECT_EQ(line.level, LogLevel::Warning);
    }
  }
  EXPECT_EQ(dropLines, 4u);
}

TEST(InviteWorker, ATransportThatThrowsIsAnsweredUnavailableAndTheWorkerGoesOn) {
  Rig rig;
  rig.transport.Throws();
  nevr_invite::Worker worker(rig.Parts());
  ASSERT_EQ(worker.Enqueue(Resolve()), Submit::Queued);
  ASSERT_EQ(worker.Enqueue(Resolve("BBBB3333")), Submit::Queued);
  const std::vector<Answer> answers = WaitForAnswers(worker, 2);
  ASSERT_EQ(answers.size(), 2u);
  for (const Answer& a : answers) {
    EXPECT_TRUE(a.sent);
    EXPECT_EQ(a.status, 0);
    EXPECT_EQ(a.result, Result::Unavailable);
  }
  worker.Stop();
  EXPECT_TRUE(Contains(rig.Lines(), "[NEVR.INVITE] answer op=resolve status=0 latency_ms=7 result=unavailable"));
}

TEST(InviteWorker, StopWaitsForTheRequestInFlightAndDropsTheQueuedOnesWithALine) {
  Rig rig;
  nevr_invite::Worker worker(rig.Parts());
  rig.transport.Hold();
  ASSERT_EQ(worker.Enqueue(Resolve("AAAA2222")), Submit::Queued);
  ASSERT_TRUE(rig.transport.WaitUntilInside());
  ASSERT_EQ(worker.Enqueue(Resolve("BBBB3333")), Submit::Queued);
  ASSERT_EQ(worker.Enqueue(Resolve("CCCC4444")), Submit::Queued);

  std::mutex m;
  std::condition_variable cv;
  bool stopped = false;
  std::thread stopper([&] {
    worker.Stop();
    std::lock_guard<std::mutex> lock(m);
    stopped = true;
    cv.notify_all();
  });
  {
    std::unique_lock<std::mutex> lock(m);
    // The request in flight is held, so Stop cannot have returned; a Stop that did not join would have.
    EXPECT_FALSE(cv.wait_for(lock, std::chrono::milliseconds(300), [&] { return stopped; }));
  }
  rig.transport.Release();
  stopper.join();

  EXPECT_EQ(rig.transport.Calls().size(), 1u);  // the queued two were never sent
  EXPECT_TRUE(Contains(rig.Lines(), "[NEVR.INVITE] worker stopped dropped=2"));
}

TEST(InviteWorker, AfterStopARequestIsRefusedWithALine) {
  Rig rig;
  nevr_invite::Worker worker(rig.Parts());
  worker.Stop();
  EXPECT_EQ(worker.Enqueue(Resolve()), Submit::Stopped);
  EXPECT_TRUE(Contains(rig.Lines(), "[NEVR.INVITE] request not sent reason=stopped code_len=8"));
  worker.Stop();  // a second Stop is harmless
  EXPECT_TRUE(rig.transport.Calls().empty());
}

TEST(InviteWorker, TakeAnswersHandsEachAnswerOverOnce) {
  Rig rig;
  nevr_invite::Worker worker(rig.Parts());
  ASSERT_EQ(worker.Enqueue(Resolve()), Submit::Queued);
  ASSERT_EQ(WaitForAnswers(worker, 1).size(), 1u);
  EXPECT_TRUE(worker.TakeAnswers().empty());
}

// ---- network.invite_codes (N133) ----

// On unless the key says off (ADR 0017). An absent key and an empty one are both unset, and unset is the default.
TEST(InviteConfig, TheFeatureIsOnWhenTheKeyIsUnset) {
  using nevr_invite::ParseSwitch;
  using nevr_invite::Switch;
  EXPECT_EQ(ParseSwitch(nullptr), Switch::On);
  EXPECT_EQ(ParseSwitch(""), Switch::On);
  for (const char* on : {"true", "TRUE", "True", "on", "On", "yes", "1"}) EXPECT_EQ(ParseSwitch(on), Switch::On) << on;
}

TEST(InviteConfig, FalseTurnsTheFeatureOff) {
  using nevr_invite::ParseSwitch;
  using nevr_invite::Switch;
  for (const char* off : {"false", "FALSE", "off", "no", "0"}) EXPECT_EQ(ParseSwitch(off), Switch::Off) << off;
}

TEST(InviteConfig, TheDefaultIsOnAndOnlyAnUnsetKeyIsTheDefaultsSource) {
  using nevr_invite::Source;
  using nevr_invite::SourceOf;
  using nevr_invite::Switch;
  EXPECT_EQ(nevr_invite::kDefault, Switch::On);
  EXPECT_EQ(SourceOf(nullptr), Source::Default);
  EXPECT_EQ(SourceOf(""), Source::Default);
  EXPECT_EQ(SourceOf("false"), Source::Config);
  EXPECT_EQ(SourceOf("ture"), Source::Config);  // a value nobody meant is the config's, not the default's
  EXPECT_STREQ(nevr_invite::SourceName(Source::Default), "default");
  EXPECT_STREQ(nevr_invite::SourceName(Source::Config), "config");
}

TEST(InviteConfig, AValueNobodyMeantIsOffAndSaidToBeUnrecognised) {
  using nevr_invite::ParseSwitch;
  using nevr_invite::Switch;
  for (const char* odd : {"enabled", "2", "tru", " true", "true ", "ture"}) EXPECT_EQ(ParseSwitch(odd), Switch::Unrecognised) << odd;
  EXPECT_FALSE(nevr_invite::SwitchIsOn(Switch::Unrecognised));
  EXPECT_TRUE(nevr_invite::SwitchIsOn(Switch::On));
  EXPECT_FALSE(nevr_invite::SwitchIsOn(Switch::Off));
}

// A list or a map under the key is not a value. It is off and reported as unrecognised, and its source is the config's: the
// default must never turn a feature on because the key was written in a shape that is not a switch (a token is sent when on).
TEST(InviteConfig, AStructuredValueIsOffAndItsSourceIsTheConfigsNotTheDefaults) {
  using nevr_invite::ParseSwitch;
  using nevr_invite::Source;
  using nevr_invite::SourceOf;
  using nevr_invite::Switch;
  EXPECT_EQ(ParseSwitch(nullptr, /*structured=*/true), Switch::Unrecognised);
  EXPECT_EQ(SourceOf(nullptr, /*structured=*/true), Source::Config);
  EXPECT_EQ(ParseSwitch(nullptr, /*structured=*/false), Switch::On);  // the same key with nothing in it is still the default
  EXPECT_EQ(SourceOf(nullptr, /*structured=*/false), Source::Default);
}
