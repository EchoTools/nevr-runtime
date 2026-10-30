// #1156 (PR-D slice D1b), docs/adr/0017: the invite service of a client (patch/invite_service.h): the config key, the address
// check, the libcurl transport and the worker together, against a scripted loopback HTTP server.
//
// Prediction, written before the service was tested. With the key on, the address a loopback server and a token held:
//   Start writes `[NEVR.INVITE] service on`; a Resolve of the typed code "ABCD2345" writes `request sent op=resolve code_len=8
//   code_fp=<8 hex>` then `answer op=resolve status=404 ... result=invalid`, and the server sees exactly one request:
//   POST /v2/rpc/invite/resolve?unwrap with `Authorization: Bearer <token>`. No line carries the code or the token.
//   With the key absent (the default, ADR 0017): `service on source=default` at Info, and a resolve goes out as above.
//   With the key `false`: `feature off reason=turned_off source=config` at Info, `request not sent reason=not_configured` for a
//   Submit, and the server never sees a connection.
//   A misspelt value: `feature off reason=unrecognised_value source=config` at Warning. An address that may not receive a
//   token (plain http to a remote host): `feature off reason=service_address_refused detail=plain_http_to_remote` at Warning,
//   nothing sent.
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
#include "runtime/patch/invite_match_api.h"
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
  nevr_invite_service::Settings Settings(Switch sw, const std::string& uri, nevr_invite::Source source = nevr_invite::Source::Config) {
    nevr_invite_service::Settings s;
    s.invite_codes = sw;
    s.invite_source = source;
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
  EXPECT_TRUE(Has(lines, LogLevel::Info, "[NEVR.INVITE] service on source=config"));
  EXPECT_TRUE(Has(lines, LogLevel::Info, "request sent op=resolve code_len=8 code_fp=" +
                                              nevr_invite::CodeFingerprint(seenCode, "run-salt")));  // the run's salt, not none
  EXPECT_TRUE(Has(lines, LogLevel::Warning, "answer op=resolve status=404"));
  for (const Line& l : lines) {
    EXPECT_EQ(l.text.find("tok-secret-1"), std::string::npos) << l.text;
    EXPECT_EQ(l.text.find("ABCD2345"), std::string::npos) << l.text;
    EXPECT_EQ(l.text.find("abcd-2345"), std::string::npos) << l.text;
  }
}

// The default (ADR 0017): an absent key is on. boot.cpp takes the value from nevr_invite::ParseSwitch and the source from
// nevr_invite::SourceOf, read from the same NevrCfgGetFlat value; this is that pair with the key absent.
TEST(InviteService, WithTheKeyAbsentTheServiceStartsAndTheStartSaysDefault) {
  Guard guard;
  LoopbackServer server({Script{Script::Kind::Reply, 404, R"({"message":"invalid"})", ""}});
  Rig rig;
  nevr_invite_service::Start(rig.Settings(nevr_invite::ParseSwitch(nullptr), server.Base(), nevr_invite::SourceOf(nullptr)));

  EXPECT_TRUE(nevr_invite_service::Active());
  EXPECT_EQ(nevr_invite_service::Submit(Resolve()), Submit::Queued);
  ASSERT_EQ(nevr_invite_service::WaitForAnswers(std::chrono::seconds(10)).size(), 1u);
  EXPECT_EQ(server.Requests().size(), 1u);
  EXPECT_TRUE(Has(rig.Lines(), LogLevel::Info, "[NEVR.INVITE] service on source=default"));
}

// `false` is the operator's own choice: nothing goes out, and the line says the config made it.
TEST(InviteService, OffWithNoConfigBehindItSaysNotConfiguredNotTurnedOff) {
  // A host whose own default is off (the Quest sentinel) passes Off from Source::Default: no player turned it off.
  Guard guard;
  LoopbackServer server({Script{}});
  Rig rig;
  nevr_invite_service::Start(rig.Settings(nevr_invite::Switch::Off, server.Base(), nevr_invite::Source::Default));

  EXPECT_FALSE(nevr_invite_service::Active());
  EXPECT_TRUE(Has(rig.Lines(), LogLevel::Info, "[NEVR.INVITE] feature off reason=not_configured source=default"));
  EXPECT_FALSE(Has(rig.Lines(), LogLevel::Info, "reason=turned_off"));
  EXPECT_TRUE(server.Requests().empty());
}

