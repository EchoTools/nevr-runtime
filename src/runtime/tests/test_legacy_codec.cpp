// Tests for the legacy <-> current EVR translator (compat/legacy_codec.{h,cpp}).
//
// Every fixture is built here, byte by byte, from the documented layouts by the small writer below. The
// writer and the readers share no code with the codec, so a layout mistake in the codec cannot cancel out.
// Each row is checked three ways: legacy bytes -> current bytes against a golden, current bytes -> legacy
// bytes against a golden, and the round trip.

#include <gtest/gtest.h>
#include <zlib.h>
#include <zstd.h>

#include <array>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "abi/symbol_hash.h"
#include "runtime/compat/evr_codec.h"
#include "runtime/compat/legacy_codec.h"
#include "runtime/tests/legacy_wire_writer.h"

namespace lc = nevr_legacy_codec;
using nlohmann::json;

namespace {

using nevr_legacy_test::Bytes;
using nevr_legacy_test::GuidOf;
using nevr_legacy_test::U32At;
using nevr_legacy_test::U64At;
using nevr_legacy_test::ZlibCompress;
using nevr_legacy_test::ZlibUncompress;
using nevr_legacy_test::ZstdCompress;
using nevr_legacy_test::ZstdUncompress;

// ---- the values the fixtures share ---------------------------------------------------------

constexpr uint64_t kPlatformOvrOrg = 4;
constexpr uint64_t kAccount = 0x1122334455667788ULL;
constexpr uint64_t kOtherAccount = 0x8877665544332211ULL;
constexpr uint64_t kLockValue = 0x0123456789ABCDEFULL;
constexpr uint64_t kModeValue = 0x5555666677778888ULL;  // the mode symbol the settings JSON names
constexpr uint64_t kLevelUnspecified = 0xFFFFFFFFFFFFFFFFULL;
constexpr uint64_t kPlatformSymbol = 0xA0A1A2A3A4A5A6A7ULL;
const char kSettingsJson[] = "{\"gametype\":22,\"appid\":\"7\"}";

lc::Context MakeContext() {
  lc::Context ctx;
  ctx.loginSession = GuidOf(0x10);
  ctx.self = {kPlatformOvrOrg, kAccount};
  ctx.channel = GuidOf(0x70);
  return ctx;
}

const lc::BuildTables& ProfileV2() {
  static const lc::BuildTables t = lc::LobbyB2ProfileV2Tables();
  return t;
}
const lc::BuildTables& ProfileV1() {
  static const lc::BuildTables t = lc::LobbyB2ProfileV1Tables();
  return t;
}

// One message in, exactly one message out toGameService / toGameClient.
lc::OutMessage OnlyToGameService(const lc::Translation& t) {
  EXPECT_EQ(t.outcome, lc::Outcome::Translated);
  EXPECT_EQ(t.toGameService.size(), 1u);
  EXPECT_TRUE(t.toGameClient.empty());
  return t.toGameService.size() == 1 ? t.toGameService[0] : lc::OutMessage{};
}
lc::OutMessage OnlyToGameClient(const lc::Translation& t) {
  EXPECT_EQ(t.outcome, lc::Outcome::Translated);
  EXPECT_EQ(t.toGameClient.size(), 1u);
  EXPECT_TRUE(t.toGameService.empty());
  return t.toGameClient.size() == 1 ? t.toGameClient[0] : lc::OutMessage{};
}

}  // namespace

// ---- symbols ------------------------------------------------------------------------------------

TEST(LegacySymbols, EveryConstantIsTheCSymbol64OfItsGameName) {
  const std::vector<std::pair<const char*, uint64_t>> rows = {
      {"SNSLoginRequest", lc::kSNSLoginRequest},
      {"SNSLoginProfileResult", lc::kSNSLoginProfileResult},
      {"SNSLoginClientSettings", lc::kSNSLoginClientSettings},
      {"SNSRefreshProfile", lc::kSNSRefreshProfile},
      {"SNSRefreshProfileResult", lc::kSNSRefreshProfileResult},
      {"SNSProfileRequestv2", lc::kSNSProfileRequestv2},
      {"SNSProfileRequest", lc::kSNSProfileRequest},
      {"SNSProfileResponsev2", lc::kSNSProfileResponsev2},
      {"SNSProfileResponse", lc::kSNSProfileResponse},
      {"SNSLeaderboardRequest", lc::kSNSLeaderboardRequest},
      {"SNSLeaderboardResponse", lc::kSNSLeaderboardResponse},
      {"SNSMatchEnded", lc::kSNSMatchEnded},
      {"SNSMatchEndedv2", lc::kSNSMatchEndedv2},
      {"SNSLobbyFindSessionRequestv8", lc::kSNSLobbyFindSessionRequestv8},
      {"SNSLobbyCreateSessionRequestv7", lc::kSNSLobbyCreateSessionRequestv7},
      {"SNSLobbyJoinSessionRequestv6", lc::kSNSLobbyJoinSessionRequestv6},
      {"SNSLobbyPlayerSessionsRequestv3", lc::kSNSLobbyPlayerSessionsRequestv3},
      {"SNSLobbyPendingSessionCancel", lc::kSNSLobbyPendingSessionCancel},
      {"SNSLobbySessionSuccessv4", lc::kSNSLobbySessionSuccessv4},
      {"SNSLobbySessionFailurev3", lc::kSNSLobbySessionFailurev3},
      {"SNSLobbySessionFailurev2", lc::kSNSLobbySessionFailurev2},
      {"SNSLobbySessionSuccessv5", lc::kSNSLobbySessionSuccessv5},
      {"SNSLobbySessionFailurev4", lc::kSNSLobbySessionFailurev4},
      {"SNSConfigSuccessv2", lc::kSNSConfigSuccessv2},
      {"SNSReconcileIAP", lc::kSNSReconcileIAP},
      {"SNSReconcileIAPResult", lc::kSNSReconcileIAPResult},
      {"SNSLobbyMatchmakerStatus", lc::kSNSLobbyMatchmakerStatus},
      {"SNSLobbyPlayerSessionsSuccessv3", lc::kSNSLobbyPlayerSessionsSuccessv3},
  };
  for (const auto& [name, value] : rows) {
    EXPECT_EQ(EchoVR::CSymbol64Hash(name), value) << name;
  }
}

