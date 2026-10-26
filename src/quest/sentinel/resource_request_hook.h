/* The log-only stage of the resource override (#871, steps 2 and 3): a record of every resource request the game makes.
 *
 * What it hooks (pinned_targets.h has the signature, the request layout and the six call sites): CResourceManager::
 * FillResourceFileRequest, the one function every reader of a manifest resource passes through (the base loads, the
 * archive loader, the streamed mips, the forced full mip load) before ReadFileAsync. One GOT hook on its PLT slot in
 * libr15.so, original first: the handler calls the game's function, then looks at what it returned and at the
 * request it filled, and writes nothing to the request. The result, the request and the call are the game's, byte for byte.
 *
 * What it records, once per distinct (type, name, index) and then at the 2nd, 4th, 8th ... repeat, so a request that is
 * made thousands of times does not fill the log: one `resource_request` line with the key (type, name, index), the owner,
 * whether an allocator was passed, the return value, the call site (the game's return address as a pinned virtual
 * address, and its name when it is one of the six), the thread id, and the request's package handle, chunk, chunk
 * offsets and sizes, offset in the chunk, size and alignment. A call that returned nonzero (the manifest has no such
 * resource) is a `resource_request_miss` line with the key, the return value and the call site; the request is not read.
 * What the first run is for: the corpus of what the Quest requests, the mip key and the index-to-level rule, and the
 * threads that ask (docs/adr/0015-quest-resource-request-log.md).
 *
 * Bounds. The key table holds kKeySlots keys and looks at most kProbeLimit slots for one; a key that finds no slot is
 * counted and logged as one `resource_request_table_full` warning, once. At most kMaxLines request lines are written;
 * the next is replaced by one `resource_request_cap` warning, once. Both limits leave the game's call untouched.
 *
 * Counters (hook_report.h), registered by RegisterCounters():
 *   resource_request_calls          the game called the function (the thunk's call counter)
 *   resource_request_thunk_faults   the thunk had no original
 * The rest is per-event lines, not counters: the counter table is shared (kMaxReportCounters).
 *
 * Development builds only. The hook is installed by the constructor sequence only when nevr_build::kIsDev; a release
 * build registers no counters and installs nothing (ctor_sequence.h). The code is in every build (#871 step 5).
 *
 * The handler logs after the original has returned, so no logging frame is live while game code runs. It may run on any
 * thread the game uses for resource requests. Built with -fno-exceptions (callback_thunk.h).
 */
#pragma once

#include <cstdint>

#include "pinned_targets.h"

namespace nevr_quest::resource_request {

using FillThunk = sentinel::pinned::LibR15FillResourceFileRequestThunk;

inline constexpr int kCounterCount = 2;
inline constexpr unsigned kKeySlots = 8192;
inline constexpr unsigned kProbeLimit = 64;
inline constexpr unsigned kMaxLines = 16384;

// Registers the counters. Call before sentinel::StartReporter. Returns false, having logged one line
// {"event":"resource_request_counters","result":"refused",...}, when any was refused.
bool RegisterCounters() noexcept;

// Arms the handler and installs the thunk in libr15.so. One line {"event":"resource_request_install",...} says what
// happened and the libr15 load base the logged call sites are relative to. True only when the slot holds the thunk.
bool Install() noexcept;

// Install() when `countersRegistered`; otherwise logs the skip and installs nothing.
bool InstallIfCounted(bool countersRegistered) noexcept;

// The name of the call site whose game return address is `pinnedReturn` (a pinned virtual address), or "unknown".
const char* SiteName(std::uint64_t pinnedReturn) noexcept;

// Test support: arms the handler without touching a GOT slot and clears the key table, the line count and the flags;
// `loadBase` is the base the caller address is made relative to.
void ArmForTest(std::uint64_t loadBase) noexcept;
void ResetForTest() noexcept;

struct Totals {
  unsigned long long lines;      // request lines written (misses included)
  unsigned long long unlogged;   // requests whose key found no slot, or arrived after the line cap
};
Totals CurrentTotals() noexcept;

}  // namespace nevr_quest::resource_request
