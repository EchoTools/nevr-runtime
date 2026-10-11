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

  // One of the runtime's own requests, as the sender sees it (the 0x28 header with TargetParam last).
  static void Send(uint64_t symbol, uint64_t targetParam) {
    const nevr_social_party::Uuid self{};
    const nevr_social_party::Message m = nevr_social_party::Standard(symbol, self, targetParam);
    nevr_party_share_check::OnClientMessage(
        symbol, reinterpret_cast<const std::uint8_t*>(m.payload.data()), m.payload.size());
  }
  static void ShareParty() { Send(nevr_social_party::kPartyDataUpdateRequest, nevr_social_party::kPartyDataScopeParty); }
  static void ShareMember() { Send(nevr_social_party::kPartyDataUpdateRequest, nevr_social_party::kPartyDataScopeMember); }
  static void SetJoinPolicy() { Send(nevr_social_party::kSetJoinPolicyRequest, 1); }
  static void Answer(const char* name) { nevr_party_share_check::OnServerMessage(Sym(name)); }
  static std::vector<nlohmann::json> Of(const char* check) {
    std::vector<nlohmann::json> out;
    for (const auto& r : Results()) {
      if (r["check"] == check) out.push_back(r);
    }
    return out;
  }
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

// PartyUpdateSuccess/Failure also answer the leader's set-join-policy request (nakama
// evr_pipeline_party_policy.go) and a party metadata update (evr_pipeline_party.go). The check pairs each answer
// with the request that is next in line, so only a share's answer counts.
TEST_F(PartyShareCheck, AJoinPolicyAnswerIsNotAShareAndTheNextRealShareFailureIsReported) {
  SetJoinPolicy();
  Answer("PartyUpdateSuccess");  // the first PartyUpdateSuccess of #398's log: not a data share
  ShareParty();
  Answer("PartyUpdateFailure");  // the share that failed
  nevr_self_check::Flush();
  const auto results = Of("party_data_share");
  ASSERT_EQ(results.size(), 1u) << "a policy answer must not be reported as a passed share";
  EXPECT_EQ(results[0]["pass"], false);
  EXPECT_EQ(results[0]["observed"], "scope=party answer=PartyUpdateFailure ok=0 failed=1");
}

TEST_F(PartyShareCheck, AnswersAreMatchedInOrderPerFamily) {
  ShareParty();
  SetJoinPolicy();
  ShareParty();
  Answer("PartyUpdateSuccess");  // the first share
  Answer("PartyUpdateSuccess");  // the policy: not counted
  Answer("PartyUpdateFailure");  // the second share
  nevr_self_check::Flush();
  const auto results = Of("party_data_share");
  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(results[0]["pass"], true);
  EXPECT_EQ(results[0]["observed"], "scope=party answer=PartyUpdateSuccess ok=1 failed=0");
  EXPECT_EQ(results[1]["pass"], false);
  EXPECT_EQ(results[1]["observed"], "scope=party answer=PartyUpdateFailure ok=1 failed=1");
}

TEST_F(PartyShareCheck, AnAnswerWithNoRequestBehindItReportsNothing) {
  Answer("PartyUpdateSuccess");
  Answer("PartyUpdateFailure");
  Answer("PartyUpdateMemberFailure");
  nevr_self_check::Flush();
  EXPECT_TRUE(Of("party_data_share").empty());
}

TEST_F(PartyShareCheck, TheFirstSuccessIsReportedOnceAndLaterOnesAreCountedNotSent) {
  ShareParty();
  ShareParty();
  ShareMember();
  Answer("PartyUpdateSuccess");
  Answer("PartyUpdateSuccess");
  Answer("PartyUpdateMemberSuccess");
  nevr_self_check::Flush();
  const auto results = Of("party_data_share");
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0]["pass"], true);
  EXPECT_EQ(results[0]["observed"], "scope=party answer=PartyUpdateSuccess ok=1 failed=0");
  EXPECT_EQ(nevr_party_share_check::Counts().ok, 3u);
}

