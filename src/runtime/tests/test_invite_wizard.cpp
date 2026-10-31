// #906, docs/adr/0017: the client half of the invite codes for the CREATE GAME / JOIN VIA CODE wizard, as pure
// logic (patch/invite_wizard.h): what the wizard's click metric asks for, which requests are sent and which are
// not, the log lines each carries, and what an answer means. No game, no socket, no hook.
//
// The metric lines below follow the shape echovr-modkit (rad-links, MIT) reports for the final PC build; no
// packet has been captured, so the shape is an input assumption (ADR 0017, "Not shown").

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "nevr/symbol_hash.h"
#include "runtime/log/symcache.h"
#include "runtime/patch/invite_wizard.h"
#include "runtime/patch/ui_texture_mips_rules.h"

namespace evr {
extern const SymCacheEntry kSymCacheData[];  // the generated symbol cache, sorted by hash (symcache_data.cpp)
extern const int kSymCacheCount;
}  // namespace evr

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

// ---- the prologue bytes a hook's Warning carries (docs/standards/logging.md, Rule 6) ----

TEST(InviteWizard, HexBytesWritesEachByteAsTwoDigitsInOrderWithNoSeparator) {
  const std::uint8_t bytes[] = {0x00, 0x0f, 0xa0, 0xff, 0x48};
  EXPECT_EQ(nevr_invite::HexBytes(bytes, sizeof(bytes)), "000fa0ff48");
  EXPECT_EQ(nevr_invite::HexBytes(bytes, 0), "");
  EXPECT_EQ(nevr_invite::HexBytes(nullptr, 3), "");  // no pointer, no bytes: HexBytes is total (the hooks pass only readable memory)
}

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

TEST(InviteMetric, TheShapeNamesEachKeyAndItsKindAndNeverAValue) {
  const std::string shape = nevr_invite::MetricShape(JoinLine("K7M2Q"));
  EXPECT_NE(shape.find("join_code=s5"), std::string::npos) << shape;
  EXPECT_NE(shape.find("panel_name=s13"), std::string::npos) << shape;
  EXPECT_EQ(shape.find("K7M2Q"), std::string::npos) << shape;
  EXPECT_EQ(shape.find("OVR-ORG-123"), std::string::npos) << shape;
  EXPECT_NE(nevr_invite::MetricShape(JoinLine("<invalid>")).find("join_code=invalid"), std::string::npos);
  EXPECT_EQ(nevr_invite::MetricShape(R"({"a":1,"b":true,"c":null,"d":[1],"e":{"f":"xy"}})"), "a=number,b=bool,c=null,d=array,e={f=s2}");
  EXPECT_EQ(nevr_invite::MetricShape("not json"), "unreadable");
  EXPECT_EQ(nevr_invite::MetricShape(""), "unreadable");
}

// The game's own metric (measured live on 2026-10-30, a typed code and a create click): the panel and event names at the top,
// the click's own fields in a nested `event_detail` object. The top-level keys the first version of the parser read were
// the echovr-modkit author's report and never matched, so a typed code reached no request.
TEST(InviteMetric, TheClicksOwnFieldsAreReadFromTheNestedEventDetail) {
  const Metric join = nevr_invite::ParseMetric(
      R"({"panel_type":"deep_linking","panel_name":"join_via_code","event_name":"join","event_detail":{"join_code":"K7M2Q"}})");
  EXPECT_EQ(join.kind, Kind::Join);
  EXPECT_EQ(join.join_code, "K7M2Q");
  const Metric create = nevr_invite::ParseMetric(
      R"({"panel_type":"deep_linking","panel_name":"create_game_review_settings","event_name":"create_invite","event_detail":{"Server_region":"US WEST","Expires_after":"3 HOURS"}})");
  EXPECT_EQ(create.kind, Kind::Create);
  EXPECT_EQ(create.region, "US WEST");
  EXPECT_EQ(create.expires_text, "3 HOURS");
  const Metric invalid = nevr_invite::ParseMetric(
      R"({"panel_type":"deep_linking","panel_name":"join_via_code","event_name":"join","event_detail":{"join_code":"<invalid>"}})");
  EXPECT_EQ(invalid.join_code, "");
  const Metric both = nevr_invite::ParseMetric(
      R"({"panel_type":"deep_linking","panel_name":"join_via_code","event_name":"join","join_code":"AAAAA","event_detail":{"join_code":"BBBBB"}})");
  EXPECT_EQ(both.join_code, "AAAAA");  // the top level wins when a key is in both
  const Metric notObject = nevr_invite::ParseMetric(
      R"({"panel_type":"deep_linking","panel_name":"join_via_code","event_name":"join","event_detail":"K7M2Q"})");
  EXPECT_EQ(notObject.join_code, "");
}

