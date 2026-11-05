#pragma once

// #2109: a dedicated server's frame wait (server_frame_wait.h). Install runs once from RunDeferredRuntimeBootstrap
// (boot.cpp) in both modes; a client logs that it does not apply (N122/N124). On a server it hooks the broadcaster's
// receive pump (CBroadcaster::SendRecvDrain, for the port) and the game's recvfrom wrapper (to hand back what the wait
// read early), and applies the overrun delta-time patch when the runtime owns CPrecisionSleep::Wait. One
// [NEVR.PATCH] line gives the outcome; the wait itself writes one summary line a minute.

#include <cstdint>

namespace nevr_server_frame_wait {

void Install(bool isServer);

/// The server's frame wait: called by the runtime's CPrecisionSleep::Wait hook (binary_bug_fixes.cpp) with the
/// microseconds the game asked for. Returns false when it does not apply (not installed as a server, or not on the
/// game loop thread); the hook then waits its own way. When it returns true the wait is done and was never shorter
/// than asked.
bool Wait(int64_t microseconds);

}  // namespace nevr_server_frame_wait
