// Tests for the platform-neutral EVR codec (compat/evr_codec.{h,cpp}), the same source the Quest
// target compiles. Every builder is checked two ways: against golden bytes read by the small
// independent reader below (which shares no code with the codec), and by parsing its output back.

#include "runtime/compat/evr_codec.h"
#include "runtime/compat/login_profile.h"
#include "runtime/tests/evr_codec_test_reader.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <nlohmann/json.hpp>
#include <string>

namespace {

// Independent little-endian reader: deliberately not EvrCodec::ReadLE64.
uint64_t U64At(const std::string& bytes, std::size_t offset) {
  uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    value |= static_cast<uint64_t>(static_cast<unsigned char>(bytes.at(offset + i))) << (8 * i);
  }
  return value;
}

const std::string kMarkerBytes("\xf6\x40\xbb\x78\xa2\xe7\x8c\xbb", 8);

}  // namespace

TEST(EvrCodecBuild, MessageHasMarkerSymbolLengthPayload) {
  const std::string message = EvrCodec::BuildMessage(0x0102030405060708ULL, "ab");
  ASSERT_EQ(message.size(), 24u + 2u);
  EXPECT_EQ(message.substr(0, 8), kMarkerBytes);
  EXPECT_EQ(message.substr(8, 8), std::string("\x08\x07\x06\x05\x04\x03\x02\x01", 8));
  EXPECT_EQ(message.substr(16, 8), std::string("\x02\0\0\0\0\0\0\0", 8));
  EXPECT_EQ(message.substr(24), "ab");
}

TEST(EvrCodecBuild, LoginRequestLayoutAndProfileJsonRoundTrip) {
  LoginProfile::LoginProfileInputs inputs;
  inputs.account_id = 987654321ULL;
  inputs.display_name = "A \"quoted\" \\ name";
  inputs.access_token = "token";
  const std::string json = LoginProfile::BuildLoginProfileJson(inputs);

  const std::optional<std::string> frame = EvrCodec::BuildLoginRequest(4, 987654321ULL, json);
  ASSERT_TRUE(frame.has_value());

  // [marker(8)][symbol(8)][length(8)][UUID(16)][platform(8)][account(8)][JSON][NUL]
  EXPECT_EQ(frame->substr(0, 8), kMarkerBytes);
  EXPECT_EQ(U64At(*frame, 8), 0xbdb41ea9e67b200aULL);
  EXPECT_EQ(U64At(*frame, 16), frame->size() - 24);
  EXPECT_EQ(frame->substr(24, 16), std::string(16, '\0'));
  EXPECT_EQ(U64At(*frame, 40), 4u);
  EXPECT_EQ(U64At(*frame, 48), 987654321ULL);
  EXPECT_EQ(frame->back(), '\0');
  EXPECT_EQ(frame->substr(56, frame->size() - 56 - 1), json);

  const nlohmann::json parsed = nlohmann::json::parse(frame->substr(56, frame->size() - 56 - 1));
  EXPECT_EQ(parsed.at("accountid").get<uint64_t>(), 987654321ULL);
  EXPECT_EQ(parsed.at("displayname").get<std::string>(), "A \"quoted\" \\ name");

  EvrCodecTest::LoginRequestFields fields;
  ASSERT_EQ(EvrCodecTest::ParseLoginRequest(*frame, &fields), EvrCodecTest::LoginRequestStatus::Ok);
  EXPECT_EQ(fields.platformCode, 4u);
  EXPECT_EQ(fields.accountId, 987654321ULL);
  EXPECT_EQ(fields.profileJson, json);
}

TEST(EvrCodecBuild, LoginRequestRefusesAProfileWithAnEmbeddedNul) {
  const std::string json("{\"a\":\"x\0y\"}", 12);
  EXPECT_FALSE(EvrCodec::BuildLoginRequest(4, 1, json).has_value());
}

TEST(EvrCodecBuild, LoginSuccessCarriesTheServerSessionPlatformAndAccount) {
  const std::string frame = EvrCodec::BuildLoginSuccess(4, 777);
  ASSERT_EQ(frame.size(), 24u + 32u);
  EXPECT_EQ(frame.substr(0, 8), kMarkerBytes);
  EXPECT_EQ(U64At(frame, 8), 0xa5acc1a90d0cce47ULL);
  EXPECT_EQ(U64At(frame, 16), 32u);
  EXPECT_EQ(frame.substr(24, 8), "NEVRSRVR");
  EXPECT_EQ(frame.substr(32, 8), std::string(8, '\0'));
  EXPECT_EQ(U64At(frame, 40), 4u);
  EXPECT_EQ(U64At(frame, 48), 777u);
}

