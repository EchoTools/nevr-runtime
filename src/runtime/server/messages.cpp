// Disable MSVC deprecation warnings for sscanf etc.
#define _CRT_SECURE_NO_WARNINGS

#include "runtime/server/messages.h"

#include <ws2tcpip.h>

#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdarg>
#include <cstdlib>
#include <cstring>

#include "abi/echovr_functions.h"
#include "core/remote_text.h"
#include "gameservice/v1/gameservice.pb.h"

// Logging wrapper for game's log system
// @param level - The log level (Info, Warning, Error, etc.)
// @param format - Printf-style format string
// @param ... - Variable arguments for format string
// Note: The second parameter to EchoVR::WriteLog (0) is an unknown flag/context value
// Note: EchoVR::WriteLog accepts va_list as its fourth parameter (verified in echovrInternal.h)
static void Log(EchoVR::LogLevel level, const CHAR* format, ...) {
  va_list args;
  va_start(args, format);
  EchoVR::WriteLog(level, 0, format, args);
  va_end(args);
}

// Maximum key sizes enforced on encoder settings received from ServerDB.
// These bounds are derived from the game engine's crypto layer:
//   - SSymCrypt::SetSecret (NRadCrypt.h:112) accepts only 16, 24, or 32 byte
//     AES keys. Any other encryptionKeySize will fail at EVP_CipherInit_ex.
//   - SHmacProvider::key_len (NRadCrypt.h:92) is uint32_t; OpenSSL HMAC_Init_ex
//     accepts keys up to EVP_MAX_MD_SIZE (64 for SHA-512). We allow 128 as a
//     generous ceiling that covers the SHA-512 block size.
//   - CRandom1600::Seed (CRandom1600.h:31) processes arbitrary length in 8-byte
//     words — 256 bytes is a generous bound.
//   - macDigestSize: the game uses SHA-256 (32) or SHA-512 (64) exclusively.
//     Values outside {32, 64} produce HMAC tags the game cannot verify.
// Evidence: Nakama server-side tests (match_session_success_test.go:23-27)
//   use 0x20 (32) for all three key sizes and 0x40 (64) for macDigestSize.
namespace {
constexpr int kMaxEncryptionKeySize = 32;
constexpr int kMaxMacKeySize = 128;
constexpr int kMaxRandomKeySize = 256;
constexpr int kValidEncryptionKeySizes[] = {16, 24, 32};
constexpr int kValidMacDigestSizes[] = {32, 64};
}  // namespace

// Validate encoder settings against the game's known crypto bounds.
// Returns true if all key sizes are within acceptable ranges.
// Logs a WARNING with the offending field and value on rejection.
static bool ValidatePacketEncoderSettings(const PacketEncoderSettings& settings, const char* which) {
  // Validate encryption key size: must be one of {16, 24, 32}
  bool encOk = false;
  for (int validSize : kValidEncryptionKeySizes) {
    if (settings.encryptionKeySize == validSize) {
      encOk = true;
      break;
    }
  }
  if (!encOk) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.GAMESERVER] rejected %s encoder settings: encryptionKeySize=%d "
        "(expected 16, 24, or 32) — lobby session success message NOT sent, match will not proceed",
        which, settings.encryptionKeySize);
    return false;
  }

  // Validate MAC key size: must be <= 128
  if (settings.macKeySize < 0 || settings.macKeySize > kMaxMacKeySize) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.GAMESERVER] rejected %s encoder settings: macKeySize=%d "
        "(max %d) — lobby session success message NOT sent, match will not proceed",
        which, settings.macKeySize, kMaxMacKeySize);
    return false;
  }

  // Validate random key size: must be <= 256
  if (settings.randomKeySize < 0 || settings.randomKeySize > kMaxRandomKeySize) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.GAMESERVER] rejected %s encoder settings: randomKeySize=%d "
        "(max %d) — lobby session success message NOT sent, match will not proceed",
        which, settings.randomKeySize, kMaxRandomKeySize);
    return false;
  }

  // Validate MAC digest size: must be one of {32, 64} (SHA-256 or SHA-512)
  bool digestOk = false;
  for (int validSize : kValidMacDigestSizes) {
    if (settings.macDigestSize == validSize) {
      digestOk = true;
      break;
    }
  }
  if (!digestOk) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.GAMESERVER] rejected %s encoder settings: macDigestSize=%d "
        "(expected 32 or 64) — lobby session success message NOT sent, match will not proceed",
        which, settings.macDigestSize);
    return false;
  }

  return true;
}

