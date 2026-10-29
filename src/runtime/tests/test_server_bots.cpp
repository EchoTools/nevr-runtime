// #1423: AI bots on a dedicated server (patch/server_bots.h): the config value, the session settings rewrite, the
// request, the schedule, the body check and the site witnesses. Pure: no game image.

#include "runtime/patch/server_bots.h"
#include "runtime/patch/server_bots_install.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cfloat>
#include <cstdint>
#include <cstring>
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
  const Case cases[] = {{"17", "above"}, {"99999999999", "above"}, {"-1", "whole"}, {"x", "whole"}, {"2.5", "whole"},
                        {" 2", "whole"}};
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

const std::string kServiceSettings =
    R"({"appid":"1369078409873402","gametype":-3791849610740453517,"level":13004389186521124930,"features":["x"]})";
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

TEST(ServerBotsSettings, PlayersPerTeamIsTheValueGiven) {
  const std::vector<uint8_t> f = Frame(kServiceSettings, kEntrants);
  const SettingsRewrite r = RewriteSessionSettings(f.data(), f.size(), 1, 7);
  ASSERT_EQ(r.result, SettingsResult::kRewritten);
  const nlohmann::json doc =
      nlohmann::json::parse(reinterpret_cast<const char*>(r.frame.data() + kSessionStartHeaderSize));
  EXPECT_EQ(doc.at("players_per_team"), 7);
}

TEST(ServerBotsSettings, AValueAlreadyThereIsKept) {
  const std::vector<uint8_t> f = Frame(R"({"appid":"a","numteams":3})", kEntrants);
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
  const std::vector<uint8_t> f = Frame(R"({"numteams":2,"players_per_team":4})", kEntrants);
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

TEST(ServerBotsSettings, ResultNames) {
  EXPECT_STREQ(SettingsResultName(SettingsResult::kNotWanted), "not_wanted");
  EXPECT_STREQ(SettingsResultName(SettingsResult::kAlreadyPresent), "already_present");
  EXPECT_STREQ(SettingsResultName(SettingsResult::kRewritten), "rewritten");
  EXPECT_STREQ(SettingsResultName(SettingsResult::kTooShort), "too_short");
  EXPECT_STREQ(SettingsResultName(SettingsResult::kNoTerminator), "no_terminator");
  EXPECT_STREQ(SettingsResultName(SettingsResult::kNotJson), "not_json");
  EXPECT_STREQ(SettingsResultName(SettingsResult::kNotObject), "not_object");
  EXPECT_FALSE(IsSettingsFailure(SettingsResult::kNotWanted));
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
  EXPECT_EQ(s.added, 2u);
  EXPECT_EQ(Decide(s, 2, true, 6, false, 5000), Gate::kDone);
}

TEST(ServerBotsSchedule, ARefusedRequestIsNotCountedAndWaitsASecond) {
  Schedule s;
  EXPECT_EQ(Decide(s, 1, true, 4, false, 1000), Gate::kAsk);
  Requested(s, false);
  EXPECT_EQ(s.added, 0u);
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
  EXPECT_EQ(s.added, 0u);
  EXPECT_EQ(Decide(s, 1, true, 4, false, 4001), Gate::kAsk) << "the next session gets its bot at once";
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
