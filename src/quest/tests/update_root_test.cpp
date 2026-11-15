// Host test for the Quest update root and the shim's version record (design piece quest-root-and-versions). It runs the pure
// rules the device runs: the app-internal folder formula (quest/auth/app_files_rules.h), the update root and its H5
// comparison, and the versions record's fields (quest/shim/update_root_rules.h). The Android side (update_root.cpp: the read
// of the process's command line, the record at android_main's entry) needs the sentinel's logging and the NDK; its record
// is read on a headset, not here.
//
// Built and run by `just test-quest-shared` (src/quest/host_tests/groups/shared.cmake).

#include <cstddef>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "quest/auth/app_files_rules.h"
#include "quest/shim/update_root_rules.h"
#include "quest/tests/test_check.h"

namespace {

namespace app = nevr_quest::app_files;
namespace root = nevr_quest::update_root;

// Bionic's AID_USER_OFFSET, as the Android headers define it: the value the formula must keep.
constexpr unsigned kBionicAidUserOffset = 100000;
// The uid of the app's process on user 0 (the device the token cache's test names) and on user 10.
constexpr unsigned kUserZeroAppUid = 10234;
constexpr unsigned kUserTenAppUid = 1010234;
constexpr std::size_t kVersionsFieldCount = 6;
constexpr char kPackage[] = "com.readyatdawn.r15";

using Fields = std::vector<std::pair<std::string, std::string>>;

std::string Find(const Fields& fields, const std::string& key) {
  for (const auto& field : fields) {
    if (field.first == key) return field.second;
  }
  return std::string();
}

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

// H5: the computed root agrees with the folder the activity names as its internal data path (getFilesDir()), with the
// root's folder name below it. Another user's folder, the files folder itself, and an empty path do not agree.
void TheRootAgreesWithTheActivitysInternalDataPathOnlyForTheSameFolder() {
  const std::string internal = "/data/user/0/com.readyatdawn.r15/files";
  const std::string computed = root::RootFromFilesDir(app::FilesDirFromCmdline(kPackage, kUserZeroAppUid));
  QCHECK(root::RootMatchesInternalDataPath(computed, internal));
  QCHECK(!root::RootMatchesInternalDataPath(computed, "/data/user/10/com.readyatdawn.r15/files"));
  QCHECK(!root::RootMatchesInternalDataPath(internal, internal));
  QCHECK(!root::RootMatchesInternalDataPath("", internal));
  QCHECK(!root::RootMatchesInternalDataPath(computed, ""));
}

// The versions record keeps the host and the runtime apart; a value the caller did not read is "unknown".
void TheVersionsRecordKeepsTheHostAndTheRuntimeApart() {
  root::VersionsFacts facts;
  facts.host_version = "6.18.0";
  facts.runtime_version = "6.18.1";
  facts.runtime_sha256 = "runtime-digest";
  facts.apk_version_code = "18";
  const Fields fields = root::VersionsFields(facts);
  QCHECK(fields.size() == kVersionsFieldCount);
  QCHECK(Find(fields, "host_version") == "6.18.0");
  QCHECK(Find(fields, "runtime_version") == "6.18.1");
  QCHECK(Find(fields, "runtime_sha256") == "runtime-digest");
  QCHECK(Find(fields, "apk_version_code") == "18");
  QCHECK(Find(fields, "host_sha256") == root::kUnknownValue);
  QCHECK(Find(fields, "runtime_path") == root::kUnknownValue);
}

// A runtime that was not mapped reads "not loaded", and a default host reads "unknown".
void AnUnmappedRuntimeIsNotLoaded() {
  const Fields fields = root::VersionsFields(root::VersionsFacts());
  QCHECK(Find(fields, "runtime_version") == "not loaded");  // the literal the record carries, not the constant that names it
  QCHECK(Find(fields, "host_version") == "unknown");
}

}  // namespace

int main() {
  TheFilesFolderIsTheAppsOwnForItsUser();
  TheRootIsNevrBelowTheFilesFolder();
  TheRootAgreesWithTheActivitysInternalDataPathOnlyForTheSameFolder();
  TheVersionsRecordKeepsTheHostAndTheRuntimeApart();
  AnUnmappedRuntimeIsNotLoaded();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "update_root_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("update_root_test: the folder formula, the update root, the H5 comparison and the versions record hold\n");
  return 0;
}
