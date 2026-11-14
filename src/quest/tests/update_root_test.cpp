// Host test for the Quest update root's folder rules (src/quest/shim/update_root_rules.h): the app-internal folder formula
// (quest/auth/app_files_rules.h), the update root below it, and its comparison with the folder the activity names. The rules are
// pure; no code of this tree calls them yet (update_root_rules.h).
//
// Built and run by `just test-quest-shared` (src/quest/host_tests/groups/shared.cmake).

#include <cstdio>
#include <string>

#include "quest/auth/app_files_rules.h"
#include "quest/shim/update_root_rules.h"
#include "quest/tests/test_check.h"

namespace {

namespace app = nevr_quest::app_files;
namespace root = nevr_quest::update_root;

// Bionic's AID_USER_OFFSET, as the Android headers define it: the value the formula must keep.
constexpr unsigned kBionicAidUserOffset = 100000;
// The uid of the app's process on user 0 and on user 10.
constexpr unsigned kUserZeroAppUid = 10234;
constexpr unsigned kUserTenAppUid = 1010234;
constexpr char kPackage[] = "com.readyatdawn.r15";

// The folder the app's own process reads and writes, per user: /data/user/<uid / offset>/<package>/files.
void TheFilesFolderIsTheAppsOwnForItsUser() {
  QCHECK(app::kAidUserOffset == kBionicAidUserOffset);
  QCHECK(app::FilesDirFromCmdline(kPackage, kUserZeroAppUid) == "/data/user/0/com.readyatdawn.r15/files");
  QCHECK(app::FilesDirFromCmdline(kPackage, kUserTenAppUid) == "/data/user/10/com.readyatdawn.r15/files");
  // The command line's first argument is the package; the rest is ignored.
  const std::string withArgument = std::string(kPackage) + std::string(1, '\0') + "-arg";
  QCHECK(app::FilesDirFromCmdline(withArgument, kUserZeroAppUid) == "/data/user/0/com.readyatdawn.r15/files");
  // A process suffix, a name with no dot and a double dot are refused, and so is an empty name: nothing falls back to
  // external storage.
  QCHECK(app::FilesDirFromCmdline("com.readyatdawn.r15:service", kUserZeroAppUid).empty());
  QCHECK(app::FilesDirFromCmdline("nodots", kUserZeroAppUid).empty());
  QCHECK(app::FilesDirFromCmdline("com..readyatdawn", kUserZeroAppUid).empty());
  QCHECK(app::FilesDirFromCmdline("", kUserZeroAppUid).empty());
}

// The update root is the folder `nevr` below the files folder; no files folder, no root; never the external folder.
void TheRootIsNevrBelowTheFilesFolder() {
  const std::string files = app::FilesDirFromCmdline(kPackage, kUserZeroAppUid);
  QCHECK(root::RootFromFilesDir(files) == "/data/user/0/com.readyatdawn.r15/files/nevr");
  QCHECK(root::RootFromFilesDir(app::FilesDirFromCmdline(kPackage, kUserTenAppUid)) ==
         "/data/user/10/com.readyatdawn.r15/files/nevr");
  QCHECK(root::RootFromFilesDir("").empty());
  QCHECK(root::RootFromFilesDir(files).find("/sdcard") == std::string::npos);
}

// The computed root agrees with the folder the activity names as its internal data path (getFilesDir()), with the root's
// folder name below it. Another user's folder, the files folder itself, and an empty path do not agree.
void TheRootAgreesWithTheActivitysInternalDataPathOnlyForTheSameFolder() {
  const std::string internal = "/data/user/0/com.readyatdawn.r15/files";
  const std::string computed = root::RootFromFilesDir(app::FilesDirFromCmdline(kPackage, kUserZeroAppUid));
  QCHECK(root::RootMatchesInternalDataPath(computed, internal));
  QCHECK(!root::RootMatchesInternalDataPath(computed, "/data/user/10/com.readyatdawn.r15/files"));
  QCHECK(!root::RootMatchesInternalDataPath(internal, internal));
  QCHECK(!root::RootMatchesInternalDataPath("", internal));
  QCHECK(!root::RootMatchesInternalDataPath(computed, ""));
  QCHECK(!root::RootMatchesInternalDataPath("", ""));
}

}  // namespace

int main() {
  TheFilesFolderIsTheAppsOwnForItsUser();
  TheRootIsNevrBelowTheFilesFolder();
  TheRootAgreesWithTheActivitysInternalDataPathOnlyForTheSameFolder();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "update_root_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("update_root_test: the folder formula, the update root and its comparison with the activity's folder hold\n");
  return 0;
}
