// #1423: AI bots on a dedicated server (patch/server_bots.h): the config value, the session settings rewrite, the
// request, the schedule, the body check, the reads of the game's memory (on buffers), Install's decision, the log
// lines and the site witnesses. Pure: no game image.

#include "runtime/patch/server_bots.h"
#include "runtime/patch/server_bots_install.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

using namespace nevr_server_bots;

// --- the config value ------------------------------------------------------------------------------------------

TEST(ServerBotsParse, AbsentOrEmptyIsOff) {
  std::string problem = "stale";
  EXPECT_EQ(ParseBotsPerSession(nullptr, &problem), 0u);
  EXPECT_TRUE(problem.empty());
  EXPECT_EQ(ParseBotsPerSession("", &problem), 0u);
  EXPECT_TRUE(problem.empty());
}

TEST(ServerBotsParse, ZeroToSixteenAreTaken) {
  for (uint32_t n = 0; n <= 16; ++n) {
    std::string problem;
    EXPECT_EQ(ParseBotsPerSession(std::to_string(n).c_str(), &problem), n);
    EXPECT_TRUE(problem.empty()) << n;
  }
}

TEST(ServerBotsParse, AnythingElseIsOffWithTheReason) {
  struct Case {
    const char* text;
    const char* reason;
  };
  // 4294967297 and 4294967312 wrap a 32-bit count to 1 and 16: the limit is checked digit by digit, before it can wrap.
  const Case cases[] = {{"17", "above"},   {"99999999999", "above"}, {"4294967297", "above"}, {"4294967312", "above"},
                        {"-1", "whole"},   {"x", "whole"},           {"2.5", "whole"},        {" 2", "whole"}};
  for (const Case& c : cases) {
    std::string problem;
    EXPECT_EQ(ParseBotsPerSession(c.text, &problem), 0u) << c.text;
    EXPECT_NE(problem.find(c.reason), std::string::npos) << c.text << ": " << problem;
  }
}

// --- the session settings ----------------------------------------------------------------------------------------

// A session start as the service's GameServerSessionStart.Stream writes it: 16-byte match id, 16-byte group id,
// player limit, entrant count, lobby type, a pad byte, the settings JSON with its NUL, then the entrants.
std::vector<uint8_t> Frame(const std::string& settings, const std::vector<uint8_t>& entrants) {
  std::vector<uint8_t> f;
  for (int i = 0; i < 16; ++i) f.push_back(static_cast<uint8_t>(0x10 + i));
  for (int i = 0; i < 16; ++i) f.push_back(static_cast<uint8_t>(0x30 + i));
  f.push_back(16);  // player limit
  f.push_back(static_cast<uint8_t>(entrants.size() / 40));
  f.push_back(1);  // lobby type
  f.push_back(0);  // pad
  f.insert(f.end(), settings.begin(), settings.end());
  f.push_back(0);
  f.insert(f.end(), entrants.begin(), entrants.end());
  return f;
}

// The game's symbol hash of the level (not an account id); the N20 sensor exempts a line that names a hash.
constexpr uint64_t kLevelSymbolHash = 13004389186521124930ull;
const std::string kServiceSettings =
    std::string(R"({"appid":"1369078409873402","gametype":-3791849610740453517,"level":)") +
    std::to_string(kLevelSymbolHash) + R"(,"features":["x"]})";
const std::vector<uint8_t> kEntrants(80, 0xAB);

TEST(ServerBotsSettings, TheHeaderIsThirtySixBytes) { EXPECT_EQ(kSessionStartHeaderSize, 36u); }

TEST(ServerBotsSettings, NoBotsLeavesTheFrameAlone) {
  const std::vector<uint8_t> f = Frame(kServiceSettings, kEntrants);
  const SettingsRewrite r = RewriteSessionSettings(f.data(), f.size(), 0, 5);
  EXPECT_EQ(r.result, SettingsResult::kNotWanted);
  EXPECT_TRUE(r.frame.empty());
}

TEST(ServerBotsSettings, BotsAddBothKeysAndKeepEverythingElse) {
  const std::vector<uint8_t> f = Frame(kServiceSettings, kEntrants);
  const SettingsRewrite r = RewriteSessionSettings(f.data(), f.size(), 2, 5);
  ASSERT_EQ(r.result, SettingsResult::kRewritten);
  EXPECT_TRUE(r.addedNumTeams);
  EXPECT_TRUE(r.addedPlayersPerTeam);
  EXPECT_EQ(r.settingsBytes, kServiceSettings.size());
  ASSERT_GT(r.frame.size(), kSessionStartHeaderSize);
  EXPECT_EQ(std::memcmp(r.frame.data(), f.data(), kSessionStartHeaderSize), 0) << "the header is unchanged";
  const char* json = reinterpret_cast<const char*>(r.frame.data() + kSessionStartHeaderSize);
  const size_t jsonLen = std::strlen(json);
  EXPECT_EQ(jsonLen, r.rewrittenBytes);
  // The entrants follow the NUL, byte for byte.
  ASSERT_EQ(r.frame.size(), kSessionStartHeaderSize + jsonLen + 1 + kEntrants.size());
  EXPECT_EQ(std::vector<uint8_t>(r.frame.end() - kEntrants.size(), r.frame.end()), kEntrants);
  const nlohmann::json doc = nlohmann::json::parse(json);
  EXPECT_EQ(doc.at("numteams"), 2);
  EXPECT_EQ(doc.at("players_per_team"), 5);
  EXPECT_TRUE(doc.at("numteams").is_number_integer()) << "the game reads an integer";
  EXPECT_EQ(doc.at("appid"), "1369078409873402");
  EXPECT_EQ(doc.at("gametype").get<int64_t>(), -3791849610740453517LL);
  EXPECT_EQ(doc.at("level").get<uint64_t>(), 13004389186521124930ULL);
  EXPECT_EQ(doc.at("features"), nlohmann::json::array({"x"}));
  // The service's own keys come first, in the order it sent them.
  EXPECT_EQ(std::string(json).rfind(kServiceSettings.substr(0, kServiceSettings.size() - 1), 0), 0u) << json;
}

TEST(ServerBotsSettings, TextOutsideAsciiKeepsItsBytes) {
  // Only the two keys change: a name outside ASCII comes out as the UTF-8 bytes the service sent, not as \u escapes.
  const std::string settings = "{\"gametype\":-3791849610740453517,\"name\":\"Zo\xC3\xAB \xF0\x9F\xA6\x8A\"}";
  const std::vector<uint8_t> f = Frame(settings, kEntrants);
  const SettingsRewrite r = RewriteSessionSettings(f.data(), f.size(), 2, 5);
  ASSERT_EQ(r.result, SettingsResult::kRewritten);
  const std::string json(reinterpret_cast<const char*>(r.frame.data() + kSessionStartHeaderSize));
  EXPECT_EQ(json.rfind(settings.substr(0, settings.size() - 1), 0), 0u) << json;
}

