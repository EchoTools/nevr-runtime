// platform_compat's CreateDirectoryW and CreateDirectoryA hooks (src/modules/platform-compat/src/platform_compat.cpp), installed for
// real (InstallEarlyFileHooks: MinHook on kernelbase.dll) in this test process. Each test calls kernel32's CreateDirectory on a path of
// its own under the run root and checks what the system did: the Wine _temp fix (missing parents made, an existing folder a success, a
// relative path behind \\?\ made under the current directory), that only paths with `_temp` get it, and that a folder under the game's
// _temp goes to the nEVR root before the fix is asked. The rules of which paths move are game_folder_redirect_rules_test.cpp.

#include <winsock2.h>  // before windows.h: core/logging.h pulls winsock2 in
#include <windows.h>

#include <MinHook.h>
#include <gtest/gtest.h>

#include <string>

#include "modules/platform-compat/src/game_folder_redirect.h"
#include "modules/platform-compat/src/platform_compat.h"
#include "runtime/tests/run_root.h"

namespace gfr = nevr_platform_compat::game_folder_redirect;

namespace {

struct Layout {
  std::wstring base;      // the test's folder under the run root
  std::wstring local_app; // the LOCALAPPDATA the nEVR root is formed under
  std::wstring exe_dir;   // this test program's folder: the redirect's base, so its _temp moves
};

Layout g_layout;
bool g_hooksOk = false;


bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::string AsAnsi(const std::wstring& w) { return std::string(w.begin(), w.end()); }

class PlatformCompatCreateDir : public ::testing::Test {
 protected:
  // One install per process: the hooks and the redirect's plan are process state, as in the game.
  static void SetUpTestSuite() {
    g_layout.base = nevr_test::NewDir(L"platform-compat-createdir");
    ASSERT_FALSE(g_layout.base.empty());
    g_layout.local_app = g_layout.base + L"\\appdata";
    ASSERT_TRUE(CreateDirectoryW(g_layout.local_app.c_str(), nullptr)) << GetLastError();
    ASSERT_TRUE(SetEnvironmentVariableW(L"LOCALAPPDATA", g_layout.local_app.c_str()));
    WCHAR exe[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    ASSERT_GT(n, 0u);
    ASSERT_LT(n, static_cast<DWORD>(MAX_PATH));
    g_layout.exe_dir = std::wstring(exe, n);
    g_layout.exe_dir.resize(g_layout.exe_dir.find_last_of(L'\\'));
    ASSERT_EQ(MH_Initialize(), MH_OK);
    g_hooksOk = nevr_platform_compat::InstallEarlyFileHooks();
  }
};

}  // namespace

TEST_F(PlatformCompatCreateDir, BothHooksInstallAndInstallingAgainIsTheSameAnswer) {
  EXPECT_TRUE(g_hooksOk);
  EXPECT_TRUE(nevr_platform_compat::InstallEarlyFileHooks()) << "once per process: a second call reports the first's result";
}

TEST_F(PlatformCompatCreateDir, ATempPathWithMissingParentsIsMadeWithThem) {
  const std::wstring path = g_layout.base + L"\\w_temp\\a\\b";
  SetLastError(ERROR_GEN_FAILURE);
  EXPECT_TRUE(CreateDirectoryW(path.c_str(), nullptr)) << GetLastError();
  EXPECT_TRUE(Exists(path));
}

TEST_F(PlatformCompatCreateDir, AnExistingTempFolderIsASuccessWithNoError) {
  const std::wstring path = g_layout.base + L"\\w_temp_existing";
  ASSERT_TRUE(CreateDirectoryW(path.c_str(), nullptr));
  SetLastError(ERROR_GEN_FAILURE);
  EXPECT_TRUE(CreateDirectoryW(path.c_str(), nullptr));
  EXPECT_EQ(GetLastError(), static_cast<DWORD>(ERROR_SUCCESS));
}

TEST_F(PlatformCompatCreateDir, APathWithoutTempKeepsTheSystemsAnswers) {
  const std::wstring existing = g_layout.base + L"\\plain";
  ASSERT_TRUE(CreateDirectoryW(existing.c_str(), nullptr));
  EXPECT_FALSE(CreateDirectoryW(existing.c_str(), nullptr));
  EXPECT_EQ(GetLastError(), static_cast<DWORD>(ERROR_ALREADY_EXISTS)) << "an existing folder is not made a success";
  const std::wstring orphan = g_layout.base + L"\\plain_no_parent\\child";
  EXPECT_FALSE(CreateDirectoryW(orphan.c_str(), nullptr));
  EXPECT_EQ(GetLastError(), static_cast<DWORD>(ERROR_PATH_NOT_FOUND)) << "missing parents are made for _temp paths only";
  EXPECT_FALSE(Exists(g_layout.base + L"\\plain_no_parent"));
}

TEST_F(PlatformCompatCreateDir, ARelativePathBehindTheNtPrefixIsMadeUnderTheCurrentDirectory) {
  WCHAR before[MAX_PATH] = {};
  ASSERT_GT(GetCurrentDirectoryW(MAX_PATH, before), 0u);
  ASSERT_TRUE(SetCurrentDirectoryW(g_layout.base.c_str()));
  const bool made = CreateDirectoryW(L"\\\\?\\rel_temp", nullptr) != FALSE;
  const DWORD error = GetLastError();
  SetCurrentDirectoryW(before);
  EXPECT_TRUE(made) << error;
  EXPECT_TRUE(Exists(g_layout.base + L"\\rel_temp"));
}

TEST_F(PlatformCompatCreateDir, ANullPathIsTheSystemsFailureAndNoCrash) {
  EXPECT_FALSE(CreateDirectoryW(nullptr, nullptr));
  EXPECT_FALSE(CreateDirectoryA(nullptr, nullptr));
}

// Under Wine kernelbase's CreateDirectoryA calls its own (hooked) CreateDirectoryW, so the answers of the two ANSI tests below also hold
// through the W hook; the A hook's own body is what a native Windows run exercises alone.
TEST_F(PlatformCompatCreateDir, TheAnsiHookMakesAnExistingTempFolderASuccess) {
  const std::string path = AsAnsi(g_layout.base + L"\\a_temp");
  ASSERT_TRUE(CreateDirectoryA(path.c_str(), nullptr));
  SetLastError(ERROR_GEN_FAILURE);
  EXPECT_TRUE(CreateDirectoryA(path.c_str(), nullptr));
  EXPECT_EQ(GetLastError(), static_cast<DWORD>(ERROR_SUCCESS));
}

TEST_F(PlatformCompatCreateDir, TheAnsiHookLeavesAPathWithoutTempAlone) {
  const std::string path = AsAnsi(g_layout.base + L"\\a_plain");
  ASSERT_TRUE(CreateDirectoryA(path.c_str(), nullptr));
  EXPECT_FALSE(CreateDirectoryA(path.c_str(), nullptr));
  EXPECT_EQ(GetLastError(), static_cast<DWORD>(ERROR_ALREADY_EXISTS));
}

TEST_F(PlatformCompatCreateDir, AFolderUnderTheGamesTempIsMadeInTheNevrRootAndNotInTheGameFolder) {
  const std::wstring game = g_layout.exe_dir + L"\\_temp\\redirect_w\\deep";
  const auto moved = gfr::RedirectW("test", game.c_str());
  ASSERT_TRUE(moved.has_value()) << "the redirect's plan names this program's folder as the game folder";
  EXPECT_TRUE(CreateDirectoryW(game.c_str(), nullptr)) << GetLastError();
  EXPECT_TRUE(Exists(*moved)) << "made where the redirect sends it, parents included";
  EXPECT_EQ(moved->compare(0, g_layout.local_app.size(), g_layout.local_app), 0) << "the redirect's target is under the nEVR root";
}

TEST_F(PlatformCompatCreateDir, TheAnsiHookRedirectsTheGamesTempToo) {
  const std::wstring game = g_layout.exe_dir + L"\\_temp\\redirect_a\\deep";
  const auto moved = gfr::RedirectW("test", game.c_str());
  ASSERT_TRUE(moved.has_value());
  EXPECT_TRUE(CreateDirectoryA(AsAnsi(game).c_str(), nullptr)) << GetLastError();
  EXPECT_TRUE(Exists(*moved));
}