// ---- login --------------------------------------------------------------------------------------

TEST(LegacyLogin, LoginRequestBecomesLogInRequestV2AndKeepsTheSession) {
  const lc::Context ctx = MakeContext();
  const std::string json = "{\"accountid\":1,\"displayname\":\"a\",\"lobbyversion\":2}";
  const std::string legacy =
      Bytes().Guid(0x20).User(kPlatformOvrOrg, kAccount).Raw(std::string("en\0\0\0\0\0\0", 8)).CStr(json).Str();
  const std::string current = Bytes().Guid(0x20).User(kPlatformOvrOrg, kAccount).CStr(json).Str();

  const lc::Translation t = lc::LegacyToCurrent(lc::kSNSLoginRequest, legacy, ctx, ProfileV2());
  const lc::OutMessage out = OnlyToGameService(t);
  EXPECT_EQ(out.symbol, nevr_evr_codec::kSymLoginRequest);
  EXPECT_EQ(out.payload, current);

  const lc::Translation back = lc::CurrentToLegacy(nevr_evr_codec::kSymLoginRequest, current, ctx, ProfileV2());
  EXPECT_EQ(OnlyToGameClient(back).symbol, lc::kSNSLoginRequest);
  EXPECT_EQ(OnlyToGameClient(back).payload, legacy);
}

TEST(LegacyLogin, LoginRequestJsonIsReplacedByTheBridgeProfileWhenGiven) {
  lc::Context ctx = MakeContext();
  ctx.loginProfileJson = "{\"accountid\":1,\"buildversion\":3}";
  const std::string legacy = Bytes().Guid(0).User(kPlatformOvrOrg, kAccount).Pad(8).CStr("{\"accountid\":2}").Str();
  const lc::Translation t = lc::LegacyToCurrent(lc::kSNSLoginRequest, legacy, ctx, ProfileV2());
  EXPECT_EQ(OnlyToGameService(t).payload,
            Bytes().Guid(0).User(kPlatformOvrOrg, kAccount).CStr(ctx.loginProfileJson).Str());
}

TEST(LegacyLogin, LoginSuccessPassesThroughBothWays) {
  const lc::Context ctx = MakeContext();
  const std::string payload = Bytes().Guid(0x30).User(kPlatformOvrOrg, kAccount).Str();
  const lc::Translation up = lc::LegacyToCurrent(nevr_evr_codec::kSymLoginSuccess, payload, ctx, ProfileV2());
  EXPECT_EQ(up.outcome, lc::Outcome::Passthrough);
  const lc::Translation down = lc::CurrentToLegacy(nevr_evr_codec::kSymLoginSuccess, payload, ctx, ProfileV2());
  EXPECT_EQ(down.outcome, lc::Outcome::Passthrough);
}

TEST(LegacyLogin, LoginFailureBecomesAProfileResultWithTheMatchingResultCode) {
  const lc::Context ctx = MakeContext();
  const std::vector<std::pair<uint64_t, uint8_t>> rows = {{401, 7}, {403, 8}, {400, 0}, {500, 0}};
  for (const auto& [status, code] : rows) {
    const std::string failure = Bytes().User(kPlatformOvrOrg, kAccount).U64(status).CStr("denied").Str();
    const lc::Translation t = lc::CurrentToLegacy(nevr_evr_codec::kSymLoginFailure, failure, ctx, ProfileV2());
    const lc::OutMessage out = OnlyToGameClient(t);
    EXPECT_EQ(out.symbol, lc::kSNSLoginProfileResult) << status;
    // session | user id | u32 | u8 result | 3 pad | u64 0: a full 0x30-byte header.
    EXPECT_EQ(out.payload, Bytes()
                               .Raw(std::string(ctx.loginSession.begin(), ctx.loginSession.end()))
                               .User(kPlatformOvrOrg, kAccount)
                               .U32(0)
                               .U8(code)
                               .Pad(3)
                               .U64(0)
                               .Str())
        << status;
    EXPECT_EQ(out.payload.size(), 0x30u);
  }
}

TEST(LegacyLogin, ProfileResultFailureBecomesALoginFailureAgain) {
  const lc::Context ctx = MakeContext();
  const std::string legacy = Bytes().Guid(0).User(kPlatformOvrOrg, kAccount).U32(0).U8(7).Pad(3).U64(0).Str();
  const lc::Translation t = lc::LegacyToCurrent(lc::kSNSLoginProfileResult, legacy, ctx, ProfileV2());
  const lc::OutMessage out = OnlyToGameService(t);
  EXPECT_EQ(out.symbol, nevr_evr_codec::kSymLoginFailure);
  EXPECT_EQ(out.payload, Bytes().User(kPlatformOvrOrg, kAccount).U64(401).CStr("").Str());
}

