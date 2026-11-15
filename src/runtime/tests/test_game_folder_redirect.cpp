// #575: the game folder redirect's hooks, installed for real (MinHook on the file-call exports of kernelbase.dll) in this
// test process, with an install and an nEVR root of the test's own under the run root. Each test makes a file call
// through kernel32 on a path under the fake install's _local or _temp and checks where the system put it. The rules
// (which paths move) are game_folder_redirect_rules_test.cpp; this file drives the hooks, the plan, the config.json copy
// and the folder creation that use them.

#include <winsock2.h>  // before windows.h: core/logging.h pulls winsock2 in
#include <windows.h>

#include <MinHook.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <cwctype>
#include <string>

#include "modules/platform-compat/src/game_folder_redirect.h"
#include "runtime/tests/run_root.h"

namespace gfr = nevr_platform_compat::game_folder_redirect;

namespace {

// The twelve calls game_folder_redirect.cpp hooks (CreateDirectoryW and CreateDirectoryA are platform_compat.cpp's).
constexpr int kOwnHooks = 12;
// A value no call in the redirect sets, to see that the redirect leaves GetLastError as it found it.
constexpr DWORD kMarkerError = 0x1234;
constexpr char kLegacyConfig[] = "{\"legacy\":true}";
// Larger than any file a test writes.
constexpr std::size_t kReadBufferBytes = 256;

struct Layout {
  std::wstring base;     // the test's folder under the run root
  std::wstring install;  // <base>\install: the engine's root (the grandparent of bin\win10)
  std::wstring exe_dir;  // <install>\bin\win10
  std::wstring root;     // <base>\nevr: the nEVR root
  std::wstring target;   // <root>\game: where _local and _temp go
};

Layout g_layout;
int g_installed = -1;

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool WriteFileText(const std::wstring& path, const char* text) {
  const HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const BOOL ok = WriteFile(h, text, static_cast<DWORD>(std::char_traits<char>::length(text)), &written, nullptr);
  CloseHandle(h);
  return ok != FALSE;
}

std::string ReadFileText(const std::wstring& path) {
  const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return std::string();
  char buffer[kReadBufferBytes] = {};
  DWORD read = 0;
  ReadFile(h, buffer, sizeof(buffer) - 1, &read, nullptr);
  CloseHandle(h);
  return std::string(buffer, read);
}

std::wstring Lower(std::wstring s) {
  for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
  return s;
}

// Names directly under `folder` (FindFirstFileW on <folder>\*, which no rule moves).
bool ListsName(const std::wstring& folder, const wchar_t* name) {
  WIN32_FIND_DATAW data{};
  const HANDLE h = FindFirstFileW((folder + L"\\*").c_str(), &data);
  if (h == INVALID_HANDLE_VALUE) return false;
  bool found = false;
  do {
    found = found || _wcsicmp(data.cFileName, name) == 0;
  } while (FindNextFileW(h, &data));
  FindClose(h);
  return found;
}

class GameFolderRedirect : public ::testing::Test {
 protected:
  // One install per process: the plan and the hooks are process state, as in the game.
  static void SetUpTestSuite() {
    g_layout.base = nevr_test::NewDir(L"game-folder-redirect");
    ASSERT_FALSE(g_layout.base.empty());
    g_layout.install = g_layout.base + L"\\install";
    g_layout.exe_dir = g_layout.install + L"\\bin\\win10";
    g_layout.root = g_layout.base + L"\\nevr";
    g_layout.target = g_layout.root + L"\\game";
    for (const std::wstring& dir : {g_layout.install, g_layout.install + L"\\bin", g_layout.exe_dir,
                                    g_layout.exe_dir + L"\\_local", g_layout.root}) {
      ASSERT_TRUE(CreateDirectoryW(dir.c_str(), nullptr)) << GetLastError();
    }
    // The player's game settings, where the game kept them before the redirect.
    ASSERT_TRUE(WriteFileText(g_layout.exe_dir + L"\\_local\\config.json", kLegacyConfig));
    ASSERT_EQ(MH_Initialize(), MH_OK);
    g_installed = gfr::InstallWith(g_layout.root, g_layout.exe_dir + L"\\echovr.exe");
  }
};

TEST_F(GameFolderRedirect, AllTwelveHooksInstall) { EXPECT_EQ(g_installed, kOwnHooks); }

TEST_F(GameFolderRedirect, TheOldConfigJsonIsCopiedIntoTheTarget) {
  EXPECT_EQ(ReadFileText(g_layout.target + L"\\_local\\config.json"), kLegacyConfig);
}

TEST_F(GameFolderRedirect, CreateFileWUnderTheInstallsLocalLandsInTheTarget) {
  const std::wstring game_path = g_layout.install + L"\\_local\\created_w.txt";
  ASSERT_TRUE(WriteFileText(game_path, "w"));
  EXPECT_EQ(ReadFileText(g_layout.target + L"\\_local\\created_w.txt"), "w");
  EXPECT_FALSE(ListsName(g_layout.install, L"_local"));  // nothing was made in the install
  EXPECT_TRUE(Exists(game_path));                       // and the game still finds its file where it asked
}

TEST_F(GameFolderRedirect, CreateFileAUnderTheExeFoldersLocalLandsInTheTarget) {
  const std::string game_path = nevr_test::AnsiPath(g_layout.exe_dir + L"\\_local\\created_a.txt");
  ASSERT_FALSE(game_path.empty());
  const HANDLE h = CreateFileA(game_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(h, INVALID_HANDLE_VALUE) << GetLastError();
  CloseHandle(h);
  EXPECT_TRUE(Exists(g_layout.target + L"\\_local\\created_a.txt"));
  EXPECT_NE(GetFileAttributesA(game_path.c_str()), INVALID_FILE_ATTRIBUTES);
}

TEST_F(GameFolderRedirect, APathOutsideTheMovedFoldersStaysInTheInstall) {
  ASSERT_TRUE(WriteFileText(g_layout.install + L"\\stays.txt", "s"));
  EXPECT_TRUE(ListsName(g_layout.install, L"stays.txt"));
  EXPECT_FALSE(ListsName(g_layout.target, L"stays.txt"));
}

TEST_F(GameFolderRedirect, FindFirstFileWAndAListTheTargetsFolder) {
  ASSERT_TRUE(WriteFileText(g_layout.install + L"\\_local\\listed.txt", "l"));
  WIN32_FIND_DATAW wide{};
  const HANDLE hw = FindFirstFileW((g_layout.install + L"\\_local\\listed.txt").c_str(), &wide);
  ASSERT_NE(hw, INVALID_HANDLE_VALUE);
  FindClose(hw);
  WIN32_FIND_DATAA ansi{};
  const std::string pattern = nevr_test::AnsiPath(g_layout.install + L"\\_local\\listed.txt");
  const HANDLE ha = FindFirstFileA(pattern.c_str(), &ansi);
  ASSERT_NE(ha, INVALID_HANDLE_VALUE);
  FindClose(ha);
  EXPECT_STREQ(ansi.cFileName, "listed.txt");
  WIN32_FIND_DATAW ex{};
  const HANDLE he = FindFirstFileExW((g_layout.install + L"\\_local\\listed.txt").c_str(), FindExInfoBasic, &ex,
                                     FindExSearchNameMatch, nullptr, 0);
  ASSERT_NE(he, INVALID_HANDLE_VALUE);
  FindClose(he);
}

TEST_F(GameFolderRedirect, AttributesDeleteCopyAndMoveReachTheTarget) {
  const std::wstring local = g_layout.install + L"\\_local\\";
  ASSERT_TRUE(WriteFileText(local + L"attr.txt", "a"));
  WIN32_FILE_ATTRIBUTE_DATA info{};
  EXPECT_TRUE(GetFileAttributesExW((local + L"attr.txt").c_str(), GetFileExInfoStandard, &info));
  EXPECT_TRUE(SetFileAttributesW((local + L"attr.txt").c_str(), FILE_ATTRIBUTE_READONLY));
  EXPECT_NE(GetFileAttributesW((g_layout.target + L"\\_local\\attr.txt").c_str()) & FILE_ATTRIBUTE_READONLY, 0u);
  EXPECT_TRUE(SetFileAttributesW((local + L"attr.txt").c_str(), FILE_ATTRIBUTE_NORMAL));

  EXPECT_TRUE(CopyFileW((local + L"attr.txt").c_str(), (local + L"copy.txt").c_str(), TRUE));
  EXPECT_TRUE(Exists(g_layout.target + L"\\_local\\copy.txt"));
  EXPECT_TRUE(MoveFileExW((local + L"copy.txt").c_str(), (local + L"moved.txt").c_str(), 0));
  EXPECT_TRUE(Exists(g_layout.target + L"\\_local\\moved.txt"));
  EXPECT_FALSE(Exists(g_layout.target + L"\\_local\\copy.txt"));
  EXPECT_TRUE(DeleteFileW((local + L"moved.txt").c_str()));
  EXPECT_FALSE(Exists(g_layout.target + L"\\_local\\moved.txt"));
  EXPECT_FALSE(ListsName(g_layout.install, L"_local"));
}

TEST_F(GameFolderRedirect, ARedirectLeavesTheLastErrorAsItFoundIt) {
  SetLastError(kMarkerError);
  const auto to = gfr::RedirectW("test", (g_layout.install + L"\\_temp\\x").c_str());
  EXPECT_TRUE(to.has_value());
  EXPECT_EQ(GetLastError(), kMarkerError);
  SetLastError(kMarkerError);
  const std::string ansi = nevr_test::AnsiPath(g_layout.install + L"\\_temp\\x");
  EXPECT_TRUE(gfr::RedirectA("test", ansi.c_str()).has_value());
  EXPECT_EQ(GetLastError(), kMarkerError);
  EXPECT_FALSE(gfr::RedirectW("test", (g_layout.install + L"\\data\\x").c_str()).has_value());
}

TEST_F(GameFolderRedirect, AMovedTempFolderIsMadeWithItsParents) {
  const auto to = gfr::RedirectW("test", (g_layout.install + L"\\_temp\\crashes\\deep").c_str());
  ASSERT_TRUE(to.has_value());
  EXPECT_TRUE(gfr::IsUnderMovedTemp(*to));
  EXPECT_TRUE(gfr::CreateRedirectedDirectoryW(&CreateDirectoryW, *to, nullptr));
  EXPECT_TRUE(Exists(g_layout.target + L"\\_temp\\crashes\\deep"));
  // Made again: an existing _temp folder is a success, as the Wine _temp fix answers.
  EXPECT_TRUE(gfr::CreateRedirectedDirectoryW(&CreateDirectoryW, *to, nullptr));
  EXPECT_EQ(GetLastError(), static_cast<DWORD>(ERROR_SUCCESS));
}

TEST_F(GameFolderRedirect, AMovedLocalFolderGetsTheSystemsOwnAnswer) {
  const auto to = gfr::RedirectW("test", (g_layout.install + L"\\_local\\no\\parent").c_str());
  ASSERT_TRUE(to.has_value());
  EXPECT_FALSE(gfr::IsUnderMovedTemp(*to));
  EXPECT_FALSE(gfr::CreateRedirectedDirectoryW(&CreateDirectoryW, *to, nullptr));
  EXPECT_EQ(GetLastError(), static_cast<DWORD>(ERROR_PATH_NOT_FOUND));
  const auto existing = gfr::RedirectW("test", (g_layout.install + L"\\_local").c_str());
  ASSERT_TRUE(existing.has_value());
  EXPECT_FALSE(gfr::CreateRedirectedDirectoryW(&CreateDirectoryW, *existing, nullptr));
  EXPECT_EQ(GetLastError(), static_cast<DWORD>(ERROR_ALREADY_EXISTS));
}

TEST_F(GameFolderRedirect, ResolvedForLogNamesTheFileActuallyRead) {
  const std::string game_path = nevr_test::AnsiPath(g_layout.exe_dir + L"\\_local\\config.json");
  const std::string resolved = gfr::ResolvedForLog(game_path.c_str());
  const std::wstring wide(resolved.begin(), resolved.end());
  EXPECT_NE(Lower(wide).find(L"\\nevr\\game\\_local\\config.json"), std::wstring::npos) << resolved;
  const std::string other = nevr_test::AnsiPath(g_layout.install + L"\\stays.txt");
  EXPECT_EQ(gfr::ResolvedForLog(other.c_str()), other);
}

}  // namespace
