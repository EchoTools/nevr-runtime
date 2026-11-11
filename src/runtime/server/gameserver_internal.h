#pragma once

// Helpers shared by gameserver.cpp (lifecycle), gameserver_serverdb.cpp (ServerDB socket) and
// gameserver_callbacks.cpp
// (broadcaster/TCP callback registration and the handlers it registers). Not part of the
// GameServerLib interface.

#include <cstdint>
#include <cstdio>
#include <string>

#include "abi/echovr.h"
#include "core/logging.h"
#include "runtime/server/broadcaster_address.h"
#include "runtime/server/gameserver.h"
#include "runtime/hook/patching.h"  // NevrUPnPConfig
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
/// A ServerDB bearer token (refreshed or re-minted under the serialized-mint lock); empty with `reason` set on failure.
std::string AcquireServerDbToken(std::string& reason);
/// UPnP settings from the runtime's globals.
bool ReadUPnPConfig(NevrUPnPConfig& out);
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

// The broadcaster's own address as the registration's default internal IP. A slot that is not IPv4 is
// named in a Warning with its family and address, and yields an empty string so the config/UPnP
// override (or nothing, which the caller's registration line shows) decides the address.
inline std::string BroadcasterInternalIp(const EchoVR::AddressInfo& slot, const char* site) {
  const nevr_game_server::BroadcasterAddress address = nevr_game_server::ReadBroadcasterAddress(slot);
  if (address.isIpv4) return address.text;
  Log(EchoVR::LogLevel::Warning,
      "[NEVR.GAMESERVER] broadcaster address is not IPv4 site=%s family=%u address=%s"
      " — registration uses the config or UPnP address, none if unset",
      site, static_cast<unsigned>(address.family), address.text.empty() ? "none" : address.text.c_str());
  return std::string();
}
