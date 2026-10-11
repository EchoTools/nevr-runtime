// Tests for the per-login legacy session (compat/legacy_session.{h,cpp}). Each test plays a short script of
// frames through the session and reads the frames that come out with the independent reader below, so the
// session's own framing is not what checks it. Message bodies are built with the writer from
// legacy_wire_writer.h, which shares no code with the codec.

#include <gtest/gtest.h>

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

#include "runtime/compat/evr_codec.h"
#include "runtime/compat/legacy_codec.h"
#include "runtime/compat/legacy_session.h"
#include "runtime/tests/legacy_wire_writer.h"

namespace ls = nevr_legacy_session;
namespace lc = nevr_legacy_codec;
using nevr_legacy_test::Bytes;
using nevr_legacy_test::GuidOf;
using nevr_legacy_test::U32At;
using nevr_legacy_test::U64At;
using nevr_legacy_test::ZlibUncompress;
using nevr_legacy_test::ZstdCompress;
using nevr_legacy_test::ZstdUncompress;
using nlohmann::json;

namespace {

constexpr uint64_t kPlatform = 4;
constexpr uint64_t kAccount = 0x1122334455667788ULL;
constexpr uint64_t kOtherAccount = 0x8877665544332211ULL;
constexpr uint64_t kLock = 0x0123456789ABCDEFULL;
constexpr uint64_t kMode = 0x5555666677778888ULL;
constexpr uint64_t kPlatformSymbol = 0xA0A1A2A3A4A5A6A7ULL;
constexpr uint64_t kUnspecified = 0xFFFFFFFFFFFFFFFFULL;

struct Message {
  uint64_t symbol = 0;
  std::string payload;
};

// Independent frame reader: [marker(8)][symbol(8)][length(8)][payload] repeated.
std::vector<Message> Split(const std::string& frame) {
  std::vector<Message> out;
  std::size_t at = 0;
  while (at + 24 <= frame.size()) {
    const uint64_t length = U64At(frame, at + 16);
    EXPECT_LE(at + 24 + length, frame.size());
    out.push_back({U64At(frame, at + 8), frame.substr(at + 24, static_cast<std::size_t>(length))});
    at += 24 + static_cast<std::size_t>(length);
  }
  EXPECT_EQ(at, frame.size());
  return out;
}

std::string Frame(uint64_t symbol, const std::string& payload) { return nevr_evr_codec::BuildMessage(symbol, payload); }

std::string GuidString(uint8_t seed) {
  const auto g = GuidOf(seed);
  return std::string(g.begin(), g.end());
}

ls::Config MakeConfig() {
  ls::Config config;
  config.tables = lc::LobbyB2ProfileV2Tables();
  config.loginProfileJson = "{\"accountid\":1,\"buildversion\":3}";
  config.profileRequestJson = "{\"fields\":[]}";
  return config;
}

std::string LegacyLogin() {
  return Frame(lc::kSNSLoginRequest, Bytes().Guid(0).User(kPlatform, kAccount).Pad(8).CStr("{\"accountid\":2}").Str());
}

std::string LoginSuccess(uint8_t sessionSeed = 0x30) {
  return Frame(nevr_evr_codec::kSymLoginSuccess, Bytes().Guid(sessionSeed).User(kPlatform, kAccount).Str());
}

std::string ZstdBody(const json& doc) {
  const std::string raw = doc.dump() + std::string(1, '\0');
  return Bytes().U32(static_cast<uint32_t>(raw.size())).Raw(ZstdCompress(raw)).Str();
}

std::string LoggedInProfileSuccess(const json& client, const json& server) {
  return Frame(nevr_evr_codec::kSymLoggedInUserProfileSuccess,
               Bytes().User(kPlatform, kAccount).Raw(ZstdBody({{"client", client}, {"server", server}})).Str());
}

std::string OtherProfileSuccess(uint64_t account, const json& profile) {
  return Frame(nevr_evr_codec::kSymOtherUserProfileSuccess,
               Bytes().User(kPlatform, account).Raw(ZstdBody(profile)).Str());
}

std::string FindV8(const std::string& channel) {
  return Frame(lc::kSNSLobbyFindSessionRequestv8, Bytes()
                                                      .U64(kLock)
                                                      .U64(kMode)
                                                      .U64(kUnspecified)
                                                      .U64(kPlatformSymbol)
                                                      .U8(0)
                                                      .U8(0)
                                                      .Pad(6)
                                                      .Raw(channel)
                                                      .CStr("{\"gametype\":22}")
                                                      .User(kPlatform, kAccount)
                                                      .Str());
}

// A success in the current family whose group is `group`, 0x20-byte PC keys.
std::string SuccessV5(const std::string& group) {
  const uint64_t flags = 1 | (uint64_t{1} << 1) | (uint64_t{0x20} << 2) | (uint64_t{0x20} << 26) |
                         (uint64_t{0x20} << 38) | (uint64_t{0x20} << 50);
  Bytes b;
  b.U64(kMode).Guid(0x50).Raw(group);
  b.U8(10).U8(0).U8(0).U8(5).U8(203).U8(0).U8(113).U8(7).U8(0x1A).U8(0x88);  // endpoint
  b.I16(1).U8(0).Pad(3).U64(flags).U64(flags);
  b.U64(1).Raw(std::string(0x60, 'a')).U64(2).Raw(std::string(0x60, 'b'));
  return Frame(lc::kSNSLobbySessionSuccessv5, b.Str());
}

}  // namespace

