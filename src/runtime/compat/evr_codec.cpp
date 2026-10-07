#include "runtime/compat/evr_codec.h"

#include <cstring>
#include <limits>

namespace EvrCodec {

namespace {

constexpr std::size_t kLoginFailureFixedPayloadSize = 24;

uint64_t ReadLE64(const uint8_t* p) {
  uint64_t value = 0;
  for (int i = 7; i >= 0; --i) value = (value << 8) | p[i];
  return value;
}

}  // namespace

void AppendLE64(std::string& buffer, uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    buffer.push_back(static_cast<char>(value & 0xFF));
    value >>= 8;
  }
}

std::string BuildMessage(uint64_t symbol, std::string_view payload) {
  std::string message;
  message.reserve(kHeaderSize + payload.size());
  message.append(reinterpret_cast<const char*>(kMarker), kMarkerSize);
  AppendLE64(message, symbol);
  AppendLE64(message, payload.size());
  message.append(payload);
  return message;
}

std::optional<std::string> BuildLoginRequest(uint64_t platformCode, uint64_t accountId,
                                             std::string_view profileJson) {
  if (profileJson.find('\0') != std::string_view::npos) return std::nullopt;

  std::string payload;
  payload.reserve(kLoginRequestFixedSize + profileJson.size() + 1);
  payload.append(kUuidSize, '\0');  // no previous session
  AppendLE64(payload, platformCode);
  AppendLE64(payload, accountId);
  payload.append(profileJson);
  payload.push_back('\0');
  return BuildMessage(kSymLoginRequest, payload);
}

std::string BuildLoginSuccess(uint64_t platformCode, uint64_t accountId) {
  std::string payload("NEVRSRVR", 8);
  payload.append(8, '\0');
  AppendLE64(payload, platformCode);
  AppendLE64(payload, accountId);
  return BuildMessage(kSymLoginSuccess, payload);
}

std::string BuildFriendListSubscribe() {
  return BuildMessage(kSymFriendListSubscribe, std::string(kFriendListSubscribePayloadSize, '\0'));
}

ReadStatus ReadMessage(const uint8_t* data, std::size_t remaining, Message* out) {
  if (remaining < kHeaderSize) return ReadStatus::End;
  if (std::memcmp(data, kMarker, kMarkerSize) != 0) return ReadStatus::BadMarker;
  out->symbol = ReadLE64(data + 8);
  out->length = ReadLE64(data + 16);
  if (out->length > remaining - kHeaderSize) return ReadStatus::Truncated;
  out->payload = data + kHeaderSize;
  return ReadStatus::Ok;
}

ReadStatus ReadMessage(const std::string& frame, std::size_t offset, Message* out) {
  if (offset > frame.size()) return ReadStatus::End;
  return ReadMessage(reinterpret_cast<const uint8_t*>(frame.data()) + offset, frame.size() - offset, out);
}

uint64_t FirstSymbol(const std::string& frame) {
  if (frame.size() < kHeaderSize) return 0;
  return ReadLE64(reinterpret_cast<const uint8_t*>(frame.data()) + 8);
}

const char* LoginRequestStatusName(LoginRequestStatus status) {
  switch (status) {
    case LoginRequestStatus::Ok: return "ok";
    case LoginRequestStatus::NotAFrame: return "not-a-frame";
    case LoginRequestStatus::WrongSymbol: return "wrong-symbol";
    case LoginRequestStatus::LengthMismatch: return "length-mismatch";
    case LoginRequestStatus::PayloadTooShort: return "payload-too-short";
    case LoginRequestStatus::NotNulTerminated: return "not-nul-terminated";
    case LoginRequestStatus::EmbeddedNul: return "embedded-nul";
  }
  return "unknown";
}

LoginRequestStatus ParseLoginRequest(const std::string& frame, LoginRequestFields* out) {
  if (frame.size() < kHeaderSize ||
      std::memcmp(frame.data(), kMarker, kMarkerSize) != 0) {
    return LoginRequestStatus::NotAFrame;
  }
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(frame.data());
  if (ReadLE64(bytes + 8) != kSymLoginRequest) return LoginRequestStatus::WrongSymbol;
  const uint64_t declared = ReadLE64(bytes + 16);
  if (declared != frame.size() - kHeaderSize) return LoginRequestStatus::LengthMismatch;
  const std::size_t payloadSize = frame.size() - kHeaderSize;
  if (payloadSize < kLoginRequestFixedSize + 1) return LoginRequestStatus::PayloadTooShort;
  const uint8_t* payload = bytes + kHeaderSize;
  if (payload[payloadSize - 1] != 0) return LoginRequestStatus::NotNulTerminated;
  const std::size_t jsonSize = payloadSize - kLoginRequestFixedSize - 1;
  const char* json = reinterpret_cast<const char*>(payload) + kLoginRequestFixedSize;
  if (std::memchr(json, '\0', jsonSize) != nullptr) return LoginRequestStatus::EmbeddedNul;
  out->platformCode = ReadLE64(payload + kUuidSize);
  out->accountId = ReadLE64(payload + kUuidSize + 8);
  out->profileJson.assign(json, jsonSize);
  return LoginRequestStatus::Ok;
}

std::optional<LoginFailure> ParseLoginFailure(const std::string& frame) {
  if (frame.size() < kHeaderSize) return std::nullopt;
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(frame.data());
  const uint64_t symbol = ReadLE64(bytes + 8);
  const uint64_t payloadLength = ReadLE64(bytes + 16);
  if (symbol != kSymLoginFailure ||
      payloadLength <= static_cast<uint64_t>(kLoginFailureFixedPayloadSize) ||
      payloadLength > static_cast<uint64_t>(std::numeric_limits<std::size_t>::max() - kHeaderSize)) {
    return std::nullopt;
  }
  const std::size_t payloadSize = static_cast<std::size_t>(payloadLength);
  if (payloadSize > frame.size() - kHeaderSize) return std::nullopt;
  return LoginFailure{ReadLE64(bytes + kHeaderSize + 16), payloadSize - kLoginFailureFixedPayloadSize};
}

const char* PlatformPrefix(uint64_t platformCode) {
  switch (platformCode) {
    case 1: return "STM";
    case 2: return "DSC";
    case 3: return "XBX";
    case 4: return "OVR-ORG";
    case 5: return "OVR";
    case 6: return "BOT";
    case 7: return "DMO";
    default: return "UNK";
  }
}

uint64_t SelectPlatformCode(bool /*hasUrlCredentials*/, bool /*noOvr*/) { return kBridgeLoginPlatform; }

std::string SelectRemoteBearer(bool hasUrlCredentials, const std::string& jwt, const std::string& serverKey) {
  return hasUrlCredentials ? serverKey : jwt;
}

bool IsBearerReplacingPath(const std::string& url) {
  const std::size_t scheme = url.find("://");
  const std::size_t hostStart = scheme == std::string::npos ? 0 : scheme + 3;
  const std::size_t pathStart = url.find('/', hostStart);
  if (pathStart == std::string::npos) return false;
  const std::size_t pathEnd = url.find_first_of("?#", pathStart);
  return url.compare(pathStart, pathEnd == std::string::npos ? std::string::npos : pathEnd - pathStart, "/ws") == 0;
}

}  // namespace EvrCodec
