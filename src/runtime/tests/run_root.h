#pragma once

// The scratch space of a Win32 test binary. The harness gives each test binary one run root, in NEVR_TEST_RUN_ROOT, and
// removes it once at the end. A test makes its folders under that root with NewDir, and does not remove them itself: the
// only removal a test makes is of what it recorded in an Owned, by name, through nevr_owned (core/owned_remove.h).
// There is no fallback to GetTempPath: when NEVR_TEST_RUN_ROOT is unset or empty, the test fails with a message that names
// the variable and the helper that sets it.

#include <gtest/gtest.h>
#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "core/owned_remove.h"
#include "runtime/tests/run_root_order.h"

namespace nevr_test {

// Refuses the run: prints why to the standard error handle and aborts. A test never runs from a root it cannot name safely.
[[noreturn]] inline void RefuseRunRoot(const wchar_t* why) {
  std::fwprintf(stderr,
                L"NEVR_TEST_RUN_ROOT %ls. It must be an absolute folder below the harness work root (run-root make, nevr-wine); "
                L"no test runs without one, and none falls back to GetTempPath or to a drive root.\n",
                why);
  std::fflush(stderr);
  std::abort();
}

// True for an absolute folder that is not a drive root: X:\<name>... or \\server\share\<name>...
inline bool AbsoluteBelowADriveRoot(const std::wstring& root) {
  std::wstring t = root;
  while (t.size() > 1 && t.back() == L'\\') t.pop_back();
  if (t.size() > 3 && ((t[0] >= L'A' && t[0] <= L'Z') || (t[0] >= L'a' && t[0] <= L'z')) && t[1] == L':' && t[2] == L'\\') {
    return true;
  }
  return t.size() > 2 && t[0] == L'\\' && t[1] == L'\\' && t.find(L'\\', 2) != std::wstring::npos &&
         t.find(L'\\', 2) + 1 < t.size();
}

// The run root. Unset, empty, relative, or a drive root: the test binary aborts with a message naming the variable.
inline std::wstring RunRoot() {
  const DWORD length = GetEnvironmentVariableW(L"NEVR_TEST_RUN_ROOT", nullptr, 0);
  if (length == 0) RefuseRunRoot(L"is not set or is empty");
  std::vector<wchar_t> buffer(length);
  const DWORD written = GetEnvironmentVariableW(L"NEVR_TEST_RUN_ROOT", buffer.data(), length);
  if (written == 0 || written >= length) RefuseRunRoot(L"could not be read");
  std::wstring root(buffer.data(), written);
  if (!AbsoluteBelowADriveRoot(root)) RefuseRunRoot(L"is relative, or names a drive root");
  return root;
}

// The same path in the ANSI code page, for the A-suffixed APIs a test calls. Empty when the code page cannot spell it
// (WideCharToMultiByte sets lpUsedDefaultChar): the caller then fails rather than using a mangled name.
inline std::string AnsiPath(const std::wstring& wide) {
  if (wide.empty()) return std::string();
  BOOL used_default = FALSE;
  const int bytes = WideCharToMultiByte(CP_ACP, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, &used_default);
  if (bytes <= 0) return std::string();
  std::string out(static_cast<std::size_t>(bytes), '\0');
  WideCharToMultiByte(CP_ACP, 0, wide.data(), static_cast<int>(wide.size()), out.data(), bytes, nullptr, &used_default);
  if (used_default != FALSE) return std::string();
  return out;
}
// A new folder under RunRoot(), named by `label`, this process and a counter. CreateDirectoryW fails when it exists. Empty
// when the folder could not be made (a failure is recorded).
inline std::wstring NewDir(const wchar_t* label) {
  const std::wstring root = RunRoot();
  if (root.empty()) return std::wstring();
  static std::atomic<unsigned long> counter{0};
  const std::wstring dir = root + L"\\" + label + L"-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                           std::to_wstring(counter.fetch_add(1));
  if (!CreateDirectoryW(dir.c_str(), nullptr)) {
    ADD_FAILURE() << "NewDir could not make a folder under NEVR_TEST_RUN_ROOT (error " << GetLastError() << ")";
    return std::wstring();
  }
  return dir;
}

// A folder under RunRoot() with a name the test chooses, made when it is not there yet. For a child process, which finds
// the folder by its name, so it cannot be a NewDir folder (those are unique).
inline std::wstring NamedDir(const std::wstring& name) {
  const std::wstring root = RunRoot();
  if (root.empty()) return std::wstring();
  const std::wstring dir = root + L"\\" + name;
  if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
    ADD_FAILURE() << "NamedDir could not make a folder under NEVR_TEST_RUN_ROOT (error " << GetLastError() << ")";
    return std::wstring();
  }
  return dir;
}

// What a test made under `root` (a folder from NewDir), by name relative to it, in creation order. RemoveNow removes exactly
// those entries, children before parents, through nevr_owned::RemoveOwned; nothing found by listing a folder is removed.
class Owned {
 public:
  explicit Owned(std::wstring root) : root_(std::move(root)) {}

  void RecordFile(const std::wstring& relative) { records_.push_back(Make(relative, false)); }
  void RecordFolder(const std::wstring& relative) { records_.push_back(Make(relative, true)); }

  // One Result per entry, in the order they were tried. Empty when the root is empty (a failure was recorded).
  std::vector<nevr_owned::Result> RemoveNow() const {
    if (root_.empty()) return std::vector<nevr_owned::Result>();
    return nevr_owned::RemoveOwned(Utf8(root_), RemovalOrder(records_));
  }

 private:
  static OwnedRecord Make(const std::wstring& relative, bool folder) {
    std::wstring slashed = relative;
    for (wchar_t& c : slashed) {
      if (c == L'\\') c = L'/';
    }
    return OwnedRecord{Utf8(slashed), folder};
  }

  static std::string Utf8(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), bytes, nullptr, nullptr);
    return out;
  }

  std::wstring root_;
  std::vector<OwnedRecord> records_;
};

}  // namespace nevr_test