TEST(ServerBotsSettings, ASixPlayerTeamWritesSix) {
  // network.max_team_size = 6 (#1422) reaches the session settings as players_per_team: 6, not the game's 5.
  const std::vector<uint8_t> f = Frame(kServiceSettings, kEntrants);
  const SettingsRewrite r = RewriteSessionSettings(f.data(), f.size(), 1, 6);
  ASSERT_EQ(r.result, SettingsResult::kRewritten);
  const nlohmann::json doc =
      nlohmann::json::parse(reinterpret_cast<const char*>(r.frame.data() + kSessionStartHeaderSize));
  EXPECT_EQ(doc.at("players_per_team"), 6);
  EXPECT_EQ(r.playersPerTeamWritten, 6);
  EXPECT_NE(SettingsLine(r, "AAAA", 171).text.find("players_per_team=6"), std::string::npos);
}

TEST(ServerBotsSettings, PlayersPerTeamIsTheValueGiven) {
  const std::vector<uint8_t> f = Frame(kServiceSettings, kEntrants);
  const SettingsRewrite r = RewriteSessionSettings(f.data(), f.size(), 1, 7);
  ASSERT_EQ(r.result, SettingsResult::kRewritten);
  const nlohmann::json doc =
      nlohmann::json::parse(reinterpret_cast<const char*>(r.frame.data() + kSessionStartHeaderSize));
  EXPECT_EQ(doc.at("players_per_team"), 7);
}

TEST(ServerBotsSettings, AValueAlreadyThereIsKept) {
  const std::vector<uint8_t> f = Frame(R"({"appid":"a","gametype":-3791849610740453517,"numteams":3})", kEntrants);
  const SettingsRewrite r = RewriteSessionSettings(f.data(), f.size(), 2, 5);
  ASSERT_EQ(r.result, SettingsResult::kRewritten);
  EXPECT_FALSE(r.addedNumTeams);
  EXPECT_TRUE(r.addedPlayersPerTeam);
  const nlohmann::json doc =
      nlohmann::json::parse(reinterpret_cast<const char*>(r.frame.data() + kSessionStartHeaderSize));
  EXPECT_EQ(doc.at("numteams"), 3);
  EXPECT_EQ(doc.at("players_per_team"), 5);
}

TEST(ServerBotsSettings, BothAlreadyThereLeavesTheFrameAlone) {
  const std::vector<uint8_t> f = Frame(R"({"gametype":-3791849610740453517,"numteams":2,"players_per_team":4})", kEntrants);
  const SettingsRewrite r = RewriteSessionSettings(f.data(), f.size(), 2, 5);
  EXPECT_EQ(r.result, SettingsResult::kAlreadyPresent);
  EXPECT_TRUE(r.frame.empty());
  EXPECT_FALSE(IsSettingsFailure(r.result));
}

TEST(ServerBotsSettings, AFrameThatCannotBeReadIsLeftAloneAndCalledAFailure) {
  std::vector<uint8_t> noNul = Frame(kServiceSettings, {});
  noNul.pop_back();  // the NUL
  const std::vector<uint8_t> notJson = Frame("{\"appid\":", kEntrants);
  const std::vector<uint8_t> notObject = Frame("[1,2]", kEntrants);
  const std::vector<uint8_t> tooShort(kSessionStartHeaderSize, 0);
  struct Case {
    const std::vector<uint8_t>* frame;
    SettingsResult want;
  };
  for (const Case& c : {Case{&noNul, SettingsResult::kNoTerminator}, Case{&notJson, SettingsResult::kNotJson},
                        Case{&notObject, SettingsResult::kNotObject}, Case{&tooShort, SettingsResult::kTooShort}}) {
    const SettingsRewrite r = RewriteSessionSettings(c.frame->data(), c.frame->size(), 2, 5);
    EXPECT_EQ(r.result, c.want) << SettingsResultName(r.result);
    EXPECT_TRUE(r.frame.empty());
    EXPECT_TRUE(IsSettingsFailure(r.result)) << SettingsResultName(r.result);
  }
  EXPECT_EQ(RewriteSessionSettings(nullptr, 100, 2, 5).result, SettingsResult::kTooShort);
}

TEST(ServerBotsSettings, OnlyARewrittenFrameReplacesTheOneTheGameGets) {
  const std::vector<uint8_t> in = Frame(kServiceSettings, kEntrants);
  std::vector<uint8_t> storage;
  void* frame = const_cast<uint8_t*>(in.data());
  uint64_t size = in.size();
  SettingsRewrite r = RewriteSessionSettings(in.data(), in.size(), 2, 5);
  const std::vector<uint8_t> rewritten = r.frame;
  ASSERT_EQ(r.result, SettingsResult::kRewritten);
  EXPECT_TRUE(UseRewrite(r, storage, &frame, &size));
  EXPECT_EQ(frame, static_cast<void*>(storage.data()));
  EXPECT_EQ(size, rewritten.size());
  EXPECT_EQ(storage, rewritten);
  for (SettingsResult other : {SettingsResult::kNotWanted, SettingsResult::kAlreadyPresent, SettingsResult::kNotJson}) {
    SettingsRewrite o;
    o.result = other;
    o.frame = {1, 2, 3};
    void* f = const_cast<uint8_t*>(in.data());
    uint64_t s = in.size();
    EXPECT_FALSE(UseRewrite(o, storage, &f, &s)) << SettingsResultName(other);
    EXPECT_EQ(f, static_cast<const void*>(in.data()));
    EXPECT_EQ(s, in.size());
  }
}

TEST(ServerBotsGameType, TheSymbolsAreTheServicesHashes) {
  // The service's own hashes, not ones computed here: ten from its table (EchoTools/nakama server/evr/
  // core_hash_lookup.go); echo_arena_practice, which the table lacks, from its own ToSymbol (server/evr/core_hash.go),
  // which gives the table's value for the other ten.
  const std::pair<const char*, uint64_t> service[] = {
      {"echo_arena", 0xcb60a4de7e1caf73ULL},          {"echo_arena_private", 0x09990965f4db8c03ULL},
      {"echo_arena_tournament", 0xd53a9c73b8b2c760ULL}, {"echo_arena_public_ai", 0xd54d62c40852c2f9ULL},
      {"echo_arena_practice", 0xe899792852ba0d5fULL},
      {"echo_combat", 0x3d5c3976578a321aULL},         {"echo_combat_private", 0x33bbf6842df97a3fULL},
      {"echo_combat_tournament", 0x6b44ed1ad5234fbcULL}, {"social_2.0", 0x042d9cf9cfddcf76ULL},
      {"social_2.0_private", 0x305d6e37c1589c45ULL},  {"social_2.0_npe", 0x163956e6e431e617ULL},
  };
  ASSERT_EQ(sizeof(kGameTypes) / sizeof(kGameTypes[0]), 11u);
  for (size_t i = 0; i < 11; ++i) {
    EXPECT_STREQ(kGameTypes[i].name, service[i].first);
    EXPECT_EQ(kGameTypes[i].symbol, service[i].second) << service[i].first;
    EXPECT_EQ(kGameTypes[i].bots, std::string(service[i].first).rfind("social", 0) != 0) << service[i].first;
  }
}