// Helper: Extract key sizes from encoder flags
// Format: bits 0-1: encryption/mac enabled
//         bits 2-13: macDigestSize
//         bits 14-25: macPBKDF2IterationCount
//         bits 26-37: macKeySize
//         bits 38-49: encryptionKeySize
//         bits 50-61: randomKeySize
PacketEncoderSettings PacketEncoderSettings::FromFlags(uint64_t flags) {
  PacketEncoderSettings settings;
  settings.encryptionEnabled = (flags & 1) != 0;
  settings.macEnabled = (flags & 2) != 0;
  settings.macDigestSize = static_cast<int>((flags >> 2) & 0xFFF);
  settings.macPBKDF2IterationCount = static_cast<int>((flags >> 14) & 0xFFF);
  settings.macKeySize = static_cast<int>((flags >> 26) & 0xFFF);
  settings.encryptionKeySize = static_cast<int>((flags >> 38) & 0xFFF);
  settings.randomKeySize = static_cast<int>((flags >> 50) & 0xFFF);
  return settings;
}

// Helper: Parse UUID string "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" to GUID
bool ParseUuidToGuid(const std::string& uuidStr, GUID& outGuid) {
  if (uuidStr.length() != 36) return false;

  // UUID format positions: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
  //                        8       13   18   23
  constexpr size_t UUID_HYPHEN_POS_1 = 8;
  constexpr size_t UUID_HYPHEN_POS_2 = 13;
  constexpr size_t UUID_HYPHEN_POS_3 = 18;
  constexpr size_t UUID_HYPHEN_POS_4 = 23;

  // Validate that the string contains only valid hexadecimal characters and hyphens in the correct positions
  for (size_t i = 0; i < 36; i++) {
    char c = uuidStr[i];
    if (i == UUID_HYPHEN_POS_1 || i == UUID_HYPHEN_POS_2 || i == UUID_HYPHEN_POS_3 || i == UUID_HYPHEN_POS_4) {
      // These positions must be hyphens
      if (c != '-') return false;
    } else {
      // All other positions must be hexadecimal digits
      // Cast to unsigned char to avoid undefined behavior with negative char values
      if (!std::isxdigit(static_cast<unsigned char>(c))) {
        return false;
      }
    }
  }

  unsigned int data1;
  unsigned int data2, data3;
  unsigned int data4[8];

  int result = sscanf(uuidStr.c_str(), "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", &data1, &data2, &data3,
                      &data4[0], &data4[1], &data4[2], &data4[3], &data4[4], &data4[5], &data4[6], &data4[7]);

  if (result != 11) return false;

  outGuid.Data1 = data1;
  outGuid.Data2 = static_cast<uint16_t>(data2);
  outGuid.Data3 = static_cast<uint16_t>(data3);
  for (int i = 0; i < 8; i++) {
    outGuid.Data4[i] = static_cast<uint8_t>(data4[i]);
  }
  return true;
}