TEST(LegacyLogin, SettingsAreRenamedForTheV1ProfileSetAndKeptForV2) {
  const lc::Context ctx = MakeContext();
  const std::string raw = "{\"env\":\"live\",\"iap_unlocked\":true}";
  const std::string payload = Bytes().U64(raw.size()).Raw(ZlibCompress(raw)).Str();

  EXPECT_EQ(lc::CurrentToLegacy(nevr_evr_codec::kSymLoginSettings, payload, ctx, ProfileV2()).outcome,
            lc::Outcome::Passthrough);
  EXPECT_EQ(lc::LegacyToCurrent(nevr_evr_codec::kSymLoginSettings, payload, ctx, ProfileV2()).outcome,
            lc::Outcome::Passthrough);

  const lc::Translation v1set = lc::CurrentToLegacy(nevr_evr_codec::kSymLoginSettings, payload, ctx, ProfileV1());
  EXPECT_EQ(OnlyToGameClient(v1set).symbol, lc::kSNSLoginClientSettings);
  EXPECT_EQ(OnlyToGameClient(v1set).payload, payload);

  const lc::Translation back = lc::LegacyToCurrent(lc::kSNSLoginClientSettings, payload, ctx, ProfileV1());
  EXPECT_EQ(OnlyToGameService(back).symbol, nevr_evr_codec::kSymLoginSettings);
}

TEST(LegacyLogin, LoggedInUserProfileSuccessBecomesALoginProfileResult) {
  const lc::Context ctx = MakeContext();
  const json doc = {{"client", {{"displayname", "Cat"}, {"weapon", "rocket"}}},
                    {"server", {{"publisher_lock", "pub_a"}, {"loadout", json::object()}}},
                    {"config", {{"x", 1}}}};
  const std::string raw = doc.dump() + std::string(1, '\0');
  const std::string current =
      Bytes().User(kPlatformOvrOrg, kAccount).U32(static_cast<uint32_t>(raw.size())).Raw(ZstdCompress(raw)).Str();

  const lc::Translation t =
      lc::CurrentToLegacy(nevr_evr_codec::kSymLoggedInUserProfileSuccess, current, ctx, ProfileV2());
  const lc::OutMessage out = OnlyToGameClient(t);
  EXPECT_EQ(out.symbol, lc::kSNSLoginProfileResult);
  const std::string& p = out.payload;
  EXPECT_EQ(p.substr(0, 16), std::string(ctx.loginSession.begin(), ctx.loginSession.end()));
  EXPECT_EQ(U64At(p, 16), kPlatformOvrOrg);
  EXPECT_EQ(U64At(p, 24), kAccount);
  EXPECT_EQ(static_cast<uint8_t>(p.at(0x24)), lc::kLoginAccepted);
  const uint64_t rawLength = U64At(p, 0x28);
  const std::string body = ZlibUncompress(p.substr(0x30), static_cast<std::size_t>(rawLength));
  const std::size_t split = body.find('\0');
  ASSERT_NE(split, std::string::npos);
  EXPECT_EQ(json::parse(body.substr(0, split)), doc["client"]);
  EXPECT_EQ(json::parse(body.substr(split + 1)), doc["server"]);

  // And back: the game service's shape is {client, server} only; "config" is not part of the legacy result.
  const lc::Translation again = lc::LegacyToCurrent(lc::kSNSLoginProfileResult, p, ctx, ProfileV2());
  const lc::OutMessage back = OnlyToGameService(again);
  EXPECT_EQ(back.symbol, nevr_evr_codec::kSymLoggedInUserProfileSuccess);
  EXPECT_EQ(U64At(back.payload, 0), kPlatformOvrOrg);
  const std::string unz = ZstdUncompress(back.payload.substr(20), U32At(back.payload, 16));
  ASSERT_FALSE(unz.empty());
  EXPECT_EQ(unz.back(), '\0');
  const json parsed = json::parse(unz.substr(0, unz.size() - 1));
  EXPECT_EQ(parsed["client"], doc["client"]);
  EXPECT_EQ(parsed["server"], doc["server"]);
  EXPECT_FALSE(parsed.contains("config"));
}

TEST(LegacyLogin, TheBridgeBuildsTheLoggedInUserProfileRequestAfterLoginSuccess) {
  lc::Context ctx = MakeContext();
  ctx.profileRequestJson = "{\"unlocksetids\":{}}";
  const lc::OutMessage m = lc::BuildLoggedInUserProfileRequest(ctx);
  EXPECT_EQ(m.symbol, nevr_evr_codec::kSymLoggedInUserProfileRequest);
  EXPECT_EQ(m.payload, Bytes()
                           .Raw(std::string(ctx.loginSession.begin(), ctx.loginSession.end()))
                           .User(kPlatformOvrOrg, kAccount)
                           .CStr(ctx.profileRequestJson)
                           .Str());
}

TEST(LegacyProfile, RefreshProfileOfSelfBecomesLoggedInUserProfileRequest) {
  lc::Context ctx = MakeContext();
  ctx.profileRequestJson = "{\"a\":1}";
  const std::string legacy = Bytes().Guid(0x40).User(kPlatformOvrOrg, kAccount).U64(1).Str();
  const lc::OutMessage out = OnlyToGameService(lc::LegacyToCurrent(lc::kSNSRefreshProfile, legacy, ctx, ProfileV2()));
  EXPECT_EQ(out.symbol, nevr_evr_codec::kSymLoggedInUserProfileRequest);
  EXPECT_EQ(out.payload, Bytes().Guid(0x40).User(kPlatformOvrOrg, kAccount).CStr("{\"a\":1}").Str());

  const lc::OutMessage back = OnlyToGameClient(
      lc::CurrentToLegacy(nevr_evr_codec::kSymLoggedInUserProfileRequest, out.payload, ctx, ProfileV2()));
  EXPECT_EQ(back.symbol, lc::kSNSRefreshProfile);
  EXPECT_EQ(back.payload, legacy);
}