TEST(InviteService, WithTheKeyFalseNothingIsSentAndTheStartSaysItIsConfigured) {
  Guard guard;
  LoopbackServer server({Script{}});
  Rig rig;
  nevr_invite_service::Start(rig.Settings(nevr_invite::ParseSwitch("false"), server.Base(), nevr_invite::SourceOf("false")));

  EXPECT_FALSE(nevr_invite_service::Active());
  EXPECT_EQ(nevr_invite_service::Submit(Resolve()), Submit::NotConfigured);
  EXPECT_TRUE(Has(rig.Lines(), LogLevel::Info, "[NEVR.INVITE] feature off reason=turned_off source=config"));
  EXPECT_TRUE(Has(rig.Lines(), LogLevel::Info, "request not sent reason=not_configured code_len=8"));
  EXPECT_TRUE(server.Requests().empty());
}

// The struct's own default is the feature's default, so a caller that sets nothing gets the same answer as an absent key.
TEST(InviteService, ASettingsStructWithNothingSetIsOnFromTheDefault) {
  nevr_invite_service::Settings s;
  EXPECT_EQ(s.invite_codes, nevr_invite::kDefault);
  EXPECT_EQ(s.invite_codes, Switch::On);
  EXPECT_EQ(s.invite_source, nevr_invite::Source::Default);
}

TEST(InviteService, AValueNobodyMeantIsOffAndWarned) {
  Guard guard;
  Rig rig;
  nevr_invite_service::Start(rig.Settings(Switch::Unrecognised, "https://svc.example"));
  EXPECT_TRUE(Has(rig.Lines(), LogLevel::Warning, "[NEVR.INVITE] feature off reason=unrecognised_value source=config"));
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
                  "[NEVR.INVITE] feature off reason=service_address_refused detail=plain_http_to_remote source=config"));
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
  EXPECT_FALSE(nevr_invite_service::Active());  // started with the key false, so off
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

// #1414: a hook this service serves refused to install (its prologue did not match). The hook's own Warning names the address
// and the bytes; the service stays off, says why in one Warning, refuses each request with a line, and contacts no server.
TEST(InviteService, AHookThatRefusedToInstallKeepsTheServiceOffAndSaysWhy) {
  Guard guard;
  LoopbackServer server({Script{}});
  Rig rig;
  nevr_invite_service::Settings settings = rig.Settings(Switch::On, server.Base());
  settings.hooks_refused = true;
  nevr_invite_service::Start(std::move(settings));

  EXPECT_FALSE(nevr_invite_service::Active());
  EXPECT_TRUE(Has(rig.Lines(), LogLevel::Warning, "[NEVR.INVITE] feature off reason=hook_prologue_mismatch"));
  int offWarnings = 0;
  for (const Line& l : rig.Lines()) {
    if (l.level == LogLevel::Warning && l.text.find("feature off") != std::string::npos) ++offWarnings;
  }
  EXPECT_EQ(offWarnings, 1);
  EXPECT_FALSE(Has(rig.Lines(), LogLevel::Info, "[NEVR.INVITE] service on"));
  EXPECT_EQ(nevr_invite_service::Submit(Resolve()), Submit::NotConfigured);
  EXPECT_TRUE(Has(rig.Lines(), LogLevel::Info, "request not sent reason=not_configured code_len=8"));
  EXPECT_TRUE(server.Requests().empty());
}

