// Tests for the #398 self-check (compat/party_share_check.h): each service answer to one of our party-data
// shares becomes one self-check result, scoped by the answer's symbol. The frames the unit sends are read back
// with the small decoder below (the game service's reader, sharing no code with the codec).

#include "runtime/compat/evr_codec.h"
#include "runtime/compat/party_share_check.h"
#include "runtime/compat/self_check.h"
#include "runtime/compat/social_party.h"

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

// The strings of one SNSRemoteLogSetv3 frame (nakama RemoteLogSet.Stream + StreamStringTable).
std::vector<std::string> Strings(const std::string& frame) {
  std::vector<std::string> out;
  const std::size_t base = 24;
  std::size_t at = base + 8 * 7;
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

class PartyShareCheck : public ::testing::Test {
 protected:
  void SetUp() override {
    nevr_self_check::ResetForTest();
    nevr_party_share_check::ResetForTest();
    g_frames.clear();
    nevr_self_check::SetEnabled(true);
    nevr_self_check::SetSender(&Sender);
    nevr_self_check::SetLoggedIn(true, nevr_evr_codec::UserId{4, 77});
  }
  static uint64_t Sym(const char* name) { return nevr_social_party::ReplySymbol(name); }
};

}  // namespace

TEST_F(PartyShareCheck, TheFourAnswerSymbolsAreRecognisedWithTheirScope) {
  using nevr_party_share_check::AnswerOf;
  ASSERT_TRUE(AnswerOf(Sym("PartyUpdateSuccess")).has_value());
  EXPECT_FALSE(AnswerOf(Sym("PartyUpdateSuccess"))->member);
  EXPECT_TRUE(AnswerOf(Sym("PartyUpdateSuccess"))->success);
  EXPECT_FALSE(AnswerOf(Sym("PartyUpdateFailure"))->member);
  EXPECT_FALSE(AnswerOf(Sym("PartyUpdateFailure"))->success);
  EXPECT_TRUE(AnswerOf(Sym("PartyUpdateMemberSuccess"))->member);
  EXPECT_TRUE(AnswerOf(Sym("PartyUpdateMemberSuccess"))->success);
  EXPECT_TRUE(AnswerOf(Sym("PartyUpdateMemberFailure"))->member);
  EXPECT_FALSE(AnswerOf(Sym("PartyUpdateMemberFailure"))->success);
}

TEST_F(PartyShareCheck, OtherSymbolsAreNotAnswers) {
  using nevr_party_share_check::AnswerOf;
  EXPECT_FALSE(AnswerOf(Sym("PartyUpdateNotify")).has_value());
  EXPECT_FALSE(AnswerOf(Sym("PartyUpdateMemberNotify")).has_value());
  EXPECT_FALSE(AnswerOf(Sym("PartyLockSuccess")).has_value());
  EXPECT_FALSE(AnswerOf(0x1234567812345678ULL).has_value());
  EXPECT_FALSE(AnswerOf(0).has_value());
}

TEST_F(PartyShareCheck, TheFirstSuccessIsReportedOnceAndLaterOnesAreCountedNotSent) {
  nevr_party_share_check::OnServerMessage(Sym("PartyUpdateSuccess"));
  nevr_party_share_check::OnServerMessage(Sym("PartyUpdateSuccess"));
  nevr_party_share_check::OnServerMessage(Sym("PartyUpdateMemberSuccess"));
  nevr_self_check::Flush();
  const auto results = Results();
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0]["check"], "party_data_share");
  EXPECT_EQ(results[0]["pass"], true);
  EXPECT_EQ(results[0]["observed"], "scope=party answer=PartyUpdateSuccess ok=1 failed=0");
}

TEST_F(PartyShareCheck, EveryFailureIsReportedWithItsScopeAndTheRunningCounts) {
  nevr_party_share_check::OnServerMessage(Sym("PartyUpdateSuccess"));
  nevr_party_share_check::OnServerMessage(Sym("PartyUpdateFailure"));
  nevr_party_share_check::OnServerMessage(Sym("PartyUpdateMemberFailure"));
  nevr_self_check::Flush();
  const auto results = Results();
  ASSERT_EQ(results.size(), 3u);
  EXPECT_EQ(results[1]["pass"], false);
  EXPECT_EQ(results[1]["observed"], "scope=party answer=PartyUpdateFailure ok=1 failed=1");
  EXPECT_EQ(results[2]["pass"], false);
  EXPECT_EQ(results[2]["observed"], "scope=member answer=PartyUpdateMemberFailure ok=1 failed=2");
}

TEST_F(PartyShareCheck, NotAnAnswerReportsNothing) {
  nevr_party_share_check::OnServerMessage(Sym("PartyUpdateNotify"));
  nevr_party_share_check::OnServerMessage(Sym("PartyLockSuccess"));
  nevr_party_share_check::OnServerMessage(0xdeadbeefULL);
  nevr_self_check::Flush();
  EXPECT_TRUE(g_frames.empty());
}

TEST_F(PartyShareCheck, NothingIsSentBeforeLoginAndEverythingAfter) {
  nevr_self_check::SetLoggedIn(false);
  nevr_party_share_check::OnServerMessage(Sym("PartyUpdateFailure"));
  nevr_self_check::Flush();
  EXPECT_TRUE(g_frames.empty());
  nevr_self_check::SetLoggedIn(true, nevr_evr_codec::UserId{4, 77});
  nevr_self_check::Flush();
  ASSERT_EQ(Results().size(), 1u);
  EXPECT_EQ(Results()[0]["pass"], false);
}

TEST_F(PartyShareCheck, AFailureAfterTheCapStillCountsInTheRunningTotals) {
  for (int i = 0; i < 12; ++i) nevr_party_share_check::OnServerMessage(Sym("PartyUpdateFailure"));
  nevr_self_check::Flush();
  const auto results = Results();
  ASSERT_EQ(results.size(), nevr_self_check::kMaxResultsPerCheck + 1u);
  EXPECT_EQ(results.back()["observed"], "capped");
  nevr_party_share_check::OnServerMessage(Sym("PartyUpdateSuccess"));  // counted, and its first-success line is capped out
  EXPECT_EQ(nevr_party_share_check::Counts().ok, 1u);
  EXPECT_EQ(nevr_party_share_check::Counts().failed, 12u);
}
