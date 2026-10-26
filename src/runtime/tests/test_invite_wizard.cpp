// #906, docs/adr/0017: the client half of the invite codes for the CREATE GAME / JOIN VIA CODE wizard, as pure
// logic (patch/invite_wizard.h): what the wizard's click metric asks for, which requests are sent and which are
// not, the log lines each carries, and what an answer means. No game, no socket, no hook.
//
// The metric lines below follow the shape echovr-modkit (rad-links, MIT) reports for the final PC build; no
// packet has been captured, so the shape is an input assumption (ADR 0017, "Not shown").

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "runtime/patch/invite_wizard.h"

namespace {

using nevr_invite::Choices;
using nevr_invite::CodeCheck;
using nevr_invite::Kind;
using nevr_invite::Metric;
using nevr_invite::Op;
using nevr_invite::Plan;
using nevr_invite::Result;
using nevr_invite::Skip;

std::string JoinLine(const std::string& code) {
  nlohmann::json j = {{"message", "click"},   {"panel_type", "deep_linking"}, {"panel_name", "join_via_code"},
                      {"event_name", "join"}, {"user_id", "OVR-ORG-123"},     {"join_code", code}};
  return j.dump();
}

std::string CreateLine(const std::string& region, const std::string& expires) {
  nlohmann::json j = {{"message", "click"},
                      {"panel_type", "deep_linking"},
                      {"panel_name", "create_game_review_settings"},
                      {"event_name", "create_invite"},
                      {"user_id", "OVR-ORG-123"},
                      {"Password", "4242"},
                      {"Server_region", region},
                      {"Expires_after", expires}};
  return j.dump();
}

Choices ArenaCustom() { return Choices{"arena", "custom", 1}; }

constexpr const char* kUuid = "0f8fad5b-d9cb-469f-a165-70867728950e";

// ---- the metric ----

TEST(InviteMetric, TheJoinClickIsAJoinWithTheTypedCode) {
  const Metric m = nevr_invite::ParseMetric(JoinLine("K7M2Q"));
  EXPECT_EQ(m.kind, Kind::Join);
  EXPECT_EQ(m.join_code, "K7M2Q");
}

TEST(InviteMetric, TheCreateClickIsACreateWithRegionAndExpiryText) {
  const Metric m = nevr_invite::ParseMetric(CreateLine("US WEST", "3 HOURS"));
  EXPECT_EQ(m.kind, Kind::Create);
  EXPECT_EQ(m.region, "US WEST");
  EXPECT_EQ(m.expires_text, "3 HOURS");
}

TEST(InviteMetric, TheGameMarksAnInvalidFieldWithASentinelThatIsNoCode) {
  const Metric m = nevr_invite::ParseMetric(JoinLine("<invalid>"));
  EXPECT_EQ(m.kind, Kind::Join);
  EXPECT_EQ(m.join_code, "");
}

TEST(InviteMetric, AJoinCodeThatIsNotTextIsNoCode) {
  const Metric m = nevr_invite::ParseMetric(
      R"({"panel_type":"deep_linking","panel_name":"join_via_code","event_name":"join","join_code":12345})");
  EXPECT_EQ(m.kind, Kind::Join);
  EXPECT_EQ(m.join_code, "");
}

TEST(InviteMetric, OtherWizardClicksAreNotRequests) {
  const std::string other =
      R"({"panel_type":"deep_linking","panel_name":"create_game_review_settings","event_name":"back"})";
  EXPECT_EQ(nevr_invite::ParseMetric(other).kind, Kind::NotWizard);
}

TEST(InviteMetric, TheSameNamesOnAnotherPanelTypeAreNotTheWizard) {
  nlohmann::json j = nlohmann::json::parse(JoinLine("K7M2Q"));
  j["panel_type"] = "social";
  EXPECT_EQ(nevr_invite::ParseMetric(j.dump()).kind, Kind::NotWizard);
}

TEST(InviteMetric, NothingThatIsNotAJsonObjectIsTheWizard) {
  EXPECT_EQ(nevr_invite::ParseMetric("").kind, Kind::NotWizard);
  EXPECT_EQ(nevr_invite::ParseMetric("not json").kind, Kind::NotWizard);
  EXPECT_EQ(nevr_invite::ParseMetric("[1,2,3]").kind, Kind::NotWizard);
  EXPECT_EQ(nevr_invite::ParseMetric("{\"panel_type\":\"deep_linking\"").kind, Kind::NotWizard);
  EXPECT_EQ(nevr_invite::ParseMetric("{}").kind, Kind::NotWizard);
}

TEST(InviteMetric, ALineLongerThanTheBoundIsDroppedUnparsed) {
  nlohmann::json j = nlohmann::json::parse(JoinLine("K7M2Q"));
  j["padding"] = std::string(nevr_invite::kMetricMaxBytes, 'x');
  const std::string line = j.dump();
  ASSERT_GT(line.size(), nevr_invite::kMetricMaxBytes);
  EXPECT_EQ(nevr_invite::ParseMetric(line).kind, Kind::NotWizard);
}

// ---- the code ----

TEST(InviteCode, ATypedCodeIsUpperCasedAndLosesSpacesAndHyphens) {
  const auto c = nevr_invite::CheckCode(" k7m-2q ");
  EXPECT_EQ(c.check, CodeCheck::Ok);
  EXPECT_EQ(c.code, "K7M2Q");
}

TEST(InviteCode, NothingTypedIsEmpty) {
  EXPECT_EQ(nevr_invite::CheckCode("").check, CodeCheck::Empty);
  EXPECT_EQ(nevr_invite::CheckCode("   ").check, CodeCheck::Empty);
  EXPECT_EQ(nevr_invite::CheckCode("---").check, CodeCheck::Empty);
}

TEST(InviteCode, EveryByteIsInTheAlphabetOrNot) {
  // Expected set written out independently of kCodeAlphabet: A-Z and 2-9, without the look-alikes 0 O 1 I L,
  // typed in either case.
  for (int b = 1; b < 256; ++b) {
    const char c = static_cast<char>(b);
    if (c == ' ' || c == '-') continue;  // dropped, not letters
    const char up = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
    const bool expected = ((up >= 'A' && up <= 'Z') || (up >= '2' && up <= '9')) && up != 'O' && up != 'I' && up != 'L';
    const auto got = nevr_invite::CheckCode(std::string("K7M") + c);
    EXPECT_EQ(got.check == CodeCheck::Ok, expected) << "byte " << b;
    if (!expected) {
      EXPECT_EQ(got.check, CodeCheck::Malformed) << "byte " << b;
    }
  }
}

TEST(InviteCode, TheLookAlikesAreRefusedByName) {
  for (const char c : std::string("0O1IL")) {
    EXPECT_EQ(nevr_invite::CheckCode(std::string("K7M2") + c).check, CodeCheck::Malformed) << c;
  }
}

TEST(InviteCode, TheAlphabetHas31DistinctSymbols) {
  EXPECT_EQ(nevr_invite::kCodeAlphabet.size(), 31u);
  for (std::size_t i = 0; i < nevr_invite::kCodeAlphabet.size(); ++i) {
    for (std::size_t j = i + 1; j < nevr_invite::kCodeAlphabet.size(); ++j) {
      EXPECT_NE(nevr_invite::kCodeAlphabet[i], nevr_invite::kCodeAlphabet[j]);
    }
  }
}

TEST(InviteCode, TheTextContainerBoundsTheLengthAt63) {
  EXPECT_EQ(nevr_invite::kCodeMaxChars, 63u);
  EXPECT_EQ(nevr_invite::CheckCode(std::string(63, 'K')).check, CodeCheck::Ok);
  EXPECT_EQ(nevr_invite::CheckCode(std::string(64, 'K')).check, CodeCheck::TooLong);
}

TEST(InviteCode, TheLengthIsMeasuredAfterTheSeparatorsGo) {
  std::string typed;  // 63 symbols with a hyphen after every fifth: 75 characters typed
  for (int i = 0; i < 63; ++i) {
    typed += 'K';
    if (i % 5 == 4) typed += '-';
  }
  ASSERT_GT(typed.size(), nevr_invite::kCodeMaxChars);
  const auto c = nevr_invite::CheckCode(typed);
  EXPECT_EQ(c.check, CodeCheck::Ok);
  EXPECT_EQ(c.code.size(), 63u);
}

TEST(InviteCode, ABadSymbolIsMalformedEvenWhenTheCodeIsAlsoTooLong) {
  EXPECT_EQ(nevr_invite::CheckCode(std::string(64, 'K') + "0").check, CodeCheck::Malformed);
}

// ---- the expiry ----

TEST(InviteExpiry, TheWizardsTextsBecomeSeconds) {
  EXPECT_EQ(nevr_invite::ExpirySeconds("30 MINUTES"), 1800);
  EXPECT_EQ(nevr_invite::ExpirySeconds("1 MINUTE"), 60);
  EXPECT_EQ(nevr_invite::ExpirySeconds("3 HOURS"), 10800);
  EXPECT_EQ(nevr_invite::ExpirySeconds("1 HOUR"), 3600);
  EXPECT_EQ(nevr_invite::ExpirySeconds("1 DAY"), 86400);
  EXPECT_EQ(nevr_invite::ExpirySeconds("7 DAYS"), 604800);
}

TEST(InviteExpiry, CaseAndSpacingAreForgiven) {
  EXPECT_EQ(nevr_invite::ExpirySeconds("3 hours"), 10800);
  EXPECT_EQ(nevr_invite::ExpirySeconds("3HOURS"), 10800);
  EXPECT_EQ(nevr_invite::ExpirySeconds("  2   days "), 172800);
}

TEST(InviteExpiry, NothingLongerThanSevenDaysIsAsked) {
  EXPECT_EQ(nevr_invite::kExpiryMaxSeconds, 7 * 24 * 60 * 60);
  EXPECT_EQ(nevr_invite::ExpirySeconds("8 DAYS"), nevr_invite::kExpiryMaxSeconds);
  EXPECT_EQ(nevr_invite::ExpirySeconds("999999999999 DAYS"), nevr_invite::kExpiryMaxSeconds);
  EXPECT_EQ(nevr_invite::ExpirySeconds("168 HOURS"), nevr_invite::kExpiryMaxSeconds);
  EXPECT_EQ(nevr_invite::ExpirySeconds("169 HOURS"), nevr_invite::kExpiryMaxSeconds);
  EXPECT_EQ(nevr_invite::ExpirySeconds("99999999999999999999999999999999 DAYS"), nevr_invite::kExpiryMaxSeconds);
  EXPECT_EQ(nevr_invite::ExpirySeconds("18446744073709551616 DAYS"), nevr_invite::kExpiryMaxSeconds);  // 2^64
  EXPECT_EQ(nevr_invite::ExpirySeconds("9223372036854775808 MINUTES"), nevr_invite::kExpiryMaxSeconds);  // 2^63
}

TEST(InviteExpiry, WhatIsNotUnderstoodAsksForNothingSoTheServiceDefaultApplies) {
  for (const char* t : {"", "soon", "3", "0 HOURS", "-3 HOURS", "3 FORTNIGHTS", "HOURS", "3.5 HOURS",
                         "3 HOURS LATER", "3 HOURS 2", "1 DAY 1 DAY"}) {
    EXPECT_EQ(nevr_invite::ExpirySeconds(t), 0) << t;
  }
}

// ---- the plan: which request is sent ----

TEST(InvitePlan, ACreateSendsTheChoicesTheRegionAndTheExpiryAndNothingElse) {
  const Plan p = nevr_invite::PlanRequest(nevr_invite::ParseMetric(CreateLine("US WEST", "3 HOURS")), ArenaCustom());
  ASSERT_TRUE(p.request.has_value());
  EXPECT_EQ(p.skip, Skip::None);
  EXPECT_EQ(p.op, Op::Create);
  EXPECT_EQ(p.request->rpc, "invite/create");
  const nlohmann::json body = nlohmann::json::parse(p.request->body);
  EXPECT_EQ(body, (nlohmann::json{{"experience", "arena"}, {"mode", "custom"}, {"map", 1}, {"region", "US WEST"},
                                  {"expires_s", 10800}}));
  EXPECT_EQ(p.expires_s, 10800);
}

TEST(InvitePlan, ThePasswordTheMetricCarriesIsNeverForwarded) {
  const Plan p = nevr_invite::PlanRequest(nevr_invite::ParseMetric(CreateLine("US WEST", "3 HOURS")), ArenaCustom());
  ASSERT_TRUE(p.request.has_value());
  EXPECT_EQ(p.request->body.find("4242"), std::string::npos);
  EXPECT_EQ(p.request->body.find("assword"), std::string::npos);
}

TEST(InvitePlan, ACreateWithAnExpiryOverTheMaximumAsksForTheMaximum) {
  const Plan p = nevr_invite::PlanRequest(nevr_invite::ParseMetric(CreateLine("EU", "30 DAYS")), ArenaCustom());
  ASSERT_TRUE(p.request.has_value());
  EXPECT_EQ(p.expires_s, nevr_invite::kExpiryMaxSeconds);
  EXPECT_EQ(nlohmann::json::parse(p.request->body)["expires_s"], nevr_invite::kExpiryMaxSeconds);
}

TEST(InvitePlan, ACreateWithAnExpiryNotUnderstoodAsksForZeroAndTheServiceDecides) {
  const Plan p = nevr_invite::PlanRequest(nevr_invite::ParseMetric(CreateLine("EU", "whenever")), ArenaCustom());
  ASSERT_TRUE(p.request.has_value());
  EXPECT_EQ(nlohmann::json::parse(p.request->body)["expires_s"], 0);
}

TEST(InvitePlan, ARegionWithQuotesStaysValidJson) {
  const Plan p =
      nevr_invite::PlanRequest(nevr_invite::ParseMetric(CreateLine("US \"WEST\" \\ x", "1 DAY")), ArenaCustom());
  ASSERT_TRUE(p.request.has_value());
  const nlohmann::json body = nlohmann::json::parse(p.request->body);
  EXPECT_EQ(body["region"], "US \"WEST\" \\ x");
}

TEST(InvitePlan, ACreateBeforeTheCardsWereChosenSendsNothing) {
  for (const Choices& c : {Choices{"", "custom", 1}, Choices{"arena", "", 1}, Choices{"", "", 0}}) {
    const Plan p = nevr_invite::PlanRequest(nevr_invite::ParseMetric(CreateLine("EU", "1 DAY")), c);
    EXPECT_FALSE(p.request.has_value());
    EXPECT_EQ(p.skip, Skip::Incomplete);
  }
}

TEST(InvitePlan, ALobbyHasNoModeToChoose) {
  const Plan p =
      nevr_invite::PlanRequest(nevr_invite::ParseMetric(CreateLine("EU", "1 DAY")), Choices{"lobby", "", 0});
  ASSERT_TRUE(p.request.has_value());
  EXPECT_EQ(nlohmann::json::parse(p.request->body)["experience"], "lobby");
}

TEST(InvitePlan, AJoinSendsTheNormalisedCodeAlone) {
  const Plan p = nevr_invite::PlanRequest(nevr_invite::ParseMetric(JoinLine("k7m-2q")), Choices{});
  ASSERT_TRUE(p.request.has_value());
  EXPECT_EQ(p.op, Op::Resolve);
  EXPECT_EQ(p.request->rpc, "invite/resolve");
  EXPECT_EQ(nlohmann::json::parse(p.request->body), (nlohmann::json{{"code", "K7M2Q"}}));
  EXPECT_EQ(p.code_len, 5u);
}

TEST(InvitePlan, AJoinWithNothingTypedSendsNothing) {
  for (const char* typed : {"", "   ", "<invalid>"}) {
    const Plan p = nevr_invite::PlanRequest(nevr_invite::ParseMetric(JoinLine(typed)), Choices{});
    EXPECT_FALSE(p.request.has_value()) << typed;
    EXPECT_EQ(p.skip, Skip::Empty) << typed;
    EXPECT_EQ(p.code_len, 0u) << typed;
  }
}

TEST(InvitePlan, AJoinWithASymbolOutsideTheAlphabetSendsNothing) {
  const Plan p = nevr_invite::PlanRequest(nevr_invite::ParseMetric(JoinLine("K7M2O")), Choices{});
  EXPECT_FALSE(p.request.has_value());
  EXPECT_EQ(p.skip, Skip::Malformed);
  EXPECT_EQ(p.code_len, 5u);
}

TEST(InvitePlan, AJoinLongerThanTheFieldCanHoldSendsNothing) {
  const Plan p = nevr_invite::PlanRequest(nevr_invite::ParseMetric(JoinLine(std::string(64, 'K'))), Choices{});
  EXPECT_FALSE(p.request.has_value());
  EXPECT_EQ(p.skip, Skip::TooLong);
  EXPECT_EQ(p.code_len, 64u);
}

TEST(InvitePlan, ClicksThatAreNotTheWizardPlanNothingAndSkipNothing) {
  const Plan p = nevr_invite::PlanRequest(nevr_invite::ParseMetric("{}"), ArenaCustom());
  EXPECT_FALSE(p.request.has_value());
  EXPECT_EQ(p.skip, Skip::None);
}

// ---- the log lines ----

TEST(InviteLog, ACodeIsNeverInAFingerprintedLine) {
  EXPECT_EQ(nevr_invite::CodeFingerprint("K7M2Q", "salt1"), "7bb022a4");
  EXPECT_EQ(nevr_invite::CodeFingerprint("K7M2Q", "salt2"), "ccf6be2d");
  EXPECT_EQ(nevr_invite::CodeFingerprint("K7M2R", "salt1"), "7eb0275d");
}

TEST(InviteLog, TheClickLinesNameTheEvent) {
  EXPECT_EQ(nevr_invite::ClickLine(Kind::Create),
            "[NEVR.INVITE] wizard click seen event=create_invite panel=deep_linking");
  EXPECT_EQ(nevr_invite::ClickLine(Kind::Join),
            "[NEVR.INVITE] wizard click seen event=join_via_code panel=deep_linking");
  EXPECT_EQ(nevr_invite::ClickLine(Kind::NotWizard), "");
}

TEST(InviteLog, ARequestNotSentSaysWhy) {
  EXPECT_EQ(nevr_invite::NotSentLine(Skip::Empty, 0), "[NEVR.INVITE] request not sent reason=empty code_len=0");
  EXPECT_EQ(nevr_invite::NotSentLine(Skip::Malformed, 5), "[NEVR.INVITE] request not sent reason=malformed code_len=5");
  EXPECT_EQ(nevr_invite::NotSentLine(Skip::TooLong, 64), "[NEVR.INVITE] request not sent reason=too_long code_len=64");
  EXPECT_EQ(nevr_invite::NotSentLine(Skip::Incomplete, 0),
            "[NEVR.INVITE] request not sent reason=incomplete code_len=0");
  EXPECT_EQ(nevr_invite::NotSentLine(Skip::None, 0), "");
}

TEST(InviteLog, ACreateRequestLineCarriesTheExpiryAndNoCode) {
  const Plan p = nevr_invite::PlanRequest(nevr_invite::ParseMetric(CreateLine("US WEST", "3 HOURS")), ArenaCustom());
  EXPECT_EQ(nevr_invite::SentLine(p, "7bb022a4"), "[NEVR.INVITE] request sent op=create expires_s=10800");
}

TEST(InviteLog, AResolveRequestLineCarriesTheLengthAndTheFingerprintNeverTheCode) {
  const Plan p = nevr_invite::PlanRequest(nevr_invite::ParseMetric(JoinLine("K7M2Q")), Choices{});
  const std::string line = nevr_invite::SentLine(p, "7bb022a4");
  EXPECT_EQ(line, "[NEVR.INVITE] request sent op=resolve code_len=5 code_fp=7bb022a4");
  EXPECT_EQ(line.find("K7M2Q"), std::string::npos);
}

TEST(InviteLog, APlanWithNoRequestHasNoSentLine) {
  EXPECT_EQ(nevr_invite::SentLine(Plan{}, "7bb022a4"), "");
}

TEST(InviteLog, TheAnswerLineCarriesTheStatusTheLatencyAndTheResult) {
  EXPECT_EQ(nevr_invite::AnswerLine(Op::Resolve, 200, 41, Result::Ok),
            "[NEVR.INVITE] answer op=resolve status=200 latency_ms=41 result=ok");
  EXPECT_EQ(nevr_invite::AnswerLine(Op::Create, 429, 7, Result::Limit),
            "[NEVR.INVITE] answer op=create status=429 latency_ms=7 result=limit");
}

// ---- the answer ----

TEST(InviteAnswer, TheTokenInTheErrorSaysWhatHappened) {
  const struct {
    const char* token;
    Result result;
  } cases[] = {{"invalid", Result::Invalid}, {"expired", Result::Expired}, {"closed", Result::Closed},
               {"denied", Result::Denied},   {"limit", Result::Limit},     {"rate", Result::Rate}};
  for (const auto& c : cases) {
    const std::string body = std::string("{\"code\":9,\"message\":\"") + c.token + "\"}";
    EXPECT_EQ(nevr_invite::ResultFromAnswer(400, body), c.result) << c.token;
    EXPECT_EQ(std::string(nevr_invite::ResultName(c.result)), c.token);
  }
}

TEST(InviteAnswer, TheTokenWinsOverTheStatus) {
  EXPECT_EQ(nevr_invite::ResultFromAnswer(404, R"({"code":5,"message":"expired"})"), Result::Expired);
}

TEST(InviteAnswer, WithoutATokenTheStatusDecides) {
  EXPECT_EQ(nevr_invite::ResultFromAnswer(200, ""), Result::Ok);
  EXPECT_EQ(nevr_invite::ResultFromAnswer(404, ""), Result::Invalid);
  EXPECT_EQ(nevr_invite::ResultFromAnswer(403, ""), Result::Denied);
  EXPECT_EQ(nevr_invite::ResultFromAnswer(429, ""), Result::Rate);
  EXPECT_EQ(nevr_invite::ResultFromAnswer(500, ""), Result::ServerError);
  EXPECT_EQ(nevr_invite::ResultFromAnswer(0, ""), Result::Unavailable);
}

TEST(InviteAnswer, ASuccessIsASuccessWhateverTheBodySays) {
  EXPECT_EQ(nevr_invite::ResultFromAnswer(200, R"({"message":"expired"})"), Result::Ok);
}

TEST(InviteAnswer, TextThatIsNotExactlyATokenIsNeverInterpreted) {
  for (const char* m : {"Expired", "expired: try later", " expired", "expired\n", "boom", ""}) {
    nlohmann::json body = {{"code", 3}, {"message", m}};
    EXPECT_EQ(nevr_invite::ResultFromAnswer(400, body.dump()), Result::ServerError) << m;
  }
  EXPECT_EQ(nevr_invite::ResultFromAnswer(400, "<html>nope</html>"), Result::ServerError);
}

TEST(InviteAnswer, ATokenInsideOtherTextIsNotAToken) {
  for (const char* token : {"invalid", "expired", "closed", "denied", "limit", "rate"}) {
    const std::string texts[] = {std::string("not ") + token, std::string(token) + "s", std::string("x") + token + "x"};
    for (const std::string& text : texts) {
      nlohmann::json body = {{"code", 3}, {"message", text}};
      EXPECT_EQ(nevr_invite::ResultFromAnswer(400, body.dump()), Result::ServerError) << text;
    }
  }
}

TEST(InviteAnswer, TheWizardSaysEachResultInItsOwnWords) {
  EXPECT_STREQ(nevr_invite::WizardText(Result::Ok), "");
  EXPECT_STREQ(nevr_invite::WizardText(Result::Invalid), "INVALID CODE");
  EXPECT_STREQ(nevr_invite::WizardText(Result::Expired), "CODE EXPIRED");
  EXPECT_STREQ(nevr_invite::WizardText(Result::Closed), "CODE CLOSED");
  EXPECT_STREQ(nevr_invite::WizardText(Result::Denied), "NOT ALLOWED");
  EXPECT_STREQ(nevr_invite::WizardText(Result::Limit), "TOO MANY CODES");
  EXPECT_STREQ(nevr_invite::WizardText(Result::Rate), "TRY AGAIN LATER");
  EXPECT_STREQ(nevr_invite::WizardText(Result::Unavailable), "NO CONNECTION");
  EXPECT_STREQ(nevr_invite::WizardText(Result::ServerError), "SERVER ERROR");
}

TEST(InviteAnswer, OnlyOkIsNotAFailure) {
  EXPECT_FALSE(nevr_invite::IsFailure(Result::Ok));
  for (const Result r : {Result::Invalid, Result::Expired, Result::Closed, Result::Denied, Result::Limit, Result::Rate,
                         Result::Unavailable, Result::ServerError}) {
    EXPECT_TRUE(nevr_invite::IsFailure(r));
  }
}

TEST(InviteAnswer, ACreatedCodeIsCheckedBeforeItIsWrittenIntoTheGame) {
  const nlohmann::json ok = {{"code", "K7M2Q"}, {"match_id", kUuid}, {"expires_at", "2026-10-21T05:38:59.63Z"}};
  const auto created = nevr_invite::ParseCreated(ok.dump());
  ASSERT_TRUE(created.has_value());
  EXPECT_EQ(created->code, "K7M2Q");
  EXPECT_EQ(created->match_id, kUuid);
  EXPECT_EQ(created->expires_text, "EXPIRES: 05:38 UTC");

  nlohmann::json bad = ok;
  bad["code"] = "K7M2O";
  EXPECT_FALSE(nevr_invite::ParseCreated(bad.dump()).has_value());
  bad = ok;
  bad["code"] = "k7m2q";  // the service sends the canonical form
  EXPECT_FALSE(nevr_invite::ParseCreated(bad.dump()).has_value());
  bad = ok;
  bad["code"] = "";
  EXPECT_FALSE(nevr_invite::ParseCreated(bad.dump()).has_value());
  bad = ok;
  bad.erase("code");
  EXPECT_FALSE(nevr_invite::ParseCreated(bad.dump()).has_value());
  bad = ok;
  bad["match_id"] = "not-a-uuid";
  EXPECT_FALSE(nevr_invite::ParseCreated(bad.dump()).has_value());
  EXPECT_FALSE(nevr_invite::ParseCreated("").has_value());
  EXPECT_FALSE(nevr_invite::ParseCreated("[]").has_value());
}

TEST(InviteAnswer, AnExpiryTheServiceDidNotSendOrSentBadlyReadsAsADash) {
  nlohmann::json j = {{"code", "K7M2Q"}, {"match_id", kUuid}};
  EXPECT_EQ(nevr_invite::ParseCreated(j.dump())->expires_text, "EXPIRES: -");
  j["expires_at"] = "tomorrow";
  EXPECT_EQ(nevr_invite::ParseCreated(j.dump())->expires_text, "EXPIRES: -");
  j["expires_at"] = "2026-10-21T25:99:00Z";
  EXPECT_EQ(nevr_invite::ParseCreated(j.dump())->expires_text, "EXPIRES: -");
  j["expires_at"] = "2026-10-21T05:38:59+02:00";  // only the UTC form is read
  EXPECT_EQ(nevr_invite::ParseCreated(j.dump())->expires_text, "EXPIRES: -");
  j["expires_at"] = "2026-10-21T05:38:59";
  EXPECT_EQ(nevr_invite::ParseCreated(j.dump())->expires_text, "EXPIRES: -");
  for (const char* bad : {"2026-10-21T05:38:59Q", "2026-10-21T05:38:59.63Q", "2026-10-21T25:00:00Z",
                          "2026-10-21T05:60:00Z", "2026-13-21T05:38:59Z", "2026-10-32T05:38:59Z",
                          "2026-10-21T05:38:59.Z"}) {
    j["expires_at"] = bad;
    EXPECT_EQ(nevr_invite::ParseCreated(j.dump())->expires_text, "EXPIRES: -") << bad;
  }
  j["expires_at"] = 12345;
  EXPECT_EQ(nevr_invite::ParseCreated(j.dump())->expires_text, "EXPIRES: -");
}

TEST(InviteAnswer, AResolvedMatchIdIsAUuidBeforeItIsStagedAsTheLobbyId) {
  const auto r = nevr_invite::ParseResolved(nlohmann::json{{"match_id", kUuid}, {"mode", "arena"}}.dump());
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->match_id, kUuid);

  const auto upper = nevr_invite::ParseResolved(nlohmann::json{{"match_id", "0F8FAD5B-D9CB-469F-A165-70867728950E"}}.dump());
  ASSERT_TRUE(upper.has_value());
  EXPECT_EQ(upper->match_id, kUuid);

  for (const char* bad : {"", "not-a-uuid", "0f8fad5bd9cb469fa16570867728950e", "{0f8fad5b-d9cb-469f-a165-70867728950e}",
                          "0f8fad5b-d9cb-469f-a165-70867728950g", "0f8fad5b-d9cb-469f-a165-70867728950e0"}) {
    EXPECT_FALSE(nevr_invite::ParseResolved(nlohmann::json{{"match_id", bad}}.dump()).has_value()) << bad;
  }
  EXPECT_FALSE(nevr_invite::ParseResolved("{}").has_value());
  EXPECT_FALSE(nevr_invite::ParseResolved("nope").has_value());
}

}  // namespace