TEST(LegacyProfile, RefreshProfileOfAnotherUserBecomesOtherUserProfileRequest) {
  lc::Context ctx = MakeContext();
  ctx.profileRequestJson = "{\"a\":1}";
  const std::string legacy = Bytes().Guid(0x40).User(kPlatformOvrOrg, kOtherAccount).U64(0).Str();
  const lc::OutMessage out = OnlyToGameService(lc::LegacyToCurrent(lc::kSNSRefreshProfile, legacy, ctx, ProfileV2()));
  EXPECT_EQ(out.symbol, nevr_evr_codec::kSymOtherUserProfileRequest);
  EXPECT_EQ(out.payload, Bytes().User(kPlatformOvrOrg, kOtherAccount).CStr("{\"a\":1}").Str());
}

TEST(LegacyProfile, ProfileRequestOfBothFamiliesBecomesOtherUserProfileRequest) {
  lc::Context ctx = MakeContext();
  ctx.profileRequestJson = "{}";
  const std::string legacy = Bytes().U64(0).User(kPlatformOvrOrg, kOtherAccount).Str();
  const std::string current = Bytes().User(kPlatformOvrOrg, kOtherAccount).CStr("{}").Str();

  const lc::OutMessage v2set =
      OnlyToGameService(lc::LegacyToCurrent(lc::kSNSProfileRequestv2, legacy, ctx, ProfileV2()));
  EXPECT_EQ(v2set.symbol, nevr_evr_codec::kSymOtherUserProfileRequest);
  EXPECT_EQ(v2set.payload, current);

  const lc::OutMessage v1set = OnlyToGameService(lc::LegacyToCurrent(lc::kSNSProfileRequest, legacy, ctx, ProfileV1()));
  EXPECT_EQ(v1set.payload, current);
}

TEST(LegacyProfile, OtherUserProfileSuccessBecomesEachLegacyReply) {
  const lc::Context ctx = MakeContext();
  const json profile = {{"displayname", "Cat"}, {"loadout", {{"instances", json::array()}}}};
  const std::string raw = profile.dump() + std::string(1, '\0');
  const std::string current =
      Bytes().User(kPlatformOvrOrg, kOtherAccount).U32(static_cast<uint32_t>(raw.size())).Raw(ZstdCompress(raw)).Str();
  const std::string legacyJson = Bytes().CStr(profile.dump()).Str();

  const lc::OutMessage response = OnlyToGameClient(lc::CurrentToLegacy(
      nevr_evr_codec::kSymOtherUserProfileSuccess, current, ctx, ProfileV2(), lc::ProfileReply::ProfileResponse));
  EXPECT_EQ(response.symbol, lc::kSNSProfileResponsev2);
  EXPECT_EQ(response.payload, Bytes().User(kPlatformOvrOrg, kOtherAccount).Raw(legacyJson).Str());

  const lc::OutMessage v1set = OnlyToGameClient(lc::CurrentToLegacy(
      nevr_evr_codec::kSymOtherUserProfileSuccess, current, ctx, ProfileV1(), lc::ProfileReply::ProfileResponse));
  EXPECT_EQ(v1set.symbol, lc::kSNSProfileResponse);

  const lc::OutMessage refresh = OnlyToGameClient(lc::CurrentToLegacy(
      nevr_evr_codec::kSymOtherUserProfileSuccess, current, ctx, ProfileV2(), lc::ProfileReply::RefreshProfileResult));
  EXPECT_EQ(refresh.symbol, lc::kSNSRefreshProfileResult);
  // user id | u32 | u8 result | 3 pad | JSON NUL
  EXPECT_EQ(refresh.payload,
            Bytes().User(kPlatformOvrOrg, kOtherAccount).U32(0).U8(lc::kLoginAccepted).Pad(3).Raw(legacyJson).Str());

  // Back to the game service's shape: JSON equal, the compressed bytes are the library's business.
  const lc::OutMessage up =
      OnlyToGameService(lc::LegacyToCurrent(lc::kSNSProfileResponsev2, response.payload, ctx, ProfileV2()));
  EXPECT_EQ(up.symbol, nevr_evr_codec::kSymOtherUserProfileSuccess);
  const std::string unz = ZstdUncompress(up.payload.substr(20), U32At(up.payload, 16));
  EXPECT_EQ(json::parse(unz.substr(0, unz.size() - 1)), profile);

  const lc::OutMessage up2 =
      OnlyToGameService(lc::LegacyToCurrent(lc::kSNSRefreshProfileResult, refresh.payload, ctx, ProfileV2()));
  EXPECT_EQ(up2.symbol, nevr_evr_codec::kSymOtherUserProfileSuccess);
}

TEST(LegacyProfile, UpdateProfilePassesThrough) {
  const lc::Context ctx = MakeContext();
  const std::string payload = Bytes().Guid(1).User(kPlatformOvrOrg, kAccount).CStr("{\"weapon\":\"rocket\"}").Str();
  EXPECT_EQ(lc::LegacyToCurrent(nevr_evr_codec::kSymUpdateProfile, payload, ctx, ProfileV2()).outcome,
            lc::Outcome::Passthrough);
}

// ---- local answers and drops ----------------------------------------------------------------

TEST(LegacyLocal, TelemetryAndMatchEndedAreDropped) {
  const lc::Context ctx = MakeContext();
  for (const uint64_t symbol : {nevr_evr_codec::kSymTelemetryEvent, lc::kSNSMatchEnded, lc::kSNSMatchEndedv2}) {
    const lc::Translation t = lc::LegacyToCurrent(symbol, "payload", ctx, ProfileV2());
    EXPECT_EQ(t.outcome, lc::Outcome::Dropped) << std::hex << symbol;
    EXPECT_TRUE(t.toGameService.empty());
    EXPECT_TRUE(t.toGameClient.empty());
  }
}

