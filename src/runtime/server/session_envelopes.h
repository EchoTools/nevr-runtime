#pragma once

// The envelopes GameServerLib sends to ServerDB when the game locks or unlocks a session, accepts player sessions or
// removes one (gameserver.cpp). Built here, apart from the socket and the session state, so the fields each one
// carries (the session id, the event code, which entrants) are one definition a test can read.

#include <cstddef>
#include <string>

#include "gameservice/v1/gameservice.pb.h"
#include "runtime/server/guid_text.h"

namespace nevr_game_server {

// LobbySessionEvent CODE_LOCKED (locked) or CODE_UNLOCKED, for `sessionId`.
inline gameservice::v1::Envelope BuildSessionLockEnvelope(const std::string& sessionId, bool locked) {
  gameservice::v1::Envelope envelope;
  auto* event = envelope.mutable_lobby_session_event();
  event->set_lobby_session_id(sessionId);
  event->set_code(locked ? gameservice::v1::LobbySessionEventMessage::CODE_LOCKED
                         : gameservice::v1::LobbySessionEventMessage::CODE_UNLOCKED);
  return envelope;
}

// LobbyEntrantConnected for `sessionId`: one entrant id per GUID in `guids`, in order.
inline gameservice::v1::Envelope BuildEntrantsConnectedEnvelope(const std::string& sessionId, const GUID* guids,
                                                                std::size_t count) {
  gameservice::v1::Envelope envelope;
  auto* connected = envelope.mutable_lobby_entrant_connected();
  connected->set_lobby_session_id(sessionId);
  for (std::size_t i = 0; i < count; i++) {
    connected->add_entrant_ids(GuidToUuidString(guids[i]));
  }
  return envelope;
}

// LobbyEntrantRemoved for `sessionId`, code DISCONNECTED.
inline gameservice::v1::Envelope BuildEntrantRemovedEnvelope(const std::string& sessionId, const GUID& guid) {
  gameservice::v1::Envelope envelope;
  auto* removed = envelope.mutable_lobby_entrant_removed();
  removed->set_lobby_session_id(sessionId);
  removed->set_entrant_id(GuidToUuidString(guid));
  removed->set_code(gameservice::v1::LobbyEntrantRemovedMessage::CODE_DISCONNECTED);
  return envelope;
}

}  // namespace nevr_game_server