// #982: the session start frame's game type, as the runtime stores it for the public Arena round default.
TEST(ServerBotsGameType, TheSessionStartFramesTypeIsReadByName) {
  const auto typeOf = [](const std::string& settings) {
    const std::vector<uint8_t> f = Frame(settings, kEntrants);
    return nevr_game_type_table::GameTypeOfSessionStart(f.data(), f.size());
  };
  EXPECT_EQ(typeOf(kServiceSettings), std::optional<std::string>("echo_arena")) << "the service's signed symbol";
  EXPECT_EQ(typeOf(R"({"gametype":"echo_combat"})"), std::optional<std::string>("echo_combat"));
  EXPECT_EQ(typeOf(R"({"gametype":"echo_arena_private"})"), std::optional<std::string>("echo_arena_private"));
  EXPECT_EQ(typeOf(R"({"gametype":301069346851901302})"), std::optional<std::string>("social_2.0"));
}

TEST(ServerBotsGameType, ASessionStartFrameWithoutAKnownTypeStoresNothing) {
  const auto typeOf = [](const std::string& settings) {
    const std::vector<uint8_t> f = Frame(settings, kEntrants);
    return nevr_game_type_table::GameTypeOfSessionStart(f.data(), f.size());
  };
  EXPECT_FALSE(typeOf(R"({"level":"mpl_arena_a"})").has_value()) << "no gametype key";
  EXPECT_FALSE(typeOf(R"({"gametype":"echo_nothing"})").has_value()) << "a name the table does not know";
  EXPECT_FALSE(typeOf(R"({"gametype":42})").has_value()) << "a number the table does not know";
  EXPECT_FALSE(typeOf("not json").has_value());
  EXPECT_FALSE(typeOf("[1,2]").has_value()) << "settings that are not an object";

  const std::vector<uint8_t> f = Frame(kServiceSettings, kEntrants);
  EXPECT_FALSE(nevr_game_type_table::GameTypeOfSessionStart(nullptr, f.size()).has_value());
  EXPECT_FALSE(nevr_game_type_table::GameTypeOfSessionStart(f.data(), kSessionStartHeaderSize).has_value())
      << "a frame with no room for settings";
  EXPECT_FALSE(nevr_game_type_table::GameTypeOfSessionStart(f.data(), kSessionStartHeaderSize + 5).has_value())
      << "a settings string that does not end inside the frame";
}

TEST(ServerBotsGameType, ReadAsTheServiceWritesItOrByName) {
  const auto read = [](const char* json) { return ReadGameType(nlohmann::ordered_json::parse(json)); };
  SessionGameType t = read(R"({"gametype":-3791849610740453517})");  // echo_arena as the service's signed int64
  EXPECT_EQ(t.name, "echo_arena");
  EXPECT_TRUE(t.bots);
  t = read(R"({"gametype":14654894462969098099})");  // the same symbol, unsigned
  EXPECT_EQ(t.name, "echo_arena");
  t = read(R"({"gametype":301069346851901302})");  // social_2.0
  EXPECT_EQ(t.name, "social_2.0");
  EXPECT_FALSE(t.bots);
  t = read(R"({"gametype":"echo_combat_private"})");
  EXPECT_EQ(t.name, "echo_combat_private");
  EXPECT_TRUE(t.bots);
  t = read(R"({"gametype":-1686183371379962529})");  // echo_arena_practice symbol hash, as the service's signed int64
  EXPECT_EQ(t.name, "echo_arena_practice");
  EXPECT_TRUE(t.bots);
  t = read(R"({"appid":"1"})");
  EXPECT_EQ(t.name, "none");
  EXPECT_FALSE(t.bots);
  for (const char* other : {R"({"gametype":7})", R"({"gametype":"echo"})", R"({"gametype":1.5})",
                            R"({"gametype":true})", R"({"gametype":null})"}) {
    t = read(other);
    EXPECT_EQ(t.name, "unknown") << other;
    EXPECT_FALSE(t.bots) << other;
  }
}

TEST(ServerBotsSettings, ASocialLobbyIsNotRewrittenAndGetsNoBots) {
  const std::string social = R"({"appid":"1369078409873402","gametype":301069346851901302,"level":1})";
  const std::vector<uint8_t> f = Frame(social, kEntrants);
  const SettingsRewrite r = RewriteSessionSettings(f.data(), f.size(), 2, 5);
  EXPECT_EQ(r.result, SettingsResult::kNotAMatch);
  EXPECT_EQ(r.gameType, "social_2.0");
  EXPECT_TRUE(r.frame.empty());
  EXPECT_FALSE(r.addedNumTeams);
  const std::vector<uint8_t> none = Frame(R"({"appid":"1"})", kEntrants);
  EXPECT_EQ(RewriteSessionSettings(none.data(), none.size(), 2, 5).result, SettingsResult::kNotAMatch);
  const SettingsRewrite arena = RewriteSessionSettings(Frame(kServiceSettings, kEntrants).data(),
                                                       Frame(kServiceSettings, kEntrants).size(), 2, 5);
  EXPECT_EQ(arena.gameType, "echo_arena");
}

TEST(ServerBotsSettings, OnlyAMatchWithBothKeysGetsBots) {
  EXPECT_TRUE(BotsThisSession(SettingsResult::kRewritten));
  EXPECT_TRUE(BotsThisSession(SettingsResult::kAlreadyPresent));
  for (SettingsResult r : {SettingsResult::kNotWanted, SettingsResult::kNotAMatch, SettingsResult::kTooShort,
                           SettingsResult::kNoTerminator, SettingsResult::kNotJson, SettingsResult::kNotObject}) {
    EXPECT_FALSE(BotsThisSession(r)) << SettingsResultName(r);
  }
}

TEST(ServerBotsSettings, TheSessionIdIsTheMatchIdTheFrameStartsWith) {
  // Frame() writes the match id 0x10..0x1F first, then the group id 0x30..0x3F; a GUID is shown in the Windows layout.
  const std::vector<uint8_t> f = Frame(kServiceSettings, kEntrants);
  EXPECT_EQ(SessionIdOf(f.data(), f.size()), "13121110-1514-1716-1819-1A1B1C1D1E1F");
  EXPECT_EQ(SessionIdOf(f.data(), 16), "13121110-1514-1716-1819-1A1B1C1D1E1F");
  EXPECT_EQ(SessionIdOf(f.data(), 15), "none");
}

TEST(ServerBotsSettings, ResultNames) {
  EXPECT_STREQ(SettingsResultName(SettingsResult::kNotWanted), "not_wanted");
  EXPECT_STREQ(SettingsResultName(SettingsResult::kNotAMatch), "not_a_match");
  EXPECT_STREQ(SettingsResultName(SettingsResult::kAlreadyPresent), "already_present");
  EXPECT_STREQ(SettingsResultName(SettingsResult::kRewritten), "rewritten");
  EXPECT_STREQ(SettingsResultName(SettingsResult::kTooShort), "too_short");
  EXPECT_STREQ(SettingsResultName(SettingsResult::kNoTerminator), "no_terminator");
  EXPECT_STREQ(SettingsResultName(SettingsResult::kNotJson), "not_json");
  EXPECT_STREQ(SettingsResultName(SettingsResult::kNotObject), "not_object");
  EXPECT_FALSE(IsSettingsFailure(SettingsResult::kNotWanted));
  EXPECT_FALSE(IsSettingsFailure(SettingsResult::kNotAMatch));
  EXPECT_FALSE(IsSettingsFailure(SettingsResult::kRewritten));
}

// --- the request -------------------------------------------------------------------------------------------------

