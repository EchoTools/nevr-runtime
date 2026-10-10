#pragma once
// A LoginRequest reader for tests only: the production code never parses a LoginRequest (it only builds
// one), so the reader is not part of evr_codec. It decodes bytes itself and shares no code with the
// codec, so a round trip through it is an independent check of the builder's layout. Used by
// test_evr_codec.cpp and src/quest/tests/evr_codec_test.cpp.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace EvrCodecTest {

enum class LoginRequestStatus {
  Ok,
  NotAFrame,         // shorter than a header, or the marker is wrong
  WrongSymbol,       // the first message is not a LoginRequest
  LengthMismatch,    // the declared payload length is not the rest of the frame
  PayloadTooShort,   // no room for UUID, platform, account and the NUL
  NotNulTerminated,  // the payload does not end in NUL
  EmbeddedNul,       // a NUL inside the profile JSON
};

struct LoginRequestFields {
  uint64_t platformCode = 0;
  uint64_t accountId = 0;
  std::string profileJson;  // without the terminating NUL
};

inline uint64_t ReadU64(const std::string& bytes, std::size_t offset) {
  uint64_t value = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    value |= static_cast<uint64_t>(static_cast<unsigned char>(bytes[offset + i])) << (8 * i);
  }
  return value;
}

// Reads exactly one LoginRequest message that spans the whole frame.
inline LoginRequestStatus ParseLoginRequest(const std::string& frame, LoginRequestFields* out) {
  static const char kMarker[8] = {'\xf6', '\x40', '\xbb', '\x78', '\xa2', '\xe7', '\x8c', '\xbb'};
  if (frame.size() < 24 || std::memcmp(frame.data(), kMarker, 8) != 0) return LoginRequestStatus::NotAFrame;
  if (ReadU64(frame, 8) != 0xbdb41ea9e67b200aULL) return LoginRequestStatus::WrongSymbol;
  if (ReadU64(frame, 16) != frame.size() - 24) return LoginRequestStatus::LengthMismatch;
  const std::size_t payloadSize = frame.size() - 24;
  if (payloadSize < 16 + 8 + 8 + 1) return LoginRequestStatus::PayloadTooShort;
  if (frame.back() != '\0') return LoginRequestStatus::NotNulTerminated;
  const std::string json = frame.substr(24 + 32, payloadSize - 32 - 1);
  if (json.find('\0') != std::string::npos) return LoginRequestStatus::EmbeddedNul;
  out->platformCode = ReadU64(frame, 24 + 16);
  out->accountId = ReadU64(frame, 24 + 24);
  out->profileJson = json;
  return LoginRequestStatus::Ok;
}

}  // namespace EvrCodecTest