TEST(LegacyLocal, LeaderboardRequestIsAnsweredWithAnEmptyBoardUnderTheSameTag) {
  const lc::Context ctx = MakeContext();
  const std::string request = Bytes().U64(0x1020304050607080ULL).U64(0).Pad(16).U64(0).Str();
  const lc::Translation t = lc::LegacyToCurrent(lc::kSNSLeaderboardRequest, request, ctx, ProfileV2());
  EXPECT_EQ(t.outcome, lc::Outcome::Local);
  EXPECT_TRUE(t.toGameService.empty());
  ASSERT_EQ(t.toGameClient.size(), 1u);
  EXPECT_EQ(t.toGameClient[0].symbol, lc::kSNSLeaderboardResponse);
  const std::string& p = t.toGameClient[0].payload;
  EXPECT_EQ(U64At(p, 0), 0x1020304050607080ULL);
  EXPECT_EQ(ZlibUncompress(p.substr(16), static_cast<std::size_t>(U64At(p, 8))), "[]");
}

TEST(LegacyConfig, ConfigAndReconcileRowsPassThroughBothWays) {
  const lc::Context ctx = MakeContext();
  for (const uint64_t symbol :
       {nevr_evr_codec::kSymConfigRequest, lc::kSNSConfigSuccessv2, nevr_evr_codec::kSymConfigFailure,
        lc::kSNSReconcileIAP, lc::kSNSReconcileIAPResult, nevr_evr_codec::kSymMatchmakerStatusRequest,
        lc::kSNSLobbyMatchmakerStatus, lc::kSNSLobbyPlayerSessionsSuccessv3}) {
    EXPECT_EQ(lc::LegacyToCurrent(symbol, "x", ctx, ProfileV2()).outcome, lc::Outcome::Passthrough)
        << std::hex << symbol;
    EXPECT_EQ(lc::CurrentToLegacy(symbol, "x", ctx, ProfileV2()).outcome, lc::Outcome::Passthrough)
        << std::hex << symbol;
  }
}

// ---- matching -----------------------------------------------------------------------------------

namespace {

std::string FindV8(uint64_t level) {
  return Bytes()
      .U64(kLockValue)
      .U64(kModeValue)
      .U64(level)
      .U64(kPlatformSymbol)
      .U8(1)
      .U8(2)
      .Pad(6)
      .Guid(0x70)
      .CStr(kSettingsJson)
      .User(kPlatformOvrOrg, kAccount)
      .Str();
}

std::string FindV11(const lc::Context& ctx, uint64_t mode, uint64_t level, uint64_t platform) {
  return Bytes()
      .U64(kLockValue)
      .U64(mode)
      .U64(level)
      .U64(platform)
      .Raw(std::string(ctx.loginSession.begin(), ctx.loginSession.end()))
      .U8(1)
      .U32(0)
      .Pad(3)      // entrant count, flags, alignment
      .Pad(16)     // the lobby the game is already in: none
      .Guid(0x70)  // group (the legacy channel)
      .CStr(kSettingsJson)
      .User(kPlatformOvrOrg, kAccount)  // the entrant
      .Str();
}

}  // namespace

TEST(LegacyMatching, FindSessionV8BecomesV11) {
  const lc::Context ctx = MakeContext();
  const std::string legacy = FindV8(kLevelUnspecified);
  const lc::OutMessage out =
      OnlyToGameService(lc::LegacyToCurrent(lc::kSNSLobbyFindSessionRequestv8, legacy, ctx, ProfileV2()));
  EXPECT_EQ(out.symbol, nevr_evr_codec::kSymFindSessionRequest);
  EXPECT_EQ(out.payload, FindV11(ctx, kModeValue, kLevelUnspecified, kPlatformSymbol));
}

TEST(LegacyMatching, FindSessionV11BecomesV8AndDropsWhatLegacyHasNoSlotFor) {
  const lc::Context ctx = MakeContext();
  const std::string current = FindV11(ctx, kModeValue, kLevelUnspecified, kPlatformSymbol);
  const lc::OutMessage back =
      OnlyToGameClient(lc::CurrentToLegacy(nevr_evr_codec::kSymFindSessionRequest, current, ctx, ProfileV2()));
  EXPECT_EQ(back.symbol, lc::kSNSLobbyFindSessionRequestv8);
  // The two unknown bytes after the platform are not carried by the current layout: they come back 0.
  EXPECT_EQ(back.payload, Bytes()
                              .U64(kLockValue)
                              .U64(kModeValue)
                              .U64(kLevelUnspecified)
                              .U64(kPlatformSymbol)
                              .U8(0)
                              .U8(0)
                              .Pad(6)
                              .Guid(0x70)
                              .CStr(kSettingsJson)
                              .User(kPlatformOvrOrg, kAccount)
                              .Str());
}

TEST(LegacyMatching, FindSessionMapsModeLevelAndPlatformThroughTheBuildTables) {
  const lc::Context ctx = MakeContext();
  lc::BuildTables tables = ProfileV2();
  tables.modes = {{kModeValue, 0xAAAA000000000001ULL}};
  tables.levels = {{0x1111000000000002ULL, 0xBBBB000000000002ULL}};
  tables.platforms = {{kPlatformSymbol, 0xCCCC000000000003ULL}};
  const lc::OutMessage out = OnlyToGameService(
      lc::LegacyToCurrent(lc::kSNSLobbyFindSessionRequestv8, FindV8(0x1111000000000002ULL), ctx, tables));
  EXPECT_EQ(out.payload, FindV11(ctx, 0xAAAA000000000001ULL, 0xBBBB000000000002ULL, 0xCCCC000000000003ULL));

  // Reply direction uses the same table backwards.
  const lc::OutMessage back =
      OnlyToGameClient(lc::CurrentToLegacy(nevr_evr_codec::kSymFindSessionRequest, out.payload, ctx, tables));
  EXPECT_EQ(U64At(back.payload, 8), kModeValue);
  EXPECT_EQ(U64At(back.payload, 16), 0x1111000000000002ULL);
  EXPECT_EQ(U64At(back.payload, 24), kPlatformSymbol);
}

