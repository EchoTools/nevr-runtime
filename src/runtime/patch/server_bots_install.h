#pragma once

// #1423: AI bots on a dedicated server (server_bots.h). Install runs once, from RunDeferredRuntimeBootstrap (boot.cpp)
// after the config is loaded; a client logs that it does not apply (N122/N124). OnServerUpdate runs from
// GameServerLib::Update (gameserver.cpp), on the game-loop thread only. Update runs on each pass of the game loop, so
// this is per-frame work outside the one dispatcher N86 names, a departure from N86 that the return to lobby's Poll
// and the telemetry snapshot there already make; N110 holds (tick.cpp is unchanged). SessionSettings is the legacy
// session start's rewrite (gameserver_callbacks.cpp).

#include <cstddef>
#include <cstdint>

namespace nevr_server_bots {

void Install(bool isServer);
void OnServerUpdate();

/// Whether the driver runs on this call of GameServerLib::Update: on a server, on the thread that runs the game's main
/// wrapper only (`loopThread` is GameLoopThreadId(), 0 before it is known). The driver calls into the net game.
inline bool DriverRuns(bool isServer, unsigned long loopThread, unsigned long currentThread) {
  return isServer && loopThread != 0 && loopThread == currentThread;
}

/// The count Install armed: 0 when the driver is off (a client, no key, 0, a bad value, a site mismatch).
uint32_t ArmedBots();

/// The session start to forward to the game: `*frame`/`*size` are replaced by a rewritten copy (held until the next
/// call) when bots are armed, the game type is a match and the settings lack numteams or players_per_team. Records
/// whether the session gets bots, and logs the outcome.
void SessionSettings(void** frame, uint64_t* size);

/// Clears the last session start's record. The legacy session start calls it before its broadcaster gate, so a session
/// start that never reaches the game (no broadcaster) does not leave the previous session's bots flag in place.
void ForgetSession();

}  // namespace nevr_server_bots
