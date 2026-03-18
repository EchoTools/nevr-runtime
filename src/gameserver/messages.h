#pragma once
#ifndef GAMESERVER_MESSAGES_H
#define GAMESERVER_MESSAGES_H

#include <cstdint>
#include <string>
#include <vector>

#include "echovr.h"
#include "symbols.h"

// Namespace aliases for convenience
namespace Sym = EchoVR::Symbols;
namespace TcpSym = EchoVR::Symbols::Tcp;

// Forward declaration for protobuf types
namespace rtapi {
namespace v1 {
class SNSLobbySessionSuccessV5Message;
class LobbySessionCreateMessage;
class LobbyEntrantsAcceptMessage;
class LobbyEntrantsRejectMessage;
class GameServerRegistrationSuccessMessage;
}  // namespace v1
}  // namespace rtapi

// Binary message encoding result
struct EncodedMessage {
  std::vector<uint8_t> data;
  size_t size() const { return data.size(); }
  const uint8_t* ptr() const { return data.data(); }
};

// Encode LobbyStartSessionV4 message to binary format
// Binary format:
//   GUID lobbySessionId (16 bytes)
//   GUID groupId (16 bytes)
//   BYTE playerLimit
//   BYTE entrantCount
//   BYTE lobbyType
//   BYTE pad1
//   CHAR[] settingsJson (null-terminated)
EncodedMessage EncodeLobbySessionStartV4(const GUID& lobbySessionId, const GUID& groupId, uint8_t playerLimit,
                                         uint8_t entrantCount, uint8_t lobbyType, uint8_t pad1,
                                         const std::string& settingsJson);

// Encode LobbySessionSuccessV5 from protobuf to binary format
// Binary wire format (0x48-byte fixed header + variable key data):
//   +0x00  uint64_t gameMode (LE)
//   +0x08  GUID lobbyId (16 bytes)
//   +0x18  GUID groupId (16 bytes)
//   +0x28  uint32_t internalIP (network order)
//   +0x2C  uint32_t externalIP (network order)
//   +0x30  uint16_t port (BE)
//   +0x32  uint16_t teamIndex (LE)
//   +0x34  uint8_t  sessionFlags
//   +0x35  3 bytes padding (zeroed)
//   +0x38  uint64_t serverEncoderFlags (LE)
//   +0x40  uint64_t clientEncoderFlags (LE)
//   +0x48  uint64_t serverSequenceId (LE)
//   +0x50  bytes serverMacKey (size from encoder flags)
//          bytes serverEncKey
//          bytes serverRandomKey
//          uint64_t clientSequenceId (LE)
//          bytes clientMacKey (size from encoder flags)
//          bytes clientEncKey
//          bytes clientRandomKey
EncodedMessage EncodeLobbySessionSuccessV5(const rtapi::v1::SNSLobbySessionSuccessV5Message& msg);

// Encode LobbySessionCreate from protobuf to LobbyStartSessionV4 binary format
EncodedMessage EncodeLobbySessionCreate(const rtapi::v1::LobbySessionCreateMessage& msg);

// Encode LobbyEntrantsAccept from protobuf to binary format
// Binary format: 1 byte padding, then array of GUIDs
EncodedMessage EncodeLobbyEntrantsAccept(const rtapi::v1::LobbyEntrantsAcceptMessage& msg);

// Encode LobbyEntrantsReject from protobuf to binary format
// Binary format: 1 byte error code, then array of GUIDs
EncodedMessage EncodeLobbyEntrantsReject(const rtapi::v1::LobbyEntrantsRejectMessage& msg);

// Encode GameServerRegistrationSuccess from protobuf to binary format
// Binary format: uint64 serverId (LE), 4 bytes IP address, uint64 unk0 (LE)
EncodedMessage EncodeRegistrationSuccess(const rtapi::v1::GameServerRegistrationSuccessMessage& msg);

// Helper: Parse UUID string to GUID
bool ParseUuidToGuid(const std::string& uuidStr, GUID& outGuid);

// Helper: Parse endpoint string "internalIP:externalIP:port" to binary format
bool ParseEndpoint(const std::string& endpointStr, uint32_t& internalIP, uint32_t& externalIP, uint16_t& port);

// Helper: Extract key sizes from encoder flags
struct PacketEncoderSettings {
  bool encryptionEnabled;
  bool macEnabled;
  int macDigestSize;
  int macPBKDF2IterationCount;
  int macKeySize;
  int encryptionKeySize;
  int randomKeySize;

  static PacketEncoderSettings FromFlags(uint64_t flags);
};

#endif  // GAMESERVER_MESSAGES_H
