#pragma once
// The Quest update root and the shim's version record, as pure rules: no logging, no Android headers, so the host tests
// run the code the device runs (the pattern of the quest/social/menu_version_label rules). update_root.cpp is the Android
// side: it reads the process's own folder and writes the records.
//
// The update root (docs/adr/0024-pc-self-update.md, section 2): the folder that holds `update/` is the app's internal files
// folder with `nevr` below it, /data/user/<uid / kAidUserOffset>/<package>/files/nevr. It is not sentinel::FilesDir(), which is
// the external folder (quest/sentinel/sentinel_log.h).
//
// The shim's `versions` record: the shim (the host on Quest) and the runtime are apart, so one line says what runs.
// Every value is text. A value that was not read is "unknown"; a runtime that was not mapped is "not loaded".

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nevr_quest::update_root {

inline constexpr char kRootFolderName[] = "nevr";
inline constexpr char kUnknownValue[] = "unknown";
inline constexpr char kNotLoadedValue[] = "not loaded";

// The root below the app's internal files folder. An empty folder gives an empty root: no folder, no root, and the
// updater then does nothing.
inline std::string RootFromFilesDir(std::string_view files_dir) {
  if (files_dir.empty()) return std::string();
  std::string root(files_dir);
  root += '/';
  root += kRootFolderName;
  return root;
}

// Whether `root` is the folder the activity names as its internal data path with the root's folder name below it. The
// activity's internalDataPath is the app's files folder (getFilesDir()), so the formula agrees with the platform only when
// this holds (H5, docs/adr/0024-pc-self-update.md section 2).
inline bool RootMatchesInternalDataPath(std::string_view root, std::string_view internal_data_path) {
  if (root.empty() || internal_data_path.empty()) return false;
  return root == RootFromFilesDir(internal_data_path);
}

// What the shim knows for its `versions` record. A caller that could not read a value keeps the default.
struct VersionsFacts {
  std::string host_version = kUnknownValue;       // the shim's NEVR-BUILD version: the host on Quest
  std::string host_sha256 = kUnknownValue;        // the shim's own file
  std::string runtime_version = kNotLoadedValue;  // the runtime's NEVR-BUILD version ("unknown": mapped, but no literal found)
  std::string runtime_sha256 = kUnknownValue;     // the runtime file this process mapped
  std::string runtime_path = kUnknownValue;       // where that file was mapped from
  std::string apk_version_code = kUnknownValue;   // the installed APK's Android versionCode
};

// The smallest versionCode an installed APK has: Android's codes are positive 32-bit integers (tools/quest_apk_version.py
// derives ours from the build's version and refuses a code below the newest release's).
inline constexpr long long kMinApkVersionCode = 1;

// The versionCode the platform reported, as the record's text. A code below the minimum means the read found none; the record
// then keeps "unknown", never "0".
inline std::string ApkVersionCodeText(long long code) {
  if (code < kMinApkVersionCode) return kUnknownValue;
  return std::to_string(code);
}

// The record's values in their order: the host and the runtime apart, then the APK. The writer adds the tag and the event.
inline std::vector<std::pair<std::string, std::string>> VersionsFields(const VersionsFacts& facts) {
  return {
      {"host_version", facts.host_version},
      {"host_sha256", facts.host_sha256},
      {"runtime_version", facts.runtime_version},
      {"runtime_sha256", facts.runtime_sha256},
      {"runtime_path", facts.runtime_path},
      {"apk_version_code", facts.apk_version_code},
  };
}

}  // namespace nevr_quest::update_root