TEST(LegacySession, TheGamesLoginIsTranslatedInPlaceWithTheBridgeProfile) {
  ls::LegacySession session(MakeConfig());
  const ls::Step step = session.FromGameClient(LegacyLogin());
  EXPECT_EQ(step.toGameClient, "");
  const auto out = Split(step.toGameService);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].symbol, nevr_evr_codec::kSymLoginRequest);
  EXPECT_EQ(out[0].payload,
            Bytes().Guid(0).User(kPlatform, kAccount).CStr("{\"accountid\":1,\"buildversion\":3}").Str());
}

TEST(LegacySession, LoginSuccessIsForwardedAndFollowedByOneProfileRequest) {
  ls::LegacySession session(MakeConfig());
  session.FromGameClient(LegacyLogin());
  const ls::Step step = session.FromGameService(LoginSuccess());

  EXPECT_EQ(step.toGameClient, LoginSuccess());
  const auto out = Split(step.toGameService);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].symbol, nevr_evr_codec::kSymLoggedInUserProfileRequest);
  EXPECT_EQ(out[0].payload, Bytes().Raw(GuidString(0x30)).User(kPlatform, kAccount).CStr("{\"fields\":[]}").Str());
  EXPECT_EQ(session.GetStats().loginsInjected, 1u);

  // The same login session answering again must not ask a second time.
  const ls::Step again = session.FromGameService(LoginSuccess());
  EXPECT_EQ(again.toGameService, "");
  EXPECT_EQ(session.GetStats().loginsInjected, 1u);
}

TEST(LegacySession, ANewLoginSessionAsksAgain) {
  ls::LegacySession session(MakeConfig());
  session.FromGameService(LoginSuccess(0x30));
  const ls::Step second = session.FromGameService(LoginSuccess(0x31));
  const auto out = Split(second.toGameService);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].payload.substr(0, 16), GuidString(0x31));
  EXPECT_EQ(session.GetStats().loginsInjected, 2u);
}

TEST(LegacySession, TheInjectedRequestsAnswerBecomesTheLoginProfileResult) {
  ls::LegacySession session(MakeConfig());
  session.FromGameService(LoginSuccess());
  const ls::Step step =
      session.FromGameService(LoggedInProfileSuccess({{"displayname", "A"}}, {{"loadout", json::object()}}));

  EXPECT_EQ(step.toGameService, "");
  const auto out = Split(step.toGameClient);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].symbol, lc::kSNSLoginProfileResult);
  EXPECT_EQ(out[0].payload.substr(0, 16), GuidString(0x30));  // the login session the game service named
  EXPECT_EQ(static_cast<uint8_t>(out[0].payload.at(0x24)), lc::kLoginAccepted);
}

