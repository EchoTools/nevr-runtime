#pragma once

#include <windows.h>

#include <cstdint>

// The runtime's one way to schedule the game's return to lobby (#58). With no TTL configured
// (network.empty_server_ttl_seconds: 0, the default) every request goes straight to the game's
// NetGameScheduleReturnToLobby, as before. With a TTL, a request made while the session has no
// accepted entrants is held for up to the TTL (decisions: return_to_lobby_hold.h).
namespace ReturnToLobby {

/// Live (accepted) entrants of the current session; 0 when none or no server is running.
using EntrantCounter = uint64_t (*)();

void SetEntrantCounter(EntrantCounter counter);

/// Sets the hold and, for a TTL above 0 on a server, detours the game's own
/// NetGameScheduleReturnToLobby (prologue-validated) so the game's empty-session return is held
/// too. Returns false only when the TTL is above 0 and the detour could not be installed.
bool Configure(uint64_t ttlSeconds);

/// Schedules the return to lobby now or holds it, per the policy.
void Request(PVOID pGame);

/// Once per game-thread update: releases or cancels a hold.
void Poll();

}  // namespace ReturnToLobby
