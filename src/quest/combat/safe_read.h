/* A read of game memory that never faults: process_vm_readv on our own pid, for which an unmapped address is EFAULT and never a signal.
 * The combat modules read the game's objects through it (the q15t plugin's `SafeRead`, reconstructed; a scan of /proc/self/maps per read,
 * which mapping_perms.h does, is too dear for a per-frame or twice-a-second read). Header only.
 */
#pragma once

#include <sys/uio.h>
#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace nevr_quest::combat {

// Calls of SafeRead since the last reset: the test seam behind the arena tests' "reads nothing" count (docs/adr/0046: a module that is scoped out reads
// no game memory). One relaxed increment beside a system call.
inline std::atomic<std::uint64_t> g_safeReadCalls{0};
inline std::uint64_t SafeReadCallCount() noexcept { return g_safeReadCalls.load(std::memory_order_relaxed); }
inline void ResetSafeReadCallCount() noexcept { g_safeReadCalls.store(0, std::memory_order_relaxed); }

// Copies `length` bytes at `address` into `out`. False when any byte could not be read (nothing is promised about `out` then), for a
// zero or implausible address, and for a range that wraps.
inline bool SafeRead(std::uintptr_t address, void* out, std::size_t length) noexcept {
  g_safeReadCalls.fetch_add(1, std::memory_order_relaxed);
  if (length == 0) return true;
  if (address < 0x10000 || address > 0x0000ffffffffffffULL - length) return false;
  iovec local{out, length};
  iovec remote{reinterpret_cast<void*>(address), length};
  return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(length);
}

// One value of type T at `address`; `*ok` is cleared when it could not be read (and left alone otherwise).
template <typename T>
T SafeRead(std::uintptr_t address, bool* ok) noexcept {
  T value{};
  if (!SafeRead(address, &value, sizeof(value))) *ok = false;
  return value;
}

}  // namespace nevr_quest::combat