TEST(LegacySession, ALaterRefreshOfOwnProfileIsNotAnotherLoginResult) {
  ls::LegacySession session(MakeConfig());
  session.FromGameService(LoginSuccess());
  session.FromGameService(LoggedInProfileSuccess({{"displayname", "A"}}, json::object()));

  // The game refreshes its own profile; the answer is the same current message as the login's.
  const ls::Step request =
      session.FromGameClient(Frame(lc::kSNSRefreshProfile, Bytes().Guid(0x30).User(kPlatform, kAccount).U64(1).Str()));
  const auto sent = Split(request.toGameService);
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].symbol, nevr_evr_codec::kSymLoggedInUserProfileRequest);

  const ls::Step answer = session.FromGameService(LoggedInProfileSuccess(json::object(), {{"loadout", 1}}));
  const auto out = Split(answer.toGameClient);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].symbol, lc::kSNSRefreshProfileResult);
}

TEST(LegacySession, OtherUserAnswersTakeTheirOwnLegacyReplyInRequestOrder) {
  ls::LegacySession session(MakeConfig());
  session.FromGameService(LoginSuccess());
  session.FromGameService(LoggedInProfileSuccess(json::object(), json::object()));

  // Two requests in flight: a profile request, then a refresh of another user.
  const std::string both =
      Frame(lc::kSNSProfileRequestv2, Bytes().U64(0).User(kPlatform, kOtherAccount).Str()) +
      Frame(lc::kSNSRefreshProfile, Bytes().Guid(0x30).User(kPlatform, kOtherAccount + 1).U64(0).Str());
  const auto sent = Split(session.FromGameClient(both).toGameService);
  ASSERT_EQ(sent.size(), 2u);
  EXPECT_EQ(sent[0].symbol, nevr_evr_codec::kSymOtherUserProfileRequest);
  EXPECT_EQ(sent[1].symbol, nevr_evr_codec::kSymOtherUserProfileRequest);

  const auto first = Split(session.FromGameService(OtherProfileSuccess(kOtherAccount, {{"a", 1}})).toGameClient);
  ASSERT_EQ(first.size(), 1u);
  EXPECT_EQ(first[0].symbol, lc::kSNSProfileResponsev2);
  const auto second = Split(session.FromGameService(OtherProfileSuccess(kOtherAccount + 1, {{"b", 2}})).toGameClient);
  ASSERT_EQ(second.size(), 1u);
  EXPECT_EQ(second[0].symbol, lc::kSNSRefreshProfileResult);
}

TEST(LegacySession, AProfileAnswerWithNothingOutstandingIsCountedAndNotForwarded) {
  ls::LegacySession session(MakeConfig());
  const ls::Step step = session.FromGameService(OtherProfileSuccess(kOtherAccount, {{"a", 1}}));
  EXPECT_EQ(step.toGameClient, "");
  EXPECT_EQ(session.GetStats().profileAnswersUnmatched, 1u);
}

TEST(LegacySession, AProfileFailureConsumesTheOutstandingRequestAndForwardsNothing) {
  ls::LegacySession session(MakeConfig());
  session.FromGameService(LoginSuccess());
  // The login's request fails; the next own-profile answer must not be attributed to the login.
  const ls::Step failed = session.FromGameService(
      Frame(nevr_evr_codec::kSymLoggedInUserProfileFailure, Bytes().User(kPlatform, kAccount).CStr("no").Str()));
  EXPECT_EQ(failed.toGameClient, "");
  EXPECT_EQ(session.GetStats().profileFailuresDropped, 1u);
  const ls::Step answer = session.FromGameService(LoggedInProfileSuccess(json::object(), json::object()));
  EXPECT_EQ(answer.toGameClient, "");
  EXPECT_EQ(session.GetStats().profileAnswersUnmatched, 1u);
}

TEST(LegacySession, ASessionSuccessLosesItsGroupOnTheWayToTheGameClient) {
  ls::LegacySession session(MakeConfig());
  session.FromGameClient(FindV8(GuidString(0x70)));

  const std::string v5 = SuccessV5(GuidString(0x71));
  const auto out = Split(session.FromGameService(v5).toGameClient);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].symbol, lc::kSNSLobbySessionSuccessv4);
  // The v4 form is the v5 form without the 16-byte group after the lobby.
  const std::string whole = Split(v5)[0].payload;
  EXPECT_EQ(out[0].payload, whole.substr(0, 24) + whole.substr(40));
}

