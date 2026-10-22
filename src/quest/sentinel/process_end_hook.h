/* The process ends when the game's android_main returns (#621).
 *
 * The native-activity glue calls android_main on its own thread and ignores the result (pinned_targets.h,
 * LibR15AndroidMain). android_main returns after the engine's shutdown, whether the game failed to initialise
 * (the first launch without its storage permission) or the activity was destroyed, and nothing then ends the
 * process. The stock engine cannot run a second android_main in the same process: CFileTypes::InitializeDirectories
 * runs once per process and NRadCrypt::Shutdown calls OPENSSL_cleanup, so every relaunch into the leftover process
 * fails its initialisation again until a force-stop. The only reset is a new process.
 *
 * One GOT hook on libr15's android_main slot. The handler calls the original, and when it returns:
 *   1. logs {"event":"android_main_returned","action":"ending_process","exit_code":0};
 *   2. runs the before-exit callback (SetBeforeExit) on a helper thread and waits for it at most
 *      kFlushBudgetMs; the integration registers the self-check report and flush there. The Info line the report
 *      writes is the record. The remote string is best effort: Flush only queues the frame on the connection's worker
 *      (ConnectorRemoteTransport::Send), the game has usually closed the login connection while it shut down
 *      (session_bridge.cpp: Close clears the login gate), and _exit does not wait for the worker. A callback that
 *      does not return in time is left behind;
 *   3. ends the process with _exit(0), as libr15's own failure paths end it with exit(0). _exit runs no static
 *      destructors: our threads and leaked objects are live.
 * The state our Quest code saves (the credential cache, device_login.txt) is written by AtomicWrite
 * (auth/atomic_write.cpp: temp file, fsync, rename, directory fsync), so a process ended mid-write leaves the
 * previous file whole; process_end_hook_test.cpp ends a process under a writer 60 times to show it.
 *
 * Steps 1-3 run after the call into the game and are reached from the handler only through function pointers,
 * so no personality-bearing frame is live across game code (callback_thunk.h, rules 1 and 5). The handler
 * touches nothing else.
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include "pinned_targets.h"

namespace nevr_quest::process_end {

using MainThunk = sentinel::pinned::LibR15AndroidMainThunk;
// Runs after android_main returned, on a helper thread, before the process ends.
using BeforeExitFn = void (*)() noexcept;
// Ends the process. The default is EndProcess; tests inject their own.
using EnderFn = void (*)() noexcept;

inline constexpr unsigned kFlushBudgetMs = 500;

// The android_app the game's android_main was called with, kept before the original runs (null until then). The
// first-run data step reads the activity from it (android_glue_layout.h); nothing else uses it.
sentinel::pinned::AndroidAppOpaque* CurrentApp() noexcept;

// The callback of step 2 (null: none). It is read when android_main returns, so it may be set before or after Install.
void SetBeforeExit(BeforeExitFn fn) noexcept;

// Arms the handler and installs the thunk in libr15.so. One line {"event":"process_end_install",...} says what
// was installed. True when the slot holds the thunk. It is InstallAt on the pinned libr15 slot, ending with EndProcess.
bool Install() noexcept;

// The whole install, on any target: arms the handler (with `ender` as what ends the process), publishes the
// after-return step and installs the thunk's entry in the target's slot. The production target is the pinned
// libr15 slot; the host test installs it in a fixture library.
bool InstallAt(const sentinel::GotTarget& target, EnderFn ender) noexcept;

// _exit(0).
void EndProcess() noexcept;

// Test support: arms the handler without touching a GOT slot, with `ender` instead of EndProcess and a
// `budgetMs` wait for the before-exit callback. ResetForTest clears the callback and the counts.
void ArmForTest(EnderFn ender, unsigned budgetMs) noexcept;
void ResetForTest() noexcept;

struct Counts {
  unsigned long long returned;
};
Counts CurrentCounts() noexcept;

}  // namespace nevr_quest::process_end
