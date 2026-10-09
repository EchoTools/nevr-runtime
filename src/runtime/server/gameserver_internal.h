#pragma once

// Helpers shared by gameserver.cpp (lifecycle, ServerDB socket) and gameserver_callbacks.cpp
// (broadcaster/TCP callback registration and the handlers it registers). Not part of the
// GameServerLib interface.

#include <cstdint>
#include <cstdio>
#include <string>

#include "abi/echovr.h"
#include "runtime/server/gameserver.h"
#include "runtime/server/upnp.h"
#include "gameservice/v1/gameservice.pb.h"

// Symbol ID for NEVRProtobufMessageV1 (binary protobuf)
inline constexpr EchoVR::SymbolId SYM_PROTOBUF_MSG = 0x9ee5107d9e29fd63ULL;

// Legacy symbol IDs sent by Nakama for backwards compatibility (skipped, handled via protobuf)
inline constexpr EchoVR::SymbolId SYM_LEGACY_SESSION_START = 0x7777777777770000ULL;
inline constexpr EchoVR::SymbolId SYM_LEGACY_PLAYERS_REJECTED = 0x7777777777770700ULL;

/// Subscribe to internal broadcaster (UDP) events.
uint16_t ListenForBroadcasterMessage(GameServerLib* self, EchoVR::SymbolId msgId, BOOL isMsgReliable, VOID* func);
/// Subscribe to TCP broadcaster (websocket) events.
uint16_t ListenForTcpBroadcasterMessage(GameServerLib* self, EchoVR::SymbolId msgId, VOID* func);
/// Send a protobuf Envelope to ServerDB as binary; false when it was not accepted.
bool SendProtobufEnvelope(GameServerLib* self, const gameservice::v1::Envelope& envelope);
/// UPnP settings from the runtime's globals.
bool ReadUPnPConfig(NevRUPnPConfig& out);
/// Ask the game to return to the lobby (no-op without a game object).
void CallScheduleReturnToLobby();

// Helper to convert GUID to UUID string format
inline std::string GuidToUuidString(const GUID& guid) {
  char buf[64];
  snprintf(buf, sizeof(buf), "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           static_cast<unsigned long>(guid.Data1), guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1], guid.Data4[2],
           guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
  return buf;
}

// Helper to convert IPv4 address (uint32_t) to string
inline std::string Ipv4ToString(uint32_t ip) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (ip >> 0) & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
  return buf;
}
