#pragma once

// Our top-level exception filter stays first on PC (#1605). The game installs its own crash filter with
// SetUnhandledExceptionFilter, and that call replaces whatever was installed: ours would never run. The bookkeeping that
// keeps ours in place is the shared rule in core/crash_first.h (remember the game's request, run ours once, then the game's);
// this file is the PC half of it, and the Quest half is quest/sentinel/crash_first_hook.h.
//
// The filter is the PC equivalent of the Quest signal handler: it is called only for an exception no frame handled, so every
// call is a fatal event. It runs in an exception context: no allocation, no Log() and no lock the faulting thread could hold
// (N70). The wiring that installs it (crash_first_win.h) sits apart from this logic so a GTest drives the logic without
// MinHook or the game.

#include <windows.h>

namespace nevr_crash_first_win {

using Filter = LPTOP_LEVEL_EXCEPTION_FILTER;

// What the first fatal exception runs, on the faulting thread. Null: nothing.
using ReportFn = void (*)(EXCEPTION_POINTERS* info) noexcept;
// Runs after the game's filter returned (it does not run if the game's filter ends the process): `chained` is whether the
// game had a filter to run, `requests` how many times the game asked to install one.
using NoteFn = void (*)(EXCEPTION_POINTERS* info, bool chained, long requests) noexcept;
void SetActions(ReportFn report, NoteFn note) noexcept;

// The game's SetUnhandledExceptionFilter call: `requested` is remembered and NOT installed, so ours stays the top-level
// filter. Returns what the game is told it displaced: its previous request, else the filter that was installed before ours
// (SeedPrior), else null. A filter that is ours is ignored (a game that hands our own filter back would make the chain call
// itself). May be called from any thread; not from the filter.
Filter RememberGameFilter(Filter requested) noexcept;

// Gives the game the filter that was installed before ours as its previous one, and as the one to run after ours. Has no
// effect once the game has made a request of its own.
void SeedPrior(Filter prior) noexcept;

// Ours. Installed with SetUnhandledExceptionFilter by InstallCrashFirstFilter.
LONG WINAPI TopLevelFilter(EXCEPTION_POINTERS* info);

long RequestCount() noexcept;
long ChainedCount() noexcept;

// Test support: forgets the remembered filter, the claim, the counts and the actions.
void ResetForTest() noexcept;

}  // namespace nevr_crash_first_win
