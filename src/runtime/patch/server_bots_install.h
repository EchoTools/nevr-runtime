#pragma once

// #1423: AI bots on a dedicated server (server_bots.h). Install runs once, from RunDeferredRuntimeBootstrap (boot.cpp)
// after the config is loaded; a client logs that it does not apply (N122/N124). OnFrame runs inside
// nevr_frame::DispatchPerFrameWork, on a server and on the game-loop thread only (N86/N110: no new dispatcher or Tick*
// site). SessionSettings is the legacy session start's rewrite (gameserver_callbacks.cpp).

#include <cstddef>
#include <cstdint>

namespace nevr_server_bots {

void Install(bool isServer);
void OnFrame();

/// Whether the per-frame driver runs on this call of the dispatcher: on a server, on the thread that runs the game's
/// main wrapper only (`loopThread` is GameLoopThreadId(), 0 before it is known). The driver calls into the net game.
inline bool DriverRuns(bool isServer, unsigned long loopThread, unsigned long currentThread) {
  return isServer && loopThread != 0 && loopThread == currentThread;
}

/// The count Install armed: 0 when the driver is off (a client, no key, 0, a bad value, a site mismatch).
uint32_t ArmedBots();

/// The session start to forward to the game: `*frame`/`*size` are replaced by a rewritten copy (held until the next
/// call) when bots are armed and the settings lack numteams or players_per_team. Logs the outcome.
void SessionSettings(void** frame, uint64_t* size);

}  // namespace nevr_server_bots
