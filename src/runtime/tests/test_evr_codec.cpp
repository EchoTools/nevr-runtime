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

// Independent little-endian reader: deliberately not nevr_evr_codec::ReadLE64.
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
  const std::string message = nevr_evr_codec::BuildMessage(0x0102030405060708ULL, "ab");
  ASSERT_EQ(message.size(), 24u + 2u);
  EXPECT_EQ(message.substr(0, 8), kMarkerBytes);
  EXPECT_EQ(message.substr(8, 8), std::string("\x08\x07\x06\x05\x04\x03\x02\x01", 8));
  EXPECT_EQ(message.substr(16, 8), std::string("\x02\0\0\0\0\0\0\0", 8));
  EXPECT_EQ(message.substr(24), "ab");
}

TEST(EvrCodecBuild, LoginRequestLayoutAndProfileJsonRoundTrip) {
  nevr_login_profile::LoginProfileInputs inputs;
  inputs.account_id = 987654321ULL;
  inputs.display_name = "A \"quoted\" \\ name";
  inputs.access_token = "token";
  const std::string json = nevr_login_profile::BuildLoginProfileJson(inputs);

  const std::optional<std::string> frame = nevr_evr_codec::BuildLoginRequest(4, 987654321ULL, json);
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

  nevr_evr_codec_test::LoginRequestFields fields;
  ASSERT_EQ(nevr_evr_codec_test::ParseLoginRequest(*frame, &fields), nevr_evr_codec_test::LoginRequestStatus::Ok);
  EXPECT_EQ(fields.platformCode, 4u);
  EXPECT_EQ(fields.accountId, 987654321ULL);
  EXPECT_EQ(fields.profileJson, json);
}

TEST(EvrCodecBuild, LoginRequestRefusesAProfileWithAnEmbeddedNul) {
  const std::string json("{\"a\":\"x\0y\"}", 12);
  EXPECT_FALSE(nevr_evr_codec::BuildLoginRequest(4, 1, json).has_value());
}

TEST(EvrCodecBuild, LoginSuccessCarriesTheServerSessionPlatformAndAccount) {
  const std::string frame = nevr_evr_codec::BuildLoginSuccess(4, 777);
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
  const std::string frame = nevr_evr_codec::BuildFriendListSubscribe();
  ASSERT_EQ(frame.size(), 24u + 0x20u);
  EXPECT_EQ(U64At(frame, 8), 0xcdc02fd1dbee3aaaULL);
  EXPECT_EQ(U64At(frame, 16), 0x20u);
  EXPECT_EQ(frame.substr(24), std::string(0x20, '\0'));
}

TEST(EvrCodecParse, WalksEveryMessageOfABatchedFrame) {
  const std::string frame = nevr_evr_codec::BuildMessage(11, "abc") + nevr_evr_codec::BuildMessage(22, "") +
                            nevr_evr_codec::BuildMessage(33, "z");
  nevr_evr_codec::Message message;
  std::size_t offset = 0;
  const uint64_t expectedSymbols[] = {11, 22, 33};
  const uint64_t expectedLengths[] = {3, 0, 1};
  for (int i = 0; i < 3; ++i) {
    ASSERT_EQ(nevr_evr_codec::ReadMessage(frame, offset, &message), nevr_evr_codec::ReadStatus::Ok) << i;
    EXPECT_EQ(message.symbol, expectedSymbols[i]);
    EXPECT_EQ(message.length, expectedLengths[i]);
    offset += 24 + static_cast<std::size_t>(message.length);
  }
  EXPECT_EQ(offset, frame.size());
  EXPECT_EQ(nevr_evr_codec::ReadMessage(frame, offset, &message), nevr_evr_codec::ReadStatus::End);
}

TEST(EvrCodecParse, ReportsEndBadMarkerAndTruncation) {
  nevr_evr_codec::Message message;
  EXPECT_EQ(nevr_evr_codec::ReadMessage(std::string(23, '\0'), 0, &message), nevr_evr_codec::ReadStatus::End);
  EXPECT_EQ(nevr_evr_codec::ReadMessage(std::string(24, '\0'), 0, &message), nevr_evr_codec::ReadStatus::BadMarker);

  std::string truncated = nevr_evr_codec::BuildMessage(5, "12345678");
  truncated.pop_back();
  ASSERT_EQ(nevr_evr_codec::ReadMessage(truncated, 0, &message), nevr_evr_codec::ReadStatus::Truncated);
  EXPECT_EQ(message.symbol, 5u);
  EXPECT_EQ(message.length, 8u);
}