// #1524: the frame path's take of a resolved match (nevr_invite_match::TakeResolvedMatch, invite_service.cpp), driven through
// the real service and a loopback server. The match queue is process-wide, so every test drains it first and its Guard shuts
// the service down. Falsifier: a Resolve Ok id not taken, taken twice, or taken from a Create or a non-Ok answer; a body that
// is no match id dropped instead of taken as an empty id; a capacity too small taken as the id instead of length 0.
namespace {

constexpr const char* kMatchIdA = "2f1c9b0e-6a3d-4c58-9d11-0a7e5b3c4d21";
constexpr const char* kMatchIdB = "0f8fad5b-d9cb-469f-a165-70867728950e";

struct Taken {
  bool ok = false;
  std::string id;
};

Taken TakeMatch(std::size_t capacity = nevr_invite_match::kMaxMatchIdBytes) {
  char buffer[nevr_invite_match::kMaxMatchIdBytes] = {};
  std::size_t length = 99;
  Taken taken;
  taken.ok = nevr_invite_match::TakeResolvedMatch(buffer, capacity, &length);
  if (taken.ok) taken.id.assign(buffer, length);
  return taken;
}

void DrainMatches() {
  while (TakeMatch().ok) {
  }
}

nevr_invite::Plan CreatePlan() {
  nevr_invite::Metric metric;
  metric.kind = nevr_invite::Kind::Create;
  nevr_invite::Choices choices;  // a create without the wizard's choices is Skip::Incomplete and never reaches the service
  choices.experience = "arena";
  choices.mode = "custom";
  return nevr_invite::PlanRequest(metric, choices);
}

}  // namespace

