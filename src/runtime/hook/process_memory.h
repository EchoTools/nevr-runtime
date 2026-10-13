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
/// <param name="pOutError">If non-null, receives the first write, protection, or cache error.</param>
/// <returns>true if the bytes were written, protection restored, and the instruction cache flushed.</returns>
/// The three system calls are parameters so a test can make each one fail on demand; production
/// passes the real ones through ProcessMemcpy below. `protect` has VirtualProtect's signature, `write`
/// WriteProcessMemory's, `flush` FlushInstructionCache's. The first error wins: a later failure
/// never replaces the one that was already recorded.
template <typename Protect, typename Write, typename Flush>
inline bool ProcessMemcpyWithOps(PVOID pDestAddr, const void* pSrcAddr, size_t szSrcSize, DWORD* pOutError,
                                 Protect&& protect, Write&& write, Flush&& flush) {
  DWORD dwOldProtect = 0;
  if (!protect(pDestAddr, szSrcSize, PAGE_EXECUTE_READWRITE, &dwOldProtect)) {
    if (pOutError != nullptr) *pOutError = GetLastError();
    return false;
  }
  SIZE_T written = 0;
  const BOOL wrote = write(GetCurrentProcess(), pDestAddr, pSrcAddr, szSrcSize, &written);
  DWORD error = wrote ? (written == szSrcSize ? ERROR_SUCCESS : ERROR_PARTIAL_COPY) : GetLastError();
  DWORD ignoredProtect = 0;
  if (!protect(pDestAddr, szSrcSize, dwOldProtect, &ignoredProtect) && error == ERROR_SUCCESS) {
    error = GetLastError();
  }
  // Even a partial write may have changed executable bytes. Flush what landed.
  if (written != 0 && !flush(GetCurrentProcess(), pDestAddr, written) &&
      error == ERROR_SUCCESS) {
    error = GetLastError();
  }
  if (pOutError != nullptr) *pOutError = error;
  return error == ERROR_SUCCESS;
}

template <typename Flush>
inline bool ProcessMemcpyWithFlush(PVOID pDestAddr, const void* pSrcAddr, size_t szSrcSize,
                                   DWORD* pOutError, Flush&& flush) {
  return ProcessMemcpyWithOps(
      pDestAddr, pSrcAddr, szSrcSize, pOutError,
      [](LPVOID address, SIZE_T size, DWORD newProtect, PDWORD oldProtect) {
        return VirtualProtect(address, size, newProtect, oldProtect);
      },
      [](HANDLE process, LPVOID address, LPCVOID source, SIZE_T size, SIZE_T* written) {
        return WriteProcessMemory(process, address, source, size, written);
      },
      static_cast<Flush&&>(flush));
}

inline bool ProcessMemcpy(PVOID pDestAddr, const void* pSrcAddr, size_t szSrcSize, DWORD* pOutError = nullptr) {
  return ProcessMemcpyWithFlush(pDestAddr, pSrcAddr, szSrcSize, pOutError,
                                [](HANDLE process, LPCVOID address, SIZE_T size) {
                                  return FlushInstructionCache(process, address, size);
                                });
}
