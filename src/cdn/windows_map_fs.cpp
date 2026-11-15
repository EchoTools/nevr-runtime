#include "cdn/windows_map_fs.h"

// NOMINMAX before windows.h: this file is outside the PCH (CMakeLists.txt), as env_windows.cpp is, so its std::min and std::sort
// calls see no min/max macros.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>  // before windows.h (tools/tests/test_header_include_order.py)
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "cdn/env_windows.h"

namespace {

// The width of a FILETIME's high word in a 64-bit stamp, and of a 64-bit file index's.
constexpr unsigned kHighWordShift = 32;
constexpr std::size_t kFileIdHalfBytes = sizeof(std::uint64_t);
// A "X:\" absolute path: drive letter, colon, separator.
constexpr std::size_t kDrivePrefixChars = 3;
constexpr std::size_t kDriveSeparatorIndex = 2;
// The \\?\ prefix GetFinalPathNameByHandleW puts before a drive path, and where that path's colon sits after it.
constexpr wchar_t kLongPathPrefix[] = L"\\\\?\\";
constexpr std::size_t kLongPathPrefixChars = sizeof(kLongPathPrefix) / sizeof(wchar_t) - 1;
constexpr std::size_t kLongPathColonAt = kLongPathPrefixChars + 1;

HANDLE ToHandle(std::int64_t handle) { return reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(handle)); }

bool IsSeparator(wchar_t c) { return c == L'\\' || c == L'/'; }

// A UTF-8 absolute path ("X:\..." or "X:/...") as UTF-16. A relative path would resolve against the game's working directory, and
// an embedded NUL would cut the path short, so both are refused.
bool WinPath(const std::string& path, std::wstring* out) {
  if (path.empty() || path.find('\0') != std::string::npos) return false;
  const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()), nullptr, 0);
  if (n <= 0) return false;
  std::wstring wide(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()), &wide[0], n);
  const bool absolute = wide.size() >= kDrivePrefixChars &&
                        ((wide[0] >= L'A' && wide[0] <= L'Z') || (wide[0] >= L'a' && wide[0] <= L'z')) && wide[1] == L':' &&
                        IsSeparator(wide[kDriveSeparatorIndex]);
  if (!absolute) return false;
  *out = std::move(wide);
  return true;
}

std::string Utf8(const std::wstring& wide) {
  if (wide.empty()) return std::string();
  const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
  if (n <= 0) return std::string();
  std::string out(static_cast<std::size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), &out[0], n, nullptr, nullptr);
  return out;
}

std::uint64_t SizeOf(const BY_HANDLE_FILE_INFORMATION& info) {
  return (static_cast<std::uint64_t>(info.nFileSizeHigh) << kHighWordShift) | info.nFileSizeLow;
}

// Removes `dir` and everything under it, as far as it can. A link inside (a junction, a symlink) is removed itself, never entered.
bool RemoveTreeW(const std::wstring& dir) {
  bool all = true;
  WIN32_FIND_DATAW found;
  const HANDLE search = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &found, FindExSearchNameMatch, nullptr, 0);
  if (search != INVALID_HANDLE_VALUE) {
    do {
      const std::wstring name = found.cFileName;
      if (name == L"." || name == L"..") continue;
      const std::wstring path = dir + L"\\" + name;
      const DWORD attributes = found.dwFileAttributes;
      if (attributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
      bool removed;
      if ((attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        removed = RemoveTreeW(path);
      } else if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
        removed = RemoveDirectoryW(path.c_str()) != 0;
      } else {
        removed = DeleteFileW(path.c_str()) != 0;
      }
      all = all && removed;
    } while (FindNextFileW(search, &found));
    FindClose(search);
  }
  return all && RemoveDirectoryW(dir.c_str()) != 0;
}