TEST(ServerBotsRequest, EachFieldAsTheSpawnBotNodeSetsIt) {
  EXPECT_EQ(kRequestSize, 0xD8u);
  // The two .data globals as the pinned build holds them: FLT_MAX three times.
  const float noPos[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
  uint8_t noPosition[kNoPositionSize];
  std::memcpy(noPosition, noPos, sizeof(noPos));
  const Request r = BuildRequest(noPosition);
  std::vector<uint8_t> want(0xD8, 0);
  const int64_t anyEntrant = -1;
  const uint32_t anyTeam = 0xFFFF;
  const float quat[4] = {0, 0, 0, 1};
  const float one = 1.0f;
  std::memcpy(&want[0x00], &anyEntrant, 8);
  std::memcpy(&want[0x08], &anyTeam, 4);
  std::memcpy(&want[0x10], quat, 16);
  std::memcpy(&want[0x20], noPos, 12);
  std::memcpy(&want[0x2C], &one, 4);
  for (size_t i = 0x38; i < 0xD0; ++i) want[i] = 0xFF;
  EXPECT_EQ(std::vector<uint8_t>(r.bytes, r.bytes + kRequestSize), want);
}

TEST(ServerBotsRequest, ThePositionIsTheBytesGiven) {
  uint8_t noPosition[kNoPositionSize];
  for (size_t i = 0; i < kNoPositionSize; ++i) noPosition[i] = static_cast<uint8_t>(0xA0 + i);
  const Request r = BuildRequest(noPosition);
  EXPECT_EQ(std::memcmp(r.bytes + 0x20, noPosition, kNoPositionSize), 0);
}

// --- the schedule ------------------------------------------------------------------------------------------------

TEST(ServerBotsSchedule, AsksOnceASecondUntilTheCount) {
  Schedule s;
  EXPECT_EQ(Decide(s, 2, true, 4, false, 1000), Gate::kAsk);
  Requested(s, true);
  EXPECT_EQ(Decide(s, 2, true, 5, false, 1999), Gate::kWaiting);
  EXPECT_EQ(Decide(s, 2, true, 5, false, 2000), Gate::kAsk);
  Requested(s, true);
  EXPECT_EQ(s.requested, 2u);
  EXPECT_EQ(Decide(s, 2, true, 6, false, 5000), Gate::kDone);
}

TEST(ServerBotsSchedule, ARefusedRequestIsNotCountedAndWaitsASecond) {
  Schedule s;
  EXPECT_EQ(Decide(s, 1, true, 4, false, 1000), Gate::kAsk);
  Requested(s, false);
  EXPECT_EQ(s.requested, 0u);
  EXPECT_EQ(Decide(s, 1, true, 4, false, 1500), Gate::kWaiting);
  EXPECT_EQ(Decide(s, 1, true, 4, false, 2000), Gate::kAsk);
}

TEST(ServerBotsSchedule, StopsAtSixteenEntrantsAndWhileTheGameHasNotAnswered) {
  Schedule s;
  EXPECT_EQ(Decide(s, 4, true, 16, false, 1000), Gate::kLobbyFull);
  EXPECT_EQ(Decide(s, 4, true, 15, true, 1000), Gate::kPending);
  EXPECT_EQ(Decide(s, 4, true, 15, false, 1000), Gate::kAsk) << "neither held the next ask back";
}

TEST(ServerBotsSchedule, LeavingTheGameStartsTheCountAgain) {
  Schedule s;
  Decide(s, 1, true, 4, false, 1000);
  Requested(s, true);
  EXPECT_EQ(Decide(s, 1, true, 5, false, 3000), Gate::kDone);
  EXPECT_EQ(Decide(s, 1, false, 0, false, 4000), Gate::kNotInGame);
  EXPECT_EQ(s.requested, 0u);
  EXPECT_EQ(Decide(s, 1, true, 4, false, 4001), Gate::kAsk) << "the next session gets its bot at once";
}

TEST(ServerBotsSchedule, LeavingTheGameClearsEveryField) {
  Schedule s;
  s.requested = 2;
  s.nextAskMs = 9000;
  s.lastNoBodyMs = 7000;
  s.warnedNoBody = true;
  s.pendingSeen = true;
  s.pendingSinceMs = 6000;
  s.warnedPending = true;
  EXPECT_EQ(Decide(s, 2, false, 0, true, 8000), Gate::kNotInGame);
  EXPECT_EQ(s.requested, 0u);
  EXPECT_EQ(s.nextAskMs, 0u) << "the next session asks at once";
  EXPECT_EQ(s.lastNoBodyMs, 0u);
  EXPECT_FALSE(s.warnedNoBody) << "the next session says its own no-body warning";
  EXPECT_FALSE(s.pendingSeen);
  EXPECT_EQ(s.pendingSinceMs, 0u);
  EXPECT_FALSE(s.warnedPending);
}

TEST(ServerBotsSchedule, TheGameNotAnsweringIsSaidOnceAfterTenSeconds) {
  Schedule s;
  EXPECT_FALSE(PendingWarningDue(s, true, 1000)) << "the flag was just seen";
  EXPECT_FALSE(PendingWarningDue(s, true, 10999));
  EXPECT_TRUE(PendingWarningDue(s, true, 11000));
  EXPECT_FALSE(PendingWarningDue(s, true, 40000)) << "once per stretch";
  EXPECT_FALSE(PendingWarningDue(s, false, 40001)) << "the game answered";
  EXPECT_FALSE(PendingWarningDue(s, true, 40002));
  EXPECT_FALSE(PendingWarningDue(s, true, 50001)) << "a new stretch waits its own ten seconds";
  EXPECT_TRUE(PendingWarningDue(s, true, 50002));
}

TEST(ServerBotsSchedule, TheNoBodyWarningIsOnceAMinute) {
  Schedule s;
  EXPECT_TRUE(NoBodyWarningDue(s, 1000));
  EXPECT_FALSE(NoBodyWarningDue(s, 60999));
  EXPECT_TRUE(NoBodyWarningDue(s, 61000));
  EXPECT_FALSE(NoBodyWarningDue(s, 61001));
}

// --- the thread ---

TEST(ServerBotsThread, TheDriverRunsOnAServerOnTheGameLoopThreadOnly) {
  EXPECT_TRUE(DriverRuns(true, 42, 42));
  EXPECT_FALSE(DriverRuns(false, 42, 42)) << "a client";
  EXPECT_FALSE(DriverRuns(true, 42, 7)) << "another thread calls the dispatcher";
  EXPECT_FALSE(DriverRuns(true, 0, 0)) << "the game loop's thread is not known yet";
}

// --- the body pools ----------------------------------------------------------------------------------------------

TEST(ServerBotsBodies, TheTwoBodiesAreTheGamesTable) {
  // echovr.exe .rdata 0x141C9E7F8 and 0x141C9E818: the hashes the static initializer 0x1400C3CE0 copies.
  EXPECT_EQ(kBotBodies[0], 0x91521E3B14547E29ULL);
  EXPECT_EQ(kBotBodies[1], 0x604653C3C65539BEULL);
}

TEST(ServerBotsBodies, EveryPoolTheLevelHasNeedsAFreeBody) {
  const BodyPool none{};
  const BodyPool free3{true, 3};
  const BodyPool empty{true, 0};
  const BodyPool negative{true, -1};
  {
    const BodyPool p[2] = {free3, none};
    EXPECT_TRUE(BodyFree(p, 2));
  }
  {
    const BodyPool p[2] = {none, free3};
    EXPECT_TRUE(BodyFree(p, 2));
  }
  {
    const BodyPool p[2] = {free3, empty};
    EXPECT_FALSE(BodyFree(p, 2)) << "the game may pick the empty one";
  }
  {
    const BodyPool p[2] = {negative, free3};
    EXPECT_FALSE(BodyFree(p, 2));
  }
  {
    const BodyPool p[2] = {none, none};
    EXPECT_FALSE(BodyFree(p, 2)) << "a level with no bot bodies takes no bots";
  }
}

// --- the game's memory, on buffers laid out as the game lays it out ----------------------------------------------

template <typename T>
void Put(std::vector<uint8_t>& buf, size_t offset, T value) {
  std::memcpy(buf.data() + offset, &value, sizeof(value));
}

TEST(ServerBotsMemory, TheNetGameFieldsAreReadAtTheirOffsets) {
  std::vector<uint8_t> netGame(0x2800, 0xEE);
  Put<int32_t>(netGame, 0, 9);
  Put<uint16_t>(netGame, 0xE2, 7);
  Put<int32_t>(netGame, 0x27D8, 1);
  NetGameView v = ReadNetGame(netGame.data());
  EXPECT_TRUE(v.inGame);
  EXPECT_EQ(v.entrants, 7u) << "two bytes, not the 0xEE after them";
  EXPECT_TRUE(v.pending);
  Put<int32_t>(netGame, 0, 8);
  Put<int32_t>(netGame, 0x27D8, 0);
  v = ReadNetGame(netGame.data());
  EXPECT_FALSE(v.inGame) << "state 8 is not in game";
  EXPECT_FALSE(v.pending);
  // RequestAddBot's `cmpxchg dword [rcx+0x27d8], esi` (0x1401A0D9F) wants 0: any other dword makes it refuse.
  Put<int32_t>(netGame, 0x27D8, 0x100);
  EXPECT_TRUE(ReadNetGame(netGame.data()).pending) << "any nonzero dword is pending";
  Put<int32_t>(netGame, 0, 0x109);
  EXPECT_FALSE(ReadNetGame(netGame.data()).inGame) << "the whole word is the state";
}

TEST(ServerBotsMemory, APoolsFreeCountIsItsEntrysAtPlus0x30) {
  std::vector<uint8_t> entries(3 * 0x88, 0xEE);
  Put<int64_t>(entries, 0 * 0x88 + 0x30, -1);
  Put<int64_t>(entries, 2 * 0x88 + 0x30, 5);
  EXPECT_EQ(PoolEntryFree(entries.data(), 2), 5);
  EXPECT_EQ(PoolEntryFree(entries.data(), 0), -1);
  EXPECT_EQ(PoolEntryFree(entries.data(), 0xABCD0002), 5) << "FindPool's index is the low 16 bits";
}

TEST(ServerBotsMemory, AnIndexAbove255ReadsItsOwnEntry) {
  // The game reads the index as a word (`movzx edx, word [rbx+8]` at 0x1404E6233): bits 8 to 15 count.
  std::vector<uint8_t> entries(0x103 * 0x88, 0xEE);
  Put<int64_t>(entries, 0x002 * 0x88 + 0x30, 5);
  Put<int64_t>(entries, 0x102 * 0x88 + 0x30, 9);
  EXPECT_EQ(PoolEntryFree(entries.data(), 0xABCD0102), 9) << "entry 0x102, not entry 2";
}

struct FakePools {
  // owner per body; an owner holds its entries pointer at +0x3E8
  std::vector<uint8_t> owner[2] = {std::vector<uint8_t>(0x3F0, 0), std::vector<uint8_t>(0x3F0, 0)};
  std::vector<uint8_t> entries[2] = {std::vector<uint8_t>(4 * 0x88, 0), std::vector<uint8_t>(4 * 0x88, 0)};
  bool found[2] = {true, true};
  bool ownerNull[2] = {false, false};
  uint64_t index[2] = {1, 3};
  std::vector<uint64_t> asked;
  FakePools() {
    for (int i = 0; i < 2; ++i) {
      const uint8_t* e = entries[i].data();
      std::memcpy(owner[i].data() + 0x3E8, &e, sizeof(e));
    }
  }
  int Find(void* pools, uint64_t body, void** ownerOut, uint64_t* indexOut) {
    EXPECT_EQ(pools, static_cast<void*>(this));
    asked.push_back(body);
    const int i = body == kBotBodies[0] ? 0 : 1;
    // The out values are written even on a miss, so a reader that ignores FindPool's 0 reads a pool it was not given.
    *ownerOut = ownerNull[i] ? nullptr : owner[i].data();
    *indexOut = index[i];
    return found[i] ? 1 : 0;
  }
};

TEST(ServerBotsMemory, EachBodysPoolIsFoundAndItsEntryRead) {
  FakePools f;
  Put<int64_t>(f.entries[0], 1 * 0x88 + 0x30, 4);
  Put<int64_t>(f.entries[1], 3 * 0x88 + 0x30, 0);
  BodyPool body[2] = {{true, 99}, {true, 99}};
  ReadBodyPools([&](void* p, uint64_t b, void** o, uint64_t* i) { return f.Find(p, b, o, i); }, &f, body);
  EXPECT_EQ(f.asked, (std::vector<uint64_t>{kBotBodies[0], kBotBodies[1]}));
  EXPECT_TRUE(body[0].found);
  EXPECT_EQ(body[0].free, 4);
  EXPECT_TRUE(body[1].found);
  EXPECT_EQ(body[1].free, 0);
}

TEST(ServerBotsMemory, APoolThatIsNotThereIsNotFound) {
  const auto read = [](FakePools& f, BodyPool body[2], void* pools) {
    ReadBodyPools([&](void* p, uint64_t b, void** o, uint64_t* i) { return f.Find(p, b, o, i); }, pools, body);
  };
  {
    FakePools f;
    f.found[0] = false;
    f.ownerNull[1] = true;
    BodyPool body[2] = {{true, 9}, {true, 9}};
    read(f, body, &f);
    EXPECT_FALSE(body[0].found) << "FindPool did not find it";
    EXPECT_FALSE(body[1].found) << "FindPool gave no owner";
  }
  {
    FakePools f;
    std::memset(f.owner[0].data() + 0x3E8, 0, sizeof(void*));
    BodyPool body[2];
    read(f, body, &f);
    EXPECT_FALSE(body[0].found) << "an owner without entries";
    EXPECT_TRUE(body[1].found);
  }
  {
    FakePools f;
    BodyPool body[2] = {{true, 9}, {true, 9}};
    read(f, body, nullptr);
    EXPECT_TRUE(f.asked.empty()) << "no pools: FindPool is not called";
    EXPECT_FALSE(body[0].found);
    EXPECT_FALSE(body[1].found);
  }
}

TEST(ServerBotsMemory, TheHostingFlagsAreTheLobbysAt0x18Plus0x1C) {
  std::vector<uint8_t> holder(0x40, 0);
  EXPECT_EQ(reinterpret_cast<uint8_t*>(HostingFlags(holder.data())), holder.data() + 0x34);
}

TEST(ServerBotsMemory, TheOfflineBitIsSetForTheCallAndOnlyItIsPutBack) {
  for (const uint32_t before : {0x0u, 0x6u, 0x7u, 0xFFFFFFFEu, 0x80000000u}) {
    uint32_t flags = before;
    uint32_t seen = 0;
    const bool result = WithOfflineBit(&flags, [&] {
      seen = flags;
      flags ^= 0x100;  // another bit changed during the call (another thread, or the call) is kept
      return before % 2 == 0;
    });
    EXPECT_EQ(seen, before | 1u) << before;
    EXPECT_EQ(flags, before ^ 0x100u) << before << ": bit 0 as it was, every other bit as the call left it";
    EXPECT_EQ(result, before % 2 == 0) << "the call's answer is returned";
  }
}

// --- one server update (Drive) ------------------------------------------------------------------------------------

// A net game, its lobby holder and its body pools, laid out as Drive reads them; RequestAddBot is a recorder.
struct FakeGame {
  std::vector<uint8_t> netGame = std::vector<uint8_t>(0x2B10, 0);
  std::vector<uint8_t> lobby = std::vector<uint8_t>(0x40, 0);
  FakePools pools;
  uint8_t noPosition[kNoPositionSize] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
  int answer = 1;  // what RequestAddBot returns
  std::vector<std::vector<uint8_t>> requests;
  std::vector<uint32_t> flagsSeen;
  DriverState state;
  FakeGame() {
    Put<int32_t>(netGame, 0, 9);  // in game
    Put<uint16_t>(netGame, 0xE2, 3);
    void* p = &pools;
    std::memcpy(netGame.data() + 0x2B08, &p, sizeof(p));
    uint8_t* l = lobby.data();
    std::memcpy(netGame.data() + 0x40, &l, sizeof(l));
    Put<uint32_t>(lobby, 0x34, 0x6);  // hostingFlags, bit 0 clear
    Put<int64_t>(pools.entries[0], 1 * 0x88 + 0x30, 4);
    Put<int64_t>(pools.entries[1], 3 * 0x88 + 0x30, 4);
  }
  std::vector<Line> Update(uint64_t nowMs, bool bots = true, bool noNetGame = false, const char* session = "AAAA") {
    return Drive(
        state, 2, noNetGame ? nullptr : netGame.data(), noPosition, nowMs, SessionView{session, bots}, 36,
        [&](void* pl, uint64_t b, void** o, uint64_t* i) { return pools.Find(pl, b, o, i); },
        [&](void* ng, const uint8_t* request) {
          EXPECT_EQ(ng, static_cast<void*>(netGame.data()));
          requests.emplace_back(request, request + kRequestSize);
          uint32_t flags;
          std::memcpy(&flags, lobby.data() + 0x34, sizeof(flags));
          flagsSeen.push_back(flags);
          return answer;
        });
  }
};

TEST(ServerBotsDrive, TheFirstUpdateSaysTheDriverRunsOnce) {
  FakeGame g;
  std::vector<Line> lines = g.Update(1000, true, true);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_FALSE(lines[0].warning);
  EXPECT_EQ(lines[0].text,
            "[NEVR.GAMESERVER] server bots driver running thread=36 (bots join a match from its session start on)");
  EXPECT_TRUE(g.Update(1000, true, true).empty()) << "once";
  EXPECT_TRUE(g.requests.empty()) << "no net game: nothing is asked";
}

TEST(ServerBotsDrive, AMatchGetsARequestWithTheOfflineBitAndTheRequestsBytes) {
  FakeGame g;
  g.state.runningLogged = true;
  const std::vector<Line> lines = g.Update(5000);
  ASSERT_EQ(g.requests.size(), 1u);
  const Request want = BuildRequest(g.noPosition);
  EXPECT_EQ(g.requests[0], std::vector<uint8_t>(want.bytes, want.bytes + kRequestSize));
  EXPECT_EQ(g.flagsSeen[0], 0x7u) << "the offline bit is set for the call";
  uint32_t after;
  std::memcpy(&after, g.lobby.data() + 0x34, sizeof(after));
  EXPECT_EQ(after, 0x6u) << "and cleared after it";
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].text, "[NEVR.GAMESERVER] server bot requested session=AAAA n=1 of 2 entrants=3");
  EXPECT_EQ(g.state.schedule.requested, 1u);
}

