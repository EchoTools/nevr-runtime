#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <limits>

namespace CrashRecovery {

inline bool IsReadableProtection(DWORD protection) {
  switch (protection & 0xffU) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
      return true;
    default:
      return false;
  }
}

// Verifies that every byte in [address, address + length) is in committed,
// readable memory. This is safe for crash-handler probes: it uses VirtualQuery
// only and never dereferences the requested address.
inline bool IsReadableMemory(const void* address, size_t length) {
  if (address == nullptr || length == 0) return false;

  uintptr_t cursor = reinterpret_cast<uintptr_t>(address);
  const uintptr_t end = cursor + length;
  if (end < cursor) return false;

  while (cursor < end) {
    MEMORY_BASIC_INFORMATION info = {};
    if (VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info)) == 0) {
      return false;
    }
    if (info.State != MEM_COMMIT || !IsReadableProtection(info.Protect)) return false;

    const uintptr_t regionStart = reinterpret_cast<uintptr_t>(info.BaseAddress);
    uintptr_t regionEnd = regionStart + info.RegionSize;
    if (regionEnd < regionStart) regionEnd = std::numeric_limits<uintptr_t>::max();
    if (regionEnd <= cursor) return false;
    cursor = regionEnd < end ? regionEnd : end;
  }
  return true;
}

}  // namespace CrashRecovery
