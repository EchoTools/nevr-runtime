#include "runtime/host/crash_send_files.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <exception>
#include <string>
#include <vector>

namespace nevr_host {
namespace {

// One ReadFile call asks for at most this much (the DWORD length of the call is not the limit).
constexpr std::size_t kReadChunkBytes = 0x100000;

// FILETIME is two 32-bit halves; the entry's age is the 64-bit value they make.
constexpr unsigned kFileTimeHighWordShift = 32;

// A handle that is closed on every way out of a function, a thrown std::bad_alloc included.
struct FileHandle {
  HANDLE handle;
  ~FileHandle() {
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
  }
  FileHandle(const FileHandle&) = delete;
  FileHandle& operator=(const FileHandle&) = delete;
};

// The same for the handle FindFirstFileW returns, which FindClose ends.
struct FindHandle {
  HANDLE handle;
  ~FindHandle() {
    if (handle != INVALID_HANDLE_VALUE) FindClose(handle);
  }
  FindHandle(const FindHandle&) = delete;
  FindHandle& operator=(const FindHandle&) = delete;
};

bool Wide(const std::string& utf8, std::wstring* out) {
  out->clear();
  if (utf8.empty()) return true;
  const int need = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  if (need <= 0) return false;
  out->assign(static_cast<std::size_t>(need), L'\0');
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), &(*out)[0], need) == need;
}

bool Narrow(const wchar_t* wide, int length, std::string* out) {
  out->clear();
  if (length == 0) return true;
  const int need = WideCharToMultiByte(CP_UTF8, 0, wide, length, nullptr, 0, nullptr, nullptr);
  if (need <= 0) return false;
  out->assign(static_cast<std::size_t>(need), '\0');
  return WideCharToMultiByte(CP_UTF8, 0, wide, length, &(*out)[0], need, nullptr, nullptr) == need;
}

bool List(const std::string& dir, std::size_t limit, std::vector<nevr_crash_send::DirectoryEntry>* out) try {
  std::wstring pattern;
  if (!Wide(dir, &pattern) || pattern.empty()) return false;
  if (pattern.back() != L'\\' && pattern.back() != L'/') pattern += L'\\';
  pattern += L'*';
  WIN32_FIND_DATAW data;
  const FindHandle find{FindFirstFileW(pattern.c_str(), &data)};
  if (find.handle == INVALID_HANDLE_VALUE) return false;
  do {
    if (out->size() >= limit) break;
    const std::wstring name = data.cFileName;
    if (name == L"." || name == L"..") continue;
    nevr_crash_send::DirectoryEntry entry;
    if (!Narrow(name.c_str(), static_cast<int>(name.size()), &entry.name)) continue;
    entry.regular = (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
    entry.modified = (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << kFileTimeHighWordShift) | data.ftLastWriteTime.dwLowDateTime;
    out->push_back(std::move(entry));
  } while (FindNextFileW(find.handle, &data) != FALSE);
  return true;
} catch (const std::exception&) {
  return false;
}

bool Read(const std::string& path, std::size_t maxBytes, std::string* out) try {
  std::wstring wide;
  if (!Wide(path, &wide) || wide.empty()) return false;
  const FileHandle file{CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
  if (file.handle == INVALID_HANDLE_VALUE) return false;
  out->assign(maxBytes, '\0');
  std::size_t got = 0;
  bool ok = true;
  while (got < maxBytes) {
    DWORD n = 0;
    const DWORD ask = static_cast<DWORD>(std::min<std::size_t>(maxBytes - got, kReadChunkBytes));
    if (ReadFile(file.handle, &(*out)[got], ask, &n, nullptr) == FALSE) {
      ok = false;
      break;
    }
    if (n == 0) break;
    got += n;
  }
  out->resize(got);
  return ok;
} catch (const std::exception&) {
  return false;
}

bool Write(const std::string& path, const std::string& bytes) try {
  std::wstring wide;
  if (!Wide(path, &wide) || wide.empty()) return false;
  FileHandle file{CreateFileW(wide.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
  if (file.handle == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const bool ok = WriteFile(file.handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != FALSE &&
                  written == bytes.size() && FlushFileBuffers(file.handle) != FALSE;
  const bool closed = CloseHandle(file.handle) != FALSE;  // the close's own result is part of the answer
  file.handle = INVALID_HANDLE_VALUE;
  return closed && ok;
} catch (const std::exception&) {
  return false;
}

bool Rename(const std::string& from, const std::string& to) try {
  std::wstring a;
  std::wstring b;
  if (!Wide(from, &a) || !Wide(to, &b) || a.empty() || b.empty()) return false;
  return MoveFileExW(a.c_str(), b.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
} catch (const std::exception&) {
  return false;
}

void Remove(const std::string& path) try {
  std::wstring wide;
  if (Wide(path, &wide) && !wide.empty()) DeleteFileW(wide.c_str());
} catch (const std::exception&) {
  // Out of memory building the wide name: the file stays, and the next start looks at it again.
}

}  // namespace

const nevr_crash_send::Files* CrashSendFiles() {
  static const nevr_crash_send::Files files = {&List, &Read, &Write, &Rename, &Remove};
  return &files;
}

std::string Utf8FromWide(const wchar_t* wide) try {
  std::string out;
  if (wide == nullptr) return out;
  const std::size_t length = wcslen(wide);
  return Narrow(wide, static_cast<int>(length), &out) ? out : std::string();
} catch (const std::exception&) {
  return std::string();
}

}  // namespace nevr_host