TEST(ServerBotsDrive, OneRequestASecondUntilTheCount) {
  FakeGame g;
  g.state.runningLogged = true;
  g.Update(5000);
  EXPECT_TRUE(g.Update(5999).empty());
  EXPECT_EQ(g.requests.size(), 1u) << "less than a second later";
  g.Update(6000);
  EXPECT_EQ(g.requests.size(), 2u);
  g.Update(9000);
  EXPECT_EQ(g.requests.size(), 2u) << "two wanted, two requested";
}

TEST(ServerBotsDrive, ASessionWithoutBotsAsksNothingAndStartsTheCountAgain) {
  FakeGame g;
  g.state.runningLogged = true;
  g.Update(5000);
  g.Update(6000);
  ASSERT_EQ(g.state.schedule.requested, 2u);
  EXPECT_TRUE(g.Update(7000, false).empty());
  EXPECT_EQ(g.requests.size(), 2u) << "a session that is not a match gets no request";
  EXPECT_EQ(g.state.schedule.requested, 0u);
  g.Update(8000);
  EXPECT_EQ(g.requests.size(), 3u) << "the next match starts from 0, without a frame out of game between";
}

TEST(ServerBotsDrive, NoFreeBodyIsAWarningAndNoRequest) {
  FakeGame g;
  g.state.runningLogged = true;
  Put<int64_t>(g.pools.entries[1], 3 * 0x88 + 0x30, 0);
  const std::vector<Line> lines = g.Update(5000);
  EXPECT_TRUE(g.requests.empty());
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_TRUE(lines[0].warning);
  EXPECT_EQ(lines[0].text,
            "[NEVR.GAMESERVER] server bots: no free bot body in this level session=AAAA requested=0 wanted=2 "
            "entrants=3 bot_arena_pool=4 bot_combat_pool=0 (no more bots this session until one frees)");
  EXPECT_TRUE(g.Update(6000).empty()) << "the warning is said at most once a minute";
  EXPECT_EQ(g.Update(5000 + 60000).size(), 1u);
  EXPECT_TRUE(g.requests.empty());
}

