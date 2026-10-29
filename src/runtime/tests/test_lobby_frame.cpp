// #929: the decode, the field text and the rate limit behind a client's lobby-frame records
// (compat/lobby_frame.h). The records themselves, written by ws_bridge.cpp, are asserted in
// test_behavioral.cpp (WsBridgeLobbyFrameLog). Payloads are built here byte by byte from the layouts the
// header documents.
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "nevr/symbol_hash.h"
#include "runtime/compat/evr_codec.h"
#include "runtime/compat/lobby_frame.h"

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
