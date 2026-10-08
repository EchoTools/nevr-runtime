// Runs the shared EVR codec (src/runtime/compat/evr_codec.cpp) on the Quest toolchain and on the host
// (just test-quest-shared). Plain main so it needs no GTest on Android; the full vector set is
// src/runtime/tests/test_evr_codec.cpp. Every builder's output is read back by an independent reader
// and the login profile is parsed with nlohmann::json.

#include "runtime/compat/evr_codec.h"
#include "runtime/compat/login_profile.h"
#include "runtime/tests/evr_codec_test_reader.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace {

int g_failures = 0;

void Expect(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "evr_codec_test FAILED: %s\n", what);
    ++g_failures;
  }
}

uint64_t U64At(const std::string& bytes, std::size_t offset) {
  uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    value |= static_cast<uint64_t>(static_cast<unsigned char>(bytes.at(offset + i))) << (8 * i);
  }
  return value;
}

}  // namespace

int main() {
  LoginProfile::LoginProfileInputs inputs;
  inputs.account_id = 4242;
  inputs.display_name = "Quest \"Player\"";
  const std::string json = LoginProfile::BuildLoginProfileJson(inputs);

  const std::optional<std::string> frame =
      EvrCodec::BuildLoginRequest(EvrCodec::kBridgeLoginPlatform, inputs.account_id, json);
  Expect(frame.has_value(), "login request builds");
  if (!frame.has_value()) return 1;

  Expect(frame->size() == 24 + 32 + json.size() + 1, "login request size");
  Expect(frame->compare(0, 8, "\xf6\x40\xbb\x78\xa2\xe7\x8c\xbb", 8) == 0, "marker");
  Expect(U64At(*frame, 8) == 0xbdb41ea9e67b200aULL, "login request symbol");
  Expect(U64At(*frame, 16) == frame->size() - 24, "declared length");
  Expect(U64At(*frame, 40) == 4, "platform code is OVR_ORG (4)");
  Expect(U64At(*frame, 48) == 4242, "account id");
  Expect(frame->back() == '\0', "NUL terminated");
  try {
    Expect(nlohmann::json::parse(frame->substr(56, frame->size() - 57)).at("displayname") == "Quest \"Player\"",
           "profile JSON parses and keeps the quoted name");
  } catch (const nlohmann::json::exception& e) {
    std::fprintf(stderr, "evr_codec_test FAILED: profile JSON did not parse: %s\n", e.what());
    ++g_failures;
  }

  EvrCodecTest::LoginRequestFields fields;
  Expect(EvrCodecTest::ParseLoginRequest(*frame, &fields) == EvrCodecTest::LoginRequestStatus::Ok,
         "round trip status");
  Expect(fields.platformCode == 4 && fields.accountId == 4242 && fields.profileJson == json, "round trip fields");

  const std::string batched = EvrCodec::BuildMessage(1, "a") + EvrCodec::BuildFriendListSubscribe();
  EvrCodec::Message message;
  Expect(EvrCodec::ReadMessage(batched, 0, &message) == EvrCodec::ReadStatus::Ok && message.symbol == 1,
         "first batched message");
  Expect(EvrCodec::ReadMessage(batched, 25, &message) == EvrCodec::ReadStatus::Ok &&
             message.symbol == EvrCodec::kSymFriendListSubscribe && message.length == 0x20,
         "second batched message");
  Expect(EvrCodec::ReadMessage(batched, batched.size() - 1, &message) == EvrCodec::ReadStatus::End, "end of frame");
  Expect(EvrCodec::IsBearerReplacingPath("wss://host/ws?format=evr") &&
             !EvrCodec::IsBearerReplacingPath("wss://host/nevr?format=evr"),
         "bearer-replacing path");

  // Truncation: a declared payload longer than what remains, including a length that wraps to 0 when
  // the header size is added to it (2^64 - 24).
  std::string truncated = EvrCodec::BuildMessage(5, "12345678");
  truncated.pop_back();
  Expect(EvrCodec::ReadMessage(truncated, 0, &message) == EvrCodec::ReadStatus::Truncated, "truncated payload");
  std::string wrapping = EvrCodec::BuildMessage(5, "");
  const uint64_t wraps = UINT64_MAX - 23;
  for (std::size_t i = 0; i < 8; ++i) wrapping[16 + i] = static_cast<char>((wraps >> (8 * i)) & 0xff);
  Expect(EvrCodec::ReadMessage(wrapping, 0, &message) == EvrCodec::ReadStatus::Truncated,
         "a declared length that wraps with the header is Truncated");

  if (g_failures != 0) return 1;
  std::puts("evr_codec_test: all vectors pass");
  return 0;
}
