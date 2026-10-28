#include "runtime/server/lobby_settings_relay.h"

#include <zlib.h>

#include <cstdarg>
#include <cstdio>
#include <exception>
#include <nlohmann/json.hpp>
#include <utility>

namespace nevr_game_server {

namespace {

constexpr size_t kLengthFieldBytes = 8;
constexpr uint64_t kMaxPackedBytes = 0xFFFFFFFFULL;  // zlib's uLong is 32 bits on the Windows targets
constexpr size_t kSessionIdLogChars = 64;

// A session id is remote text. Named in the log as an identifier, never longer than a UUID plus slack.
std::string SessionForLog(const std::string& sessionId) {
  if (sessionId.empty()) return "none";
  return sessionId.substr(0, kSessionIdLogChars);
}

std::string Format(const char* format, ...) __attribute__((format(printf, 1, 2)));

std::string Format(const char* format, ...) {
  char buffer[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  return buffer;
}

LobbySettingsReport Dropped(LobbySettingsReport report, LobbySettingsDrop drop, int zlibResult,
                            const std::string& sessionId) {
  report.relayed = false;
  report.drop = drop;
  report.level = LobbySettingsLogLevel::kWarning;
  report.line = Format(
      "[NEVR.GAMESERVER] lobby settings dropped reason=%s zlib_rc=%d session_id=%s wire_bytes=%llu - clients get no "
      "lobby settings from this frame",
      LobbySettingsDropName(drop), zlibResult, SessionForLog(sessionId).c_str(),
      static_cast<unsigned long long>(report.wireBytes));
  return report;
}

}  // namespace

const char* LobbySettingsDropName(LobbySettingsDrop drop) {
  switch (drop) {
    case LobbySettingsDrop::kNone: return "none";
    case LobbySettingsDrop::kTooShort: return "too_short";
    case LobbySettingsDrop::kBadLength: return "bad_length";
    case LobbySettingsDrop::kInflateFailed: return "inflate_failed";
    case LobbySettingsDrop::kNotJson: return "not_json";
    case LobbySettingsDrop::kNotObject: return "not_object";
    case LobbySettingsDrop::kNoBroadcaster: return "no_broadcaster";
    case LobbySettingsDrop::kException: return "exception";
  }
  return "unknown";
}

LobbySettingsDecoded DecodeLobbySettings(const void* data, uint64_t size) {
  LobbySettingsDecoded decoded;
  if (data == nullptr || size < kLengthFieldBytes) {
    decoded.drop = LobbySettingsDrop::kTooShort;
    return decoded;
  }
  const auto* bytes = static_cast<const uint8_t*>(data);

  uint64_t declared = 0;
  for (size_t i = 0; i < kLengthFieldBytes; ++i) declared |= static_cast<uint64_t>(bytes[i]) << (8U * i);
  const uint64_t packedBytes = size - kLengthFieldBytes;
  if (declared == 0 || declared > kLobbySettingsMaxJsonBytes || packedBytes > kMaxPackedBytes) {
    decoded.drop = LobbySettingsDrop::kBadLength;
    return decoded;
  }

  std::string json(static_cast<size_t>(declared), '\0');
  uLongf inflatedBytes = static_cast<uLongf>(declared);
  const int rc = uncompress(reinterpret_cast<Bytef*>(json.data()), &inflatedBytes, bytes + kLengthFieldBytes,
                            static_cast<uLong>(packedBytes));
  if (rc != Z_OK) {
    decoded.drop = LobbySettingsDrop::kInflateFailed;
    decoded.zlibResult = rc;
    return decoded;
  }
  // The retail handler hands the declared length to Decompress and parses what came out (0x14017dee0).
  json.resize(static_cast<size_t>(inflatedBytes));
  // A NUL-terminated document (nakama's StreamJson default) is accepted; the local event carries plain JSON.
  while (!json.empty() && json.back() == '\0') json.pop_back();
  // nlohmann stops reading at a NUL, so a document with one inside would pass the parse below and reach the game
  // whole; an embedded NUL is refused here instead.
  if (json.find('\0') != std::string::npos) {
    decoded.drop = LobbySettingsDrop::kNotJson;
    return decoded;
  }

  const nlohmann::json document = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
  if (document.is_discarded()) {
    decoded.drop = LobbySettingsDrop::kNotJson;
    return decoded;
  }
  if (!document.is_object()) {
    decoded.drop = LobbySettingsDrop::kNotObject;
    return decoded;
  }

  decoded.drop = LobbySettingsDrop::kNone;
  decoded.json = std::move(json);
  return decoded;
}

std::vector<uint8_t> BuildLobbySettingsLocalEvent(const std::string& json) {
  std::vector<uint8_t> message(kLobbySettingsItemBytes, 0);
  message.insert(message.end(), json.begin(), json.end());
  return message;
}

LobbySettingsReport RelayLobbySettings(const void* data, uint64_t size, const std::string& sessionId,
                                       const LobbySettingsSinks& sinks) {
  LobbySettingsReport report;
  report.wireBytes = size;
  try {
    LobbySettingsDecoded decoded = DecodeLobbySettings(data, size);
    if (decoded.drop != LobbySettingsDrop::kNone) {
      return Dropped(report, decoded.drop, decoded.zlibResult, sessionId);
    }
    report.jsonBytes = decoded.json.size();

    if (!sinks.raiseLocalEvent || !sinks.sendToPeer) {
      return Dropped(report, LobbySettingsDrop::kNoBroadcaster, 0, sessionId);
    }

    // The local handler (0x14017de60) parses and stores the document at NetGame+0x63260; the send below
    // carries the same document to the clients already connected.
    std::vector<uint8_t> message = BuildLobbySettingsLocalEvent(decoded.json);
    if (!sinks.raiseLocalEvent(message.data(), message.size())) {
      return Dropped(report, LobbySettingsDrop::kNoBroadcaster, 0, sessionId);
    }

    uint8_t zeroItem[kLobbySettingsItemBytes] = {};
    report.sendResult = sinks.sendToPeer(zeroItem, sizeof(zeroItem), decoded.json.data(), decoded.json.size(),
                                         kLobbySettingsAllPeers);
  } catch (const std::exception&) {
    return Dropped(report, LobbySettingsDrop::kException, 0, sessionId);
  }

  report.relayed = true;
  report.drop = LobbySettingsDrop::kNone;
  report.level = LobbySettingsLogLevel::kInfo;
  report.line = Format(
      "[NEVR.GAMESERVER] lobby settings relayed to the game and all connected peers session_id=%s wire_bytes=%llu "
      "json_bytes=%llu send_rc=%d",
      SessionForLog(sessionId).c_str(), static_cast<unsigned long long>(report.wireBytes),
      static_cast<unsigned long long>(report.jsonBytes), static_cast<int>(report.sendResult));
  return report;
}

std::string FormatLobbySettingsCleared(const LobbySettingsRecord& record, const char* reason,
                                       const std::string& sessionId) {
  return Format(
      "[NEVR.GAMESERVER] lobby settings record cleared reason=%s session_id=%s had_settings=%d json_bytes=%llu "
      "settings_session_id=%s",
      reason, SessionForLog(sessionId).c_str(), record.present ? 1 : 0,
      static_cast<unsigned long long>(record.jsonBytes), SessionForLog(record.sessionId).c_str());
}

}  // namespace nevr_game_server