// The share page's JOIN (#1731): the page script's own strings (`8c3244ce60942e17.dll`) name the panel and the event.
TEST(InviteMetric, TheSharePagesJoinIsRecognisedByItsPanelAndEventAndNothingElseIs) {
  EXPECT_TRUE(nevr_invite::IsShareJoin(
      R"({"panel_type":"deep_linking","panel_name":"create_game_summary","event_name":"join","user_id":"OVR-ORG-1"})"));
  EXPECT_TRUE(nevr_invite::IsShareJoin(
      R"({"panel_type":"deep_linking","panel_name":"create_game_summary","event_name":"join","event_detail":{}})"));
  EXPECT_FALSE(nevr_invite::IsShareJoin(
      R"({"panel_type":"deep_linking","panel_name":"create_game_summary","event_name":"exit"})"));
  EXPECT_FALSE(nevr_invite::IsShareJoin(
      R"({"panel_type":"deep_linking","panel_name":"join_via_code","event_name":"join"})"));
  EXPECT_FALSE(nevr_invite::IsShareJoin(
      R"({"panel_type":"other","panel_name":"create_game_summary","event_name":"join"})"));
  EXPECT_FALSE(nevr_invite::IsShareJoin("not json"));
  EXPECT_FALSE(nevr_invite::IsShareJoin(""));
  EXPECT_EQ(nevr_invite::ParseMetric(R"({"panel_type":"deep_linking","panel_name":"create_game_summary","event_name":"join"})").kind,
            Kind::NotWizard);  // the parser the typed code and the create use does not take it
  EXPECT_EQ(nevr_invite::ShareJoinClickLine(), "[NEVR.INVITE] wizard click seen event=join panel=create_game_summary");
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
  EXPECT_EQ(nevr_invite::ExpirySeconds("99999999999999999999 DAYS"), nevr_invite::kExpiryMaxSeconds);  // 10^20: past 2^64, the multiply overflows
  EXPECT_EQ(nevr_invite::ExpirySeconds("9223372036854775808 MINUTES"), nevr_invite::kExpiryMaxSeconds);  // 2^63
}

TEST(InviteExpiry, WhatIsNotUnderstoodAsksForNothingSoTheServiceDefaultApplies) {
  for (const char* t : {"", "soon", "3", "0 HOURS", "-3 HOURS", "3 FORTNIGHTS", "HOURS", "3.5 HOURS",
                         "3 HOURS LATER", "3 HOURS 2", "1 DAY 1 DAY"}) {
    EXPECT_EQ(nevr_invite::ExpirySeconds(t), 0) << t;
  }
}

// ---- the plan: which request is sent ----

TEST(InvitePlan, ACreateSendsTheChoicesAnEmptyRegionAndTheExpiryAndNothingElse) {
  const Plan p = nevr_invite::PlanRequest(nevr_invite::ParseMetric(CreateLine("US WEST", "3 HOURS")), ArenaCustom());
  ASSERT_TRUE(p.request.has_value());
  EXPECT_EQ(p.skip, Skip::None);
  EXPECT_EQ(p.op, Op::Create);
  EXPECT_EQ(p.request->rpc, "invite/create");
  const nlohmann::json body = nlohmann::json::parse(p.request->body);
  EXPECT_EQ(body, (nlohmann::json{{"experience", "arena"}, {"mode", "custom"}, {"map", ""}, {"region", ""},
                                  {"expires_s", 10800}}));
  EXPECT_EQ(p.expires_s, 10800);
}

// The runtime names a level symbol through the generated symbol cache (invite_wizard_hook.cpp LevelName); so does this test, with
// the real table.
const char* SymCacheName(std::uint64_t symbol) {
  return nevr_ui_texture_mips::NameForHash(evr::kSymCacheData, static_cast<std::size_t>(evr::kSymCacheCount), symbol);
}

// What the service accepts for a private combat game: `LevelsByMode` in nakama's server/evr/gameserver_session_start.go
// (nakama cebca6aa6; the service hashes the request's `map` text and checks it against this list).
const char* const kNakamaCombatLevels[] = {"mpl_combat_combustion", "mpl_combat_dyson", "mpl_combat_fission",
                                           "mpl_combat_gauss"};