TEST(LegacyMatching, FindSessionRejectsATruncatedPayload) {
  const lc::Context ctx = MakeContext();
  std::string legacy = FindV8(kLevelUnspecified);
  legacy.resize(legacy.size() - 20);  // cuts into the user id
  EXPECT_EQ(lc::LegacyToCurrent(lc::kSNSLobbyFindSessionRequestv8, legacy, ctx, ProfileV2()).outcome,
            lc::Outcome::Malformed);
}

TEST(LegacyMatching, JoinSessionV6BecomesV7WithTheTeamAsTheTail) {
  const lc::Context ctx = MakeContext();
  for (const int16_t team : {int16_t{-1}, int16_t{1}}) {
    const std::string legacy = Bytes()
                                   .Guid(0xE0)
                                   .U64(kLockValue)
                                   .U64(kPlatformSymbol)
                                   .U64(1)
                                   .U64(3)
                                   .CStr("{\"appid\":\"7\"}")
                                   .User(kPlatformOvrOrg, kAccount)
                                   .I16(team)
                                   .Str();
    const std::string current = Bytes()
                                    .Guid(0xE0)
                                    .U64(kLockValue)
                                    .U64(kPlatformSymbol)
                                    .Raw(std::string(ctx.loginSession.begin(), ctx.loginSession.end()))
                                    .U32(1)
                                    .Pad(4)  // flags (entrant count in the low byte), alignment
                                    .U64(0)  // request flags
                                    .CStr("{\"appid\":\"7\"}")
                                    .User(kPlatformOvrOrg, kAccount)
                                    .I16(team)
                                    .Str();
    const lc::OutMessage out =
        OnlyToGameService(lc::LegacyToCurrent(lc::kSNSLobbyJoinSessionRequestv6, legacy, ctx, ProfileV2()));
    EXPECT_EQ(out.symbol, nevr_evr_codec::kSymJoinSessionRequest);
    EXPECT_EQ(out.payload, current) << team;

    const lc::OutMessage back =
        OnlyToGameClient(lc::CurrentToLegacy(nevr_evr_codec::kSymJoinSessionRequest, current, ctx, ProfileV2()));
    EXPECT_EQ(back.symbol, lc::kSNSLobbyJoinSessionRequestv6);
    // The legacy request carries two unknown u64 the current layout has no slot for; they come back 0.
    EXPECT_EQ(back.payload, Bytes()
                                .Guid(0xE0)
                                .U64(kLockValue)
                                .U64(kPlatformSymbol)
                                .U64(0)
                                .U64(0)
                                .CStr("{\"appid\":\"7\"}")
                                .User(kPlatformOvrOrg, kAccount)
                                .I16(team)
                                .Str());
  }
}

TEST(LegacyMatching, JoinSessionV6WithoutATeamIndexReadsAsMinusOne) {
  const lc::Context ctx = MakeContext();
  const std::string legacy = Bytes()
                                 .Guid(0xE0)
                                 .U64(kLockValue)
                                 .U64(kPlatformSymbol)
                                 .U64(1)
                                 .U64(3)
                                 .CStr("{}")
                                 .User(kPlatformOvrOrg, kAccount)
                                 .Str();
  const lc::OutMessage out =
      OnlyToGameService(lc::LegacyToCurrent(lc::kSNSLobbyJoinSessionRequestv6, legacy, ctx, ProfileV2()));
  EXPECT_EQ(out.payload.substr(out.payload.size() - 2), Bytes().I16(-1).Str());
}

TEST(LegacyMatching, CreateSessionV7BecomesV9) {
  const lc::Context ctx = MakeContext();
  const std::string legacy = Bytes()
                                 .U64(0xFFFFFFFFFFFFFFFFULL)
                                 .U64(kLockValue)
                                 .U64(0x0999095165F4DB8CULL)
                                 .U64(kLevelUnspecified)
                                 .U64(kPlatformSymbol)
                                 .U8(1)
                                 .Pad(7)  // lobby type, then 7 bytes whose meaning is not established
                                 .Guid(0x70)
                                 .CStr("{\"gametype\":33}")
                                 .User(kPlatformOvrOrg, kAccount)
                                 .I16(-1)
                                 .Str();
  const std::string current = Bytes()
                                  .U64(0xFFFFFFFFFFFFFFFFULL)
                                  .U64(kLockValue)
                                  .U64(0x0999095165F4DB8CULL)
                                  .U64(kLevelUnspecified)
                                  .U64(kPlatformSymbol)
                                  .Raw(std::string(ctx.loginSession.begin(), ctx.loginSession.end()))
                                  .U8(1)
                                  .Pad(7)  // entrant count, alignment
                                  .U8(1)
                                  .Pad(3)  // lobby type, alignment
                                  .U32(0)  // flags
                                  .Guid(0x70)
                                  .CStr("{\"gametype\":33}")
                                  .User(kPlatformOvrOrg, kAccount)
                                  .I16(-1)
                                  .Str();
  const lc::OutMessage out =
      OnlyToGameService(lc::LegacyToCurrent(lc::kSNSLobbyCreateSessionRequestv7, legacy, ctx, ProfileV2()));
  EXPECT_EQ(out.symbol, nevr_evr_codec::kSymCreateSessionRequest);
  EXPECT_EQ(out.payload, current);

  const lc::OutMessage back =
      OnlyToGameClient(lc::CurrentToLegacy(nevr_evr_codec::kSymCreateSessionRequest, current, ctx, ProfileV2()));
  EXPECT_EQ(back.symbol, lc::kSNSLobbyCreateSessionRequestv7);
  EXPECT_EQ(back.payload, legacy);
}

