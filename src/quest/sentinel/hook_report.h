/* Bounded reporting of the hook counters, from a thread that is not on any game call path.
 *
 * Logging from inside a hooked function is unsafe (hook_log.h), so a hook only increments an
 * atomic counter. A reporter thread, created from the sentinel's ELF constructor before the
 * first hook is installed, logs the counters:
 *
 *   - "reporter_started" once, with its three intervals;
 *   - during the first `graceMs`, it wakes every `firstMs` and logs a counter ONCE, the first
 *     time it is non-zero ("first_change");
 *   - when the grace window ends it logs "never_fired" ONCE for each calls counter that is
 *     still zero: the hook was not called in that window. That is also what a failed install
 *     looks like (the reporter starts before the install), so read it together with the
 *     got_hook install line. A counter registered as ReportKind::kFaults is never reported
 *     never_fired: zero faults is the healthy state;
 *   - from then on every counter is on the steady cadence: one pass per `steadyMs`, and a
 *     counter is logged only if its value changed ("changed"); a counter that fires for the
 *     first time after the grace window is logged "first_change" at the next steady pass.
 *
 * So the total rate is bounded: at most one line per counter during the grace window, then
 * at most one line per counter per `steadyMs`, no matter how many counters never fire.
 * Production uses 1 s, 10 s and 60 s.
 *
 * Teardown. The sentinel is a DT_NEEDED dependency of the game and is never unloaded, so the
 * thread simply ends with the process and StopReporter() is never called at process exit
 * (a dlclose of the sentinel would unmap code the thread is running and crash it; nothing
 * dlcloses it). StopReporter() wakes the thread through a condition variable and joins it, and
 * is used by tests. There is no sleep in the loop. The thread is created with pthread_create
 * directly so no exception can come out of creating it. Whether pthread_create works from an
 * ELF constructor on the headset is inferred from the Bionic main-branch source (the loader
 * does not hold a lock that thread creation needs) and has not been verified on a Quest.
 *
 * fork(): the reporter takes its mutex only to read the counter table, and never logs while
 * holding it. pthread_atfork handlers hold the mutex across the fork so the child does not
 * inherit it locked; the child has no reporter thread, and StopReporter in the child does not
 * try to join one.
 */
#pragma once

#include <atomic>
#include <cstdint>

namespace sentinel {

enum class ReportKind { kCalls, kFaults };

// Registers a counter to report (at most 8). `name` must outlive the reporter (a literal).
// Returns false, and logs, when the table is full or the reporter is already running.
bool RegisterReportCounter(const char* name, const std::atomic<std::uint64_t>* value,
                           ReportKind kind = ReportKind::kCalls);

// Starts the reporter thread. Idempotent. Returns false, and logs one error line, when the
// thread cannot be created.
bool StartReporter(unsigned firstMs, unsigned graceMs, unsigned steadyMs);

// Wakes and joins the reporter thread, and forgets the registered counters. Safe to call
// when it is not running, and safe against a concurrent StartReporter/StopReporter: one lock
// covers the whole start or stop, join included.
void StopReporter();

// Whether the reporter thread is running.
bool ReporterRunning();

}  // namespace sentinel
