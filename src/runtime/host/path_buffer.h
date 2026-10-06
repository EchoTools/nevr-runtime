#pragma once

// Fixed-buffer path helpers for the host-side units (design step 3). No heap, no
// I/O, no locks: safe under the loader lock and from the crash path.

#include <windows.h>

#include <cstddef>
#include <cstring>
#include <cwchar>

namespace NevrHost {

// Append <base>\<suffix> into dst (dst_size wide characters), returning false on
// an empty base or truncation. base and suffix are NUL-terminated.
inline bool AppendPath(wchar_t* dst, std::size_t dst_size, const wchar_t* base,
                       const wchar_t* suffix) {
  if (dst == nullptr || base == nullptr || suffix == nullptr || dst_size == 0) return false;
  const std::size_t base_len = wcslen(base);
  if (base_len == 0) return false;
  const std::size_t suffix_len = wcslen(suffix);
  const std::size_t separator = base[base_len - 1] == L'\\' ? 0 : 1;
  if (base_len + separator + suffix_len + 1 > dst_size) return false;
  std::memcpy(dst, base, base_len * sizeof(wchar_t));
  std::size_t at = base_len;
  if (separator != 0) dst[at++] = L'\\';
  std::memcpy(dst + at, suffix, (suffix_len + 1) * sizeof(wchar_t));
  return true;
}

// Copy %name% into dst (dst_size wide characters) with GetEnvironmentVariableW.
// An unset or truncated variable yields false and an empty dst.
inline bool CopyEnv(wchar_t* dst, std::size_t dst_size, const wchar_t* name) {
  if (dst == nullptr || dst_size == 0) return false;
  dst[0] = L'\0';
  const DWORD length = GetEnvironmentVariableW(name, dst, static_cast<DWORD>(dst_size));
  if (length == 0 || length >= dst_size) {
    dst[0] = L'\0';
    return false;
  }
  return true;
}

// Create `path` and every missing intermediate directory (mkdir -p), using a
// temporary NUL at each separator. Mutates `path` transiently and restores it.
// Returns false only for a malformed path (empty / null); access problems are
// left to the caller's CreateFileW, which reports them precisely.
inline bool CreateDirectories(wchar_t* path) {
  if (path == nullptr || path[0] == L'\0') return false;
  std::size_t at = 0;
  while (path[at] == L'\\' || path[at] == L'/') ++at;  // UNC prefix
  if (path[at] != L'\0' && path[at + 1] == L':') at += 2;  // drive letter
  for (; path[at] != L'\0'; ++at) {
    if (path[at] != L'\\' && path[at] != L'/') continue;
    const wchar_t saved = path[at];
    path[at] = L'\0';
    CreateDirectoryW(path, nullptr);
    path[at] = saved;
  }
  CreateDirectoryW(path, nullptr);
  return true;
}

// Write the directory containing `module` (with a trailing backslash) into dst.
inline bool DirOfModule(wchar_t* dst, std::size_t dst_size, HMODULE module) {
  if (dst == nullptr || dst_size == 0) return false;
  dst[0] = L'\0';
  const DWORD length = GetModuleFileNameW(module, dst, static_cast<DWORD>(dst_size));
  if (length == 0 || length >= dst_size) {
    dst[0] = L'\0';
    return false;
  }
  wchar_t* separator = wcsrchr(dst, L'\\');
  if (separator == nullptr) {
    dst[0] = L'\0';
    return false;
  }
  separator[1] = L'\0';
  return true;
}

}  // namespace NevrHost