TEST(ServerBotsDrive, AFullLobbyGetsNoRequest) {
  FakeGame g;
  g.state.runningLogged = true;
  Put<uint16_t>(g.netGame, 0xE2, 16);  // the game's 16 entrants
  EXPECT_TRUE(g.Update(5000).empty());
  EXPECT_TRUE(g.requests.empty());
}

TEST(ServerBotsDrive, NoLobbyNoRequest) {
  FakeGame g;
  g.state.runningLogged = true;
  std::memset(g.netGame.data() + 0x40, 0, sizeof(void*));
  EXPECT_TRUE(g.Update(5000).empty());
  EXPECT_TRUE(g.requests.empty());
}

TEST(ServerBotsDrive, ARefusalIsAWarningAndNotCounted) {
  FakeGame g;
  g.state.runningLogged = true;
  g.answer = 0;
  const std::vector<Line> lines = g.Update(5000);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_TRUE(lines[0].warning);
  EXPECT_EQ(lines[0].text, "[NEVR.GAMESERVER] server bot request refused by the game session=AAAA n=0 of 2 entrants=3");
  EXPECT_EQ(g.state.schedule.requested, 0u);
}

TEST(ServerBotsDrive, ARequestTheGameHasNotAnsweredIsSaidAfterTenSeconds) {
  FakeGame g;
  g.state.runningLogged = true;
  Put<int32_t>(g.netGame, 0x27D8, 1);
  EXPECT_TRUE(g.Update(5000).empty());
  EXPECT_TRUE(g.Update(14999).empty());
  const std::vector<Line> lines = g.Update(15000);
  EXPECT_TRUE(g.requests.empty());
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].text,
            "[NEVR.GAMESERVER] server bots: the game has not answered the last bot request for 10 s session=AAAA "
            "requested=0 wanted=2 (no more bots this session until it does)");
}

