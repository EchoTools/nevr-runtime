// game_folder_redirect_rules_test.cpp: which paths the game folder redirect moves to the nEVR root, and where (#575).
// The rules are pure (modules/platform-compat/src/game_folder_redirect_rules.h); the hooks are Windows-only.

#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "modules/platform-compat/src/game_folder_redirect_rules.h"

namespace {

namespace gfr = nevr_platform_compat::game_folder_redirect;

const std::wstring kInstall = L"C:\\Games\\ready-at-dawn-echo-arena";
const std::wstring kExeDir = L"C:\\Games\\ready-at-dawn-echo-arena\\bin\\win10";
// BasesForExe returns the engine's root and the exe folder.
constexpr std::size_t kRootAndExeFolder = 2;

const std::wstring kTarget = L"C:\\Users\\p\\AppData\\Local\\nevr\\echovr\\game";

gfr::Plan MakePlan() {
  gfr::Plan plan;
  plan.bases = {kInstall, kExeDir};
  plan.target = kTarget;
  return plan;
}

std::optional<std::wstring> Rewrite(const std::wstring& path, const std::wstring& cwd = kExeDir) {
  return gfr::Rewrite(MakePlan(), path, cwd);
}

TEST(GameFolderRedirectRules, LocalUnderTheInstallRootMoves) {
  EXPECT_EQ(Rewrite(kInstall + L"\\_local\\r14logs\\a.log"), kTarget + L"\\_local\\r14logs\\a.log");
}

TEST(GameFolderRedirectRules, TempUnderTheInstallRootMoves) {
  EXPECT_EQ(Rewrite(kInstall + L"\\_temp\\crashes"), kTarget + L"\\_temp\\crashes");
}

TEST(GameFolderRedirectRules, TheFolderItselfMoves) {
  EXPECT_EQ(Rewrite(kInstall + L"\\_local"), kTarget + L"\\_local");
  EXPECT_EQ(Rewrite(kInstall + L"\\_temp\\"), kTarget + L"\\_temp");
}

TEST(GameFolderRedirectRules, CaseDoesNotMatter) {
  EXPECT_EQ(Rewrite(L"c:\\GAMES\\Ready-At-Dawn-Echo-Arena\\_LOCAL\\Settings"), kTarget + L"\\_LOCAL\\Settings");
}

TEST(GameFolderRedirectRules, ForwardSlashesAndMixedSlashesMove) {
  // CreateLogFile @0x140100790 joins with '/': <root>/_local/r14logs/.
  EXPECT_EQ(Rewrite(L"C:/Games/ready-at-dawn-echo-arena/_local/r14logs/x.log"), kTarget + L"\\_local\\r14logs\\x.log");
  EXPECT_EQ(Rewrite(kInstall + L"/_local\\r14logs//x.log"), kTarget + L"\\_local\\r14logs\\x.log");
}

TEST(GameFolderRedirectRules, ANameThatOnlyStartsWithAMovedNameStays) {
  EXPECT_EQ(Rewrite(kInstall + L"\\_localX\\a.txt"), std::nullopt);
  EXPECT_EQ(Rewrite(kInstall + L"\\_temporary\\a.txt"), std::nullopt);
  EXPECT_EQ(Rewrite(kInstall + L"\\x_local\\a.txt"), std::nullopt);
}

TEST(GameFolderRedirectRules, GameDataStays) {
  EXPECT_EQ(Rewrite(kInstall + L"\\_data\\5932408047\\rad15\\win10\\manifests\\x"), std::nullopt);
  EXPECT_EQ(Rewrite(kInstall + L"\\content\\_local\\x"), std::nullopt);  // a _local deeper down is not the game's
  EXPECT_EQ(Rewrite(kInstall + L"\\sourcedb"), std::nullopt);
  EXPECT_EQ(Rewrite(kExeDir + L"\\echovr.exe"), std::nullopt);
}

TEST(GameFolderRedirectRules, ABaseOnlySharingAPrefixStays) {
  EXPECT_EQ(Rewrite(kInstall + L"2\\_local\\a"), std::nullopt);
}

TEST(GameFolderRedirectRules, TheExeFolderIsABaseToo) {
  // HandleCrashDump's second step makes _temp|crashes relative to the working directory (0x1401d0614).
  EXPECT_EQ(Rewrite(kExeDir + L"\\_temp\\crashes\\a.log"), kTarget + L"\\_temp\\crashes\\a.log");
}

TEST(GameFolderRedirectRules, ARelativePathResolvesAgainstTheWorkingDirectory) {
  EXPECT_EQ(Rewrite(L"_local\\r14logs\\*.log"), kTarget + L"\\_local\\r14logs\\*.log");
  EXPECT_EQ(Rewrite(L".\\_temp|x", kInstall), std::nullopt);  // '|' is the engine's separator, never the system's
  EXPECT_EQ(Rewrite(L"..\\..\\_local\\config.json"), kTarget + L"\\_local\\config.json");
  EXPECT_EQ(Rewrite(L"_local\\a", L"D:\\elsewhere"), std::nullopt);
}

TEST(GameFolderRedirectRules, ARelativePathBehindTheLongPrefixResolvesToo) {
  // #1845: the game passes \\?\_temp\... with no drive; the result is a real absolute path.
  EXPECT_EQ(Rewrite(L"\\\\?\\_temp\\crashes"), kTarget + L"\\_temp\\crashes");
}

TEST(GameFolderRedirectRules, TheLongPrefixOfAnAbsolutePathIsKept) {
  EXPECT_EQ(Rewrite(L"\\\\?\\" + kInstall + L"\\_local\\a"), L"\\\\?\\" + kTarget + L"\\_local\\a");
}

TEST(GameFolderRedirectRules, SharesAndDevicesStay) {
  EXPECT_EQ(Rewrite(L"\\\\server\\share\\_local\\a"), std::nullopt);
  EXPECT_EQ(Rewrite(L"\\\\?\\UNC\\server\\share\\_local\\a"), std::nullopt);
  EXPECT_EQ(Rewrite(L"\\\\.\\pipe\\_local"), std::nullopt);
}

TEST(GameFolderRedirectRules, AShareIsNeverReadAsAFolderOfTheCurrentDrive) {
  gfr::Plan plan = MakePlan();
  plan.bases.push_back(L"C:\\server\\share");
  EXPECT_EQ(gfr::Rewrite(plan, L"\\\\server\\share\\_local\\a", kExeDir), std::nullopt);
  EXPECT_EQ(gfr::Rewrite(plan, L"\\server\\share\\_local\\a", kExeDir), plan.target + L"\\_local\\a");
}

TEST(GameFolderRedirectRules, TrailingDotsAndSpacesNameTheSameFolder) {
  // Win32 drops a name's trailing dots and spaces: "_local." is the game folder's _local.
  EXPECT_EQ(Rewrite(kInstall + L"\\_local.\\x"), kTarget + L"\\_local\\x");
  EXPECT_EQ(Rewrite(kInstall + L"\\_temp . \\x"), kTarget + L"\\_temp\\x");
}

TEST(GameFolderRedirectRules, TheDevicePrefixOfADrivePathIsKept) {
  EXPECT_EQ(Rewrite(L"\\\\.\\" + kInstall + L"\\_local\\x"), L"\\\\.\\" + kTarget + L"\\_local\\x");
  EXPECT_EQ(Rewrite(L"\\\\.\\_local\\x", kInstall), std::nullopt);  // a device name, never a folder of the current one
}

TEST(GameFolderRedirectRules, ADriveRelativePathResolvesOnTheCurrentDrive) {
  EXPECT_EQ(Rewrite(L"c:_local\\x"), kTarget + L"\\_local\\x");  // the current folder is the exe folder on C:
  EXPECT_EQ(Rewrite(L"D:_local\\x"), std::nullopt);               // D:'s current folder is not known
}

TEST(GameFolderRedirectRules, DotDotTakesBackOneFolder) {
  // From a folder below the exe folder, ..\_local is the exe folder's _local; a dropped .. would leave it below.
  EXPECT_EQ(Rewrite(L"..\\_local\\a", kExeDir + L"\\sub"), kTarget + L"\\_local\\a");
  EXPECT_EQ(Rewrite(L"sub\\..\\_local\\a"), kTarget + L"\\_local\\a");
}

TEST(GameFolderRedirectRules, DotDotNeverClimbsAboveTheDrive) {
  EXPECT_EQ(Rewrite(L"C:\\..\\..\\Games\\ready-at-dawn-echo-arena\\_local\\x"), kTarget + L"\\_local\\x");
  EXPECT_EQ(gfr::Normalize(L"C:\\..\\a"), L"C:\\a");
}

TEST(GameFolderRedirectRules, OldConfigJsonIsLookedForInTheExeFolderFirst) {
  const std::vector<std::wstring> paths = gfr::LegacyConfigJsonPaths(MakePlan());
  ASSERT_EQ(paths.size(), kRootAndExeFolder);
  EXPECT_EQ(paths[0], kExeDir + L"\\_local\\config.json");
  EXPECT_EQ(paths[1], kInstall + L"\\_local\\config.json");
}

TEST(GameFolderRedirectRules, RewritingTwiceChangesNothing) {
  const std::optional<std::wstring> once = Rewrite(kInstall + L"\\_local\\a");
  ASSERT_TRUE(once.has_value());
  EXPECT_EQ(Rewrite(*once), std::nullopt);
}

TEST(GameFolderRedirectRules, NoTargetMovesNothing) {
  gfr::Plan plan = MakePlan();
  plan.target.clear();
  EXPECT_EQ(gfr::Rewrite(plan, kInstall + L"\\_local\\a", kExeDir), std::nullopt);
}

TEST(GameFolderRedirectRules, CannotMatchIsTheCheapRejection) {
  EXPECT_TRUE(gfr::CannotMatch(kInstall + L"\\_data\\x"));
  EXPECT_FALSE(gfr::CannotMatch(L"x\\_LOCAL"));
  EXPECT_FALSE(gfr::CannotMatch(L"x/_Temp/y"));
}

TEST(GameFolderRedirectRules, MovedNameNamesTheFolder) {
  const gfr::Plan plan = MakePlan();
  EXPECT_EQ(gfr::MovedName(plan, kTarget + L"\\_local\\a"), L"_local");
  EXPECT_EQ(gfr::MovedName(plan, L"\\\\?\\" + kTarget + L"\\_temp"), L"_temp");
  EXPECT_EQ(gfr::MovedName(plan, kTarget + L"\\_localX"), L"");
  EXPECT_EQ(gfr::MovedName(plan, kInstall + L"\\_local"), L"");
}

TEST(GameFolderRedirectRules, BasesAreTheEnginesRootAndTheExeFolder) {
  const std::vector<std::wstring> bases = gfr::BasesForExe(kExeDir + L"\\echovr.exe");
  ASSERT_EQ(bases.size(), kRootAndExeFolder);
  EXPECT_EQ(bases[0], kInstall);
  EXPECT_EQ(bases[1], kExeDir);
}

TEST(GameFolderRedirectRules, TheNearestMarkerDecidesTheRoot) {
  const std::vector<std::wstring> bases = gfr::BasesForExe(L"D:\\win10\\echo\\bin\\WIN7\\echovr.exe");
  ASSERT_EQ(bases.size(), kRootAndExeFolder);
  EXPECT_EQ(bases[0], L"D:\\win10\\echo");
}

TEST(GameFolderRedirectRules, NoMarkerLeavesTheExeFolderAlone) {
  const std::vector<std::wstring> bases = gfr::BasesForExe(L"D:\\echo\\echovr.exe");
  ASSERT_EQ(bases.size(), 1u);
  EXPECT_EQ(bases[0], L"D:\\echo");
}

TEST(GameFolderRedirectRules, NormalizeTakesOnlyDrivePaths) {
  EXPECT_EQ(gfr::Normalize(L"C:/a//b/./c/../d/"), L"C:\\a\\b\\d");
  EXPECT_EQ(gfr::Normalize(L"\\\\?\\C:\\a"), L"C:\\a");
  EXPECT_EQ(gfr::Normalize(L"a\\b"), L"");
  EXPECT_EQ(gfr::Normalize(L"C:a"), L"");
}

}  // namespace