namespace {

enum class AddrKind { Ipv4, Ipv6, Invalid };

// Classifies one address field. The text is the field as sent; a v6 literal may be
// bracketed. Only AF_INET parses are accepted by the wire format (4-byte fields).
AddrKind ClassifyAddress(const std::string& text, struct in_addr* v4) {
  struct in_addr probe4;
  if (inet_pton(AF_INET, text.c_str(), v4 ? v4 : &probe4) == 1) return AddrKind::Ipv4;
  std::string bare = text;
  if (bare.size() >= 2 && bare.front() == '[' && bare.back() == ']') bare = bare.substr(1, bare.size() - 2);
  struct in6_addr probe6;
  if (inet_pton(AF_INET6, bare.c_str(), &probe6) == 1) return AddrKind::Ipv6;
  return AddrKind::Invalid;
}

bool RefuseEndpoint(const std::string& endpointStr, const char* field, const char* reason, const char* family,
                    std::string* reasonOut) {
  if (reasonOut) *reasonOut = reason;
  const std::string shown = nevr_remote_text::Escape(endpointStr);
  Log(EchoVR::LogLevel::Warning,
      "[NEVR.GAMESERVER] ParseEndpoint refused field=%s reason=%s family=%s endpoint=%s endpoint_len=%zu"
      " — lobby session success message NOT sent",
      field, reason, family, shown.c_str(), endpointStr.size());
  return false;
}

// Splits "internal:external" (the part before the port). A v4 pair splits at the first colon as it always
// did; a bracketed side ends at its ']'. An unbracketed v6 pair has no unambiguous split, so every colon is
// tried and the endpoint counts as v6 when some split gives two addresses, one of them v6.
bool SplitAddressPair(const std::string& head, std::string& internalStr, std::string& externalStr,
                      bool& unbracketedV6) {
  unbracketedV6 = false;
  if (!head.empty() && head.front() == '[') {
    size_t close = head.find(']');
    if (close == std::string::npos || close + 1 >= head.size() || head[close + 1] != ':') return false;
    internalStr = head.substr(0, close + 1);
    externalStr = head.substr(close + 2);
    return true;
  }
  size_t first = head.find(':');
  if (first == std::string::npos) return false;
  internalStr = head.substr(0, first);
  externalStr = head.substr(first + 1);
  if (!externalStr.empty() && externalStr.front() == '[') return true;
  if (ClassifyAddress(internalStr, nullptr) == AddrKind::Ipv4 && externalStr.find(':') == std::string::npos) {
    return true;
  }
  for (size_t i = head.find(':'); i != std::string::npos; i = head.find(':', i + 1)) {
    const AddrKind left = ClassifyAddress(head.substr(0, i), nullptr);
    const AddrKind right = ClassifyAddress(head.substr(i + 1), nullptr);
    if (left != AddrKind::Invalid && right != AddrKind::Invalid &&
        (left == AddrKind::Ipv6 || right == AddrKind::Ipv6)) {
      unbracketedV6 = true;
      return true;
    }
  }
  return true;
}

}  // namespace

// An address as a log value: a text that parses as IPv4 or IPv6 (bracketed or not) is the real value, unquoted;
// anything else is remote text and goes through nevr_remote_text::Escape (Rule 4). *family, when given, is
// set to ipv4, ipv6 or unknown.
std::string AddressForLog(const std::string& text, const char** family) {
  const AddrKind kind = ClassifyAddress(text, nullptr);
  if (family) *family = kind == AddrKind::Ipv4 ? "ipv4" : kind == AddrKind::Ipv6 ? "ipv6" : "unknown";
  return kind == AddrKind::Invalid ? nevr_remote_text::Escape(text) : text;
}

// Helper: Parse endpoint string "internalIP:externalIP:port" to components.
// A refusal logs a Warning with the field, the reason (not_ipv4, bad_address, bad_port), the family and the
// endpoint as received, and puts the reason token in *reason when given.
bool ParseEndpoint(const std::string& endpointStr, uint32_t& internalIP, uint32_t& externalIP, uint16_t& port,
                   std::string* reason) {
  size_t firstColon = endpointStr.find(':');
  size_t lastColon = endpointStr.rfind(':');

  if (firstColon == std::string::npos || lastColon == std::string::npos || firstColon == lastColon) {
    return RefuseEndpoint(endpointStr, "endpoint", "bad_address", "unknown", reason);
  }

  std::string portStr = endpointStr.substr(lastColon + 1);
  std::string internalStr, externalStr;
  bool unbracketedV6 = false;
  if (!SplitAddressPair(endpointStr.substr(0, lastColon), internalStr, externalStr, unbracketedV6)) {
    return RefuseEndpoint(endpointStr, "endpoint", "bad_address", "unknown", reason);
  }
  if (unbracketedV6) return RefuseEndpoint(endpointStr, "endpoint", "not_ipv4", "ipv6", reason);

  // Parse IPs using inet_pton
  struct in_addr inAddr, exAddr;
  const AddrKind internalKind = ClassifyAddress(internalStr, &inAddr);
  if (internalKind != AddrKind::Ipv4) {
    return RefuseEndpoint(endpointStr, "internal", internalKind == AddrKind::Ipv6 ? "not_ipv4" : "bad_address",
                          internalKind == AddrKind::Ipv6 ? "ipv6" : "unknown", reason);
  }
  const AddrKind externalKind = ClassifyAddress(externalStr, &exAddr);
  if (externalKind != AddrKind::Ipv4) {
    return RefuseEndpoint(endpointStr, "external", externalKind == AddrKind::Ipv6 ? "not_ipv4" : "bad_address",
                          externalKind == AddrKind::Ipv6 ? "ipv6" : "unknown", reason);
  }

  internalIP = inAddr.s_addr;  // Already in network byte order, but we want host order for LE storage
  externalIP = exAddr.s_addr;

  // Parse port with errno checking (defense against N1 — std::stoul can throw)
  errno = 0;
  char* endptr = nullptr;
  unsigned long portVal = strtoul(portStr.c_str(), &endptr, 10);
  if (errno != 0 || endptr == portStr.c_str() || *endptr != '\0') {
    if (reason) *reason = "bad_port";
    const std::string shownPort = nevr_remote_text::Escape(portStr);
    const std::string shown = nevr_remote_text::Escape(endpointStr);
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.GAMESERVER] ParseEndpoint port parse failed field=port reason=bad_port family=ipv4 port_str=%s port_len=%zu"
        " endpoint=%s endpoint_len=%zu — lobby session success message NOT sent",
        shownPort.c_str(), portStr.size(), shown.c_str(), endpointStr.size());
    return false;
  }
  if (portVal > UINT16_MAX) {
    if (reason) *reason = "bad_port";
    const std::string shown = nevr_remote_text::Escape(endpointStr);
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.GAMESERVER] ParseEndpoint port out of range field=port reason=bad_port family=ipv4 port=%lu max=%u"
        " endpoint=%s endpoint_len=%zu — lobby session success message NOT sent",
        portVal, UINT16_MAX, shown.c_str(), endpointStr.size());
    return false;
  }
  port = static_cast<uint16_t>(portVal);
  return true;
}