TEST(ServerBotsDrive, TheLastRequestUnansweredIsSaidAfterTenSeconds) {
  FakeGame g;
  g.state.runningLogged = true;
  g.Update(5000);
  g.Update(6000);
  ASSERT_EQ(g.state.schedule.requested, 2u) << "both asked: the schedule is done";
  Put<int32_t>(g.netGame, 0x27D8, 1);  // the game has not answered the second
  EXPECT_TRUE(g.Update(7000).empty());
  EXPECT_TRUE(g.Update(16999).empty());
  const std::vector<Line> lines = g.Update(17000);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_TRUE(lines[0].warning);
  EXPECT_EQ(lines[0].text,
            "[NEVR.GAMESERVER] server bots: the game has not answered the last bot request for 10 s session=AAAA "
            "requested=2 wanted=2 (no more bots this session until it does)");
  EXPECT_EQ(g.requests.size(), 2u);
}

TEST(ServerBotsDrive, ANewSessionCountsFromZeroWithNoFrameOutOfGame) {
  FakeGame g;
  g.state.runningLogged = true;
  g.Update(5000);
  g.Update(6000);
  ASSERT_EQ(g.requests.size(), 2u);
  g.Update(7000, true, false, "BBBB");
  EXPECT_EQ(g.requests.size(), 3u) << "the second match, still in game, gets its first bot";
  EXPECT_EQ(g.state.schedule.requested, 1u);
  EXPECT_EQ(g.state.session, "BBBB");
}

TEST(ServerBotsDrive, ANewSessionAsksAtOnceThoughTheLastAskWasJustNow) {
  FakeGame g;
  g.state.runningLogged = true;
  g.Update(5000);
  g.Update(5500);
  ASSERT_EQ(g.requests.size(), 1u) << "the first session waits its second out";
  g.Update(5600, true, false, "BBBB");
  EXPECT_EQ(g.requests.size(), 2u) << "the new session's clock starts fresh";
  EXPECT_EQ(g.state.schedule.requested, 1u);
}

TEST(ServerBotsDrive, OutOfGameAPendingFlagIsNotSaid) {
  FakeGame g;
  g.state.runningLogged = true;
  Put<int32_t>(g.netGame, 0, 8);
  Put<int32_t>(g.netGame, 0x27D8, 1);
  for (uint64_t now : {5000u, 16000u, 30000u}) EXPECT_TRUE(g.Update(now).empty()) << now;
  EXPECT_TRUE(g.requests.empty());
}

TEST(ServerBotsDrive, OutOfGameNothingIsAsked) {
  FakeGame g;
  g.state.runningLogged = true;
  Put<int32_t>(g.netGame, 0, 8);
  EXPECT_TRUE(g.Update(5000).empty());
  EXPECT_TRUE(g.requests.empty());
}

// --- Install's decision and the lines it logs -------------------------------------------------------------------

TEST(ServerBotsInstall, AClientLogsOneInfoSkipLineAndReadsNothing) {
  const InstallResult r = InstallLines(false, "4", nullptr);
  EXPECT_EQ(r.armed, 0u);
  ASSERT_EQ(r.lines.size(), 1u);
  EXPECT_FALSE(r.lines[0].warning);
  EXPECT_EQ(r.lines[0].text, "[NEVR.GAMESERVER] server bots not applied: this is a client (the server adds bots)");
}

TEST(ServerBotsInstall, NoValueOrZeroIsOffWithItsSourceAndReadsNoImage) {
  for (const char* text : {static_cast<const char*>(nullptr), "", "0"}) {
    const InstallResult r = InstallLines(true, text, nullptr);
    EXPECT_EQ(r.armed, 0u);
    ASSERT_EQ(r.lines.size(), 1u);
    EXPECT_FALSE(r.lines[0].warning);
    EXPECT_EQ(r.lines[0].text, std::string("[NEVR.GAMESERVER] server bots off bots_per_session=0 source=") +
                                   (text != nullptr && text[0] != '\0' ? "config.yaml" : "default"));
  }
}

TEST(ServerBotsInstall, ABadValueIsOneWarningThatSaysBotsAreOff) {
  const InstallResult r = InstallLines(true, "17", nullptr);
  EXPECT_EQ(r.armed, 0u);
  ASSERT_EQ(r.lines.size(), 1u) << "no second line saying bots_per_session=0 while config.yaml holds 17";
  EXPECT_TRUE(r.lines[0].warning);
  EXPECT_EQ(r.lines[0].text,
            "[NEVR.GAMESERVER] network.bots_per_session=17 is above the limit of 16 (allowed 0..16); server bots off");
}

std::vector<uint8_t> ImageWithWitnesses();

TEST(ServerBotsInstall, AllSitesHoldingArmsTheCount) {
  const std::vector<uint8_t> image = ImageWithWitnesses();
  const InstallResult r = InstallLines(true, "3", image.data());
  EXPECT_EQ(r.armed, 3u);
  ASSERT_EQ(r.lines.size(), 1u);
  EXPECT_FALSE(r.lines[0].warning);
  EXPECT_EQ(r.lines[0].text,
            "[NEVR.GAMESERVER] server bots armed bots_per_session=3 sites=7 (each arena or combat match in game gets "
            "up to 3 AI bots, one a second)");
}

TEST(ServerBotsInstall, ASiteThatDiffersIsAWarningWithItsBytesAndLeavesTheDriverOff) {
  std::vector<uint8_t> image = ImageWithWitnesses();
  image[0x611096 + 6] = 0x02;  // AddBot offline test: test al, 2
  const InstallResult r = InstallLines(true, "3", image.data());
  EXPECT_EQ(r.armed, 0u);
  ASSERT_EQ(r.lines.size(), 1u);
  EXPECT_TRUE(r.lines[0].warning);
  EXPECT_EQ(r.lines[0].text,
            "[NEVR.GAMESERVER] server bots off bots_per_session=3: the game's AddBot offline test at 0x140611096 is "
            "not the build this was read from expected=8b471cf7d0a801 actual=8b471cf7d0a802");
}

TEST(ServerBotsLines, NoFreeBodyNamesEachPool) {
  const BodyPool body[2] = {{true, 0}, {}};
  const Line l = NoBodyLine("AAAA", 2, 4, 6, body);
  EXPECT_TRUE(l.warning);
  EXPECT_EQ(l.text,
            "[NEVR.GAMESERVER] server bots: no free bot body in this level session=AAAA requested=2 wanted=4 entrants=6 "
            "bot_arena_pool=0 bot_combat_pool=none (no more bots this session until one frees)");
}

