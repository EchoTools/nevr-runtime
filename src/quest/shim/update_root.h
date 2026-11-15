#pragma once
// The Android side of the Quest update root and the shim's version record. The rules (the formula, the H5 comparison, the
// field list) are quest/shim/update_root_rules.h; this file reads the process's own folder and writes the records.

#include <string>

#include "quest/shim/update_root_rules.h"

namespace nevr_quest::update_root {

// The running process's update root: the app's internal files folder with `nevr` below it. Empty when the process has no
// such folder (its command line is not a package name), in which case nothing is updated.
std::string Resolve();

// H5, at android_main's entry (registered with process_end::SetAtEntry in entry.cpp, before the game's android_main runs).
// The activity the native glue passes names its internal data path; the computed root must be that folder with `nevr`
// below it. Writes one `update_root` record: Info when they agree, Warning when they do not, and a Warning when the
// activity names no path. Then writes the `versions` record a second time with the APK's versionCode, which the platform gives
// only through the activity (PackageInfo.versionCode, read through the activity's VM and object): Info with the code, or, when
// it cannot be read, LogVersionsApkUnread with the reason. `app` is the android_app android_main was called with (null:
// nothing is read).
void LogRootAtAndroidMain(const void* app) noexcept;

// The shim's `versions` record (the host and the runtime apart, and the APK's versionCode), one Info line with the tag
// NEVR.HOST. The runtime's hash and path come from the loader that maps it; the caller fills them in.
void LogVersions(const VersionsFacts& facts);

// The same record as a Warning, for a start whose APK versionCode could not be read: the facts as given (apk_version_code stays
// "unknown"), a `reason` token naming the step that failed, and the consequence.
void LogVersionsApkUnread(const VersionsFacts& facts, const char* reason);

}  // namespace nevr_quest::update_root
