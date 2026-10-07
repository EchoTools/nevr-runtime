#pragma once
#include "core/pch.h"

/// <summary>
/// Copies memory from a source buffer of a given size to a destination process memory buffer.
/// </summary>
/// <param name="pDestAddr">The process address where the source buffer should be copied to.</param>
/// <param name="pSrcAddr">The source buffer to copy to process memory.</param>
/// <param name="szSrcSize">The size of the data to copy.</param>
/// <returns>None</returns>
inline VOID ProcessMemcpy(PVOID pDestAddr, PVOID pSrcAddr, size_t szSrcSize) {
  // Change the memory protection on the given address range, write process memory, then restore the original
  // protection.
  DWORD dwOldProtect;
  if (VirtualProtect(pDestAddr, szSrcSize, PAGE_EXECUTE_READWRITE, &dwOldProtect)) {
    WriteProcessMemory(GetCurrentProcess(), pDestAddr, pSrcAddr, szSrcSize, NULL);
    VirtualProtect(pDestAddr, szSrcSize, dwOldProtect, &dwOldProtect);
  }
}
