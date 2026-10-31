/* The resource request hook (#871 steps 2 and 3; the loose-file overlay of #1737 C5): a record of every resource request
 * the game makes, and the loose-file overlay that serves a request the manifest does not hold.
 *
 * What it hooks (pinned_targets.h has the signature, the request layout and the six call sites): CResourceManager::
 * FillResourceFileRequest, the one function every reader of a manifest resource passes through (the base loads, the
 * archive loader, the streamed mips, the forced full mip load) before ReadFileAsync. One GOT hook on its PLT slot in
 * libr15.so, original first, except on a manifest miss: the handler decides the miss itself (a binary search of the
 * manifest, the same one the game runs), and then serves a file from the platform data directory the way the reference
 * build's cave does (`qcp_patches.py` P9): GetUnpackagedResourcePath, CSysFile::Open and CSysFile::Size, then the
 * request's owner, index, handle, size, allocator and valid word. If any of the three fails, the game's function runs
 * as it would have, and raises its own "Cannot find resource". On a hit the game's call, its arguments, its result and
 * the request it fills are the game's, byte for byte.
 *
 * What it records (log lines are development builds only; counters are in every build): once per distinct
 * (type, name, index) and then at the 2nd, 4th, 8th ... repeat, so a request that is made thousands of times does not
 * fill the log: one `resource_request` line with the key (type, name, index), the owner, whether an allocator was
 * passed, the return value, the call site (the game's return address as a pinned virtual address, and its name when it
 * is one of the six), the thread id, and the request's package handle, chunk, chunk offsets and sizes, offset in the
 * chunk, size and alignment. A call that returned nonzero (the manifest has no such resource) is a
 * `resource_request_miss` line with the key, the return value and the call site; the request is not read. A served
 * overlay file is one `resource_overlay_served` line and a failed stage one `resource_overlay_failed` line, at most
 * kOverlayLines of them in all (docs/adr/0018-quest-resource-request-log.md).
 *
 * Bounds. The key table holds kKeySlots keys and looks at most kProbeLimit slots for one; a key that finds no slot is
 * counted and logged as one `resource_request_table_full` warning, once. At most kMaxLines request lines are written;
 * the next is replaced by one `resource_request_cap` warning, once. Both limits leave the game's call untouched.
 *
 * Counters (hook_report.h), registered by RegisterCounters():
 *   resource_request_calls          the game called the function (the thunk's call counter)
 *   resource_request_thunk_faults   the thunk had no original
 *   resource_overlay_served         manifest misses served from a loose file
 *   resource_overlay_failed         manifest misses whose loose file could not be served (the game's error follows)
 *
 * Every build. The constructor sequence installs the hook in every build (ctor_sequence.h); nevr_build::kIsDev decides
 * only whether the log lines are written. The handler runs its overlay with no frame live across game code that holds
 * a destructor, and it logs after the original has returned, so no logging frame is live while game code runs. It may
 * run on any thread the game uses for resource requests. Built with -fno-exceptions (callback_thunk.h).
 */
#pragma once

#include <cstdint>

#include "pinned_targets.h"

namespace nevr_quest::resource_request {

using FillThunk = sentinel::pinned::LibR15FillResourceFileRequestThunk;

inline constexpr int kCounterCount = 4;
inline constexpr unsigned kKeySlots = 8192;
inline constexpr unsigned kProbeLimit = 64;
inline constexpr unsigned kMaxLines = 16384;
// The overlay's served and failed lines share this cap (one `resource_overlay_cap` line when they are reached).
inline constexpr unsigned kOverlayLines = 32;

// The libr15 functions the overlay calls, as the dynamic linker resolves them (ResolveGameFnsForTest). Each member is the
// function at the pinned vaddr; the signatures are pinned_targets.h's.
struct GameFns {
  void (*clearRequest)(unsigned char* request);
  int (*unpackagedPath)(const sentinel::pinned::CResourceManagerOpaque* manager, std::uint64_t type, std::uint64_t name,
                        unsigned char* path);
  int (*open)(const char* path, unsigned flags, long* handle);
  int (*size)(long handle, unsigned long long* size);
};

// Maps a symbol name to its address in libr15 (nullptr when it is not there).
using SymbolLookup = void* (*)(const char* symbol);

// Registers the counters. Call before sentinel::StartReporter. Returns false, having logged one line
// {"event":"resource_request_counters","result":"refused",...}, when any was refused.
bool RegisterCounters() noexcept;

// Arms the handler and installs the thunk in libr15.so, and resolves the overlay's game functions (one WARN line per symbol
// that does not resolve to its pinned address; the overlay is then off and the slot still installs). One line
// {"event":"resource_request_install",...} says what happened, the libr15 load base the logged call sites are relative to,
// the overlay ("ready" or the refusing symbol), whether the lines are written and that the counters were registered. True
// only when the slot holds the thunk. `logRequests` is nevr_build::kIsDev. Same as InstallCounted(true, logRequests): the
// caller has registered the counters (RegisterCounters).
bool Install(bool logRequests) noexcept;

// Installs whatever the counters did: the install line says counters "registered" or "refused". The hook and the overlay do
// not depend on the counters; only the reporter's view of them does.
bool InstallCounted(bool countersRegistered, bool logRequests) noexcept;

// The name of the call site whose game return address is `pinnedReturn` (a pinned virtual address), or "unknown".
const char* SiteName(std::uint64_t pinnedReturn) noexcept;

// Test support: arms the handler without touching a GOT slot, clears the key table, the line count and the flags.
// `loadBase` is the base the caller address is made relative to; `logRequests` decides whether lines are written.
void ArmForTest(std::uint64_t loadBase, bool logRequests) noexcept;
// Test support: the game functions the overlay calls (nullptr: the overlay is unavailable and a miss passes to the game).
void SetGameFnsForTest(const GameFns* fns) noexcept;
// Test support: resolves the four symbols through `lookup` against `base`. Returns nullptr when every one resolves to
// base + its pinned vaddr (and writes `out`); otherwise the name of the first symbol that does not, after one WARN line
// per refused symbol {"event":"resource_overlay_resolve",...}.
const char* ResolveGameFnsForTest(std::uint64_t base, SymbolLookup lookup, GameFns* out) noexcept;
void ResetForTest() noexcept;

struct Totals {
  unsigned long long lines;      // request lines written (misses included)
  unsigned long long unlogged;   // requests whose key found no slot, or arrived after the line cap
};
Totals CurrentTotals() noexcept;

struct OverlayTotals {
  unsigned long long served;  // manifest misses served from a loose file
  unsigned long long failed;  // manifest misses whose loose file could not be served (the game's error follows)
};
OverlayTotals CurrentOverlayTotals() noexcept;

}  // namespace nevr_quest::resource_request
