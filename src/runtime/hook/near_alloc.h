#pragma once

// Memory within a signed 32-bit displacement of a code address, for a patch that rewrites a
// rip-relative operand to point at a buffer we own (#938). The technique MinHook uses for its
// trampolines: walk the address space with VirtualQuery and take the first free region.

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace nevr_near_alloc {

/// How far from the anchor a block may be. Short of 2 GiB so that a second address a few hundred
/// bytes from the anchor, and the block's own size, still fit a rel32.
inline constexpr uintptr_t kMaxDistance = 0x70000000;
/// Most VirtualQuery steps per direction, so a crowded address space ends the search.
inline constexpr int kMaxSteps = 4096;

inline uintptr_t RoundUp(uintptr_t v, uintptr_t unit) { return (v + unit - 1) / unit * unit; }
inline uintptr_t RoundDown(uintptr_t v, uintptr_t unit) { return v / unit * unit; }

/// Commits `size` bytes (read-write) within kMaxDistance of `anchor`; nullptr when none is free.
inline void* AllocateNear(uintptr_t anchor, size_t size) {
  SYSTEM_INFO info{};
  GetSystemInfo(&info);
  const uintptr_t unit = info.dwAllocationGranularity != 0 ? info.dwAllocationGranularity : 0x10000;
  const uintptr_t minAddr = reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress);
  const uintptr_t maxAddr = reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);
  const uintptr_t low = anchor > kMaxDistance + minAddr ? anchor - kMaxDistance : minAddr;
  const uintptr_t high = anchor < maxAddr - kMaxDistance ? anchor + kMaxDistance : maxAddr;

  // Upward from the anchor.
  uintptr_t at = RoundUp(anchor, unit);
  for (int step = 0; step < kMaxSteps && at + size < high; ++step) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(at), &mbi, sizeof(mbi)) == 0) break;
    const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    if (mbi.State == MEM_FREE && regionEnd - at >= size) {
      void* p = VirtualAlloc(reinterpret_cast<LPVOID>(at), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
      if (p != nullptr) return p;
    }
    at = RoundUp(regionEnd, unit);
  }

  // Downward from the anchor.
  at = RoundDown(anchor > size ? anchor - size : 0, unit);
  for (int step = 0; step < kMaxSteps && at > low; ++step) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(at), &mbi, sizeof(mbi)) == 0) break;
    const uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    if (mbi.State == MEM_FREE) {
      const uintptr_t regionEnd = regionBase + mbi.RegionSize;
      const uintptr_t candidate = RoundDown(regionEnd - size, unit);
      if (regionEnd >= size && candidate >= regionBase && candidate >= low) {
        void* p = VirtualAlloc(reinterpret_cast<LPVOID>(candidate), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (p != nullptr) return p;
      }
    }
    const uintptr_t allocBase = reinterpret_cast<uintptr_t>(mbi.AllocationBase != nullptr ? mbi.AllocationBase : mbi.BaseAddress);
    if (allocBase <= unit) break;
    at = RoundDown(allocBase - 1, unit);
  }
  return nullptr;
}

}  // namespace nevr_near_alloc