bool NakamaAccepts(const std::string& level) {
  for (const char* accepted : kNakamaCombatLevels) {
    if (level == accepted) return true;
  }
  return false;
}

struct MapCard {
  const char* mode;
  int card;
  const char* level;
};

// Every map card the wizard can show: the combat map page, after PAYLOAD (FISSION, SURGE) or CAPTURE POINT (DYSON, COMBUSTION).
// SURGE is the level named gauss.
const MapCard kMapCards[] = {{"payload", 1, "mpl_combat_fission"},
                             {"payload", 2, "mpl_combat_gauss"},
                             {"capture", 1, "mpl_combat_dyson"},
                             {"capture", 2, "mpl_combat_combustion"}};

// #1156, found by the first live call: the service reads `map` as a string (a level name) and refuses the whole request as
// `invalid` when it is a number.
TEST(InvitePlan, TheMapIsAlwaysAStringNeverANumber) {
  for (const Choices& choices : {Choices{"arena", "custom", 0}, Choices{"combat", "payload", 1}, Choices{"combat", "capture", 2},
                                 Choices{"combat", "random", 0}, Choices{"lobby", "", 0}}) {
    const int map = choices.map;
    const Plan p = nevr_invite::PlanRequest(nevr_invite::ParseMetric(CreateLine("US WEST", "3 HOURS")), choices, SymCacheName);
    ASSERT_TRUE(p.request.has_value()) << map;
    const nlohmann::json body = nlohmann::json::parse(p.request->body);
    ASSERT_TRUE(body.contains("map")) << map;
    EXPECT_TRUE(body["map"].is_string()) << map;
  }
}

// The chosen card's level is what the create asks for, resolved through the symbol cache, and every one of them is a name the
// service accepts for a combat game.
TEST(InviteMapCards, EveryMapCardResolvesToALevelNameNakamaAccepts) {
  for (const MapCard& card : kMapCards) {
    const Plan p = nevr_invite::PlanRequest(nevr_invite::ParseMetric(CreateLine("EU", "1 DAY")),
                                            Choices{"combat", card.mode, card.card}, SymCacheName);
    ASSERT_TRUE(p.request.has_value()) << card.mode << " card " << card.card;
    EXPECT_EQ(p.skip, Skip::None);
    const std::string level = nlohmann::json::parse(p.request->body)["map"];
    EXPECT_EQ(level, card.level) << card.mode << " card " << card.card;
    EXPECT_TRUE(NakamaAccepts(level)) << level;
  }
}

TEST(InviteMapCards, TheFourLevelSymbolsAreTheHashesOfTheirNamesAndTheCacheKnowsThem) {
  EXPECT_EQ(nevr_invite::kLevelFission, EchoVR::CSymbol64Hash("mpl_combat_fission"));
  EXPECT_EQ(nevr_invite::kLevelGauss, EchoVR::CSymbol64Hash("mpl_combat_gauss"));
  EXPECT_EQ(nevr_invite::kLevelDyson, EchoVR::CSymbol64Hash("mpl_combat_dyson"));
  EXPECT_EQ(nevr_invite::kLevelCombustion, EchoVR::CSymbol64Hash("mpl_combat_combustion"));
  for (const MapCard& card : kMapCards) {
    const std::uint64_t symbol = nevr_invite::MapLevelSymbol(Choices{"combat", card.mode, card.card});
    ASSERT_NE(symbol, 0u) << card.mode << " card " << card.card;
    const char* name = SymCacheName(symbol);
    ASSERT_NE(name, nullptr) << card.mode << " card " << card.card;
    EXPECT_STREQ(name, card.level);
  }
}

TEST(InviteMapCards, ACardTheMapPageDoesNotHaveIsRefusedNotSentAsAnyLevel) {
  for (const Choices& choices : {Choices{"combat", "payload", 3}, Choices{"combat", "capture", 3}, Choices{"combat", "payload", 9},
                                 Choices{"combat", "capture", -1}}) {
    const Plan p = nevr_invite::PlanRequest(nevr_invite::ParseMetric(CreateLine("EU", "1 DAY")), choices, SymCacheName);
    EXPECT_FALSE(p.request.has_value()) << choices.mode << " card " << choices.map;
    EXPECT_EQ(p.skip, Skip::NoLevel) << choices.mode << " card " << choices.map;
  }
}