class WinMapFs : public nevr_maps::MapFs {
 public:
  bool StampOf(std::int64_t handle, nevr_maps::FileStamp* out) override {
    const HANDLE file = ToHandle(handle);
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(file, &info)) return false;
    nevr_maps::FileStamp stamp;
    stamp.volume = info.dwVolumeSerialNumber;
    stamp.id_lo = (static_cast<std::uint64_t>(info.nFileIndexHigh) << kHighWordShift) | info.nFileIndexLow;
    stamp.size = SizeOf(info);
    stamp.last_write = (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << kHighWordShift) |
                       info.ftLastWriteTime.dwLowDateTime;
    FILE_ID_INFO id;
    if (GetFileInformationByHandleEx(file, FileIdInfo, &id, sizeof(id))) {
      stamp.volume = id.VolumeSerialNumber;
      std::memcpy(&stamp.id_lo, id.FileId.Identifier, kFileIdHalfBytes);
      std::memcpy(&stamp.id_hi, id.FileId.Identifier + kFileIdHalfBytes, kFileIdHalfBytes);
    }
    *out = stamp;
    return true;
  }

  std::string FinalPathOf(std::int64_t handle) override {
    const HANDLE file = ToHandle(handle);
    constexpr DWORD kFlags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    std::wstring path(MAX_PATH, L'\0');
    DWORD n = GetFinalPathNameByHandleW(file, &path[0], static_cast<DWORD>(path.size()), kFlags);
    if (n >= path.size()) {  // too small: n is the length the path needs, its terminator included
      path.assign(n, L'\0');
      n = GetFinalPathNameByHandleW(file, &path[0], static_cast<DWORD>(path.size()), kFlags);
    }
    if (n == 0 || n >= path.size()) return std::string();
    path.resize(n);
    if (path.size() > kLongPathColonAt && path.compare(0, kLongPathPrefixChars, kLongPathPrefix) == 0 &&
        path[kLongPathColonAt] == L':') {
      path.erase(0, kLongPathPrefixChars);
    }
    return Utf8(path);
  }

  // The held open is WindowsEnv's (share-read only, a link is refused); the path must be absolute, as every MapFs path is.
  bool OpenHeld(const std::string& path, std::int64_t* handle, std::uint64_t* size) override {
    std::wstring checked;
    if (!WinPath(path, &checked)) return false;
    return nevr_cdn::WindowsEnv().OpenBlob(path, handle, size);
  }

  bool ListDir(const std::string& dir, std::vector<std::string>* names) override {
    std::wstring wide;
    if (!WinPath(dir, &wide)) return false;
    if (!IsSeparator(wide.back())) wide += L'\\';
    WIN32_FIND_DATAW found;
    const HANDLE search = FindFirstFileExW((wide + L"*").c_str(), FindExInfoBasic, &found, FindExSearchNameMatch, nullptr, 0);
    if (search == INVALID_HANDLE_VALUE) return false;
    std::vector<std::string> listed;
    do {
      const std::wstring name = found.cFileName;
      if (name != L"." && name != L"..") listed.push_back(Utf8(name));
    } while (FindNextFileW(search, &found));
    const bool ended = GetLastError() == ERROR_NO_MORE_FILES;
    FindClose(search);
    if (!ended) return false;
    std::sort(listed.begin(), listed.end());
    *names = std::move(listed);
    return true;
  }

  bool RemoveTree(const std::string& dir) override {
    std::wstring wide;
    if (!WinPath(dir, &wide)) return false;
    while (wide.size() > kDrivePrefixChars && IsSeparator(wide.back())) wide.pop_back();
    const DWORD attributes = GetFileAttributesW(wide.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      const DWORD error = GetLastError();
      return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return RemoveDirectoryW(wide.c_str()) != 0;
    return RemoveTreeW(wide);
  }

  bool RenameDir(const std::string& from, const std::string& to) override {
    std::wstring source, target;
    return WinPath(from, &source) && WinPath(to, &target) &&
           MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
  }
};

}  // namespace

namespace nevr_maps {

std::unique_ptr<MapFs> MakeWinMapFs() { return std::unique_ptr<MapFs>(new WinMapFs()); }

}  // namespace nevr_maps
