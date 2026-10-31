/* The sentinel's side of keeping our crash handler first (#1605): the handler itself and what it runs, with no hook machinery,
 * so sentinel.cpp (which is built with exceptions) can include it. The hooks that stop the game from displacing the handler are
 * in crash_first_hook.h, built without exceptions.
 */
#pragma once

#include <signal.h>

#include <cstdint>

namespace nevr_quest::crash_first {

// The signals the sentinel's handler owns.
inline constexpr int kGuardedSignals[] = {SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE};
inline constexpr int kGuardedCount = static_cast<int>(sizeof(kGuardedSignals) / sizeof(kGuardedSignals[0]));

// The index of `signo` in kGuardedSignals, or -1.
int GuardedIndex(int signo) noexcept;

// What the crash path does, in order, for the first fatal signal. Each runs on the faulting thread inside the signal
// handler, so each must be async-signal-safe. A null function is skipped. `note` runs last, after the game's handler
// returned (it does not run if the game's handler ends the process): `chained` is whether the game had asked for a handler,
// `displaced` how many times the game asked to install one of the guarded signals' handlers.
using ActionFn = void (*)(int signo, siginfo_t* info, void* ucontext) noexcept;
using NoteFn = void (*)(int signo, bool chained, std::uint32_t displaced) noexcept;
struct Actions {
  ActionFn report;
  ActionFn dump;
  NoteFn note;
};

// Installs our handler for the guarded signals through libc, on an alternate stack for the calling thread when it has none,
// and records the actions that were in place first (the "before the sentinel" view the game is told about). Idempotent.
// Returns the number of signals now handled by OnSignal (kGuardedCount on success). Call it from sentinel::Arm.
int InstallHandlers(const Actions& actions) noexcept;

// Puts the pre-arm actions back (teardown and tests). Safe to call without InstallHandlers.
void RestoreHandlers() noexcept;

// Called by the sentinel's dlopen hook after the game's dlopen returned a handle (integration/post_load.h). When the library
// is one of the provider libraries that import sigaction and signal (libpnsrad, libpnsradmatchmaking, libpnsovr) and is
// mapped, hooks both of its slots the way Install hooks libr15's; a mapping it already hooked is left alone, a new mapping
// of the same library (closed and loaded again) is hooked again. Runs outside any game call, so it may log and lock. Any other
// library is ignored at once.
void OnModuleLoaded(const char* name, void* handle) noexcept;

// The signal handler itself, exposed for the host test (the production code installs it through InstallHandlers).
void OnSignal(int signo, siginfo_t* info, void* ucontext);

// Test support: forgets the remembered actions, the claim, the counts and the installed actions.
void ResetForTest() noexcept;

}  // namespace nevr_quest::crash_first