TEST(EvrCodecParse, ADeclaredLengthNearTheMaximumDoesNotOverflow) {
  std::string frame = nevr_evr_codec::BuildMessage(5, "");
  const uint64_t huge = std::numeric_limits<uint64_t>::max();
  for (std::size_t i = 0; i < 8; ++i) frame[16 + i] = static_cast<char>((huge >> (8 * i)) & 0xff);
  frame.append(100, 'x');
  nevr_evr_codec::Message message;
  EXPECT_EQ(nevr_evr_codec::ReadMessage(frame, 0, &message), nevr_evr_codec::ReadStatus::Truncated);
}

TEST(EvrCodecParse, ADeclaredLengthThatWrapsWithTheHeaderIsTruncatedNotOk) {
  // length + 24 wraps to 0 for this length; a check written as `length + kHeaderSize > remaining` reads it as Ok.
  std::string frame = nevr_evr_codec::BuildMessage(5, "");
  const uint64_t wraps = std::numeric_limits<uint64_t>::max() - 23;
  for (std::size_t i = 0; i < 8; ++i) frame[16 + i] = static_cast<char>((wraps >> (8 * i)) & 0xff);
  nevr_evr_codec::Message message;
  ASSERT_EQ(nevr_evr_codec::ReadMessage(frame, 0, &message), nevr_evr_codec::ReadStatus::Truncated);
  EXPECT_EQ(message.length, wraps);
}

TEST(EvrCodecParse, PayloadIsNullUnlessTheMessageIsWhole) {
  const std::string whole = nevr_evr_codec::BuildMessage(7, "abc");
  nevr_evr_codec::Message message;
  ASSERT_EQ(nevr_evr_codec::ReadMessage(whole, 0, &message), nevr_evr_codec::ReadStatus::Ok);
  ASSERT_NE(message.payload, nullptr);

  // The same Message object reused for a truncated, a bad-marker and an end read must not keep the
  // previous message's payload.
  std::string truncated = nevr_evr_codec::BuildMessage(8, "12345678");
  truncated.pop_back();
  ASSERT_EQ(nevr_evr_codec::ReadMessage(whole, 0, &message), nevr_evr_codec::ReadStatus::Ok);
  EXPECT_EQ(nevr_evr_codec::ReadMessage(truncated, 0, &message), nevr_evr_codec::ReadStatus::Truncated);
  EXPECT_EQ(message.payload, nullptr);
  ASSERT_EQ(nevr_evr_codec::ReadMessage(whole, 0, &message), nevr_evr_codec::ReadStatus::Ok);
  EXPECT_EQ(nevr_evr_codec::ReadMessage(std::string(24, '\0'), 0, &message), nevr_evr_codec::ReadStatus::BadMarker);
  EXPECT_EQ(message.payload, nullptr);
  ASSERT_EQ(nevr_evr_codec::ReadMessage(whole, 0, &message), nevr_evr_codec::ReadStatus::Ok);
  EXPECT_EQ(nevr_evr_codec::ReadMessage(whole, whole.size(), &message), nevr_evr_codec::ReadStatus::End);
  EXPECT_EQ(message.payload, nullptr);
}

TEST(EvrCodecParse, FirstSymbolIsZeroOnAShortFrame) {
  EXPECT_EQ(nevr_evr_codec::FirstSymbol(nevr_evr_codec::BuildMessage(0xdeadbeefULL, "x")), 0xdeadbeefULL);
  EXPECT_EQ(nevr_evr_codec::FirstSymbol(std::string(23, 'x')), 0u);
}

TEST(EvrCodecParse, LoginRequestRejectsEveryMalformedShape) {
  nevr_evr_codec_test::LoginRequestFields fields;
  const std::string good = *nevr_evr_codec::BuildLoginRequest(4, 9, "{}");
  ASSERT_EQ(nevr_evr_codec_test::ParseLoginRequest(good, &fields), nevr_evr_codec_test::LoginRequestStatus::Ok);

  EXPECT_EQ(nevr_evr_codec_test::ParseLoginRequest(good.substr(0, 10), &fields), nevr_evr_codec_test::LoginRequestStatus::NotAFrame);
  std::string badMarker = good;
  badMarker[0] = '\0';
  EXPECT_EQ(nevr_evr_codec_test::ParseLoginRequest(badMarker, &fields), nevr_evr_codec_test::LoginRequestStatus::NotAFrame);

  EXPECT_EQ(nevr_evr_codec_test::ParseLoginRequest(nevr_evr_codec::BuildLoginSuccess(4, 9), &fields),
            nevr_evr_codec_test::LoginRequestStatus::WrongSymbol);

  EXPECT_EQ(nevr_evr_codec_test::ParseLoginRequest(good + "x", &fields), nevr_evr_codec_test::LoginRequestStatus::LengthMismatch);
  EXPECT_EQ(nevr_evr_codec_test::ParseLoginRequest(good.substr(0, good.size() - 1), &fields),
            nevr_evr_codec_test::LoginRequestStatus::LengthMismatch);

  EXPECT_EQ(nevr_evr_codec_test::ParseLoginRequest(nevr_evr_codec::BuildMessage(nevr_evr_codec::kSymLoginRequest, std::string(32, '\0')),
                                        &fields),
            nevr_evr_codec_test::LoginRequestStatus::PayloadTooShort);
  EXPECT_EQ(nevr_evr_codec_test::ParseLoginRequest(
                nevr_evr_codec::BuildMessage(nevr_evr_codec::kSymLoginRequest, std::string(32, '\0') + "{}"), &fields),
            nevr_evr_codec_test::LoginRequestStatus::NotNulTerminated);
  EXPECT_EQ(nevr_evr_codec_test::ParseLoginRequest(
                nevr_evr_codec::BuildMessage(nevr_evr_codec::kSymLoginRequest, std::string(32, '\0') + std::string("{\0}\0", 4)),
                &fields),
            nevr_evr_codec_test::LoginRequestStatus::EmbeddedNul);
}

