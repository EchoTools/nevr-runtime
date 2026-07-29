#pragma once

#include <cstdint>

/// HookLiveness — runtime proof that an installed hook actually runs.
///
/// Three bugs in this ledger were the same defect: a fix wired to a path that
/// never executes.
///   N86  TickPlugins/TickModules wired into CPrecisionSleep::Wait, which is
///        never called in server mode. Plugins got no OnFrame for a day.
///   N83  a guard installed on a function whose identity was invented.
///   N67  a fix landed in a translation unit CMake does not build.
/// Every one passed its gate, because every gate checked SOURCE SHAPE — "the
/// call site exists", "the type is atomic" — where the claim was about RUNTIME
/// BEHAVIOUR. Source-shape checks cannot see this class at all.
///
/// This is the missing measurement: hooks self-report entry counts, and the
/// report names any hook that installed successfully and has never been
/// entered. That is the exact signature of N86, and it would have been visible
/// in the first 30 seconds of the first server run.
///
/// Deliberately cheap: one InterlockedIncrement on a fixed array slot. No heap,
/// no lock, safe from any thread. IDs are compile-time constants so a hot hook
/// costs an increment on a known index.
namespace HookLiveness {

enum Id : int {
  kGetTimeMicroseconds = 0,
  kGetTimeMilliseconds,
  kPrecisionSleepWait,
  kEndMultiplayer,
  kSpinWaitForValue,
  kHttpListenerBringup,
  kBroadcasterListen,
  kBroadcasterReceiveLocal,
  kCount
};

/// Record one entry into the hook. Call as the FIRST statement of the hook body.
void Mark(Id id);

/// Emit one line per hook: name, entries, and whether it was ever entered.
/// Call from a site known to run (see N86 — do not pick one on faith).
void Report(const char* context);

/// Number of hooks with zero entries. A nonzero result is not automatically a
/// bug — some hooks are mode-specific — but every one is a claim needing proof.
int NeverEnteredCount();

}  // namespace HookLiveness
