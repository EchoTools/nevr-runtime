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
 * game that saves and restores sees a consistent history). Every other signal goes to libc untouched. The hooks cannot reach
 * a direct system call.
 *
 * libpnsrad, libpnsradmatchmaking and libpnsovr import both functions too. The game loads them after the constructor (none is
 * a DT_NEEDED of libr15), so OnModuleLoaded hooks their slots right after the game's dlopen returns, before CModuleLoader
 * calls the module's RadPluginMain, with the same handlers. A library's own ELF constructors run inside dlopen, before that
 * moment, and are not covered.
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
#include <optional>

#include "crash_first_arm.h"
#include "pinned_targets.h"

namespace nevr_quest::crash_first {

// The hooks on libr15's `sigaction` and `signal` slots. One line {"event":"crash_first_install",...} says what was
// installed. True when both slots hold their thunks. `Install` is InstallAt on the pinned libr15 slots.
bool Install() noexcept;
bool InstallAt(const sentinel::GotTarget& sigactionTarget, const sentinel::GotTarget& signalTarget) noexcept;

// One provider library the late install reaches (pinned_targets.h kCrashLateModules): its name, the build ID and the two slots'
// link-time addresses the install checks. The host test replaces the table with fixture libraries that carry no pin.
struct LateModule {
  const char* module;
  const char* buildId;                          // nullptr: not checked
  std::optional<std::uint64_t> sigactionSlot;   // nullopt: the one relocation of the symbol
  std::optional<std::uint64_t> signalSlot;
};
inline constexpr int kLateModuleMax = 3;
// Test support: replaces the table (at most kLateModuleMax entries; the strings must outlive it). nullptr, 0: the pinned one.
void SetLateModulesForTest(const LateModule* modules, int count) noexcept;

struct Counts {
  std::uint32_t displaced[kGuardedCount];  // requests to install a handler, kept instead of installed
  std::uint32_t queried;                   // requests that only asked for the current action
  std::uint32_t passedThrough;             // calls for any other signal
  std::uint32_t lateInstalls;              // slots of provider libraries hooked after they loaded
};
Counts CurrentCounts() noexcept;

}  // namespace nevr_quest::crash_first