TEST(EvrCodecBuild, FriendListSubscribeIs0x20ZeroBytes) {
  const std::string frame = EvrCodec::BuildFriendListSubscribe();
  ASSERT_EQ(frame.size(), 24u + 0x20u);
  EXPECT_EQ(U64At(frame, 8), 0xcdc02fd1dbee3aaaULL);
  EXPECT_EQ(U64At(frame, 16), 0x20u);
  EXPECT_EQ(frame.substr(24), std::string(0x20, '\0'));
}

TEST(EvrCodecParse, WalksEveryMessageOfABatchedFrame) {
  const std::string frame = EvrCodec::BuildMessage(11, "abc") + EvrCodec::BuildMessage(22, "") +
                            EvrCodec::BuildMessage(33, "z");
  EvrCodec::Message message;
  std::size_t offset = 0;
  const uint64_t expectedSymbols[] = {11, 22, 33};
  const uint64_t expectedLengths[] = {3, 0, 1};
  for (int i = 0; i < 3; ++i) {
    ASSERT_EQ(EvrCodec::ReadMessage(frame, offset, &message), EvrCodec::ReadStatus::Ok) << i;
    EXPECT_EQ(message.symbol, expectedSymbols[i]);
    EXPECT_EQ(message.length, expectedLengths[i]);
    offset += 24 + static_cast<std::size_t>(message.length);
  }
  EXPECT_EQ(offset, frame.size());
  EXPECT_EQ(EvrCodec::ReadMessage(frame, offset, &message), EvrCodec::ReadStatus::End);
}

TEST(EvrCodecParse, ReportsEndBadMarkerAndTruncation) {
  EvrCodec::Message message;
  EXPECT_EQ(EvrCodec::ReadMessage(std::string(23, '\0'), 0, &message), EvrCodec::ReadStatus::End);
  EXPECT_EQ(EvrCodec::ReadMessage(std::string(24, '\0'), 0, &message), EvrCodec::ReadStatus::BadMarker);

  std::string truncated = EvrCodec::BuildMessage(5, "12345678");
  truncated.pop_back();
  ASSERT_EQ(EvrCodec::ReadMessage(truncated, 0, &message), EvrCodec::ReadStatus::Truncated);
  EXPECT_EQ(message.symbol, 5u);
  EXPECT_EQ(message.length, 8u);
}

TEST(EvrCodecParse, ADeclaredLengthNearTheMaximumDoesNotOverflow) {
  std::string frame = EvrCodec::BuildMessage(5, "");
  const uint64_t huge = std::numeric_limits<uint64_t>::max();
  for (std::size_t i = 0; i < 8; ++i) frame[16 + i] = static_cast<char>((huge >> (8 * i)) & 0xff);
  frame.append(100, 'x');
  EvrCodec::Message message;
  EXPECT_EQ(EvrCodec::ReadMessage(frame, 0, &message), EvrCodec::ReadStatus::Truncated);
}

TEST(EvrCodecParse, ADeclaredLengthThatWrapsWithTheHeaderIsTruncatedNotOk) {
  // length + 24 wraps to 0 for this length; a check written as `length + kHeaderSize > remaining` reads it as Ok.
  std::string frame = EvrCodec::BuildMessage(5, "");
  const uint64_t wraps = std::numeric_limits<uint64_t>::max() - 23;
  for (std::size_t i = 0; i < 8; ++i) frame[16 + i] = static_cast<char>((wraps >> (8 * i)) & 0xff);
  EvrCodec::Message message;
  ASSERT_EQ(EvrCodec::ReadMessage(frame, 0, &message), EvrCodec::ReadStatus::Truncated);
  EXPECT_EQ(message.length, wraps);
}

TEST(EvrCodecParse, FirstSymbolIsZeroOnAShortFrame) {
  EXPECT_EQ(EvrCodec::FirstSymbol(EvrCodec::BuildMessage(0xdeadbeefULL, "x")), 0xdeadbeefULL);
  EXPECT_EQ(EvrCodec::FirstSymbol(std::string(23, 'x')), 0u);
}

