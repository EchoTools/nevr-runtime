#pragma once
// The app-internal files folder of the process, as a pure rule: no I/O, no Android headers. The package name comes from the
// process's command line and the Android user from its uid. The token cache (quest/auth/file_store.cpp) uses this one
// formula, so it exists once.

#include <cstddef>
#include <string>

namespace nevr_quest::app_files {

// Bionic's AID_USER_OFFSET (<private/android_filesystem_config.h>): each Android user's app uids start at a multiple of it,
// so uid / kAidUserOffset is the user.
inline constexpr unsigned kAidUserOffset = 100000;

// A Java package name has at least three characters (a.b) and at most this many.
inline constexpr std::size_t kMinPackageChars = 3;
inline constexpr std::size_t kMaxPackageChars = 128;

// "/data/user/<uid / kAidUserOffset>/<package>/files" for the package named in `cmdline` (its first NUL-terminated argument).
// "" when that argument does not look like a Java package name (a ":service" process, an unexpected launcher): the caller
// must not fall back to external storage.
inline std::string FilesDirFromCmdline(const std::string& cmdline, unsigned uid) {
  const std::string name = cmdline.substr(0, cmdline.find('\0'));
  // A Java package name: dotted identifiers, at least two segments, no ':' process suffix.
  if (name.size() < kMinPackageChars || name.size() > kMaxPackageChars || name.front() == '.' || name.back() == '.') {
    return "";
  }
  bool dot = false;
  for (const char c : name) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
    if (!ok) return "";
    if (c == '.') dot = true;
  }
  if (!dot || name.find("..") != std::string::npos) return "";
  return "/data/user/" + std::to_string(uid / kAidUserOffset) + "/" + name + "/files";
}

}  // namespace nevr_quest::app_files
