#pragma once
// Self-check "pc_quit" (#392, docs/engine/remote-log.md): the client's game loop returned, so the client is about
// to exit. GameMainWrapperHook (crash_recovery.cpp) returns GameMain's result and WinMain's CRT start-up passes it
// to ExitProcess, so it is the process exit code (hook/addresses.h, GAME_MAIN_WRAPPER: a hook that dropped it
// made the same close path exit 1). One result, `game_loop_returned exit_code=<n>`: it passes for 0 and fails for
// anything else.
//
// The result is sent at once: after this the process has nothing left to run, and a later flush would never come.
// That is best effort (a bridge that is already gone sends nothing); the Info line
// `[NEVR.SELFCHECK] check=pc_quit ...` is written whatever the connection does.
//
// What it cannot see: how the player quit. The game loop returns for the main-menu QUIT and for a closed window
// alike, so this shows that the client left through the game's own return path with the exit code it will have,
// not which control was used. A server never reaches it (its exit goes through PerformGracefulShutdown). Quest has
// no equivalent yet (#392 on Quest waits for a quit entry).
//
// Header only on top of the self-check unit (compat/self_check.h); the caller links self_check.cpp. It runs on the
// game's main thread after GameMain returned, outside the loader lock and not in a hooked game function.

#include <cstdint>
#include <string>

#include "runtime/compat/self_check.h"

namespace nevr_quit_check {

/// The value GameMain returned (the process exit code): reports it and sends the result before the process goes.
inline void OnGameLoopReturned(long long gameResult) {
  // Register is idempotent by name, so asking again returns the same check (and survives the unit's test reset).
  const nevr_self_check::CheckId check = nevr_self_check::Register(
      {"pc_quit", "the client leaves through the game's own return path with exit code 0", nullptr});
  const std::int32_t exitCode = static_cast<std::int32_t>(gameResult);  // ExitProcess takes the low 32 bits
  nevr_self_check::Report(check, "game_loop_returned exit_code=" + std::to_string(exitCode), exitCode == 0);
  nevr_self_check::Flush();
}

}  // namespace nevr_quit_check