TEST(ServerBotsLines, TheGameNotAnsweringIsAWarning) {
  const Line l = PendingLine("AAAA", 1, 4);
  EXPECT_TRUE(l.warning);
  EXPECT_EQ(l.text,
            "[NEVR.GAMESERVER] server bots: the game has not answered the last bot request for 10 s session=AAAA "
            "requested=1 wanted=4 (no more bots this session until it does)");
}

TEST(ServerBotsLines, ARequestIsInfoAndARefusalAWarning) {
  const Line ok = RequestLine(true, "AAAA", 2, 4, 6);
  EXPECT_FALSE(ok.warning);
  EXPECT_EQ(ok.text, "[NEVR.GAMESERVER] server bot requested session=AAAA n=2 of 4 entrants=6");
  const Line refused = RequestLine(false, "AAAA", 1, 4, 6);
  EXPECT_TRUE(refused.warning);
  EXPECT_EQ(refused.text, "[NEVR.GAMESERVER] server bot request refused by the game session=AAAA n=1 of 4 entrants=6");
}

TEST(ServerBotsLines, TheSettingsResultIsInfoAndAFailureAWarning) {
  SettingsRewrite r;
  r.result = SettingsResult::kRewritten;
  r.gameType = "echo_arena";
  r.addedNumTeams = true;
  r.settingsBytes = 50;
  r.rewrittenBytes = 85;
  Line l = SettingsLine(r, "AAAA", 171);
  EXPECT_FALSE(l.warning);
  EXPECT_EQ(l.text,
            "[NEVR.GAMESERVER] server bots session settings rewritten session=AAAA game_type=echo_arena numteams=2 "
            "players_per_team=kept "
            "settings_bytes=50 rewritten_bytes=85 frame_bytes=171");
  r = SettingsRewrite{};
  r.result = SettingsResult::kNotJson;
  r.addedPlayersPerTeam = true;
  r.playersPerTeamWritten = 5;
  l = SettingsLine(r, "AAAA", 40);
  EXPECT_TRUE(l.warning);
  EXPECT_EQ(l.text,
            "[NEVR.GAMESERVER] server bots session settings not_json session=AAAA game_type=none numteams=kept "
            "players_per_team=5 settings_bytes=0 rewritten_bytes=0 frame_bytes=40 (forwarded as it came; no bots this "
            "session)");
  r = SettingsRewrite{};
  r.result = SettingsResult::kNotAMatch;
  r.gameType = "social_2.0";
  l = SettingsLine(r, "AAAA", 140);
  EXPECT_FALSE(l.warning) << "a social lobby without bots is expected";
  EXPECT_EQ(l.text,
            "[NEVR.GAMESERVER] server bots session settings not_a_match session=AAAA game_type=social_2.0 "
            "numteams=kept players_per_team=kept settings_bytes=0 rewritten_bytes=0 frame_bytes=140 (forwarded as it "
            "came; bots join arena and combat matches only)");
}

// --- the sites ---------------------------------------------------------------------------------------------------

std::vector<uint8_t> ImageWithWitnesses() {
  size_t end = 0;
  for (const Witness& w : kWitnesses) end = std::max(end, w.rva + w.size);
  std::vector<uint8_t> image(end, 0xCC);
  for (const Witness& w : kWitnesses) std::memcpy(image.data() + w.rva, w.bytes, w.size);
  return image;
}

TEST(ServerBotsSites, AllHoldOnTheBuildTheyWereReadFrom) {
  const std::vector<uint8_t> image = ImageWithWitnesses();
  EXPECT_EQ(FirstMismatch(image.data()), kWitnessCount);
}

TEST(ServerBotsSites, AByteOffAtAnySiteNamesThatSite) {
  for (size_t i = 0; i < kWitnessCount; ++i) {
    std::vector<uint8_t> image = ImageWithWitnesses();
    image[kWitnesses[i].rva + kWitnesses[i].size - 1] ^= 0xFF;
    EXPECT_EQ(FirstMismatch(image.data()), i) << kWitnesses[i].name;
  }
}

TEST(ServerBotsSites, TheSitesAreTheOnesReadFromTheBinary) {
  const struct {
    const char* name;
    uintptr_t rva;
    size_t size;
  } want[] = {{"RequestAddBot", 0x1A0D70, 54},       {"RequestAddBot lobby", 0x1A0DC2, 4},
              {"RequestAddBot lobby argument", 0x1A0EAE, 4}, {"AddBot offline test", 0x611096, 7},
              {"FindPool", 0x502D80, 60},            {"bot body allocation", 0x4E6233, 60},
              {"bot body pools", 0x1523DF, 7}};
  ASSERT_EQ(kWitnessCount, sizeof(want) / sizeof(want[0]));
  for (size_t i = 0; i < kWitnessCount; ++i) {
    EXPECT_STREQ(kWitnesses[i].name, want[i].name);
    EXPECT_EQ(kWitnesses[i].rva, want[i].rva) << want[i].name;
    EXPECT_EQ(kWitnesses[i].size, want[i].size) << want[i].name;
  }
  // The offsets the driver reads are the ones the witnessed instructions use.
  const auto u8 = [](uint8_t b) { return static_cast<size_t>(b); };
  const auto u16 = [](const uint8_t* b) { return static_cast<size_t>(b[0] | (b[1] << 8)); };
  EXPECT_EQ(u8(kRequestAddBotBytes[15]), static_cast<size_t>(kNetGameInGame));    // cmp dword [rcx], 9
  EXPECT_EQ(u8(kRequestAddBotBytes[28]), kNetGameEntrantsOffset);                  // cmp word [rcx+0xE2], 0x10
  EXPECT_EQ(u8(kRequestAddBotBytes[32]), static_cast<size_t>(kMaxEntrants));
  EXPECT_EQ(u16(&kRequestAddBotBytes[50]), kNetGamePendingOffset);                 // lock cmpxchg [rcx+0x27D8]
  EXPECT_EQ(u8(kRequestAddBotLobbyBytes[3]), kNetGameLobbyOffset);                 // mov rdi, [rbx+0x40]
  EXPECT_EQ(u8(kRequestAddBotLobbyArgBytes[3]), kLobbyFromHolderOffset);           // lea rcx, [rdi+0x18]
  EXPECT_EQ(u8(kAddBotOfflineBytes[2]), kLobbyHostingFlagsOffset);                 // mov eax, [rdi+0x1c]
  EXPECT_EQ(u8(kAddBotOfflineBytes[6]), static_cast<size_t>(kHostingFlagOffline)); // test al, 1
  EXPECT_EQ(u8(kBodyAllocBytes[7]), kPoolEntrySize);                               // imul rdi, rdx, 0x88
  EXPECT_EQ(u16(&kBodyAllocBytes[40]), kPoolOwnerEntriesOffset);                   // add rdi, [rsi+0x3E8]
  EXPECT_EQ(u8(kBodyAllocBytes[56]), kPoolEntryFreeOffset);                        // sub rax, [rdi+0x30]
  EXPECT_EQ(u16(&kBotPoolsBytes[3]), kNetGamePoolsOffset);                         // mov rcx, [rdi+0x2B08]
}

}  // namespace