// Helper: Write little-endian value to buffer
template <typename T>
static void WriteLE(std::vector<uint8_t>& buf, T value) {
  for (size_t i = 0; i < sizeof(T); i++) {
    buf.push_back(static_cast<uint8_t>(value & 0xFF));
    value >>= 8;
  }
}

// Helper: Write big-endian uint16_t to buffer
static void WriteBE16(std::vector<uint8_t>& buf, uint16_t value) {
  buf.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  buf.push_back(static_cast<uint8_t>(value & 0xFF));
}

// Helper: Write GUID to buffer (raw bytes, as stored in memory)
static void WriteGuid(std::vector<uint8_t>& buf, const GUID& guid) {
  WriteLE(buf, guid.Data1);
  WriteLE(buf, guid.Data2);
  WriteLE(buf, guid.Data3);
  for (int i = 0; i < 8; i++) {
    buf.push_back(guid.Data4[i]);
  }
}

// Helper: Write bytes from string/bytes field
static void WriteBytes(std::vector<uint8_t>& buf, const std::string& data, size_t expectedSize) {
  for (size_t i = 0; i < expectedSize; i++) {
    if (i < data.size()) {
      buf.push_back(static_cast<uint8_t>(data[i]));
    } else {
      buf.push_back(0);  // Pad with zeros if data is shorter
    }
  }
}

EncodedMessage EncodeLobbySessionStartV4(const GUID& lobbySessionId, const GUID& groupId, uint8_t playerLimit,
                                         uint8_t entrantCount, uint8_t lobbyType, uint8_t pad1,
                                         const std::string& settingsJson) {
  EncodedMessage result;

  WriteGuid(result.data, lobbySessionId);
  WriteGuid(result.data, groupId);
  result.data.push_back(playerLimit);
  result.data.push_back(entrantCount);
  result.data.push_back(lobbyType);
  result.data.push_back(pad1);

  // Write settings JSON including null terminator
  for (char c : settingsJson) {
    result.data.push_back(static_cast<uint8_t>(c));
  }
  result.data.push_back(0);  // Null terminator

  return result;
}

