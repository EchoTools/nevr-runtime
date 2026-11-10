#pragma once

// The byte ranges this runtime has written into the game's memory, so a later writer can ask whether its own range
// overlaps one of ours before it writes. ProcessMemcpy (process_memory.h), the runtime's one memory-patching idiom,
// records every range it wrote; a detour is recorded by HookGuard (hook_guard.h), and nevr_hook_guard::FindOverlap
// answers for both.
//
// Header-only with process-global inline storage, so every file that includes process_memory.h shares one table
// without a link dependency. Fixed capacity, no heap: ProcessMemcpy runs at boot, some of it before the game's log
// exists. A record that does not fit is counted (DroppedCount), never silent: FindOverlap's callers log the count.

#include <winsock2.h>
#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace nevr_write_ranges {

/// Whether [a, a + aLen) and [b, b + bLen) share a byte. An empty range overlaps nothing.
constexpr bool Overlaps(std::uintptr_t a, std::size_t aLen, std::uintptr_t b, std::size_t bLen) {
  return aLen != 0 && bLen != 0 && a < b + bLen && b < a + aLen;
}

struct Range {
  std::uintptr_t start;
  std::size_t length;
  const char* name;  // a string literal from the writer; not owned
};

/// Recorded ranges. The runtime's byte patches are a few dozen; a code cave or a data cell a writer adds is one each.
constexpr int kCapacity = 512;

namespace detail {
inline Range g_ranges[kCapacity];
inline LONG g_count = 0;
inline LONG g_dropped = 0;
inline SRWLOCK g_lock = SRWLOCK_INIT;
}  // namespace detail

/// Records [start, start + length) under `name`. A range already recorded with the same start and length is not
/// recorded twice (a patch put back writes its own range again).
inline void Record(const void* start, std::size_t length, const char* name) {
  if (start == nullptr || length == 0) return;
  const auto s = reinterpret_cast<std::uintptr_t>(start);
  AcquireSRWLockExclusive(&detail::g_lock);
  for (LONG i = 0; i < detail::g_count; ++i) {
    if (detail::g_ranges[i].start == s && detail::g_ranges[i].length == length) {
      ReleaseSRWLockExclusive(&detail::g_lock);
      return;
    }
  }
  if (detail::g_count < kCapacity) {
    detail::g_ranges[detail::g_count++] = Range{s, length, name != nullptr ? name : "(unnamed)"};
  } else {
    InterlockedIncrement(&detail::g_dropped);
  }
  ReleaseSRWLockExclusive(&detail::g_lock);
}

/// The first recorded range that overlaps [start, start + length), copied to `*hit`. False when none does.
inline bool FindOverlap(std::uintptr_t start, std::size_t length, Range* hit) {
  bool found = false;
  AcquireSRWLockShared(&detail::g_lock);
  for (LONG i = 0; i < detail::g_count && !found; ++i) {
    const Range& r = detail::g_ranges[i];
    if (Overlaps(start, length, r.start, r.length)) {
      if (hit != nullptr) *hit = r;
      found = true;
    }
  }
  ReleaseSRWLockShared(&detail::g_lock);
  return found;
}

/// Record() calls that found the table full: writes nothing checks against.
inline int DroppedCount() { return static_cast<int>(InterlockedCompareExchange(&detail::g_dropped, 0, 0)); }

#ifdef NEVR_TEST_HOOKS
inline void ResetForTest() {
  AcquireSRWLockExclusive(&detail::g_lock);
  detail::g_count = 0;
  detail::g_dropped = 0;
  ReleaseSRWLockExclusive(&detail::g_lock);
}
#endif

}  // namespace nevr_write_ranges
