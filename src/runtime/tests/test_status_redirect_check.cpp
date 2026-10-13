// Tests for the #408 self-check (compat/status_redirect_check.h): every connection the game opens to its API base
// is counted as redirected or passed through, and the check says so with the running counts.

#include "runtime/compat/evr_codec.h"
#include "runtime/compat/self_check.h"
#include "runtime/compat/status_redirect_check.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace {

uint64_t U64(const std::string& s, std::size_t at) {
  uint64_t v = 0;
  for (std::size_t i = 0; i < 8; ++i) v |= static_cast<uint64_t>(static_cast<unsigned char>(s.at(at + i))) << (8 * i);
  return v;
}
uint32_t U32(const std::string& s, std::size_t at) {
  uint32_t v = 0;
  for (std::size_t i = 0; i < 4; ++i) v |= static_cast<uint32_t>(static_cast<unsigned char>(s.at(at + i))) << (8 * i);
  return v;
}
std::vector<std::string> Strings(const std::string& frame) {
  std::vector<std::string> out;
  std::size_t at = 24 + 8 * 7;
  const uint64_t count = U64(frame, at);
  at += 8;
  std::vector<uint32_t> offsets(static_cast<std::size_t>(count), 0);
  for (std::size_t i = 1; i < count; ++i) {
    offsets[i] = U32(frame, at);
    at += 4;
  }
  for (std::size_t i = 0; i < count; ++i) {
    std::string s;
    for (std::size_t p = at + offsets[i]; p < frame.size() && frame[p] != '\0'; ++p) s.push_back(frame[p]);
    out.push_back(std::move(s));
  }
  return out;
}

std::vector<std::string> g_frames;
bool Sender(const std::string& frame) {
  g_frames.push_back(frame);
  return true;
}
std::vector<nlohmann::json> Results() {
  std::vector<nlohmann::json> out;
  for (const std::string& f : g_frames) {
    for (const std::string& s : Strings(f)) out.push_back(nlohmann::json::parse(s));
  }
  return out;
}

using nevr_self_check::Observation;
using nevr_status_redirect_check::Ledger;

const char kNevr[] = "https://nevr.example:7350";

}  // namespace

// ---- classification: every connect the game opens is counted by class --------------------------------------------

TEST(StatusRedirectLedger, AnApiBaseSentToTheConfiguredServiceIsARedirect) {
  Ledger ledger;
  ledger.NoteConnect("https://api.readyatdawn.com", kNevr);
  ledger.NoteConnect("https://api-live.readyatdawn.com", kNevr);  // the libr15 template api-%s.readyatdawn.com
  const auto c = ledger.Counts();
  EXPECT_EQ(c.apiRedirected, 2u);
  EXPECT_EQ(c.apiPassthrough, 0u);
  EXPECT_EQ(c.uncovered, 0u);
}

TEST(StatusRedirectLedger, AnApiBaseThatWasLeftAloneIsAPassthrough) {
  Ledger ledger;
  ledger.NoteConnect("https://api.readyatdawn.com", "https://api.readyatdawn.com");
  EXPECT_EQ(ledger.Counts().apiPassthrough, 1u);
  EXPECT_EQ(ledger.Counts().apiRedirected, 0u);
}

// The failing shape of #408 on the headset was "Couldn't resolve host name": the request went to a host the
// redirect does not cover. A service-domain host that is neither the API nor the graph base lands in `uncovered`,
// by host name only (no path, no query, no key), and is not silently ignored.
TEST(StatusRedirectLedger, AServiceHostThatIsNotTheApiBaseIsUncoveredAndNamedByHostOnly) {
  Ledger ledger;
  ledger.NoteConnect("https://status.readyatdawn.com:443/status/services,news?env=live&projectid=rad14&key=SECRET",
                     "https://status.readyatdawn.com:443/status/services,news?env=live&projectid=rad14&key=SECRET");
  const auto c = ledger.Counts();
  EXPECT_EQ(c.uncovered, 1u);
  EXPECT_EQ(c.apiPassthrough, 0u) << "it is not an API base, so it must not be read as one";
  Observation o;
  ASSERT_TRUE(ledger.Take(&o));
  EXPECT_FALSE(o.pass);
  EXPECT_NE(o.observed.find("uncovered=1"), std::string::npos);
  EXPECT_NE(o.observed.find("status.readyatdawn.com"), std::string::npos);
  EXPECT_EQ(o.observed.find("?"), std::string::npos) << "no query string";
  EXPECT_EQ(o.observed.find("SECRET"), std::string::npos) << "no key";
  EXPECT_EQ(o.observed.find("/status"), std::string::npos) << "no path";
}

TEST(StatusRedirectLedger, ASameDomainHostThatTheRedirectDidCoverIsNotUncovered) {
  Ledger ledger;
  ledger.NoteConnect("https://config.readyatdawn.com", kNevr);
  EXPECT_EQ(ledger.Counts().uncovered, 0u);
}

TEST(StatusRedirectLedger, AHostOutsideTheServiceDomainIsCountedExternalAndIsNotAFailure) {
  Ledger ledger;
  ledger.NoteConnect("https://cdn.example.com/assets", "https://cdn.example.com/assets");
  const auto c = ledger.Counts();
  EXPECT_EQ(c.external, 1u);
  EXPECT_EQ(c.uncovered, 0u);
  EXPECT_EQ(c.apiPassthrough, 0u);
}

TEST(StatusRedirectLedger, TheGraphBaseIsCountedApartFromTheApiBase) {
  Ledger ledger;
  ledger.NoteConnect("https://graph.oculus.com", kNevr);
  ledger.NoteConnect("https://graph.oculus.com", "https://graph.oculus.com");
  const auto c = ledger.Counts();
  EXPECT_EQ(c.graph, 2u);
  EXPECT_EQ(c.apiRedirected, 0u);
  EXPECT_EQ(c.apiPassthrough, 0u);
  EXPECT_EQ(c.uncovered, 0u);
}