TEST(LegacySession, ANewLegacyLoginClearsWhatWasOutstandingAndAsksAgainForTheSameSession) {
  ls::LegacySession session(MakeConfig());
  session.FromGameService(LoginSuccess(0x30));  // owes the login result
  session.FromGameClient(LegacyLogin());        // the game logs in again

  const ls::Step again = session.FromGameService(LoginSuccess(0x30));
  EXPECT_EQ(Split(again.toGameService).size(), 1u);
  EXPECT_EQ(session.GetStats().loginsInjected, 2u);

  // Exactly one answer is owed: the first becomes the login result, a second has nothing to attach to.
  const auto first =
      Split(session.FromGameService(LoggedInProfileSuccess(json::object(), json::object())).toGameClient);
  ASSERT_EQ(first.size(), 1u);
  EXPECT_EQ(first[0].symbol, lc::kSNSLoginProfileResult);
  EXPECT_EQ(session.FromGameService(LoggedInProfileSuccess(json::object(), json::object())).toGameClient, "");
  EXPECT_EQ(session.GetStats().profileAnswersUnmatched, 1u);
}

TEST(LegacySession, ALegacyFindCarriesTheLoginSessionTheGameServiceNamed) {
  ls::LegacySession session(MakeConfig());
  session.FromGameService(LoginSuccess(0x31));
  const auto sent = Split(session.FromGameClient(FindV8(GuidString(0x70))).toGameService);
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].payload.substr(32, 16), GuidString(0x31));  // after lock, mode, level, platform
}

TEST(LegacySession, LeaderboardIsAnsweredLocallyAndTelemetryIsDropped) {
  ls::LegacySession session(MakeConfig());
  const std::string frame = Frame(lc::kSNSLeaderboardRequest, Bytes().U64(0x1020304050607080ULL).Pad(24).Str()) +
                            Frame(nevr_evr_codec::kSymTelemetryEvent, "tele");
  const ls::Step step = session.FromGameClient(frame);
  EXPECT_EQ(step.toGameService, "");
  const auto out = Split(step.toGameClient);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].symbol, lc::kSNSLeaderboardResponse);
  EXPECT_EQ(U64At(out[0].payload, 0), 0x1020304050607080ULL);
  EXPECT_EQ(session.GetStats().answeredLocally, 1u);
  EXPECT_EQ(session.GetStats().dropped, 1u);
}

TEST(LegacySession, ALoginFailureIsAProfileResultAndEndsTheLogin) {
  ls::LegacySession session(MakeConfig());
  session.FromGameService(LoginSuccess());
  const ls::Step step = session.FromGameService(
      Frame(nevr_evr_codec::kSymLoginFailure, Bytes().User(kPlatform, kAccount).U64(401).CStr("denied").Str()));
  const auto out = Split(step.toGameClient);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].symbol, lc::kSNSLoginProfileResult);
  EXPECT_EQ(static_cast<uint8_t>(out[0].payload.at(0x24)), lc::kLoginCredentialsRejected);

  // The next login session is a fresh login: it is asked for again.
  const ls::Step next = session.FromGameService(LoginSuccess(0x30));
  EXPECT_EQ(Split(next.toGameService).size(), 1u);
}

TEST(LegacySession, UnsupportedAndMalformedMessagesAreCountedAndNotForwarded) {
  ls::LegacySession session(MakeConfig());
  const std::string truncatedFind = Frame(lc::kSNSLobbyFindSessionRequestv8, std::string(20, '\0'));
  const std::string unknown = Frame(0x1234567812345678ULL, "x");
  const ls::Step step = session.FromGameClient(truncatedFind + unknown);
  EXPECT_EQ(step.toGameService, "");
  EXPECT_EQ(step.toGameClient, "");
  EXPECT_EQ(session.GetStats().malformed, 1u);
  EXPECT_EQ(session.GetStats().unsupported, 1u);
}

TEST(LegacySession, MessagesKeepTheirOrderAcrossAFrame) {
  ls::LegacySession session(MakeConfig());
  const std::string frame = Frame(nevr_evr_codec::kSymConfigRequest, "first") +
                            Frame(nevr_evr_codec::kSymTelemetryEvent, "gone") + FindV8(GuidString(0x70)) +
                            Frame(nevr_evr_codec::kSymConfigRequest, "last");
  const auto out = Split(session.FromGameClient(frame).toGameService);
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[0].payload, "first");
  EXPECT_EQ(out[1].symbol, nevr_evr_codec::kSymFindSessionRequest);
  EXPECT_EQ(out[2].payload, "last");
}