TEST(InviteMapCards, ASymbolWithNoNameIsRefusedNotSentAsAnyLevel) {
  const Choices choices{"combat", "payload", 1};
  const Metric metric = nevr_invite::ParseMetric(CreateLine("EU", "1 DAY"));
  const auto none = [](std::uint64_t) -> const char* { return nullptr; };
  const auto empty = [](std::uint64_t) -> const char* { return ""; };
  EXPECT_EQ(nevr_invite::PlanRequest(metric, choices, none).skip, Skip::NoLevel);
  EXPECT_EQ(nevr_invite::PlanRequest(metric, choices, empty).skip, Skip::NoLevel);
  EXPECT_EQ(nevr_invite::PlanRequest(metric, choices).skip, Skip::NoLevel) << "no namer at all";
  EXPECT_FALSE(nevr_invite::PlanRequest(metric, choices, none).request.has_value());
}

// A namer that would name any symbol, even 0: a card with no level must not reach it.
TEST(InviteMapCards, ACardWithNoSymbolNeverReachesTheNamer) {
  const auto names_anything = [](std::uint64_t) -> const char* { return "mpl_combat_dyson"; };
  const Metric metric = nevr_invite::ParseMetric(CreateLine("EU", "1 DAY"));
  EXPECT_EQ(nevr_invite::PlanRequest(metric, Choices{"combat", "payload", 3}, names_anything).skip, Skip::NoLevel);
  EXPECT_EQ(nevr_invite::PlanRequest(metric, Choices{"combat", "capture", 7}, names_anything).skip, Skip::NoLevel);
}

TEST(InviteMapCards, AModeWithNoMapPageSendsNoLevelAndIsNotRefused) {
  for (const Choices& choices : {Choices{"arena", "custom", 0}, Choices{"arena", "ai", 0}, Choices{"combat", "random", 0},
                                 Choices{"lobby", "", 0}}) {
    const Plan p = nevr_invite::PlanRequest(nevr_invite::ParseMetric(CreateLine("EU", "1 DAY")), choices, SymCacheName);
    ASSERT_TRUE(p.request.has_value()) << choices.experience << " " << choices.mode;
    EXPECT_EQ(nlohmann::json::parse(p.request->body)["map"], "") << choices.experience << " " << choices.mode;
  }
}

