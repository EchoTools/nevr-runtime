// Host test for the Quest update root and the shim's version record (design piece quest-root-and-versions). It runs the pure
// rules the device runs: the app-internal folder formula (quest/auth/app_files_rules.h), the update root and its H5
// comparison, the versions record's fields and the APK versionCode's text (quest/shim/update_root_rules.h), and the read of the
// runtime's version from its NEVR-BUILD literal (core/build_literal_scan.h, which the shim's loader uses). The Android side
// (update_root.cpp: the read of the process's command line, the versionCode through JNI, the records at android_main's entry)
// needs the sentinel's logging and the NDK; its records are read on a headset, not here.
//
// Built and run by `just test-quest-shared` (src/quest/host_tests/groups/shared.cmake).

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "core/build_literal_scan.h"
#include "quest/auth/app_files_rules.h"
#include "quest/shim/update_root_rules.h"
#include "quest/tests/test_check.h"

namespace {

namespace app = nevr_quest::app_files;
namespace bl = nevr_build_literal;
namespace root = nevr_quest::update_root;

// Bionic's AID_USER_OFFSET, as the Android headers define it: the value the formula must keep.
constexpr unsigned kBionicAidUserOffset = 100000;
// The uid of the app's process on user 0 (the device the token cache's test names) and on user 10.
constexpr unsigned kUserZeroAppUid = 10234;
constexpr unsigned kUserTenAppUid = 1010234;
constexpr std::size_t kVersionsFieldCount = 6;
constexpr long long kExampleVersionCode = 18;
constexpr char kPackage[] = "com.readyatdawn.r15";
// A build literal's parts (core/build_identity.h): the version and a 40-digit lower-case commit.
constexpr char kLiteralVersion[] = "6.18.1-dev.3+abcdef012345";
constexpr char kLiteralCommit[] = "0123456789abcdef0123456789abcdef01234567";
constexpr char kFiller = 'x';
constexpr std::size_t kTrailerBytes = 3;

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

// The APK's versionCode is its decimal digits; a code below 1 means the read found none and the record keeps "unknown".
void TheApkVersionCodeIsItsDigitsOrUnknown() {
  QCHECK(root::ApkVersionCodeText(kExampleVersionCode) == std::to_string(kExampleVersionCode));
  QCHECK(root::ApkVersionCodeText(root::kMinApkVersionCode) == std::to_string(root::kMinApkVersionCode));
  QCHECK(root::ApkVersionCodeText(std::numeric_limits<std::int32_t>::max()) ==
         std::to_string(std::numeric_limits<std::int32_t>::max()));
  QCHECK(root::ApkVersionCodeText(root::kMinApkVersionCode - 1) == root::kUnknownValue);
  constexpr long long kNoVersionCode = -1;  // below the minimum, so no version: the value reads as unknown
  QCHECK(root::ApkVersionCodeText(kNoVersionCode) == root::kUnknownValue);
  root::VersionsFacts facts;
  facts.apk_version_code = root::ApkVersionCodeText(kExampleVersionCode);
  QCHECK(Find(root::VersionsFields(facts), "apk_version_code") == std::to_string(kExampleVersionCode));
}

// A build literal as the build embeds it (core/build_identity.h): NUL, "NEVR-BUILD ", the version, a space, the commit, NUL.
std::string Literal(const std::string& version, const std::string& commit = kLiteralCommit) {
  std::string literal(1, '\0');
  literal += "NEVR-BUILD ";
  literal += version;
  literal += ' ';
  literal += commit;
  literal.push_back('\0');
  return literal;
}

std::string VersionIn(const std::string& bytes) { return bl::VersionIn(bytes.data(), bytes.size()); }

// bl::VersionOfFile over an anonymous temporary file that holds `bytes`.
std::string VersionOfFileHolding(const std::string& bytes) {
  std::FILE* file = std::tmpfile();
  QCHECK(file != nullptr);
  if (file == nullptr) return std::string();
  QCHECK(std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size());
  QCHECK(std::fflush(file) == 0);
  const std::string version = bl::VersionOfFile(::fileno(file), bytes.size());
  std::fclose(file);
  return version;
}

// The runtime's version is the literal's version field, and only a whole literal gives one: the prefix, a version of at most
// bl::kMaxVersionBytes characters, a space, 40 lower-case hex digits and the closing NUL. A copy of the prefix with anything else
// after it (the reader's own copy, in the data of a file built from the same sources) is not a literal and does not hide the
// real one after it.
void TheBuildLiteralIsFoundOnlyWhenItIsWhole() {
  const std::string literal = Literal(kLiteralVersion);
  QCHECK(VersionIn(literal) == kLiteralVersion);
  QCHECK(VersionIn(std::string(kTrailerBytes, kFiller) + literal + std::string(kTrailerBytes, kFiller)) == kLiteralVersion);
  const std::string decoy = Literal("1.2.3", std::string(bl::kCommitBytes, 'g'));
  QCHECK(VersionIn(decoy).empty());
  QCHECK(VersionIn(decoy + std::string(kTrailerBytes, kFiller) + literal) == kLiteralVersion);
  // The commit: upper case, one digit short, one digit long.
  QCHECK(VersionIn(Literal(kLiteralVersion, std::string(bl::kCommitBytes, 'A'))).empty());
  QCHECK(VersionIn(Literal(kLiteralVersion, std::string(bl::kCommitBytes - 1, 'a'))).empty());
  QCHECK(VersionIn(Literal(kLiteralVersion, std::string(bl::kCommitBytes + 1, 'a'))).empty());
  // The bounds: no closing NUL (cut short, or another byte in its place) and no opening NUL.
  QCHECK(VersionIn(literal.substr(0, literal.size() - 1)).empty());
  QCHECK(VersionIn(literal.substr(0, literal.size() - 1) + kFiller).empty());
  QCHECK(VersionIn(kFiller + literal.substr(1)).empty());
  // The version: empty, a character outside the set, the longest allowed and one longer.
  QCHECK(VersionIn(Literal("")).empty());
  QCHECK(VersionIn(Literal("1_2")).empty());
  QCHECK(VersionIn(Literal(std::string(bl::kMaxVersionBytes, '1'))) == std::string(bl::kMaxVersionBytes, '1'));
  QCHECK(VersionIn(Literal(std::string(bl::kMaxVersionBytes + 1, '1'))).empty());
}

// The file is read in pieces of bl::kChunkBytes. A literal that the end of a piece cuts after any of its bytes is found whole,
// the longest literal included (a version of bl::kMaxVersionBytes characters: its first byte is bl::kLiteralBytes - 1 bytes
// before the end of the piece that holds all but its last byte), and so is one that lies in a later piece; a file with no
// literal, or only a copy of the prefix, gives none.
void TheBuildLiteralIsFoundWhereTheReadsSplitIt() {
  const std::string longest(bl::kMaxVersionBytes, '1');
  const std::string longestLiteral = Literal(longest);
  QCHECK(longestLiteral.size() == bl::kLiteralBytes);
  for (std::size_t split = 1; split < longestLiteral.size(); ++split) {
    const std::string bytes =
        std::string(bl::kChunkBytes - split, kFiller) + longestLiteral + std::string(kTrailerBytes, kFiller);
    const std::string found = VersionOfFileHolding(bytes);
    QCHECK(found == longest);
    if (found != longest) std::fprintf(stderr, "  literal cut after %zu of its %zu bytes\n", split, longestLiteral.size());
  }
  const std::string literal = Literal(kLiteralVersion);
  QCHECK(VersionOfFileHolding(literal + std::string(kTrailerBytes, kFiller)) == kLiteralVersion);
  const std::string decoy = Literal("1.2.3", std::string(bl::kCommitBytes, 'g'));
  const std::string later = std::string(bl::kChunkBytes + kTrailerBytes, kFiller) + literal;
  QCHECK(VersionOfFileHolding(decoy + later) == kLiteralVersion);
  QCHECK(VersionOfFileHolding(decoy + std::string(bl::kChunkBytes, kFiller)).empty());
  QCHECK(VersionOfFileHolding(std::string(bl::kChunkBytes * 2, kFiller)).empty());
  QCHECK(VersionOfFileHolding(std::string()).empty());
}

}  // namespace

int main() {
  TheFilesFolderIsTheAppsOwnForItsUser();
  TheRootIsNevrBelowTheFilesFolder();
  TheRootAgreesWithTheActivitysInternalDataPathOnlyForTheSameFolder();
  TheVersionsRecordKeepsTheHostAndTheRuntimeApart();
  AnUnmappedRuntimeIsNotLoaded();
  TheApkVersionCodeIsItsDigitsOrUnknown();
  TheBuildLiteralIsFoundOnlyWhenItIsWhole();
  TheBuildLiteralIsFoundWhereTheReadsSplitIt();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "update_root_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf(
      "update_root_test: the folder formula, the update root, the H5 comparison, the versions record and the build literal "
      "scan hold\n");
  return 0;
}
