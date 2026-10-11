#include "runtime/compat/evr_codec.h"

#include <cstring>
#include <limits>

namespace nevr_evr_codec {

uint64_t ReadLE64(const uint8_t* p) {
  uint64_t value = 0;
  for (int i = 7; i >= 0; --i) value = (value << 8) | p[i];
  return value;
}

void AppendLE64(std::string& buffer, uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    buffer.push_back(static_cast<char>(value & 0xFF));
    value >>= 8;
  }
}

namespace {

constexpr std::size_t kLoginFailureFixedPayloadSize = 24;

ReadStatus ReadMessageAt(const uint8_t* data, std::size_t remaining, Message* out) {
  *out = Message{};
  if (remaining < kHeaderSize) return ReadStatus::End;
  if (std::memcmp(data, kMarker, kMarkerSize) != 0) return ReadStatus::BadMarker;
  out->symbol = ReadLE64(data + 8);
  out->length = ReadLE64(data + 16);
  if (out->length > remaining - kHeaderSize) return ReadStatus::Truncated;
  out->payload = data + kHeaderSize;
  return ReadStatus::Ok;
}

}  // namespace

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

std::string BuildLoginRemovedNotify(const UserId& user, uint8_t reason, std::string_view json) {
  std::string payload;
  payload.reserve(kLoginRemovedFixedSize + json.size());
  if (kLoginRemovedUserIdSwapped) {
    AppendLE64(payload, user.accountId);
    AppendLE64(payload, user.platformCode);
  } else {
    AppendLE64(payload, user.platformCode);
    AppendLE64(payload, user.accountId);
  }
  for (int i = 0; i < 4; ++i) payload.push_back(static_cast<char>((kLoginRemovedWord10 >> (8 * i)) & 0xff));
  payload.push_back(static_cast<char>(reason));
  payload.append(kLoginRemovedFixedSize - payload.size(), '\0');
  payload.append(json.data(), json.size());
  return BuildMessage(kSymLoginRemovedNotify, payload);
}

std::optional<UserId> ParseLoginSuccessUserId(const std::string& frame) {
  Message message;
  if (ReadMessage(frame, 0, &message) != ReadStatus::Ok || message.symbol != kSymLoginSuccess ||
      message.length < kUuidSize + 16) {
    return std::nullopt;
  }
  UserId id;
  id.platformCode = ReadLE64(message.payload + kUuidSize);
  id.accountId = ReadLE64(message.payload + kUuidSize + 8);
  return id;
}

std::string BuildRemoteLogSet(const UserId& user, uint64_t level, const std::vector<std::string>& logs) {
  std::string payload;
  AppendLE64(payload, user.platformCode);
  AppendLE64(payload, user.accountId);
  for (int i = 0; i < 4; ++i) AppendLE64(payload, 0);  // the session UUID and the 16 text bytes
  AppendLE64(payload, level);
  const auto appendLE32 = [&payload](uint32_t value) {
    for (int i = 0; i < 4; ++i) payload.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
  };
  appendLE32(static_cast<uint32_t>(logs.size()));
  uint32_t offset = 0;
  for (const std::string& log : logs) {
    appendLE32(offset);
    offset += static_cast<uint32_t>(log.size() + 1);
  }
  for (const std::string& log : logs) {
    payload.append(log);
    payload.push_back('\0');
  }
  return BuildMessage(kSymRemoteLogSet, payload);
}

std::string BuildFriendListSubscribe() {
  return BuildMessage(kSymFriendListSubscribe, std::string(kFriendListSubscribePayloadSize, '\0'));
}

ReadStatus ReadMessage(const std::string& frame, std::size_t offset, Message* out) {
  if (offset > frame.size()) {
    *out = Message{};
    return ReadStatus::End;
  }
  return ReadMessageAt(reinterpret_cast<const uint8_t*>(frame.data()) + offset, frame.size() - offset, out);
}

uint64_t FirstSymbol(const std::string& frame) {
  if (frame.size() < kHeaderSize) return 0;
  return ReadLE64(reinterpret_cast<const uint8_t*>(frame.data()) + 8);
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

}  // namespace nevr_evr_codec
