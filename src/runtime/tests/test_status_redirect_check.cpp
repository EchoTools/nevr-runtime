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

}  // namespace

TEST(StatusRedirectReporter, NothingToSayBeforeTheFirstConnection) {
  nevr_status_redirect_check::Reporter reporter;
  Observation o;
  EXPECT_FALSE(reporter.Take(0, 0, &o));
}

TEST(StatusRedirectReporter, TheFirstRedirectWithNoPassthroughPassesOnce) {
  nevr_status_redirect_check::Reporter reporter;
  Observation o;
  ASSERT_TRUE(reporter.Take(3, 0, &o));
  EXPECT_TRUE(o.pass);
  EXPECT_EQ(o.observed, "api_connects=3 redirected=3 passthrough=0");
  EXPECT_FALSE(reporter.Take(5, 0, &o)) << "a pass is said once";
  EXPECT_FALSE(reporter.Take(5, 0, &o));
}

TEST(StatusRedirectReporter, FailuresAreSaidAtPowersOfTwoWithTheRunningCounts) {
  nevr_status_redirect_check::Reporter reporter;
  Observation o;
  ASSERT_TRUE(reporter.Take(0, 1, &o));
  EXPECT_FALSE(o.pass);
  EXPECT_EQ(o.observed, "api_connects=1 redirected=0 passthrough=1");
  ASSERT_TRUE(reporter.Take(0, 2, &o));
  EXPECT_EQ(o.observed, "api_connects=2 redirected=0 passthrough=2");
  EXPECT_FALSE(reporter.Take(0, 3, &o)) << "between powers of two";
  ASSERT_TRUE(reporter.Take(0, 5, &o)) << "a jump past 4 is said once";
  EXPECT_EQ(o.observed, "api_connects=5 redirected=0 passthrough=5");
  EXPECT_FALSE(reporter.Take(0, 7, &o));
  ASSERT_TRUE(reporter.Take(0, 8, &o));
  EXPECT_FALSE(o.pass);
  EXPECT_FALSE(reporter.Take(0, 8, &o)) << "unchanged";
}

TEST(StatusRedirectReporter, APassthroughAfterARedirectIsAFailureNotASecondPass) {
  nevr_status_redirect_check::Reporter reporter;
  Observation o;
  ASSERT_TRUE(reporter.Take(2, 0, &o));
  EXPECT_TRUE(o.pass);
  ASSERT_TRUE(reporter.Take(2, 1, &o));
  EXPECT_FALSE(o.pass);
  EXPECT_EQ(o.observed, "api_connects=3 redirected=2 passthrough=1");
}

TEST(StatusRedirectReporter, ARedirectAfterAPassthroughIsNotAPass) {
  nevr_status_redirect_check::Reporter reporter;
  Observation o;
  ASSERT_TRUE(reporter.Take(0, 1, &o));
  EXPECT_FALSE(reporter.Take(4, 1, &o)) << "the failure stands; no late pass";
}

TEST(StatusRedirectLedger, CountsEachConnectionByItsOutcome) {
  nevr_status_redirect_check::Ledger ledger;
  ledger.NoteApiConnect(true);
  ledger.NoteApiConnect(true);
  ledger.NoteApiConnect(false);
  Observation o;
  ASSERT_TRUE(ledger.Take(&o));
  EXPECT_EQ(o.observed, "api_connects=3 redirected=2 passthrough=1");
  EXPECT_FALSE(o.pass);
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
  nevr_status_redirect_check::Register(&nevr_status_redirect_check::LedgerProbe);
  nevr_self_check::Flush();
  EXPECT_TRUE(g_frames.empty()) << "no connection yet";
  nevr_status_redirect_check::NoteApiConnect(true);
  nevr_self_check::Flush();
  nevr_status_redirect_check::NoteApiConnect(false);
  nevr_self_check::Flush();
  const auto results = Results();
  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(results[0]["check"], "status_redirect");
  EXPECT_EQ(results[0]["pass"], true);
  EXPECT_EQ(results[1]["pass"], false);
  EXPECT_EQ(results[1]["observed"], "api_connects=2 redirected=1 passthrough=1");
}
