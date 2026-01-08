// Disable MSVC deprecation warnings for sscanf etc.
#define _CRT_SECURE_NO_WARNINGS

#include "messages.h"

#include <WS2tcpip.h>

#include <cstdarg>
#include <cstring>

#include "echovrunexported.h"
#include "rtapi/realtime_v1.pb.h"

// Logging wrapper for game's log system
// @param level - The log level (Info, Warning, Error, etc.)
// @param format - Printf-style format string
// @param ... - Variable arguments for format string
// Note: The second parameter to EchoVR::WriteLog (0) is an unknown flag/context value
static void Log(EchoVR::LogLevel level, const CHAR* format, ...) {
  va_list args;
  va_start(args, format);
  EchoVR::WriteLog(level, 0, format, args);
  va_end(args);
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
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
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

// Helper: Parse endpoint string "internalIP:externalIP:port" to components
bool ParseEndpoint(const std::string& endpointStr, uint32_t& internalIP, uint32_t& externalIP, uint16_t& port) {
  size_t firstColon = endpointStr.find(':');
  size_t lastColon = endpointStr.rfind(':');

  if (firstColon == std::string::npos || lastColon == std::string::npos || firstColon == lastColon) {
    return false;
  }

  std::string internalStr = endpointStr.substr(0, firstColon);
  std::string externalStr = endpointStr.substr(firstColon + 1, lastColon - firstColon - 1);
  std::string portStr = endpointStr.substr(lastColon + 1);

  // Parse IPs using inet_pton
  struct in_addr inAddr, exAddr;
  if (inet_pton(AF_INET, internalStr.c_str(), &inAddr) != 1) return false;
  if (inet_pton(AF_INET, externalStr.c_str(), &exAddr) != 1) return false;

  internalIP = inAddr.s_addr;  // Already in network byte order, but we want host order for LE storage
  externalIP = exAddr.s_addr;

  // Parse port
  port = static_cast<uint16_t>(std::stoul(portStr));
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

EncodedMessage EncodeLobbySessionSuccessV5(const realtime::SNSLobbySessionSuccessV5Message& msg) {
  EncodedMessage result;

  // 1. GameMode (uint64_t LE) - use game_mode which is fixed64
  WriteLE(result.data, msg.game_mode());

  // 2. LobbyID (GUID)
  GUID lobbyId = {};
  if (!ParseUuidToGuid(msg.lobby_id(), lobbyId)) {
    // Return empty result on parse failure
    return result;
  }
  WriteGuid(result.data, lobbyId);

  // 3. GroupID (GUID) - V5 includes this
  GUID groupId = {};
  if (!ParseUuidToGuid(msg.group_id(), groupId)) {
    return result;
  }
  WriteGuid(result.data, groupId);

  // 4. Endpoint: internalIP (4 bytes) + externalIP (4 bytes) + port (2 bytes BE)
  uint32_t internalIP = 0, externalIP = 0;
  uint16_t port = 0;
  if (!ParseEndpoint(msg.endpoint(), internalIP, externalIP, port)) {
    return result;
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

  // 5. TeamIndex (int16_t LE)
  WriteLE(result.data, static_cast<int16_t>(msg.team_index()));

  // 6. UserSlot (uint16_t LE)
  WriteLE(result.data, static_cast<uint16_t>(msg.user_slot()));

  // 7. Flags32 (uint16_t LE)
  WriteLE(result.data, static_cast<uint16_t>(msg.flags32()));

  // 8. SessionFlags (uint8_t)
  result.data.push_back(static_cast<uint8_t>(msg.session_flags()));

  // 9. ServerEncoderFlags (uint64_t LE)
  WriteLE(result.data, msg.server_encoder_flags());

  // 10. ClientEncoderFlags (uint64_t LE)
  WriteLE(result.data, msg.client_encoder_flags());

  // Get key sizes from flags
  auto serverSettings = PacketEncoderSettings::FromFlags(msg.server_encoder_flags());
  auto clientSettings = PacketEncoderSettings::FromFlags(msg.client_encoder_flags());

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

EncodedMessage EncodeLobbySessionCreate(const realtime::LobbySessionCreateMessage& msg) {
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

EncodedMessage EncodeLobbyEntrantsAccept(const realtime::LobbyEntrantsAcceptMessage& msg) {
  EncodedMessage result;

  // Binary format: 1 byte padding, then array of GUIDs
  result.data.push_back(0);  // Padding byte (Skip(1) in Go)

  for (const auto& entrantIdStr : msg.entrant_ids()) {
    GUID guid = {};
    if (!ParseUuidToGuid(entrantIdStr, guid)) {
      Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] Skipping invalid entrant GUID: %s", entrantIdStr.c_str());
      continue;  // Skip invalid GUIDs
    }
    WriteGuid(result.data, guid);
  }

  return result;
}

EncodedMessage EncodeLobbyEntrantsReject(const realtime::LobbyEntrantsRejectMessage& msg) {
  EncodedMessage result;

  // Binary format: 1 byte error code, then array of GUIDs
  result.data.push_back(static_cast<uint8_t>(msg.code()));

  for (const auto& entrantIdStr : msg.entrant_ids()) {
    GUID guid = {};
    if (!ParseUuidToGuid(entrantIdStr, guid)) {
      Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] Skipping invalid entrant GUID: %s", entrantIdStr.c_str());
      continue;  // Skip invalid GUIDs
    }
    WriteGuid(result.data, guid);
  }

  return result;
}

EncodedMessage EncodeRegistrationSuccess(const realtime::GameServerRegistrationSuccessMessage& msg) {
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
    // Invalid IP, write zeros
    for (int i = 0; i < 4; i++) {
      result.data.push_back(0);
    }
  }

  // unk0 (always 0)
  WriteLE(result.data, static_cast<uint64_t>(0));

  return result;
}