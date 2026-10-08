/* Bounded reporting of the hook counters, from a thread that is not on any game call path.
 *
 * Logging from inside a hooked function is unsafe (hook_log.h), so a hook only increments an
 * atomic counter. A reporter thread, created from the sentinel's ELF constructor before the
 * first hook is installed, logs the counters:
 *
 *   - once, when a counter first becomes non-zero ("first_change"), checked every `firstMs`;
 *   - afterwards only when the value has changed, at most once per `steadyMs`.
 *
 * So a hook that never fires logs nothing after the install line, a hook that fires logs its
 * first call within `firstMs`, and a hot hook logs at most one line per counter per
 * `steadyMs`. Production uses 1 s and 60 s.
 *
 * Teardown. The sentinel is a DT_NEEDED dependency of the game and is never unloaded, so the
 * thread simply ends with the process. StopReporter() (used by tests and available to a
 * teardown path) wakes the thread through a condition variable and joins it; there is no
 * sleep in the loop. The thread is created with pthread_create directly so that no exception
 * can come out of creating it (this library is built without exceptions).
 */
#pragma once

#include <atomic>
#include <cstdint>

namespace sentinel {

// Registers a counter to report (at most 8). `name` must outlive the reporter (a literal).
// Returns false, and logs, when the table is full or the reporter is already running.
bool RegisterReportCounter(const char* name, const std::atomic<std::uint64_t>* value);

// Starts the reporter thread. Idempotent. Returns false, and logs one error line, when the
// thread cannot be created.
bool StartReporter(unsigned firstMs, unsigned steadyMs);

// Wakes and joins the reporter thread, and forgets the registered counters. Safe to call
// when it is not running.
void StopReporter();

}  // namespace sentinel