TEST(StatusRedirectLedger, ANullOrEmptyUriIsIgnored) {
  Ledger ledger;
  ledger.NoteConnect(nullptr, nullptr);
  ledger.NoteConnect("", "");
  const auto c = ledger.Counts();
  EXPECT_EQ(c.apiRedirected + c.apiPassthrough + c.uncovered + c.external + c.graph, 0u);
}

TEST(StatusRedirectLedger, UncoveredHostsAreBoundedAndDistinct) {
  Ledger ledger;
  for (int i = 0; i < 20; ++i) {
    const std::string host = "https://h" + std::to_string(i % 6) + ".readyatdawn.com";
    ledger.NoteConnect(host.c_str(), host.c_str());
  }
  EXPECT_EQ(ledger.Counts().uncovered, 20u);
  Observation o;
  ASSERT_TRUE(ledger.Take(&o));
  std::size_t commas = 0;
  for (char ch : o.observed) commas += ch == ',' ? 1 : 0;
  EXPECT_LE(commas, 3u) << "at most four distinct hosts are named";
  EXPECT_LT(o.observed.size(), 200u);
}

// ---- what is said ------------------------------------------------------------------------------------------------

TEST(StatusRedirectReporter, NothingToSayBeforeTheFirstConnection) {
  Ledger ledger;
  Observation o;
  EXPECT_FALSE(ledger.Take(&o));
}

TEST(StatusRedirectReporter, TheFirstRedirectWithNothingWrongPassesOnce) {
  Ledger ledger;
  Observation o;
  ledger.NoteConnect("https://api.readyatdawn.com", kNevr);
  ASSERT_TRUE(ledger.Take(&o));
  EXPECT_TRUE(o.pass);
  EXPECT_EQ(o.observed, "api_connects=1 redirected=1 passthrough=0 uncovered=0 graph=0 external=0");
  ledger.NoteConnect("https://api.readyatdawn.com", kNevr);
  EXPECT_FALSE(ledger.Take(&o)) << "a pass is said once";
}

TEST(StatusRedirectReporter, FailuresAreSaidAtPowersOfTwoWithTheRunningCounts) {
  Ledger ledger;
  Observation o;
  auto fail = [&] { ledger.NoteConnect("https://api.readyatdawn.com", "https://api.readyatdawn.com"); };
  fail();
  ASSERT_TRUE(ledger.Take(&o));
  EXPECT_FALSE(o.pass);
  EXPECT_EQ(o.observed, "api_connects=1 redirected=0 passthrough=1 uncovered=0 graph=0 external=0");
  fail();
  ASSERT_TRUE(ledger.Take(&o));
  EXPECT_NE(o.observed.find("passthrough=2"), std::string::npos);
  fail();
  EXPECT_FALSE(ledger.Take(&o)) << "between powers of two";
  fail();
  fail();
  ASSERT_TRUE(ledger.Take(&o)) << "a jump past 4 is said once";
  EXPECT_NE(o.observed.find("passthrough=5"), std::string::npos);
  EXPECT_FALSE(ledger.Take(&o)) << "unchanged";
}

TEST(StatusRedirectReporter, UncoveredAndPassthroughShareTheFailureLevels) {
  Ledger ledger;
  Observation o;
  ledger.NoteConnect("https://api.readyatdawn.com", "https://api.readyatdawn.com");
  ASSERT_TRUE(ledger.Take(&o));
  ledger.NoteConnect("https://x.readyatdawn.com", "https://x.readyatdawn.com");
  ASSERT_TRUE(ledger.Take(&o)) << "one failure of each kind is two failures: level 2";
  EXPECT_NE(o.observed.find("passthrough=1"), std::string::npos);
  EXPECT_NE(o.observed.find("uncovered=1"), std::string::npos);
}

TEST(StatusRedirectReporter, ARedirectAfterAFailureIsNotAPass) {
  Ledger ledger;
  Observation o;
  ledger.NoteConnect("https://api.readyatdawn.com", "https://api.readyatdawn.com");
  ASSERT_TRUE(ledger.Take(&o));
  ledger.NoteConnect("https://api.readyatdawn.com", kNevr);
  EXPECT_FALSE(ledger.Take(&o)) << "the failure stands; no late pass";
}

class StatusRedirectCheck : public ::testing::Test {
 protected:
  void SetUp() override {
    nevr_self_check::ResetForTest();
    nevr_status_redirect_check::ResetForTest();
    g_frames.clear();
    nevr_self_check::SetEnabled(true);
    nevr_self_check::SetSender(&Sender);
    nevr_self_check::SetLoggedIn(true, nevr_evr_codec::UserId{4, 5});
  }
};

TEST_F(StatusRedirectCheck, TheRegisteredProbeSendsOneResultPerChange) {
  nevr_status_redirect_check::Register(&nevr_status_redirect_check::ProcessProbe);
  nevr_self_check::Flush();
  EXPECT_TRUE(g_frames.empty()) << "no connection yet";
  nevr_status_redirect_check::NoteConnect("https://api.readyatdawn.com", kNevr);
  nevr_self_check::Flush();
  nevr_status_redirect_check::NoteConnect("https://api.readyatdawn.com", "https://api.readyatdawn.com");
  nevr_self_check::Flush();
  const auto results = Results();
  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(results[0]["check"], "status_redirect");
  EXPECT_EQ(results[0]["pass"], true);
  EXPECT_EQ(results[1]["pass"], false);
}