TEST(EvrCodecParse, LoginFailureReadsTheStatusCodeAndMessageLengthOnly) {
  std::string payload(24, '\0');
  payload[16] = 0x90;  // status 400 = 0x190, little endian
  payload[17] = 0x01;
  payload += "bad credentials";
  const std::string frame = nevr_evr_codec::BuildMessage(nevr_evr_codec::kSymLoginFailure, payload);
  const std::optional<nevr_evr_codec::LoginFailure> failure = nevr_evr_codec::ParseLoginFailure(frame);
  ASSERT_TRUE(failure.has_value());
  EXPECT_EQ(failure->statusCode, 400u);
  EXPECT_EQ(failure->messageBytes, 15u);

  EXPECT_FALSE(nevr_evr_codec::ParseLoginFailure(nevr_evr_codec::BuildMessage(nevr_evr_codec::kSymLoginFailure, std::string(24, '\0')))
                   .has_value());
  EXPECT_FALSE(nevr_evr_codec::ParseLoginFailure(frame.substr(0, frame.size() - 1)).has_value());
  EXPECT_FALSE(nevr_evr_codec::ParseLoginFailure(nevr_evr_codec::BuildMessage(nevr_evr_codec::kSymLoginSuccess, payload)).has_value());
}

TEST(EvrCodecPolicy, BridgeLoginsAsOvrOrg) {
  static_assert(nevr_evr_codec::kBridgeLoginPlatform == 4, "OVR_ORG in the game's numbering");
  EXPECT_EQ(nevr_evr_codec::SelectPlatformCode(true, true), 4u);
  EXPECT_EQ(nevr_evr_codec::SelectPlatformCode(false, false), 4u);
  EXPECT_STREQ(nevr_evr_codec::PlatformPrefix(nevr_evr_codec::kBridgeLoginPlatform), "OVR-ORG");
  EXPECT_STREQ(nevr_evr_codec::PlatformPrefix(0), "UNK");
  EXPECT_STREQ(nevr_evr_codec::PlatformPrefix(8), "UNK");
}

TEST(EvrCodecPolicy, RemoteBearerIsTheServerKeyOnlyWithUrlCredentials) {
  EXPECT_EQ(nevr_evr_codec::SelectRemoteBearer(false, "jwt", "key"), "jwt");
  EXPECT_EQ(nevr_evr_codec::SelectRemoteBearer(true, "jwt", "key"), "key");
  EXPECT_EQ(nevr_evr_codec::SelectRemoteBearer(true, "jwt", ""), "");
}

TEST(EvrCodecPolicy, OnlyTheWsCatchAllPathReplacesTheBearer) {
  EXPECT_TRUE(nevr_evr_codec::IsBearerReplacingPath("wss://g.echovrce.com:443/ws"));
  EXPECT_TRUE(nevr_evr_codec::IsBearerReplacingPath("wss://g.echovrce.com/ws?format=evr"));
  EXPECT_TRUE(nevr_evr_codec::IsBearerReplacingPath("wss://g.echovrce.com/ws#frag"));
  EXPECT_FALSE(nevr_evr_codec::IsBearerReplacingPath("wss://g.echovrce.com/nevr?format=evr"));
  EXPECT_FALSE(nevr_evr_codec::IsBearerReplacingPath("wss://g.echovrce.com/wss"));
  EXPECT_FALSE(nevr_evr_codec::IsBearerReplacingPath("wss://g.echovrce.com/ws/x"));
  EXPECT_FALSE(nevr_evr_codec::IsBearerReplacingPath("wss://g.echovrce.com"));
}