EncodedMessage EncodeLobbySessionSuccessV5(const gameservice::v1::SNSLobbySessionSuccessV5Message& msg,
                                           std::string* failureReason) {
  EncodedMessage result;

  // 1. GameMode (uint64_t LE) - use game_mode which is fixed64
  WriteLE(result.data, msg.game_mode());

  // 2. LobbyID (GUID)
  GUID lobbyId = {};
  if (!ParseUuidToGuid(msg.lobby_id(), lobbyId)) {
    if (failureReason) *failureReason = "bad_lobby_id";
    return {};
  }
  WriteGuid(result.data, lobbyId);

  // 3. GroupID (GUID) - V5 includes this
  GUID groupId = {};
  if (!ParseUuidToGuid(msg.group_id(), groupId)) {
    if (failureReason) *failureReason = "bad_group_id";
    return {};
  }
  WriteGuid(result.data, groupId);

  // 4. Endpoint: internalIP (4 bytes) + externalIP (4 bytes) + port (2 bytes BE)
  uint32_t internalIP = 0, externalIP = 0;
  uint16_t port = 0;
  if (!ParseEndpoint(msg.endpoint(), internalIP, externalIP, port, failureReason)) {
    return {};
  }
  // IPs stored as raw bytes (inet_pton gives network order, we write raw)
  result.data.push_back(static_cast<uint8_t>(internalIP & 0xFF));
  result.data.push_back(static_cast<uint8_t>((internalIP >> 8) & 0xFF));
  result.data.push_back(static_cast<uint8_t>((internalIP >> 16) & 0xFF));
  result.data.push_back(static_cast<uint8_t>((internalIP >> 24) & 0xFF));
  result.data.push_back(static_cast<uint8_t>(externalIP & 0xFF));
  result.data.push_back(static_cast<uint8_t>((externalIP >> 8) & 0xFF));
  result.data.push_back(static_cast<uint8_t>((externalIP >> 16) & 0xFF));
  result.data.push_back(static_cast<uint8_t>((externalIP >> 24) & 0xFF));
  WriteBE16(result.data, port);  // Port is big-endian

  // 5. TeamIndex (uint16_t LE) — at +0x32
  WriteLE(result.data, static_cast<uint16_t>(msg.team_index()));

  // 6. SessionFlags (uint8) at +0x34, followed by 3 bytes padding
  result.data.push_back(static_cast<uint8_t>(msg.session_flags()));
  result.data.push_back(0);
  result.data.push_back(0);
  result.data.push_back(0);

  // 7. ServerEncoderFlags (uint64_t LE) — at +0x38
  WriteLE(result.data, msg.server_encoder_flags());

  // 10. ClientEncoderFlags (uint64_t LE)
  WriteLE(result.data, msg.client_encoder_flags());

  // Get key sizes from flags
  auto serverSettings = PacketEncoderSettings::FromFlags(msg.server_encoder_flags());
  auto clientSettings = PacketEncoderSettings::FromFlags(msg.client_encoder_flags());

  // Validate encoder settings before forwarding to game (N2)
  if (!ValidatePacketEncoderSettings(serverSettings, "server") ||
      !ValidatePacketEncoderSettings(clientSettings, "client")) {
    if (failureReason) *failureReason = "bad_encoder_settings";
    return {};
  }

  // 11. ServerSequenceId (uint64_t LE)
  WriteLE(result.data, msg.server_sequence_id());

  // 12. ServerMacKey (dynamic size)
  WriteBytes(result.data, msg.server_mac_key(), serverSettings.macKeySize);

  // 13. ServerEncKey (dynamic size)
  WriteBytes(result.data, msg.server_enc_key(), serverSettings.encryptionKeySize);

  // 14. ServerRandomKey (dynamic size)
  WriteBytes(result.data, msg.server_random_key(), serverSettings.randomKeySize);

  // 15. ClientSequenceId (uint64_t LE)
  WriteLE(result.data, msg.client_sequence_id());

  // 16. ClientMacKey (dynamic size)
  WriteBytes(result.data, msg.client_mac_key(), clientSettings.macKeySize);

  // 17. ClientEncKey (dynamic size)
  WriteBytes(result.data, msg.client_enc_key(), clientSettings.encryptionKeySize);

  // 18. ClientRandomKey (dynamic size)
  WriteBytes(result.data, msg.client_random_key(), clientSettings.randomKeySize);

  return result;
}

EncodedMessage EncodeLobbySessionCreate(const gameservice::v1::LobbySessionCreateMessage& msg) {
  EncodedMessage result;

  // Parse lobby_session_id to GUID
  GUID lobbySessionId = {};
  if (!ParseUuidToGuid(msg.lobby_session_id(), lobbySessionId)) {
    return result;
  }

  // Parse group_id to GUID
  GUID groupId = {};
  if (!ParseUuidToGuid(msg.group_id(), groupId)) {
    return result;
  }

  // Binary format: GUID matchID, GUID groupID, byte playerLimit, byte entrantCount, byte lobbyType, byte pad, JSON
  // settings Note: protobuf has max_entrants which we use for playerLimit, entrantCount starts at 0
  return EncodeLobbySessionStartV4(lobbySessionId, groupId,
                                   static_cast<uint8_t>(msg.max_entrants()),  // playerLimit
                                   0,                                         // entrantCount (starts at 0)
                                   static_cast<uint8_t>(msg.lobby_type()),
                                   0,  // pad1
                                   msg.settings_json());
}

