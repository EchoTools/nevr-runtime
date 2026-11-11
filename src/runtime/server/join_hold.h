#pragma once

// Join hold on the PC dedicated server (#1737 row 101f; rules in join_hold_rules.h, the game's functions in
// docs/reference/join-hold.md). With network.join_hold_ms above 0, the server's forward of the service's accept
// (LobbyAcceptPlayersSuccessV2) waits until the session's levels are seen enabled, then goes to the game in arrival
// order. Install runs once from RunDeferredRuntimeBootstrap (boot.cpp) after the config is loaded; a client or a
// server with the key off logs that it does not apply (N122/N124) and no hook is installed. Admit, ReleaseDue and the
// session calls run on the game thread (GameServerLib::Update's ProcessReceivedMessages); the two detours run on the
// game's own threads, so the state is behind one mutex held for constant-time updates only.

#include <cstddef>
#include <cstdint>

namespace nevr_join_hold {

void Install(bool isServer);

/// True when the accept was held (a copy was queued): the caller must not forward it now. False: forward it now.
/// `entrants` is how many entrants it names; `sessionId` only goes into the log.
bool Admit(const void* encoded, std::size_t size, std::size_t entrants, const char* sessionId);

/// Forwards what is due to `broadcaster` (an EchoVR::Broadcaster*): all held accepts, in order, once the levels are
/// enabled, or at the timeout. One atomic read when nothing is held.
void ReleaseDue(void* broadcaster, const char* sessionId);

/// A session was created on this server (arms the hold; an earlier session's held accepts are dropped).
void OnSessionCreate(const char* sessionId);

/// The session ended (`cause` is service_ended or game_ended) or the server is shutting down: disarm and drop.
void OnSessionEnd(const char* cause, const char* sessionId);

}  // namespace nevr_join_hold
