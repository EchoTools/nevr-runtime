#include "runtime/lifecycle/update_fs_win32.h"

#include <windows.h>

#include <exception>
#include <new>
#include <string>
#include <utility>

#include "core/update/names.h"

namespace nevr_update {
namespace {

namespace names = nevr_update_names;

bool IsReparsePoint(const std::wstring& path) {
  const DWORD a = GetFileAttributesW(path.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

// Removes a folder and what is in it. Never follows a link: a reparse point is removed itself and not entered.
void RemoveTreeWide(const std::wstring& dir) {
  if (!IsReparsePoint(dir)) {
    WIN32_FIND_DATAW f;
    const HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &f);
    if (h != INVALID_HANDLE_VALUE) {
      do {
        const std::wstring name = f.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring child = dir + L"\\" + name;
        if ((f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
          RemoveTreeWide(child);
        } else {
          SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
          DeleteFileW(child.c_str());
        }
      } while (FindNextFileW(h, &f) != FALSE);
      FindClose(h);
    }
  }
  RemoveDirectoryW(dir.c_str());
}

// The host's own flags (runtime/host/update_swap.cpp, UpdateLock::Acquire): exclusive, delete on close.
class Win32Lock final : public FsLock {
 public:
  explicit Win32Lock(HANDLE handle) : handle_(handle) {}
  ~Win32Lock() override { Release(); }
  void Release() noexcept override {
    if (handle_ == INVALID_HANDLE_VALUE) return;
    CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
  }

 private:
  HANDLE handle_;
};

class Win32Fs final : public UpdateFs {
 public:
  char Separator() const noexcept override { return '\\'; }

  // A file of update\, at most the cap the host applies (ADR 0024, section 4.3).
  ReadResult ReadSmallFile(const std::string& utf8, std::string* out) const noexcept override {
    try {
      std::wstring path;
      if (!Utf8ToWide(utf8, &path)) return ReadResult::kUnreadable;
      const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        return (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? ReadResult::kAbsent : ReadResult::kUnreadable;
      }
      LARGE_INTEGER size{};
      if (GetFileSizeEx(file, &size) == FALSE) {
        CloseHandle(file);
        return ReadResult::kUnreadable;
      }
      if (size.QuadPart < 0 || static_cast<unsigned long long>(size.QuadPart) > names::kMaxJsonBytes) {
        CloseHandle(file);
        return ReadResult::kTooBig;
      }
      std::string text(static_cast<std::size_t>(size.QuadPart), '\0');
      DWORD got = 0;
      const BOOL ok = text.empty() ? TRUE : ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &got, nullptr);
      CloseHandle(file);
      if (ok == FALSE || got != text.size()) return ReadResult::kUnreadable;
      *out = std::move(text);
      return ReadResult::kOk;
    } catch (const std::bad_alloc&) {
      return ReadResult::kUnreadable;
    }
  }

  // A temporary file beside the target, then a rename over it: a reader never sees half a file. The name is the host's own
  // spelling (`<file>.tmp-<pid>`), which a start removes after a kill between the write and the rename.
  bool WriteByRename(const std::string& utf8, const std::string& text) const noexcept override {
    try {
      std::wstring path;
      if (!Utf8ToWide(utf8, &path)) return false;
      const std::wstring temporary = path + L".tmp-" + std::to_wstring(GetCurrentProcessId());
      const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file == INVALID_HANDLE_VALUE) return false;
      DWORD wrote = 0;
      const BOOL ok = text.empty() ? TRUE : WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr);
      const BOOL flushed = ok != FALSE && FlushFileBuffers(file) != FALSE;
      CloseHandle(file);
      if (!flushed || wrote != text.size() ||
          MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        DeleteFileW(temporary.c_str());
        return false;
      }
      return true;
    } catch (const std::bad_alloc&) {
      return false;
    }
  }

  bool RemoveFile(const std::string& utf8) const noexcept override {
    try {
      std::wstring path;
      return Utf8ToWide(utf8, &path) && DeleteFileW(path.c_str()) != FALSE;
    } catch (const std::bad_alloc&) {
      return false;
    }
  }

  void RemoveTree(const std::string& utf8) const noexcept override {
    try {
      std::wstring path;
      if (Utf8ToWide(utf8, &path)) RemoveTreeWide(path);
    } catch (const std::bad_alloc&) {  // out of memory: the folder stays, and the next stage removes what no staged.json names
    }
  }

  bool IsLink(const std::string& utf8) const noexcept override {
    try {
      std::wstring path;
      return Utf8ToWide(utf8, &path) && IsReparsePoint(path);
    } catch (const std::bad_alloc&) {
      return true;  // cannot tell: assume a link, so the engine does not write through it
    }
  }

  bool Exists(const std::string& utf8) const noexcept override {
    try {
      std::wstring path;
      return Utf8ToWide(utf8, &path) && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    } catch (const std::bad_alloc&) {
      return true;  // cannot tell: assume it exists, so the engine waits instead of staging over a pending release
    }
  }

  std::unique_ptr<FsLock> TakeLock(const std::string& update_dir, std::uint32_t timeout_ms, std::uint32_t step_ms,
                                   int* os_error) const noexcept override {
    try {
      std::wstring path;
      if (!Utf8ToWide(update_dir + "\\" + names::kLockFile, &path)) {
        if (os_error != nullptr) *os_error = static_cast<int>(ERROR_NO_UNICODE_TRANSLATION);
        return nullptr;
      }
      const ULONGLONG started = GetTickCount64();
      for (;;) {
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
          Win32Lock* lock = new (std::nothrow) Win32Lock(file);  // no exception between the open and the owner of the handle
          if (lock == nullptr) {
            CloseHandle(file);
            if (os_error != nullptr) *os_error = static_cast<int>(ERROR_NOT_ENOUGH_MEMORY);
            return nullptr;
          }
          return std::unique_ptr<FsLock>(lock);
        }
        if (os_error != nullptr) *os_error = static_cast<int>(GetLastError());
        if (GetTickCount64() - started >= timeout_ms) return nullptr;
        Sleep(step_ms == 0 ? 1 : step_ms);
      }
    } catch (const std::bad_alloc&) {  // out of memory: no lock
      if (os_error != nullptr) *os_error = static_cast<int>(ERROR_NOT_ENOUGH_MEMORY);
      return nullptr;
    }
  }
};

}  // namespace

bool Utf8ToWide(const std::string& utf8, std::wstring* out) {
  out->clear();
  if (utf8.empty()) return true;
  const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  if (n <= 0) return false;
  out->assign(static_cast<std::size_t>(n), L'\0');
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), out->data(), n) == n;
}

const UpdateFs& Win32UpdateFs() noexcept {
  static const Win32Fs fs;
  return fs;
}

}  // namespace nevr_update