TEST(InviteMatch, AResolveOkIsTakenOnceAndThenNothingIsWaiting) {
  DrainMatches();
  Guard guard;
  const std::string body = std::string(R"({"match_id":")") + kMatchIdA + R"("})";
  LoopbackServer server({Script{Script::Kind::Reply, 200, body, ""}});
  Rig rig;
  nevr_invite_service::Start(rig.Settings(Switch::On, server.Base()));

  ASSERT_EQ(nevr_invite_service::Submit(Resolve()), Submit::Queued);
  ASSERT_TRUE(nevr_invite_service::WaitForAnswerPendingForTest(std::chrono::seconds(10)));
  EXPECT_TRUE(nevr_invite_match::ResolvedMatchPending());

  const Taken first = TakeMatch();
  EXPECT_TRUE(first.ok);
  EXPECT_EQ(first.id, kMatchIdA);
  EXPECT_FALSE(nevr_invite_match::ResolvedMatchPending());
  EXPECT_FALSE(TakeMatch().ok);
}

TEST(InviteMatch, TwoResolveOksAreTakenOneEachInTheOrderTheyArrived) {
  DrainMatches();
  Guard guard;
  LoopbackServer server({Script{Script::Kind::Reply, 200, std::string(R"({"match_id":")") + kMatchIdA + R"("})", ""},
                         Script{Script::Kind::Reply, 200, std::string(R"({"match_id":")") + kMatchIdB + R"("})", ""}});
  Rig rig;
  nevr_invite_service::Start(rig.Settings(Switch::On, server.Base()));

  ASSERT_EQ(nevr_invite_service::Submit(Resolve()), Submit::Queued);
  ASSERT_EQ(nevr_invite_service::Submit(Resolve()), Submit::Queued);
  std::vector<std::string> ids;
  for (int i = 0; i < 400 && ids.size() < 2; ++i) {
    const Taken t = TakeMatch();
    if (t.ok) {
      ids.push_back(t.id);
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
  }
  EXPECT_EQ(ids, (std::vector<std::string>{kMatchIdA, kMatchIdB}));
  EXPECT_FALSE(TakeMatch().ok);
}

// ParseResolved keeps only the UUID of a match id, so a body that parses is never longer than the 63-byte cap; a body that
// does not parse is still one answer, taken as an empty id.
TEST(InviteMatch, A200WhoseBodyIsNoMatchIdIsTakenAsAnEmptyIdNotDropped) {
  DrainMatches();
  Guard guard;
  LoopbackServer server({Script{Script::Kind::Reply, 200, R"({"mode":"echo_arena"})", ""}});
  Rig rig;
  nevr_invite_service::Start(rig.Settings(Switch::On, server.Base()));

  ASSERT_EQ(nevr_invite_service::Submit(Resolve()), Submit::Queued);
  ASSERT_TRUE(nevr_invite_service::WaitForAnswerPendingForTest(std::chrono::seconds(10)));
  const Taken got = TakeMatch();
  EXPECT_TRUE(got.ok);
  EXPECT_TRUE(got.id.empty());
  EXPECT_FALSE(TakeMatch().ok);
}

// A Resolve answer that is not Ok (404 is Invalid) is answered but never taken as a match.
TEST(InviteMatch, ANonOkResolveIsAnsweredButNotTaken) {
  DrainMatches();
  Guard guard;
  LoopbackServer server({Script{Script::Kind::Reply, 404, R"({"message":"invalid"})", ""}});
  Rig rig;
  nevr_invite_service::Start(rig.Settings(Switch::On, server.Base()));

  ASSERT_EQ(nevr_invite_service::Submit(Resolve()), Submit::Queued);
  ASSERT_TRUE(nevr_invite_service::WaitForAnswerPendingForTest(std::chrono::seconds(10)));
  EXPECT_FALSE(TakeMatch().ok);
  EXPECT_FALSE(nevr_invite_match::ResolvedMatchPending());
}

// A Create answer carries a match id too; it is not a resolved match and is never taken as one.
TEST(InviteMatch, ACreateAnswerWithAMatchIdIsIgnoredByTheResolveTake) {
  DrainMatches();
  Guard guard;
  const std::string body = std::string(R"({"code":"K7M2Q","match_id":")") + kMatchIdA + R"("})";
  LoopbackServer server({Script{Script::Kind::Reply, 200, body, ""}});
  Rig rig;
  nevr_invite_service::Start(rig.Settings(Switch::On, server.Base()));

  const nevr_invite::Plan plan = CreatePlan();
  ASSERT_EQ(plan.op, nevr_invite::Op::Create);
  ASSERT_EQ(nevr_invite_service::Submit(plan), Submit::Queued);
  ASSERT_TRUE(nevr_invite_service::WaitForAnswerPendingForTest(std::chrono::seconds(10)));
  EXPECT_FALSE(TakeMatch().ok);
}

// The capacity is the caller's buffer: an id longer than it is taken as length 0, and still counts as one answer.
TEST(InviteMatch, AnIdLongerThanTheCallersBufferIsTakenAsLengthZero) {
  DrainMatches();
  Guard guard;
  const std::string body = std::string(R"({"match_id":")") + kMatchIdA + R"("})";
  LoopbackServer server({Script{Script::Kind::Reply, 200, body, ""}});
  Rig rig;
  nevr_invite_service::Start(rig.Settings(Switch::On, server.Base()));

  ASSERT_EQ(nevr_invite_service::Submit(Resolve()), Submit::Queued);
  ASSERT_TRUE(nevr_invite_service::WaitForAnswerPendingForTest(std::chrono::seconds(10)));
  const Taken got = TakeMatch(8);
  EXPECT_TRUE(got.ok);
  EXPECT_TRUE(got.id.empty());
  EXPECT_FALSE(TakeMatch().ok);
}

// Nothing waiting: false, with no service started, and a null output or length is refused without a take.
TEST(InviteMatch, NothingWaitingIsFalseAndANullArgumentIsRefused) {
  DrainMatches();
  EXPECT_FALSE(TakeMatch().ok);
  EXPECT_FALSE(nevr_invite_match::ResolvedMatchPending());
  std::size_t length = 0;
  char buffer[8] = {};
  EXPECT_FALSE(nevr_invite_match::TakeResolvedMatch(nullptr, sizeof buffer, &length));
  EXPECT_FALSE(nevr_invite_match::TakeResolvedMatch(buffer, sizeof buffer, nullptr));
}

// The texts the answers ask the wizard's pages to show (nevr_invite_match::TakeShareText), filed by the same take as the matches.
// Falsifier: a created code that leaves the share page's link, or a refused resolve that leaves the entry page's title, unfiled.
namespace {

std::vector<nevr_invite_match::ShareText> TakeTexts() {
  std::vector<nevr_invite_match::ShareText> out;
  nevr_invite_match::ShareText text;
  while (nevr_invite_match::TakeShareText(&text)) out.push_back(text);
  return out;
}

}  // namespace

TEST(InviteShareText, ACreatedCodeFilesItsCodeItsExpiryAndTheComingSoonLink) {
  DrainMatches();
  TakeTexts();
  Guard guard;
  const std::string body = std::string(R"({"code":"K7M2Q","match_id":")") + kMatchIdA + R"(","expires_at":"2026-10-21T05:38:59.63Z"})";
  LoopbackServer server({Script{Script::Kind::Reply, 200, body, ""}});
  Rig rig;
  nevr_invite_service::Start(rig.Settings(Switch::On, server.Base()));

  ASSERT_EQ(nevr_invite_service::Submit(CreatePlan()), Submit::Queued);
  ASSERT_TRUE(nevr_invite_service::WaitForAnswerPendingForTest(std::chrono::seconds(10)));
  EXPECT_FALSE(TakeMatch().ok) << "a created code names no match to join";
  const std::vector<nevr_invite_match::ShareText> texts = TakeTexts();
  ASSERT_EQ(texts.size(), 3u);
  EXPECT_STREQ(texts[0].field, "code");
  EXPECT_STREQ(texts[0].text, "K7M2Q");
  EXPECT_EQ(texts[0].actor, 0xa9bd0cb4e6f2004cULL);
  EXPECT_STREQ(texts[1].field, "expires");
  EXPECT_STREQ(texts[1].text, "EXPIRES: 05:38 UTC");
  EXPECT_STREQ(texts[2].field, "link");
  EXPECT_STREQ(texts[2].text, "<coming soon>");
  EXPECT_EQ(texts[2].length, 13u);
  EXPECT_EQ(texts[2].actor, 0xc3f9f167b2a06043ULL);
  EXPECT_EQ(texts[2].element, 0x3657cd7405999810ULL);
  EXPECT_TRUE(TakeTexts().empty());
}

TEST(InviteShareText, ARefusedResolveFilesTheEntryTitleAndAResolvedOneFilesNoText) {
  DrainMatches();
  TakeTexts();
  {
    Guard guard;
    LoopbackServer server({Script{Script::Kind::Reply, 404, R"({"message":"invalid"})", ""}});
    Rig rig;
    nevr_invite_service::Start(rig.Settings(Switch::On, server.Base()));
    ASSERT_EQ(nevr_invite_service::Submit(Resolve()), Submit::Queued);
    ASSERT_TRUE(nevr_invite_service::WaitForAnswerPendingForTest(std::chrono::seconds(10)));
    EXPECT_FALSE(TakeMatch().ok);
    const std::vector<nevr_invite_match::ShareText> texts = TakeTexts();
    ASSERT_EQ(texts.size(), 1u);
    EXPECT_STREQ(texts[0].field, "entry_title");
    EXPECT_STREQ(texts[0].text, "INVALID CODE");
  }
  {
    Guard guard;
    const std::string body = std::string(R"({"match_id":")") + kMatchIdA + R"("})";
    LoopbackServer server({Script{Script::Kind::Reply, 200, body, ""}});
    Rig rig;
    nevr_invite_service::Start(rig.Settings(Switch::On, server.Base()));
    ASSERT_EQ(nevr_invite_service::Submit(Resolve()), Submit::Queued);
    ASSERT_TRUE(nevr_invite_service::WaitForAnswerPendingForTest(std::chrono::seconds(10)));
    EXPECT_TRUE(TakeMatch().ok);
    EXPECT_TRUE(TakeTexts().empty());
  }
}

TEST(InviteShareText, NothingWaitingIsFalseAndANullArgumentIsRefused) {
  TakeTexts();
  nevr_invite_match::ShareText text;
  EXPECT_FALSE(nevr_invite_match::TakeShareText(&text));
  EXPECT_FALSE(nevr_invite_match::TakeShareText(nullptr));
}

