#pragma once
#include "core/pch.h"

/// <summary>
/// Copies memory from a source buffer of a given size to a destination process memory buffer,
/// lifting the page protection for the write and restoring it afterwards. The one memory-patching
/// idiom in the runtime.
/// </summary>
/// <param name="pDestAddr">The process address where the source buffer should be copied to.</param>
/// <param name="pSrcAddr">The source buffer to copy to process memory.</param>
/// <param name="szSrcSize">The size of the data to copy.</param>
/// <param name="pOutError">If non-null, receives GetLastError() when the copy failed.</param>
/// <returns>true if the bytes were written.</returns>
inline bool ProcessMemcpy(PVOID pDestAddr, const void* pSrcAddr, size_t szSrcSize, DWORD* pOutError = nullptr) {
  DWORD dwOldProtect;
  if (!VirtualProtect(pDestAddr, szSrcSize, PAGE_EXECUTE_READWRITE, &dwOldProtect)) {
    if (pOutError != nullptr) *pOutError = GetLastError();
    return false;
  }
  SIZE_T written = 0;
  const BOOL wrote = WriteProcessMemory(GetCurrentProcess(), pDestAddr, pSrcAddr, szSrcSize, &written);
  if (pOutError != nullptr) *pOutError = wrote ? 0 : GetLastError();
  VirtualProtect(pDestAddr, szSrcSize, dwOldProtect, &dwOldProtect);
  return wrote && written == szSrcSize;
}