TEST(EvrCodecParse, LoginRequestRejectsEveryMalformedShape) {
  EvrCodecTest::LoginRequestFields fields;
  const std::string good = *EvrCodec::BuildLoginRequest(4, 9, "{}");
  ASSERT_EQ(EvrCodecTest::ParseLoginRequest(good, &fields), EvrCodecTest::LoginRequestStatus::Ok);

  EXPECT_EQ(EvrCodecTest::ParseLoginRequest(good.substr(0, 10), &fields), EvrCodecTest::LoginRequestStatus::NotAFrame);
  std::string badMarker = good;
  badMarker[0] = '\0';
  EXPECT_EQ(EvrCodecTest::ParseLoginRequest(badMarker, &fields), EvrCodecTest::LoginRequestStatus::NotAFrame);

  EXPECT_EQ(EvrCodecTest::ParseLoginRequest(EvrCodec::BuildLoginSuccess(4, 9), &fields),
            EvrCodecTest::LoginRequestStatus::WrongSymbol);

  EXPECT_EQ(EvrCodecTest::ParseLoginRequest(good + "x", &fields), EvrCodecTest::LoginRequestStatus::LengthMismatch);
  EXPECT_EQ(EvrCodecTest::ParseLoginRequest(good.substr(0, good.size() - 1), &fields),
            EvrCodecTest::LoginRequestStatus::LengthMismatch);

  EXPECT_EQ(EvrCodecTest::ParseLoginRequest(EvrCodec::BuildMessage(EvrCodec::kSymLoginRequest, std::string(32, '\0')),
                                        &fields),
            EvrCodecTest::LoginRequestStatus::PayloadTooShort);
  EXPECT_EQ(EvrCodecTest::ParseLoginRequest(
                EvrCodec::BuildMessage(EvrCodec::kSymLoginRequest, std::string(32, '\0') + "{}"), &fields),
            EvrCodecTest::LoginRequestStatus::NotNulTerminated);
  EXPECT_EQ(EvrCodecTest::ParseLoginRequest(
                EvrCodec::BuildMessage(EvrCodec::kSymLoginRequest, std::string(32, '\0') + std::string("{\0}\0", 4)),
                &fields),
            EvrCodecTest::LoginRequestStatus::EmbeddedNul);
}

TEST(EvrCodecParse, LoginFailureReadsTheStatusCodeAndMessageLengthOnly) {
  std::string payload(24, '\0');
  payload[16] = 0x90;  // status 400 = 0x190, little endian
  payload[17] = 0x01;
  payload += "bad credentials";
  const std::string frame = EvrCodec::BuildMessage(EvrCodec::kSymLoginFailure, payload);
  const std::optional<EvrCodec::LoginFailure> failure = EvrCodec::ParseLoginFailure(frame);
  ASSERT_TRUE(failure.has_value());
  EXPECT_EQ(failure->statusCode, 400u);
  EXPECT_EQ(failure->messageBytes, 15u);

  EXPECT_FALSE(EvrCodec::ParseLoginFailure(EvrCodec::BuildMessage(EvrCodec::kSymLoginFailure, std::string(24, '\0')))
                   .has_value());
  EXPECT_FALSE(EvrCodec::ParseLoginFailure(frame.substr(0, frame.size() - 1)).has_value());
  EXPECT_FALSE(EvrCodec::ParseLoginFailure(EvrCodec::BuildMessage(EvrCodec::kSymLoginSuccess, payload)).has_value());
}

TEST(EvrCodecPolicy, BridgeLoginsAsOvrOrg) {
  static_assert(EvrCodec::kBridgeLoginPlatform == 4, "OVR_ORG in the game's numbering");
  EXPECT_EQ(EvrCodec::SelectPlatformCode(true, true), 4u);
  EXPECT_EQ(EvrCodec::SelectPlatformCode(false, false), 4u);
  EXPECT_STREQ(EvrCodec::PlatformPrefix(EvrCodec::kBridgeLoginPlatform), "OVR-ORG");
  EXPECT_STREQ(EvrCodec::PlatformPrefix(0), "UNK");
  EXPECT_STREQ(EvrCodec::PlatformPrefix(8), "UNK");
}

TEST(EvrCodecPolicy, RemoteBearerIsTheServerKeyOnlyWithUrlCredentials) {
  EXPECT_EQ(EvrCodec::SelectRemoteBearer(false, "jwt", "key"), "jwt");
  EXPECT_EQ(EvrCodec::SelectRemoteBearer(true, "jwt", "key"), "key");
  EXPECT_EQ(EvrCodec::SelectRemoteBearer(true, "jwt", ""), "");
}

TEST(EvrCodecPolicy, OnlyTheWsCatchAllPathReplacesTheBearer) {
  EXPECT_TRUE(EvrCodec::IsBearerReplacingPath("wss://g.echovrce.com:443/ws"));
  EXPECT_TRUE(EvrCodec::IsBearerReplacingPath("wss://g.echovrce.com/ws?format=evr"));
  EXPECT_TRUE(EvrCodec::IsBearerReplacingPath("wss://g.echovrce.com/ws#frag"));
  EXPECT_FALSE(EvrCodec::IsBearerReplacingPath("wss://g.echovrce.com/nevr?format=evr"));
  EXPECT_FALSE(EvrCodec::IsBearerReplacingPath("wss://g.echovrce.com/wss"));
  EXPECT_FALSE(EvrCodec::IsBearerReplacingPath("wss://g.echovrce.com/ws/x"));
  EXPECT_FALSE(EvrCodec::IsBearerReplacingPath("wss://g.echovrce.com"));
}
