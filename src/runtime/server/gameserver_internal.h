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
#include "runtime/server/guid_text.h"
#include "runtime/server/gameserver.h"
#include "runtime/hook/patching.h"  // NevrUPnPConfig
#include "runtime/server/upnp.h"
#include "runtime/server/session_demand.h"
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
/// Forwards a session start the server held for its content, then its held accepts, in arrival order (PLAN-v5 R6-a-2b).
void ReleaseHeldSessionStart(GameServerLib* self, nevr_session_demand::Due due);
