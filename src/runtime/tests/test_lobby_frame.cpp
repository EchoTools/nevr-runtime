// #929: the decode, the field text and the rate limit behind a client's lobby-frame records
// (compat/lobby_frame.h). The records themselves, written by ws_bridge.cpp, are asserted in
// test_behavioral.cpp (WsBridgeLobbyFrameLog). Payloads are built here byte by byte from the layouts the
// header documents.
#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "nevr/symbol_hash.h"
#include "runtime/compat/evr_codec.h"
#include "runtime/compat/lobby_frame.h"
#include "runtime/tests/lobby_record_golden.h"

namespace lf = nevr_lobby_frame;

namespace {

void PutLe(std::string& out, uint64_t value, size_t bytes) {
  for (size_t i = 0; i < bytes; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
}

std::string GuidBytes(uint8_t tail) {
  const uint8_t bytes[16] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                             0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, tail};
  return std::string(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

const uint8_t* Bytes(const std::string& s) { return reinterpret_cast<const uint8_t*>(s.data()); }

std::string Entrants(const std::vector<uint64_t>& accounts) {
  std::string out;
  for (const uint64_t account : accounts) {
    PutLe(out, 4, 8);
    PutLe(out, account, 8);
  }
  return out;
}

std::string Find(size_t declaredCount, const std::vector<uint64_t>& accounts, bool settingsNul = true) {
  std::string out;
  PutLe(out, 1, 8);
  PutLe(out, 0xAAAA, 8);
  PutLe(out, 0xBBBB, 8);
  PutLe(out, 4, 8);
  out += GuidBytes(0x01);
  PutLe(out, declaredCount, 1);
  PutLe(out, 0, 4);
  PutLe(out, 0, 3);
  out += GuidBytes(0x02);
  out += GuidBytes(0x03);
  out += "{}";
  if (settingsNul) out.push_back('\0');
  out += Entrants(accounts);
  return out;
}

std::string Join(size_t declaredCount, const std::vector<uint64_t>& accounts) {
  std::string out;
  out += GuidBytes(0x04);
  PutLe(out, 1, 8);
  PutLe(out, 4, 8);
  out += GuidBytes(0x01);
  PutLe(out, declaredCount, 4);
  PutLe(out, 0, 4);
  PutLe(out, 0, 8);
  out += "{}";
  out.push_back('\0');
  out += Entrants(accounts);
  return out;
}

// A failure frame as the service sends it: mode(8), group(16), code(4), pad(4), message(64), expiry(8). The message
// is cut to its field's 64 bytes and the field is NUL-filled after the text.
std::string Failure(uint32_t code, const std::string& message, char expiryByte = '\0') {
  std::string out;
  PutLe(out, 7, 8);
  out += GuidBytes(0x06);
  PutLe(out, code, 4);
  PutLe(out, 0, 4);
  std::string field = message.substr(0, 64);
  field.resize(64, '\0');
  out += field;
  out.append(8, expiryByte);
  return out;
}

}  // namespace

TEST(LobbyFrameSymbols, EveryConstantIsTheCSymbol64OfItsGameName) {
  EXPECT_EQ(EchoVR::CSymbol64Hash("SNSLobbyFindSessionRequestv11"), nevr_evr_codec::kSymFindSessionRequest);
  EXPECT_EQ(EchoVR::CSymbol64Hash("SNSLobbyJoinSessionRequestv7"), nevr_evr_codec::kSymJoinSessionRequest);
  EXPECT_EQ(EchoVR::CSymbol64Hash("SNSLobbySessionSuccessv5"), lf::kSymSessionSuccess);
  EXPECT_EQ(EchoVR::CSymbol64Hash("SNSLobbySessionFailurev4"), lf::kSymSessionFailure);
}

TEST(LobbyFrameKind, RequestsAreHeardFromTheGameAndAnswersFromTheService) {
  EXPECT_EQ(lf::KindOf(nevr_evr_codec::kSymFindSessionRequest, false), lf::Kind::Find);
  EXPECT_EQ(lf::KindOf(nevr_evr_codec::kSymJoinSessionRequest, false), lf::Kind::Join);
  EXPECT_EQ(lf::KindOf(lf::kSymSessionSuccess, true), lf::Kind::Success);
  EXPECT_EQ(lf::KindOf(lf::kSymSessionFailure, true), lf::Kind::Failure);
  EXPECT_FALSE(lf::KindOf(nevr_evr_codec::kSymFindSessionRequest, true));
  EXPECT_FALSE(lf::KindOf(nevr_evr_codec::kSymJoinSessionRequest, true));
  EXPECT_FALSE(lf::KindOf(lf::kSymSessionSuccess, false));
  EXPECT_FALSE(lf::KindOf(lf::kSymSessionFailure, false));
  EXPECT_FALSE(lf::KindOf(nevr_evr_codec::kSymCreateSessionRequest, false));
  EXPECT_FALSE(lf::KindOf(0, false));
}

TEST(LobbyFrameDecode, AFindNamesItsModeLobbiesAndEntrants) {
  const std::string payload = Find(2, {11, 22});
  lf::Decoded d;
  ASSERT_TRUE(lf::Decode(lf::Kind::Find, Bytes(payload), payload.size(), &d));
  EXPECT_EQ(d.mode, 0xAAAAU);
  EXPECT_EQ(lf::FormatGuid(d.currentLobby), "44332211-6655-8877-99AA-BBCCDDEEFF02");
  EXPECT_EQ(lf::FormatGuid(d.group), "44332211-6655-8877-99AA-BBCCDDEEFF03");
  ASSERT_EQ(d.entrants.size(), 2U);
  EXPECT_EQ(d.entrants[0].account, 11U);
  EXPECT_EQ(d.entrants[1].account, 22U);
  EXPECT_EQ(d.entrants[1].platform, 4U);
}

TEST(LobbyFrameDecode, AJoinNamesItsLobbyAndEntrants) {
  const std::string payload = Join(1, {33}) + std::string("\xff\xff", 2);  // and the optional team tail
  lf::Decoded d;
  ASSERT_TRUE(lf::Decode(lf::Kind::Join, Bytes(payload), payload.size(), &d));
  EXPECT_EQ(lf::FormatGuid(d.lobby), "44332211-6655-8877-99AA-BBCCDDEEFF04");
  ASSERT_EQ(d.entrants.size(), 1U);
  EXPECT_EQ(d.entrants[0].account, 33U);
}

TEST(LobbyFrameDecode, EveryStrictPrefixOfARequestIsRefused) {
  const std::vector<std::pair<lf::Kind, std::string>> requests = {{lf::Kind::Find, Find(2, {11, 22})},
                                                                  {lf::Kind::Join, Join(2, {33, 44})}};
  for (const auto& [kind, whole] : requests) {
    lf::Decoded d;
    EXPECT_TRUE(lf::Decode(kind, Bytes(whole), whole.size(), &d));
    for (size_t cut = 0; cut < whole.size(); ++cut) {
      EXPECT_FALSE(lf::Decode(kind, Bytes(whole), cut, &d)) << "kind " << static_cast<int>(kind) << " cut " << cut;
    }
  }
}

TEST(LobbyFrameDecode, ARequestWithoutTheEndOfItsSettingsOrWithTooManyEntrantsIsRefused) {
  lf::Decoded d;
  const std::string noNul = Find(0, {}, false);
  EXPECT_FALSE(lf::Decode(lf::Kind::Find, Bytes(noNul), noNul.size(), &d));
  const std::vector<uint64_t> seventeen(17, 5);
  const std::string tooMany = Find(17, seventeen);
  EXPECT_FALSE(lf::Decode(lf::Kind::Find, Bytes(tooMany), tooMany.size(), &d));
  const std::vector<uint64_t> sixteen(16, 5);
  const std::string atTheLimit = Find(16, sixteen);
  EXPECT_TRUE(lf::Decode(lf::Kind::Find, Bytes(atTheLimit), atTheLimit.size(), &d));
  EXPECT_EQ(d.entrants.size(), 16U);
  const std::string declaresMore = Join(3, {1, 2});
  EXPECT_FALSE(lf::Decode(lf::Kind::Join, Bytes(declaresMore), declaresMore.size(), &d));
}

TEST(LobbyFrameDecode, ASuccessNamesItsLobbyAndAFailureItsCode) {
  std::string success;
  PutLe(success, 7, 8);
  success += GuidBytes(0x05);
  success += GuidBytes(0x06);
  lf::Decoded d;
  ASSERT_TRUE(lf::Decode(lf::Kind::Success, Bytes(success), success.size(), &d));
  EXPECT_EQ(lf::FormatGuid(d.lobby), "44332211-6655-8877-99AA-BBCCDDEEFF05");
  EXPECT_EQ(lf::FormatGuid(d.group), "44332211-6655-8877-99AA-BBCCDDEEFF06");
  EXPECT_FALSE(lf::Decode(lf::Kind::Success, Bytes(success), success.size() - 1, &d));

  std::string failure;
  PutLe(failure, 7, 8);
  failure += GuidBytes(0x06);
  PutLe(failure, 0x01020308, 4);
  ASSERT_TRUE(lf::Decode(lf::Kind::Failure, Bytes(failure), failure.size(), &d));
  EXPECT_EQ(d.code, 0x01020308U);
  EXPECT_FALSE(lf::Decode(lf::Kind::Failure, Bytes(failure), failure.size() - 1, &d));
}

TEST(LobbyFrameDecode, AFailureNamesItsCodeAndCountsTheBytesOfItsMessage) {
  const std::string payload = Failure(8, "lobby is full");
  lf::Decoded d;
  ASSERT_TRUE(lf::Decode(lf::Kind::Failure, Bytes(payload), payload.size(), &d));
  EXPECT_EQ(d.mode, 7U);
  EXPECT_EQ(d.code, 8U);
  EXPECT_EQ(lf::FormatGuid(d.group), "44332211-6655-8877-99AA-BBCCDDEEFF06");
  EXPECT_EQ(d.messageBytes, 13U);
}

TEST(LobbyFrameDecode, AnEmptyFailureMessageCountsZero) {
  const std::string payload = Failure(9, "");
  lf::Decoded d;
  ASSERT_TRUE(lf::Decode(lf::Kind::Failure, Bytes(payload), payload.size(), &d));
  EXPECT_EQ(d.code, 9U);
  EXPECT_EQ(d.messageBytes, 0U);
}

TEST(LobbyFrameDecode, AMessageThatFillsItsFieldCountsTheFieldAndNotTheExpiryAfterIt) {
  // 64 bytes with no NUL, then an expiry that is not zero: the count stops at the end of the field.
  const std::string payload = Failure(8, std::string(64, 'x'), '\x5a');
  lf::Decoded d;
  ASSERT_TRUE(lf::Decode(lf::Kind::Failure, Bytes(payload), payload.size(), &d));
  EXPECT_EQ(d.messageBytes, 64U);
  EXPECT_EQ(d.code, 8U);
}

TEST(LobbyFrameDecode, AFailureThatEndsInsideItsMessageCountsOnlyWhatItHolds) {
  const std::string cut = Failure(8, "ab").substr(0, 34);  // two message bytes, no NUL, no expiry
  lf::Decoded d;
  ASSERT_TRUE(lf::Decode(lf::Kind::Failure, Bytes(cut), cut.size(), &d));
  EXPECT_EQ(d.messageBytes, 2U);
  EXPECT_EQ(d.code, 8U);
}

TEST(LobbyFrameDecode, AFailureWithoutAMessageFieldCountsZero) {
  const std::string cut = Failure(8, "ab").substr(0, 28);  // mode, group and code only
  lf::Decoded d;
  ASSERT_TRUE(lf::Decode(lf::Kind::Failure, Bytes(cut), cut.size(), &d));
  EXPECT_EQ(d.code, 8U);
  EXPECT_EQ(d.messageBytes, 0U);
}

TEST(LobbyFrameText, AGuidIsUpperCaseInTheWindowsLayoutAndTheNilGuidIsNone) {
  lf::Guid g{};
  EXPECT_EQ(lf::FormatGuidOrNone(g), "none");
  const std::string bytes = GuidBytes(0xb0);
  std::copy(bytes.begin(), bytes.end(), g.begin());
  EXPECT_EQ(lf::FormatGuidOrNone(g), "44332211-6655-8877-99AA-BBCCDDEEFFB0");
}

TEST(LobbyFrameText, AccountsAreXpidsInRequestOrder) {
  EXPECT_EQ(lf::FormatAccounts({{4, 1001}, {4, 7}}), "OVR-ORG-1001,OVR-ORG-7");
  EXPECT_EQ(lf::FormatAccounts({}), "");
}

TEST(LobbyFrameText, FailureReasonsAreTheGamesOwnNamesAsOneWord) {
  const char* expected[] = {"unspecified",        "unsupported_version", "bad_request",   "timeout",
                            "lobby_invalid",      "lobby_incompatible",  "lobby_no_crossplay",
                            "lobby_locked",       "lobby_full",          "internal_error",
                            "missing_entitlement", "banned",             "kicked",         "access_denied"};
  for (uint32_t code = 0; code < 14; ++code) EXPECT_STREQ(lf::FailureReason(code), expected[code]) << code;
  EXPECT_STREQ(lf::FailureReason(14), "unknown");
  EXPECT_STREQ(lf::FailureReason(0xffffffffU), "unknown");
}

TEST(LobbyFrameLimiter, AKindWritesItsBurstThenCountsTheRest) {
  lf::RecordLimiter limiter;
  for (unsigned i = 0; i < lf::RecordLimiter::kBurst; ++i) {
    EXPECT_TRUE(limiter.Admit(lf::Kind::Join, 1000 + i).log) << i;
  }
  for (unsigned i = 0; i < 25; ++i) {
    const lf::RecordLimiter::Verdict v = limiter.Admit(lf::Kind::Join, 2000 + i);
    EXPECT_FALSE(v.log) << i;
    EXPECT_EQ(v.flushed[static_cast<size_t>(lf::Kind::Join)], 0U);
  }
}

TEST(LobbyFrameLimiter, TheEndedWindowsCountIsReportedOnceAndANewBurstStarts) {
  lf::RecordLimiter limiter;
  for (unsigned i = 0; i < lf::RecordLimiter::kBurst + 25; ++i) limiter.Admit(lf::Kind::Join, 1000);
  const lf::RecordLimiter::Verdict atTheEdge = limiter.Admit(lf::Kind::Join, 1000 + lf::RecordLimiter::kWindowMs - 1);
  EXPECT_FALSE(atTheEdge.log);
  const lf::RecordLimiter::Verdict next = limiter.Admit(lf::Kind::Join, 1000 + lf::RecordLimiter::kWindowMs);
  EXPECT_TRUE(next.log);
  EXPECT_EQ(next.flushed[static_cast<size_t>(lf::Kind::Join)], 26U);  // 25 and the one at the edge
  const lf::RecordLimiter::Verdict after = limiter.Admit(lf::Kind::Join, 1000 + lf::RecordLimiter::kWindowMs + 1);
  EXPECT_TRUE(after.log);
  EXPECT_EQ(after.flushed[static_cast<size_t>(lf::Kind::Join)], 0U);
}

TEST(LobbyFrameLimiter, KindsHaveTheirOwnBurstsAndAnyFrameReportsAnEndedWindow) {
  lf::RecordLimiter limiter;
  for (unsigned i = 0; i < lf::RecordLimiter::kBurst + 3; ++i) limiter.Admit(lf::Kind::Join, 500);
  EXPECT_TRUE(limiter.Admit(lf::Kind::Failure, 500).log);  // a join storm does not hide a failure
  const lf::RecordLimiter::Verdict v = limiter.Admit(lf::Kind::Failure, 500 + lf::RecordLimiter::kWindowMs);
  EXPECT_EQ(v.flushed[static_cast<size_t>(lf::Kind::Join)], 3U);
  EXPECT_EQ(v.flushed[static_cast<size_t>(lf::Kind::Failure)], 0U);
}

TEST(LobbyFrameLimiter, ResetForgetsEveryWindow) {
  lf::RecordLimiter limiter;
  for (unsigned i = 0; i < lf::RecordLimiter::kBurst + 3; ++i) limiter.Admit(lf::Kind::Find, 10);
  limiter.Reset();
  const lf::RecordLimiter::Verdict v = limiter.Admit(lf::Kind::Find, 10);
  EXPECT_TRUE(v.log);
  EXPECT_EQ(v.flushed[static_cast<size_t>(lf::Kind::Find)], 0U);
}

// #1413: the request a session record names is the last find or join the game sent on this client, its age is
// measured at the answer, and an answer is outstanding only for the first answer to that request.
TEST(LobbyFrameRequests, NoRequestSentMeansNoContextAndNoneInTheLine) {
  lf::RequestTracker tracker;
  const lf::RequestContext answer = tracker.Answer(5000);
  EXPECT_FALSE(answer.sent);
  EXPECT_FALSE(answer.outstanding);
  EXPECT_EQ(lf::FormatRequestFields(answer), "last_request=none last_request_age_ms=none request_outstanding=no");
}

TEST(LobbyFrameRequests, AnAnswerTakesTheAgeOfTheRequestItAnswers) {
  lf::RequestTracker tracker;
  tracker.Sent(lf::Kind::Find, 1000);
  const lf::RequestContext answer = tracker.Answer(1250);
  EXPECT_TRUE(answer.sent);
  EXPECT_EQ(answer.kind, lf::Kind::Find);
  EXPECT_EQ(answer.ageMs, 250U);
  EXPECT_TRUE(answer.outstanding);
  EXPECT_EQ(lf::FormatRequestFields(answer), "last_request=find last_request_age_ms=250 request_outstanding=yes");
}

TEST(LobbyFrameRequests, ASecondAnswerToTheSameRequestIsNotOutstanding) {
  lf::RequestTracker tracker;
  tracker.Sent(lf::Kind::Join, 1000);
  EXPECT_TRUE(tracker.Answer(1100).outstanding);
  const lf::RequestContext again = tracker.Answer(1200);
  EXPECT_TRUE(again.sent);
  EXPECT_EQ(again.kind, lf::Kind::Join);
  EXPECT_EQ(again.ageMs, 200U);
  EXPECT_FALSE(again.outstanding);
}

TEST(LobbyFrameRequests, TheLastRequestWinsAndANewRequestIsOutstandingAgain) {
  lf::RequestTracker tracker;
  tracker.Sent(lf::Kind::Find, 1000);
  EXPECT_TRUE(tracker.Answer(1010).outstanding);
  tracker.Sent(lf::Kind::Join, 1500);
  const lf::RequestContext answer = tracker.Answer(1600);
  EXPECT_EQ(answer.kind, lf::Kind::Join);
  EXPECT_EQ(answer.ageMs, 100U);
  EXPECT_TRUE(answer.outstanding);
}

TEST(LobbyFrameRequests, AClockThatGoesBackwardsGivesAZeroAge) {
  lf::RequestTracker tracker;
  tracker.Sent(lf::Kind::Find, 2000);
  const lf::RequestContext answer = tracker.Answer(1000);
  EXPECT_TRUE(answer.sent);
  EXPECT_EQ(answer.ageMs, 0U);
}

TEST(LobbyFrameRequests, ResetForgetsTheLastRequest) {
  lf::RequestTracker tracker;
  tracker.Sent(lf::Kind::Find, 1000);
  tracker.Reset();
  EXPECT_FALSE(tracker.Answer(1100).sent);
}

TEST(LobbyFrameText, PartyFieldsOutsideAnyPartyAreNoneZeroAndNotLeader) {
  EXPECT_EQ(lf::FormatPartyFields(0, 0, 0, 100), "party_id=none party_size=0 party_leader=no");
}

TEST(LobbyFrameText, PartyFieldsOfAPartyTheLocalAccountLeads) {
  EXPECT_EQ(lf::FormatPartyFields(100, 3, 100, 100), "party_id=100 party_size=3 party_leader=yes");
}

TEST(LobbyFrameText, PartyFieldsOfAPartyLedByAnotherAccountAreNotLeader) {
  EXPECT_EQ(lf::FormatPartyFields(7, 2, 77, 100), "party_id=7 party_size=2 party_leader=no");
}

TEST(LobbyFrameText, TheLeaderIsUnknownUntilTheLocalAccountAndTheOwnerAreBothKnown) {
  EXPECT_EQ(lf::FormatPartyFields(7, 1, 100, 0), "party_id=7 party_size=1 party_leader=unknown");
  EXPECT_EQ(lf::FormatPartyFields(7, 1, 0, 100), "party_id=7 party_size=1 party_leader=unknown");
}

// #1429: the session record, its notices and its warning, shared by the PC bridge and the Quest router. The
// golden lines are in lobby_record_golden.h; these pin each function on its own.
TEST(LobbyFrameRecords, ASessionSuccessRecordIsTheGoldenLine) {
  lf::Decoded d;
  const std::string payload = lobby_record_golden::SuccessPayload();
  ASSERT_TRUE(lf::Decode(lf::Kind::Success, Bytes(payload), payload.size(), &d));
  lf::RequestContext request;
  request.sent = true;
  request.kind = lf::Kind::Find;
  request.ageMs = 250;
  request.outstanding = true;
  const lf::PartyContext party{lobby_record_golden::kPartyId, lobby_record_golden::kPartyMembers,
                               lobby_record_golden::kPartyOwner, lobby_record_golden::kPartySelf};
  EXPECT_EQ(lf::FormatSessionRecord(d, lobby_record_golden::kConn, party, request),
            std::string(lobby_record_golden::kSuccessAfterFind));
}

TEST(LobbyFrameRecords, AFailureRecordNamesItsReasonAndCountsItsMessageNeverCopiesIt) {
  lf::Decoded d;
  const std::string payload = lobby_record_golden::FailureWithMessage(8, "private-text");
  ASSERT_TRUE(lf::Decode(lf::Kind::Failure, Bytes(payload), payload.size(), &d));
  const std::string line = lf::FormatSessionRecord(d, 2, std::nullopt, lf::RequestContext{});
  EXPECT_NE(line.find("code=8 reason=lobby_full "), std::string::npos) << line;
  EXPECT_NE(line.find("message_bytes=12 "), std::string::npos) << line;
  EXPECT_EQ(line.find("private-text"), std::string::npos) << line;
}

TEST(LobbyFrameRecords, APartyWithNoSourceReadsUnknownNotNone) {
  EXPECT_EQ(lf::FormatPartyFields(std::nullopt), "party_id=unknown party_size=unknown party_leader=unknown");
  EXPECT_EQ(lf::FormatPartyFields(lf::PartyContext{}), "party_id=none party_size=0 party_leader=no");
}

TEST(LobbyFrameRecords, AFindIsNotARecordSoItHasNoSessionLine) {
  lf::Decoded d;
  d.kind = lf::Kind::Find;
  EXPECT_EQ(lf::FormatSessionRecord(d, 2, std::nullopt, lf::RequestContext{}), "");
}

TEST(LobbyFrameRecords, TheSuppressedNoticeAndTheUnreadableWarningAreTheirPinnedText) {
  EXPECT_EQ(lf::FormatSuppressedNotice(lf::Kind::Success, 3),
            "[NEVR.WS] lobby frame records suppressed kind=success count=3 window_ms=10000 limit=10 (these frames "
            "were handled, only their records were not written)");
  EXPECT_EQ(lf::FormatUnreadable(lf::Kind::Success, 2, 1),
            "[NEVR.WS] lobby session success could not be read conn=2 payload_bytes=1");
}

TEST(LobbyFrameRecords, AnAnswerTakesTheRequestBeforeItAndOnlyTheFirstAnswerIsOutstanding) {
  lf::ClientLobbyRecords records;
  records.Requested(lf::Kind::Find, 1000);
  const lf::ClientLobbyRecords::Admission first = records.Admit(lf::Kind::Success, 1250);
  EXPECT_TRUE(first.request.sent);
  EXPECT_EQ(first.request.kind, lf::Kind::Find);
  EXPECT_EQ(first.request.ageMs, 250U);
  EXPECT_TRUE(first.request.outstanding);
  const lf::ClientLobbyRecords::Admission second = records.Admit(lf::Kind::Failure, 1300);
  EXPECT_FALSE(second.request.outstanding);
  EXPECT_EQ(second.request.ageMs, 300U);
}

TEST(LobbyFrameRecords, ANewerRequestReplacesTheOneAnAnswerWouldTake) {
  lf::ClientLobbyRecords records;
  records.Requested(lf::Kind::Find, 1000);
  records.Requested(lf::Kind::Join, 1100);
  const lf::ClientLobbyRecords::Admission answer = records.Admit(lf::Kind::Success, 1150);
  EXPECT_EQ(answer.request.kind, lf::Kind::Join);
  EXPECT_EQ(answer.request.ageMs, 50U);
}

TEST(LobbyFrameRecords, TheLimitIsPerKindAndItsEndedWindowIsCountedOnTheNextAnswer) {
  lf::ClientLobbyRecords records;
  unsigned written = 0;
  for (int i = 0; i < 11; ++i) {
    if (records.Admit(lf::Kind::Success, 1000).verdict.log) ++written;
  }
  EXPECT_EQ(written, 10U);
  const lf::ClientLobbyRecords::Admission later = records.Admit(lf::Kind::Success, 11000);
  EXPECT_EQ(later.verdict.flushed[static_cast<std::size_t>(lf::Kind::Success)], 1U);
  EXPECT_TRUE(later.verdict.log);
}

TEST(LobbyFrameRecords, ResetForgetsTheRequestAndTheWindow) {
  lf::ClientLobbyRecords records;
  records.Requested(lf::Kind::Find, 1000);
  for (int i = 0; i < 10; ++i) records.Admit(lf::Kind::Success, 1000);
  records.Reset();
  const lf::ClientLobbyRecords::Admission after = records.Admit(lf::Kind::Success, 1100);
  EXPECT_FALSE(after.request.sent);
  EXPECT_TRUE(after.verdict.log);
  EXPECT_EQ(after.verdict.flushed[static_cast<std::size_t>(lf::Kind::Success)], 0U);
}
