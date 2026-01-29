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
namespace realtime {
class SNSLobbySessionSuccessV5Message;
class LobbySessionCreateMessage;
class LobbyEntrantsAcceptMessage;
class LobbyEntrantsRejectMessage;
class GameServerRegistrationSuccessMessage;
}  // namespace realtime

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
// Binary format (little-endian unless noted):
//   uint64_t gameMode
//   GUID lobbyId (16 bytes)
//   GUID groupId (16 bytes)
//   Endpoint: uint32_t internalIP + uint32_t externalIP + uint16_t port (BE)
//   int16_t teamIndex
//   uint32_t userSlot (flags32 + session_flags combined as unk1)
//   uint64_t serverEncoderFlags
//   uint64_t clientEncoderFlags
//   uint64_t serverSequenceId
//   bytes serverMacKey (dynamic size from encoder flags)
//   bytes serverEncKey (dynamic size from encoder flags)
//   bytes serverRandomKey (dynamic size from encoder flags)
//   uint64_t clientSequenceId
//   bytes clientMacKey (dynamic size from encoder flags)
//   bytes clientEncKey (dynamic size from encoder flags)
//   bytes clientRandomKey (dynamic size from encoder flags)
EncodedMessage EncodeLobbySessionSuccessV5(const realtime::SNSLobbySessionSuccessV5Message& msg);

// Encode LobbySessionCreate from protobuf to LobbyStartSessionV4 binary format
EncodedMessage EncodeLobbySessionCreate(const realtime::LobbySessionCreateMessage& msg);

// Encode LobbyEntrantsAccept from protobuf to binary format
// Binary format: 1 byte padding, then array of GUIDs
EncodedMessage EncodeLobbyEntrantsAccept(const realtime::LobbyEntrantsAcceptMessage& msg);

// Encode LobbyEntrantsReject from protobuf to binary format
// Binary format: 1 byte error code, then array of GUIDs
EncodedMessage EncodeLobbyEntrantsReject(const realtime::LobbyEntrantsRejectMessage& msg);

// Encode GameServerRegistrationSuccess from protobuf to binary format
// Binary format: uint64 serverId (LE), 4 bytes IP address, uint64 unk0 (LE)
EncodedMessage EncodeRegistrationSuccess(const realtime::GameServerRegistrationSuccessMessage& msg);

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