TEST(InviteMapCards, ThePayloadAndCapturePagesLeadToDifferentLevelsForTheSameCard) {
  EXPECT_NE(nevr_invite::MapLevelSymbol(Choices{"combat", "payload", 1}), nevr_invite::MapLevelSymbol(Choices{"combat", "capture", 1}));
  EXPECT_NE(nevr_invite::MapLevelSymbol(Choices{"combat", "payload", 2}), nevr_invite::MapLevelSymbol(Choices{"combat", "capture", 2}));
  EXPECT_EQ(nevr_invite::MapLevelSymbol(Choices{"arena", "payload", 1}), 0u) << "only combat has the map page";
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

// No generic region code exists and the row's text is display text, so the request carries none and the service picks a
// server by the player's stored pings. Whatever the row says, the body's region is empty.
TEST(InvitePlan, TheRowsRegionTextIsNeverSent) {
  for (const char* text : {"US WEST", "us-west", "EU", "", "US \"WEST\" \\ x"}) {
    const Plan p = nevr_invite::PlanRequest(nevr_invite::ParseMetric(CreateLine(text, "1 DAY")), ArenaCustom());
    ASSERT_TRUE(p.request.has_value()) << text;
    const nlohmann::json body = nlohmann::json::parse(p.request->body);
    ASSERT_TRUE(body.contains("region")) << text;
    EXPECT_EQ(body["region"], "") << text;
  }
}

TEST(InvitePlan, AModeWithQuotesStaysValidJson) {
  const Plan p = nevr_invite::PlanRequest(nevr_invite::ParseMetric(CreateLine("US WEST", "1 DAY")),
                                          Choices{"arena", "a\"b\\ c", 0});
  ASSERT_TRUE(p.request.has_value());
  const nlohmann::json body = nlohmann::json::parse(p.request->body);
  EXPECT_EQ(body["mode"], "a\"b\\ c");
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
  EXPECT_EQ(nevr_invite::NotSentLine(Skip::NoLevel, 0), "[NEVR.INVITE] request not sent reason=no_level code_len=0");
  EXPECT_EQ(nevr_invite::NotSentLine(Skip::None, 0), "");
}

TEST(InviteLog, ARequestTheWorkerCannotSendSaysWhy) {
  EXPECT_EQ(nevr_invite::NotSentLine(Skip::NotConfigured, 0),
            "[NEVR.INVITE] request not sent reason=not_configured code_len=0");
  EXPECT_EQ(nevr_invite::NotSentLine(Skip::NoToken, 8), "[NEVR.INVITE] request not sent reason=no_token code_len=8");
  EXPECT_EQ(nevr_invite::NotSentLine(Skip::Busy, 8), "[NEVR.INVITE] request not sent reason=busy code_len=8");
  EXPECT_EQ(nevr_invite::NotSentLine(Skip::Stopped, 0), "[NEVR.INVITE] request not sent reason=stopped code_len=0");
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

// #1156: a refusal the service did not name (a route that is not registered, a rejected token) is told apart from one it did.
TEST(InviteLog, AnAnswerTheStatusAloneDecidedSaysSo) {
  EXPECT_EQ(nevr_invite::AnswerLine(Op::Resolve, 404, 12, Result::Invalid, true),
            "[NEVR.INVITE] answer op=resolve status=404 latency_ms=12 result=invalid via=status");
  EXPECT_EQ(nevr_invite::AnswerLine(Op::Resolve, 404, 12, Result::Invalid, false),
            "[NEVR.INVITE] answer op=resolve status=404 latency_ms=12 result=invalid");
}

// #1156: for the test endpoint only (never the log, never the menu): the message of a refusal the service did not name, so a
// run can tell a missing route from a failed allocation.
TEST(InviteAnswer, TheMessageOfAnUnnamedRefusalIsReadableAndBounded) {
  EXPECT_EQ(nevr_invite::RefusalMessage(R"({"code":14,"message":"no servers available"})"), "no servers available");
  EXPECT_EQ(nevr_invite::RefusalMessage("not json"), "");
  EXPECT_EQ(nevr_invite::RefusalMessage(R"({"code":14})"), "");
  EXPECT_EQ(nevr_invite::RefusalMessage(R"({"message":"a\u0001b\nc\u00e9"})"), "a?b?c??");  // é is two UTF-8 bytes
  const std::string longText(300, 'x');
  EXPECT_EQ(nevr_invite::RefusalMessage(nlohmann::json{{"message", longText}}.dump()).size(), nevr_invite::kRefusalMessageMax);
}

TEST(InviteAnswer, OnlyAMessageOfTheSixTokensNamesTheResult) {
  EXPECT_TRUE(nevr_invite::AnswerNamesResult(404, R"({"code":5,"message":"invalid"})"));
  EXPECT_TRUE(nevr_invite::AnswerNamesResult(429, R"({"message":"rate"})"));
  EXPECT_TRUE(nevr_invite::AnswerNamesResult(503, R"({"code":14,"message":"unavailable"})"));
  EXPECT_FALSE(nevr_invite::AnswerNamesResult(404, R"({"code":5,"message":"RPC function not found"})"));
  EXPECT_FALSE(nevr_invite::AnswerNamesResult(401, R"({"code":16,"message":"Auth token invalid"})"));
  EXPECT_FALSE(nevr_invite::AnswerNamesResult(404, "not json"));
  EXPECT_FALSE(nevr_invite::AnswerNamesResult(404, ""));
  EXPECT_FALSE(nevr_invite::AnswerNamesResult(200, R"({"message":"invalid"})"));
  EXPECT_FALSE(nevr_invite::AnswerNamesResult(0, R"({"message":"invalid"})"));
}

// ---- the answer ----

TEST(InviteAnswer, TheTokenInTheErrorSaysWhatHappened) {
  const struct {
    const char* token;
    Result result;
  } cases[] = {{"invalid", Result::Invalid}, {"expired", Result::Expired}, {"closed", Result::Closed},
               {"denied", Result::Denied},   {"limit", Result::Limit},     {"rate", Result::Rate},
               {"unavailable", Result::NoServer}};
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
  EXPECT_STREQ(nevr_invite::WizardText(Result::NoServer), "NO SERVER AVAILABLE");
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

// nakama writes its match id as MatchID.String(): "<uuid>.<node>" (server/match_id.go:57). The lobby GUID is the UUID
// part; the node names the nakama node and is not part of the GUID.
TEST(InviteAnswer, ANakamaMatchIdWithANodeIsAcceptedAsItsUuid) {
  const std::string withNode = std::string(kUuid) + ".nakama1";
  const auto r = nevr_invite::ParseResolved(nlohmann::json{{"match_id", withNode}}.dump());
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->match_id, kUuid);

  const auto created = nevr_invite::ParseCreated(nlohmann::json{{"code", "K7M2Q"}, {"match_id", withNode}}.dump());
  ASSERT_TRUE(created.has_value());
  EXPECT_EQ(created->match_id, kUuid);

  const std::string upper = "0F8FAD5B-D9CB-469F-A165-70867728950E.nakama1";
  const auto upperResolved = nevr_invite::ParseResolved(nlohmann::json{{"match_id", upper}}.dump());
  ASSERT_TRUE(upperResolved.has_value());
  EXPECT_EQ(upperResolved->match_id, kUuid);
}

TEST(InviteAnswer, AMatchIdWithAMalformedNodeOrUuidIsRefused) {
  const std::string uuid = kUuid;
  const std::vector<std::string> bads = {
      uuid + ".",                              // an empty node
      uuid + ".nakama 1",                      // a space in the node
      uuid + ".nakama\t1",                     // a control character in the node
      uuid + std::string(".nakama\x01", 8),    // another control character
      uuid + ".nakama.1",                      // a second dot
      "not-a-uuid.nakama1",                    // a bad UUID before the dot
      uuid.substr(0, 35) + "x.nakama1",        // a non-hex digit in the UUID
      "0f8fad5bd9cb469fa16570867728950e.nakama1",  // a UUID without its hyphens
  };
  for (const std::string& bad : bads) {
    EXPECT_FALSE(nevr_invite::ParseResolved(nlohmann::json{{"match_id", bad}}.dump()).has_value()) << bad;
    EXPECT_FALSE(nevr_invite::ParseCreated(nlohmann::json{{"code", "K7M2Q"}, {"match_id", bad}}.dump()).has_value())
        << bad;
  }
}

// The body production sent on 2026-10-30 is 118 bytes: a code, an RFC3339 expiry and a match id with a node. The create
// and the resolve answers both parse in full, and the expiry reads as the wizard's text.
TEST(InviteAnswer, ARealProductionCreateBodyParsesInFull) {
  const std::string body = std::string("{\"code\":\"K7M2Q\",\"expires_at\":\"2026-10-30T06:31:00Z\",\"match_id\":\"") +
                           kUuid + ".nakama1\"}";
  const auto created = nevr_invite::ParseCreated(body);
  ASSERT_TRUE(created.has_value());
  EXPECT_EQ(created->code, "K7M2Q");
  EXPECT_EQ(created->match_id, kUuid);
  EXPECT_EQ(created->expires_text, "EXPIRES: 06:31 UTC");
}

TEST(InviteAnswer, ARealProductionResolveBodyParsesInFull) {
  const std::string body = std::string("{\"match_id\":\"") + kUuid + ".nakama1\",\"mode\":\"echo_arena\",\"level\":\"mpl_arena_a\"}";
  const auto resolved = nevr_invite::ParseResolved(body);
  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(resolved->match_id, kUuid);
}

// The submit sees every remote log line; only one that names the wizard's panel is parsed at all.
TEST(InviteHook, OnlyALineThatNamesTheWizardPanelMayBeAMetric) {
  EXPECT_TRUE(nevr_invite::MayBeWizardMetric(R"({"panel_type":"deep_linking"})"));
  EXPECT_FALSE(nevr_invite::MayBeWizardMetric(R"({"panel_type":"main_menu"})"));
  EXPECT_FALSE(nevr_invite::MayBeWizardMetric(""));
}

// A create with no card choices is not sent, and the line says why.
TEST(InviteHook, ACreateWithNoCardChoicesIsNotSentAndSaysWhy) {
  nevr_invite::Metric create;
  create.kind = nevr_invite::Kind::Create;
  const nevr_invite::Plan incomplete = nevr_invite::PlanRequest(create, nevr_invite::Choices{});
  EXPECT_EQ(incomplete.skip, nevr_invite::Skip::Incomplete);
  EXPECT_EQ(nevr_invite::NotSentLine(incomplete.skip, incomplete.code_len),
            "[NEVR.INVITE] request not sent reason=incomplete code_len=0");
}

}  // namespace