TEST(LegacyMatching, PlayerSessionsV3BecomesV5AndBack) {
  const lc::Context ctx = MakeContext();
  const std::string legacy = Bytes()
                                 .Guid(0x90)
                                 .U64(kPlatformSymbol)
                                 .U64(2)
                                 .User(kPlatformOvrOrg, kAccount)
                                 .User(kPlatformOvrOrg, kOtherAccount)
                                 .Str();
  const std::string current = Bytes()
                                  .Raw(std::string(ctx.loginSession.begin(), ctx.loginSession.end()))
                                  .User(kPlatformOvrOrg, kAccount)  // the requester: the first listed id
                                  .Guid(0x90)
                                  .U64(kPlatformSymbol)
                                  .U64(2)
                                  .User(kPlatformOvrOrg, kAccount)
                                  .User(kPlatformOvrOrg, kOtherAccount)
                                  .Str();
  const lc::OutMessage out =
      OnlyToGameService(lc::LegacyToCurrent(lc::kSNSLobbyPlayerSessionsRequestv3, legacy, ctx, ProfileV2()));
  EXPECT_EQ(out.symbol, nevr_evr_codec::kSymPlayerSessionsRequest);
  EXPECT_EQ(out.payload, current);

  const lc::OutMessage back =
      OnlyToGameClient(lc::CurrentToLegacy(nevr_evr_codec::kSymPlayerSessionsRequest, current, ctx, ProfileV2()));
  EXPECT_EQ(back.symbol, lc::kSNSLobbyPlayerSessionsRequestv3);
  EXPECT_EQ(back.payload, legacy);
}

TEST(LegacyMatching, PendingSessionCancelCarriesTheLoginSessionOnTheCurrentSide) {
  const lc::Context ctx = MakeContext();
  const lc::OutMessage out =
      OnlyToGameService(lc::LegacyToCurrent(lc::kSNSLobbyPendingSessionCancel, std::string(1, '\0'), ctx, ProfileV2()));
  EXPECT_EQ(out.symbol, nevr_evr_codec::kSymPendingSessionCancel);
  EXPECT_EQ(out.payload, std::string(ctx.loginSession.begin(), ctx.loginSession.end()));

  const lc::OutMessage back =
      OnlyToGameClient(lc::CurrentToLegacy(nevr_evr_codec::kSymPendingSessionCancel, out.payload, ctx, ProfileV2()));
  EXPECT_EQ(back.symbol, lc::kSNSLobbyPendingSessionCancel);
  EXPECT_EQ(back.payload, std::string(1, '\0'));
}

namespace {

uint64_t EncoderFlags(bool quest, uint64_t digest, uint64_t mac, uint64_t enc, uint64_t rnd) {
  uint64_t f = 0;
  const int s = quest ? 1 : 0;
  if (quest) f |= 1;
  f |= uint64_t{1} << s;        // encryption
  f |= uint64_t{1} << (1 + s);  // mac
  f |= digest << (2 + s);
  f |= mac << (26 + s);
  f |= enc << (38 + s);
  f |= rnd << (50 + s);
  return f;
}

std::string KeyBytes(std::size_t n, uint8_t fill) { return std::string(n, static_cast<char>(fill)); }

// Everything after the group: endpoint .. the second set of keys, for 0x20-byte keys on both sides.
std::string SuccessTail(bool quest) {
  const uint64_t gameServerFlags = EncoderFlags(quest, 0x20, 0x20, 0x20, 0x20);
  const uint64_t gameClientFlags = EncoderFlags(quest, 0x40, 0x20, 0x20, 0x20);
  return Bytes()
      .U8(10)
      .U8(0)
      .U8(0)
      .U8(5)
      .U8(203)
      .U8(0)
      .U8(113)
      .U8(7)
      .Be16(6792)  // internal, external, port (big endian)
      .I16(1)
      .U8(0)
      .Pad(3)  // team, session flags, alignment
      .U64(gameServerFlags)
      .U64(gameClientFlags)
      .U64(0x1111)
      .Raw(KeyBytes(0x20, 0xA1))
      .Raw(KeyBytes(0x20, 0xA2))
      .Raw(KeyBytes(0x20, 0xA3))
      .U64(0x2222)
      .Raw(KeyBytes(0x20, 0xB1))
      .Raw(KeyBytes(0x20, 0xB2))
      .Raw(KeyBytes(0x20, 0xB3))
      .Str();
}

}  // namespace

TEST(LegacyMatching, SessionSuccessV5LosesTheGroupToBecomeV4) {
  const lc::Context ctx = MakeContext();
  const std::string v5 = Bytes().U64(kModeValue).Guid(0x50).Guid(0x70).Raw(SuccessTail(false)).Str();
  const std::string v4 = Bytes().U64(kModeValue).Guid(0x50).Raw(SuccessTail(false)).Str();
  const lc::OutMessage out = OnlyToGameClient(lc::CurrentToLegacy(lc::kSNSLobbySessionSuccessv5, v5, ctx, ProfileV2()));
  EXPECT_EQ(out.symbol, lc::kSNSLobbySessionSuccessv4);
  EXPECT_EQ(out.payload, v4);

  // Back: the group comes from the context (the channel the game asked for).
  const lc::OutMessage back =
      OnlyToGameService(lc::LegacyToCurrent(lc::kSNSLobbySessionSuccessv4, v4, ctx, ProfileV2()));
  EXPECT_EQ(back.symbol, lc::kSNSLobbySessionSuccessv5);
  EXPECT_EQ(back.payload, v5);
}