TEST_F(PartyShareCheck, MemberScopeSharesPairWithMemberAnswers) {
  ShareMember();
  Answer("PartyUpdateMemberFailure");
  nevr_self_check::Flush();
  const auto results = Of("party_data_share");
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0]["pass"], false);
  EXPECT_EQ(results[0]["observed"], "scope=member answer=PartyUpdateMemberFailure ok=0 failed=1");
}

TEST_F(PartyShareCheck, NotAnAnswerReportsNothing) {
  ShareParty();
  Answer("PartyUpdateNotify");
  Answer("PartyLockSuccess");
  nevr_party_share_check::OnServerMessage(0xdeadbeefULL);
  nevr_self_check::Flush();
  EXPECT_TRUE(g_frames.empty());
}

TEST_F(PartyShareCheck, NothingIsSentBeforeLoginAndEverythingAfter) {
  nevr_self_check::SetLoggedIn(false);
  ShareParty();
  Answer("PartyUpdateFailure");
  nevr_self_check::Flush();
  EXPECT_TRUE(g_frames.empty());
  nevr_self_check::SetLoggedIn(true, nevr_evr_codec::UserId{4, 77});
  nevr_self_check::Flush();
  ASSERT_FALSE(Of("party_data_share").empty());
}

// Past the per-check cap the failures stop being sent one by one, so the running totals ride their own check at
// 1, 2, 4, 8, 16 ... answers: #398's 9 failures and 3 successes would otherwise read "failed=8" then "capped".
TEST_F(PartyShareCheck, TheRunningTotalsAreSentPastTheCap) {
  for (int i = 0; i < 20; ++i) {
    ShareParty();
    Answer("PartyUpdateFailure");
  }
  nevr_self_check::Flush();
  const auto results = Of("party_data_share");
  ASSERT_EQ(results.size(), nevr_self_check::kMaxResultsPerCheck + 1u);
  EXPECT_EQ(results.back()["observed"], "capped");
  const auto totals = Of("party_data_share_totals");
  ASSERT_GE(totals.size(), 5u);
  EXPECT_EQ(totals[0]["observed"], "answers=1 ok=0 failed=1 other=0 unmatched=0");
  EXPECT_EQ(totals.back()["observed"], "answers=16 ok=0 failed=16 other=0 unmatched=0");
  EXPECT_EQ(totals.back()["pass"], false);
}

// A new login is a new session: the totals of the one that ended are sent (as the user it was), and the counts,
// the pending requests and the first-success latch start over.
TEST_F(PartyShareCheck, ANewSessionStartsOverAndSendsTheEndedSessionsTotals) {
  ShareParty();
  Answer("PartyUpdateSuccess");
  ShareParty();
  Answer("PartyUpdateFailure");
  SetJoinPolicy();  // still unanswered when the session ends
  nevr_self_check::SetLoggedIn(true, nevr_evr_codec::UserId{4, 88});  // the next LoginSuccess
  EXPECT_EQ(nevr_party_share_check::Counts().ok, 0u);
  EXPECT_EQ(nevr_party_share_check::Counts().failed, 0u);
  ShareParty();
  Answer("PartyUpdateSuccess");  // the first success of the NEW session is reported again
  nevr_self_check::Flush();
  const auto results = Of("party_data_share");
  ASSERT_EQ(results.size(), 3u);
  EXPECT_EQ(results[2]["observed"], "scope=party answer=PartyUpdateSuccess ok=1 failed=0");
  EXPECT_EQ(results[2]["userid"], "OVR-ORG-88");
  bool sawEnded = false;
  for (const auto& r : Of("party_data_share_totals")) {
    if (r["observed"] == "answers=2 ok=1 failed=1 other=0 unmatched=0") {
      sawEnded = true;
      EXPECT_EQ(r["userid"], "OVR-ORG-77") << "the ended session's totals are the user it was";
    }
  }
  EXPECT_TRUE(sawEnded);
}