EncodedMessage EncodeLobbyEntrantsAccept(const gameservice::v1::LobbyEntrantsAcceptMessage& msg) {
  EncodedMessage result;

  // Binary format: 1 byte padding, then array of GUIDs
  result.data.push_back(0);  // Padding byte (Skip(1) in Go)

  int skipped = 0;
  int total = msg.entrant_ids_size();
  for (const auto& entrantIdStr : msg.entrant_ids()) {
    GUID guid = {};
    if (!ParseUuidToGuid(entrantIdStr, guid)) {
      Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] Skipping invalid entrant GUID: %s", entrantIdStr.c_str());
      skipped++;
      continue;  // Skip invalid GUIDs
    }
    WriteGuid(result.data, guid);
  }
  if (skipped > 0) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] LobbyEntrantsAccept: skipped %d of %d entrant GUIDs (invalid format)",
        skipped, total);
  }

  return result;
}

EncodedMessage EncodeLobbyEntrantsReject(const gameservice::v1::LobbyEntrantsRejectMessage& msg) {
  EncodedMessage result;

  // Binary format: 1 byte error code, then array of GUIDs
  result.data.push_back(static_cast<uint8_t>(msg.code()));

  int skipped = 0;
  int total = msg.entrant_ids_size();
  for (const auto& entrantIdStr : msg.entrant_ids()) {
    GUID guid = {};
    if (!ParseUuidToGuid(entrantIdStr, guid)) {
      Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] Skipping invalid entrant GUID: %s", entrantIdStr.c_str());
      skipped++;
      continue;  // Skip invalid GUIDs
    }
    WriteGuid(result.data, guid);
  }
  if (skipped > 0) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] LobbyEntrantsReject: skipped %d of %d entrant GUIDs (invalid format)",
        skipped, total);
  }

  return result;
}

EncodedMessage EncodeLobbySmiteEntrant(uint64_t playerId) {
  EncodedMessage result;

  // Binary wire format (0x10 bytes):
  //   +0x00  uint64  player_id  (LE)
  //   +0x08  uint64  reserved   (zero)
  WriteLE(result.data, playerId);
  WriteLE(result.data, static_cast<uint64_t>(0));

  return result;
}

EncodedMessage EncodeRegistrationSuccess(const gameservice::v1::GameServerRegistrationSuccessMessage& msg) {
  EncodedMessage result;

  // Binary format: uint64 serverId (LE), 4 bytes IP address, uint64 unk0 (LE)
  WriteLE(result.data, msg.server_id());

  // Parse external IP address
  struct in_addr addr;
  if (inet_pton(AF_INET, msg.external_ip_address().c_str(), &addr) == 1) {
    // Write IP as 4 raw bytes
    result.data.push_back(static_cast<uint8_t>(addr.s_addr & 0xFF));
    result.data.push_back(static_cast<uint8_t>((addr.s_addr >> 8) & 0xFF));
    result.data.push_back(static_cast<uint8_t>((addr.s_addr >> 16) & 0xFF));
    result.data.push_back(static_cast<uint8_t>((addr.s_addr >> 24) & 0xFF));
  } else {
    // The game's own field is 4 bytes, so a non-IPv4 address is written as zeros. Say so: the
    // operator would otherwise see a server that registered with 0.0.0.0 and no reason.
    const std::string& external = msg.external_ip_address();
    const char* family = "unknown";
    const std::string shown = AddressForLog(external, &family);
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.GAMESERVER] registration success external address is not IPv4 server_id=%llu family=%s"
        " external_ip_address=%s external_ip_address_len=%zu — the game's 4-byte field is written as 0.0.0.0",
        static_cast<unsigned long long>(msg.server_id()), family, shown.c_str(), external.size());
    for (int i = 0; i < 4; i++) {
      result.data.push_back(0);
    }
  }

  // unk0 (always 0)
  WriteLE(result.data, static_cast<uint64_t>(0));

  return result;
}
