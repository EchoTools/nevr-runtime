/* Our crash handler stays first on the Quest (#1605).
 *
 * The sentinel arms breakpad as the process's first crash handler (sentinel.cpp). The game then installs its own, through
 * libr15's imports of libc `sigaction` and `signal`, and the kernel delivers a fatal signal to that one: the sentinel's
 * handler never runs, so a segfault leaves no minidump and no report (2026-10-30: six crashes in CSendEventNode::Enter,
 * `nevr-crashes/` empty).
 *
 * Two GOT hooks on libr15's `sigaction` and `signal` slots. For the five fatal signals (SIGSEGV, SIGBUS, SIGABRT, SIGILL,
 * SIGFPE) the hook REMEMBERS what the game asked for and does not install it, and answers the game's query for the old
 * action with what the game last asked for (before its first request: what the process had before the sentinel armed, so a
 * game that saves and restores sees a consistent history). Every other signal goes to libc untouched. The hooks cannot
 * reach a call another library makes (libpnsovr, libpnsrad and libpnsradmatchmaking also import both; they load later, and
 * this hook covers libr15 only), or a direct system call.
 *
 * When a fatal signal arrives, our handler (OnSignal) runs, in this order, on the faulting thread:
 *   1. the report (core/crash_report.h): it needs the signal's ucontext for the registers and the backtrace;
 *   2. the minidump (breakpad);
 *   3. the restore: the actions that were in place before the sentinel armed go back, as breakpad does after a dump, so
 *      a fault that is raised again when the handler returns reaches the system's handler (the tombstone), a game handler
 *      that returns for a fault it cannot fix does not loop, and a game handler that aborts reaches the system's handler
 *      instead of ours again. This is the same one-shot rule breakpad has;
 *   4. the game's remembered handler, if the game asked for one (SA_SIGINFO or plain, as it asked), so the game's own
 *      crash path still runs after ours.
 * A second thread that faults while the first is in steps 1 and 2 waits for it (bounded) and then takes steps 3 and 4.
 *
 * The handlers (the thunk entries' callees) log nothing and take no lock a signal handler could be holding: the game's
 * requests are stored under a mutex in an ordinary thread, and the signal handler reads them through a seqlock
 * (core/crash_first.h). What the hooks did is counted (Counts) and reported by the crash path.
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstdint>

#include "crash_first_arm.h"
#include "pinned_targets.h"

namespace nevr_quest::crash_first {

// The hooks on libr15's `sigaction` and `signal` slots. One line {"event":"crash_first_install",...} says what was
// installed. True when both slots hold their thunks. `Install` is InstallAt on the pinned libr15 slots.
bool Install() noexcept;
bool InstallAt(const sentinel::GotTarget& sigactionTarget, const sentinel::GotTarget& signalTarget) noexcept;

struct Counts {
  std::uint32_t displaced[kGuardedCount];  // requests to install a handler, kept instead of installed
  std::uint32_t queried;                   // requests that only asked for the current action
  std::uint32_t passedThrough;             // calls for any other signal
};
Counts CurrentCounts() noexcept;

}  // namespace nevr_quest::crash_first
