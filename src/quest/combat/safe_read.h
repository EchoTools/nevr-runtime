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

// How many reads have been asked for, in this process: a relaxed counter beside a system call, read by the host tests to show that a module added no read
// on a path it must leave alone (an arena level). Nothing in production reads it.
inline std::atomic<std::uint32_t> g_safeReadCalls{0};
inline std::uint32_t SafeReadCalls() noexcept { return g_safeReadCalls.load(std::memory_order_relaxed); }

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
