#pragma once

// Decides where a console CTRL+C/close/break event is routed. Pure, so the decision is unit-tested
// without the process-wide handler in crash_recovery.cpp.
//
// Returning FALSE from our handler hands the event to the game's own handler, and the process then
// exits at the end of GameServerLib::Terminate. That is only true when both hold:
//   - the game's handler is behind ours in the chain (we were re-armed to the front after the game
//     installed it), and
//   - GameServerLib started, so the game's teardown can reach Terminate.
// A server that never got as far as GameServerLib::Initialize would otherwise wait out the shutdown
// watchdog and exit 1 on a clean operator stop (#241).

namespace ConsoleCtrlPolicy {

inline bool ShouldDeferToGame(bool gameHandlerBehindUs, bool gameServerLibStarted) {
  return gameHandlerBehindUs && gameServerLibStarted;
}

// Why the runtime shuts down itself instead of deferring. Only meaningful when ShouldDeferToGame is false.
inline const char* NoDeferReason(bool gameHandlerBehindUs, bool gameServerLibStarted) {
  if (!gameHandlerBehindUs) return "no game console handler behind ours";
  if (!gameServerLibStarted) return "GameServerLib never started, so the game teardown cannot reach Terminate";
  return "deferring";
}

}  // namespace ConsoleCtrlPolicy