TEST(LegacyMatching, SessionSuccessQuestEncoderFlagsAreRewrittenToTheLegacyLayout) {
  lc::Context ctx = MakeContext();
  ctx.currentUsesQuestFlags = true;
  const std::string v5 = Bytes().U64(kModeValue).Guid(0x50).Guid(0x70).Raw(SuccessTail(true)).Str();
  const std::string v4 = Bytes().U64(kModeValue).Guid(0x50).Raw(SuccessTail(false)).Str();
  EXPECT_EQ(OnlyToGameClient(lc::CurrentToLegacy(lc::kSNSLobbySessionSuccessv5, v5, ctx, ProfileV2())).payload, v4);
}

TEST(LegacyMatching, SessionSuccessWhoseLengthDisagreesWithItsKeySizesIsMalformed) {
  const lc::Context ctx = MakeContext();
  std::string v5 = Bytes().U64(kModeValue).Guid(0x50).Guid(0x70).Raw(SuccessTail(false)).Str();
  v5.resize(v5.size() - 1);
  EXPECT_EQ(lc::CurrentToLegacy(lc::kSNSLobbySessionSuccessv5, v5, ctx, ProfileV2()).outcome, lc::Outcome::Malformed);
}

TEST(LegacyMatching, SessionFailureV4BecomesV3ForTheV2SetAndV2ForV1) {
  const lc::Context ctx = MakeContext();
  const std::string v4 = Bytes()
                             .U64(kModeValue)
                             .Guid(0x60)
                             .U32(9)
                             .U32(255)
                             .Raw(std::string("server is full") + std::string(50, '\0'))
                             .U64(1760000000)
                             .Str();
  const lc::OutMessage v3 = OnlyToGameClient(lc::CurrentToLegacy(lc::kSNSLobbySessionFailurev4, v4, ctx, ProfileV2()));
  EXPECT_EQ(v3.symbol, lc::kSNSLobbySessionFailurev3);
  EXPECT_EQ(v3.payload, Bytes().U64(kModeValue).Guid(0x60).U32(9).U32(255).Str());

  const lc::OutMessage v2 = OnlyToGameClient(lc::CurrentToLegacy(lc::kSNSLobbySessionFailurev4, v4, ctx, ProfileV1()));
  EXPECT_EQ(v2.symbol, lc::kSNSLobbySessionFailurev2);
  EXPECT_EQ(v2.payload, Bytes().Guid(0x60).U32(9).Str());
}

// ---- whole frames -------------------------------------------------------------------------------

TEST(LegacyFrame, FromGameClientTranslatesKnownRowsAndWithholdsWhatTheGameServiceWouldChokeOn) {
  const lc::Context ctx = MakeContext();
  std::string frame;
  frame += nevr_evr_codec::BuildMessage(lc::kSNSLobbyFindSessionRequestv8, FindV8(kLevelUnspecified));
  frame += nevr_evr_codec::BuildMessage(nevr_evr_codec::kSymTelemetryEvent, "tele");
  frame += nevr_evr_codec::BuildMessage(0x1234567812345678ULL, "who knows");
  frame += nevr_evr_codec::BuildMessage(nevr_evr_codec::kSymConfigRequest, "cfg");

  const lc::FrameTranslation t = lc::TranslateFrameFromGameClient(frame, ctx, ProfileV2());
  EXPECT_EQ(t.translated, 1u);
  EXPECT_EQ(t.passed, 1u);
  EXPECT_EQ(t.dropped, 1u);
  EXPECT_EQ(t.unsupported, 1u);
  EXPECT_EQ(t.malformed, 0u);
  EXPECT_EQ(t.toGameClient, "");
  EXPECT_EQ(t.toGameService,
            nevr_evr_codec::BuildMessage(nevr_evr_codec::kSymFindSessionRequest,
                                         FindV11(ctx, kModeValue, kLevelUnspecified, kPlatformSymbol)) +
                nevr_evr_codec::BuildMessage(nevr_evr_codec::kSymConfigRequest, "cfg"));
}

TEST(LegacyFrame, FromGameServiceTranslatesUsingTheGivenProfileReply) {
  const lc::Context ctx = MakeContext();
  const std::string failure = Bytes().User(kPlatformOvrOrg, kAccount).U64(401).CStr("no").Str();
  std::string frame = nevr_evr_codec::BuildMessage(nevr_evr_codec::kSymLoginFailure, failure);
  frame += nevr_evr_codec::BuildMessage(nevr_evr_codec::kSymConnectionUnrequire, "");
  const lc::FrameTranslation t =
      lc::TranslateFrameFromGameService(frame, ctx, ProfileV2(), lc::ProfileReply::LoginProfileResult);
  EXPECT_EQ(t.translated, 1u);
  EXPECT_EQ(t.passed, 1u);  // the transport-level Unrequire is not a row to translate
  EXPECT_EQ(t.unsupported, 0u);
  ASSERT_GE(t.toGameClient.size(), nevr_evr_codec::kHeaderSize);
  EXPECT_EQ(nevr_evr_codec::FirstSymbol(t.toGameClient), lc::kSNSLoginProfileResult);
  EXPECT_EQ(t.toGameClient.substr(t.toGameClient.size() - nevr_evr_codec::kHeaderSize),
            nevr_evr_codec::BuildMessage(nevr_evr_codec::kSymConnectionUnrequire, ""));
}

TEST(LegacyFrame, ABadMarkerEndsTheFrameAndCountsAsMalformed) {
  const lc::Context ctx = MakeContext();
  std::string frame = nevr_evr_codec::BuildMessage(nevr_evr_codec::kSymConfigRequest, "cfg");
  frame += std::string(40, 'x');
  const lc::FrameTranslation t = lc::TranslateFrameFromGameClient(frame, ctx, ProfileV2());
  EXPECT_EQ(t.passed, 1u);
  EXPECT_EQ(t.malformed, 1u);
}
