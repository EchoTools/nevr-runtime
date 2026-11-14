// The PC UpdateFs (src/runtime/lifecycle/update_fs_win32.cpp) against a temp folder: each operation the engine asks of it
// (core/update/fs.h). Compiles no game stub and no Log.

#include <gtest/gtest.h>

#include <windows.h>

#include <atomic>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "core/update/fs.h"
#include "core/update/names.h"
#include "runtime/lifecycle/update_fs_win32.h"

namespace {

using nevr_update::ReadResult;
using nevr_update::Win32UpdateFs;

constexpr std::uint32_t kNoWaitMs = 0;
constexpr std::uint32_t kStepMs = 10;
constexpr std::uint32_t kShortWaitMs = 100;

std::string Utf8(const std::wstring& w) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
  return out;
}

class Dir {
 public:
  Dir() {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    static std::atomic<int> n{0};
    wide_ = std::wstring(temp) + L"nevr-update-fs-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(n.fetch_add(1));
    CreateDirectoryW(wide_.c_str(), nullptr);
  }
  ~Dir() { Remove(wide_); }
  std::string Path(const std::string& rel = std::string()) const {
    return rel.empty() ? Utf8(wide_) : Utf8(wide_) + "\\" + rel;
  }
  std::wstring Wide(const std::wstring& rel) const { return wide_ + L"\\" + rel; }
  void Write(const std::wstring& rel, const std::string& text) const {
    std::ofstream out(Wide(rel).c_str(), std::ios::binary | std::ios::trunc);
    out << text;
  }
  std::string Read(const std::wstring& rel) const {
    std::ifstream in(Wide(rel).c_str(), std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
  }
  bool Exists(const std::wstring& rel) const { return GetFileAttributesW(Wide(rel).c_str()) != INVALID_FILE_ATTRIBUTES; }

 private:
  static void Remove(const std::wstring& dir) {
    WIN32_FIND_DATAW f;
    const HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &f);
    if (h != INVALID_HANDLE_VALUE) {
      do {
        const std::wstring name = f.cFileName;
        if (name == L"." || name == L"..") continue;
        if ((f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) Remove(dir + L"\\" + name);
        else DeleteFileW((dir + L"\\" + name).c_str());
      } while (FindNextFileW(h, &f) != FALSE);
      FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
  }
  std::wstring wide_;
};

}  // namespace

TEST(UpdateFsWin32, TheSeparatorIsABackslash) { EXPECT_EQ(Win32UpdateFs().Separator(), '\\'); }

TEST(UpdateFsWin32, ReadSmallFileTellsPresentAbsentAndTooBig) {
  Dir dir;
  dir.Write(L"a.json", "{}");
  std::string text;
  EXPECT_EQ(Win32UpdateFs().ReadSmallFile(dir.Path("a.json"), &text), ReadResult::kOk);
  EXPECT_EQ(text, "{}");
  EXPECT_EQ(Win32UpdateFs().ReadSmallFile(dir.Path("none.json"), &text), ReadResult::kAbsent);
  EXPECT_EQ(Win32UpdateFs().ReadSmallFile(dir.Path("no-folder\\none.json"), &text), ReadResult::kAbsent);
  dir.Write(L"big.json", std::string(nevr_update_names::kMaxJsonBytes + 1, 'x'));
  EXPECT_EQ(Win32UpdateFs().ReadSmallFile(dir.Path("big.json"), &text), ReadResult::kTooBig);
  dir.Write(L"edge.json", std::string(nevr_update_names::kMaxJsonBytes, 'x'));
  EXPECT_EQ(Win32UpdateFs().ReadSmallFile(dir.Path("edge.json"), &text), ReadResult::kOk);
  EXPECT_EQ(text.size(), nevr_update_names::kMaxJsonBytes);
}

TEST(UpdateFsWin32, AFileAnotherProgramHoldsOpenExclusivelyIsUnreadableNotAbsent) {
  Dir dir;
  dir.Write(L"held.json", "{}");
  const HANDLE held = CreateFileW(dir.Wide(L"held.json").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(held, INVALID_HANDLE_VALUE);
  std::string text;
  EXPECT_EQ(Win32UpdateFs().ReadSmallFile(dir.Path("held.json"), &text), ReadResult::kUnreadable);
  CloseHandle(held);
}

TEST(UpdateFsWin32, AUtf8PathReachesAFolderWithANonAsciiName) {
  Dir dir;
  const std::wstring rel = L"pr\u00f6fil";
  CreateDirectoryW(dir.Wide(rel).c_str(), nullptr);
  ASSERT_TRUE(Win32UpdateFs().WriteByRename(dir.Path(Utf8(rel) + "\\state.json"), "{\"a\":1}"));
  EXPECT_EQ(dir.Read(rel + L"\\state.json"), "{\"a\":1}");
  std::string text;
  EXPECT_EQ(Win32UpdateFs().ReadSmallFile(dir.Path(Utf8(rel) + "\\state.json"), &text), ReadResult::kOk);
  EXPECT_EQ(text, "{\"a\":1}");
}

TEST(UpdateFsWin32, APathThatIsNotUtf8FailsTheOperation) {
  const std::string bad = "C:\\x\xff\xfe";
  std::string text;
  EXPECT_NE(Win32UpdateFs().ReadSmallFile(bad, &text), ReadResult::kOk);
  EXPECT_FALSE(Win32UpdateFs().WriteByRename(bad, "x"));
  EXPECT_FALSE(Win32UpdateFs().Exists(bad));
}

TEST(UpdateFsWin32, WriteByRenameReplacesAFileThatIsThereAndLeavesNoTemporary) {
  Dir dir;
  dir.Write(L"state.json", "old");
  ASSERT_TRUE(Win32UpdateFs().WriteByRename(dir.Path("state.json"), "new"));
  EXPECT_EQ(dir.Read(L"state.json"), "new");
  ASSERT_TRUE(Win32UpdateFs().WriteByRename(dir.Path("fresh.json"), ""));
  EXPECT_EQ(dir.Read(L"fresh.json"), "");
  WIN32_FIND_DATAW f;
  const HANDLE h = FindFirstFileW(dir.Wide(L"*.tmp-*").c_str(), &f);
  EXPECT_EQ(h, INVALID_HANDLE_VALUE) << "a temporary was left behind";
  if (h != INVALID_HANDLE_VALUE) FindClose(h);
}

TEST(UpdateFsWin32, AFailedRenameLeavesNoTemporaryBehind) {
  Dir dir;
  CreateDirectoryW(dir.Wide(L"state.json").c_str(), nullptr);  // a folder where the file should go: the rename over it fails
  EXPECT_FALSE(Win32UpdateFs().WriteByRename(dir.Path("state.json"), "x"));
  WIN32_FIND_DATAW f;
  const HANDLE h = FindFirstFileW(dir.Wide(L"*.tmp-*").c_str(), &f);
  EXPECT_EQ(h, INVALID_HANDLE_VALUE) << "the temporary of a failed write was left behind";
  if (h != INVALID_HANDLE_VALUE) FindClose(h);
}

TEST(UpdateFsWin32, AFileAnotherProgramHoldsOpenForWritingIsStillRead) {
  Dir dir;
  dir.Write(L"state.json", "{}");
  const HANDLE writer = CreateFileW(dir.Wide(L"state.json").c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(writer, INVALID_HANDLE_VALUE);
  std::string text;
  EXPECT_EQ(Win32UpdateFs().ReadSmallFile(dir.Path("state.json"), &text), ReadResult::kOk);
  CloseHandle(writer);
}

TEST(UpdateFsWin32, WriteByRenameFailsWhenTheFolderIsAbsentAndCreatesNothing) {
  Dir dir;
  EXPECT_FALSE(Win32UpdateFs().WriteByRename(dir.Path("no-folder\\state.json"), "x"));
  EXPECT_FALSE(dir.Exists(L"no-folder"));
}

TEST(UpdateFsWin32, RemoveFileRemovesAndReportsAnAbsentFile) {
  Dir dir;
  dir.Write(L"pending.json", "{}");
  EXPECT_TRUE(Win32UpdateFs().RemoveFile(dir.Path("pending.json")));
  EXPECT_FALSE(dir.Exists(L"pending.json"));
  EXPECT_FALSE(Win32UpdateFs().RemoveFile(dir.Path("pending.json")));
}

TEST(UpdateFsWin32, RemoveTreeRemovesAFolderAndWhatIsInIt) {
  Dir dir;
  CreateDirectoryW(dir.Wide(L"staging").c_str(), nullptr);
  CreateDirectoryW(dir.Wide(L"staging\\1.0.0").c_str(), nullptr);
  dir.Write(L"staging\\1.0.0\\nevr-runtime.dll", "x");
  const HANDLE ro = CreateFileW(dir.Wide(L"staging\\ro.dll").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_READONLY, nullptr);
  CloseHandle(ro);
  Win32UpdateFs().RemoveTree(dir.Path("staging"));
  EXPECT_FALSE(dir.Exists(L"staging"));
}

TEST(UpdateFsWin32, RemoveTreeRemovesALinkAndNeverEntersIt) {
  Dir dir;
  CreateDirectoryW(dir.Wide(L"target").c_str(), nullptr);
  dir.Write(L"target\\keep.txt", "keep");
  if (CreateSymbolicLinkW(dir.Wide(L"link").c_str(), dir.Wide(L"target").c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY) == FALSE) {
    GTEST_SKIP() << "no symbolic links here: error " << GetLastError();
  }
  ASSERT_TRUE(Win32UpdateFs().IsLink(dir.Path("link")));
  Win32UpdateFs().RemoveTree(dir.Path("link"));
  EXPECT_FALSE(dir.Exists(L"link"));
  EXPECT_EQ(dir.Read(L"target\\keep.txt"), "keep") << "the link's target was entered";
}

TEST(UpdateFsWin32, IsLinkAndExistsTellFilesFoldersLinksAndNothing) {
  Dir dir;
  dir.Write(L"f", "x");
  CreateDirectoryW(dir.Wide(L"d").c_str(), nullptr);
  EXPECT_TRUE(Win32UpdateFs().Exists(dir.Path("f")));
  EXPECT_TRUE(Win32UpdateFs().Exists(dir.Path("d")));
  EXPECT_FALSE(Win32UpdateFs().Exists(dir.Path("none")));
  EXPECT_FALSE(Win32UpdateFs().IsLink(dir.Path("f")));
  EXPECT_FALSE(Win32UpdateFs().IsLink(dir.Path("d")));
  EXPECT_FALSE(Win32UpdateFs().IsLink(dir.Path("none")));
  if (CreateSymbolicLinkW(dir.Wide(L"l").c_str(), dir.Wide(L"d").c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY) != FALSE) {
    EXPECT_TRUE(Win32UpdateFs().IsLink(dir.Path("l")));
    EXPECT_TRUE(Win32UpdateFs().Exists(dir.Path("l")));
  }
}

TEST(UpdateFsWin32, TheLockIsExclusiveReleasedByTheOwnerAndDeletedWithIt) {
  Dir dir;
  int error = 0;
  auto first = Win32UpdateFs().TakeLock(dir.Path(), kNoWaitMs, kStepMs, &error);
  ASSERT_NE(first, nullptr) << error;
  EXPECT_TRUE(dir.Exists(L"lock"));
  int other_error = 0;
  EXPECT_EQ(Win32UpdateFs().TakeLock(dir.Path(), kShortWaitMs, kStepMs, &other_error), nullptr);
  EXPECT_NE(other_error, 0);
  first->Release();
  auto second = Win32UpdateFs().TakeLock(dir.Path(), kNoWaitMs, kStepMs, &error);
  EXPECT_NE(second, nullptr) << error;
  second.reset();
  EXPECT_FALSE(dir.Exists(L"lock")) << "the lock file outlives its holder";
}

TEST(UpdateFsWin32, ALockInAFolderWhoseNameIsNotUtf8IsNotTakenAndSaysWhy) {
  int error = 0;
  EXPECT_EQ(Win32UpdateFs().TakeLock("C:\\x\xff\xfe", kNoWaitMs, kStepMs, &error), nullptr);
  EXPECT_EQ(error, static_cast<int>(ERROR_NO_UNICODE_TRANSLATION));
}

TEST(UpdateFsWin32, ALockWaitsForTheHolderAndTakesOverWhenItLetsGo) {
  Dir dir;
  int error = 0;
  auto first = Win32UpdateFs().TakeLock(dir.Path(), kNoWaitMs, kStepMs, &error);
  ASSERT_NE(first, nullptr);
  std::thread releaser([&] {
    Sleep(kShortWaitMs);
    first->Release();
  });
  constexpr std::uint32_t kLongWaitMs = 5000;
  auto second = Win32UpdateFs().TakeLock(dir.Path(), kLongWaitMs, kStepMs, &error);
  releaser.join();
  EXPECT_NE(second, nullptr);
}
