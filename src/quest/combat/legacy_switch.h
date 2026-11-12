/* The former switch files, which the combat modules no longer read (docs/adr/0046 C2): a module whose switch is a `combat:` key asks here
 * whether an old switch file is still in the data directory, so it can warn and name the key that replaces it. The same directory holds
 * the gun-hand markers (weapon_arm.h), which are player state (docs/adr/0046 rule 4).
 * Header only; the directory is /sdcard/Android/data/<package>/files/<kDataSubdir>, with the package from /proc/self/cmdline.
 * Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace nevr_quest::combat::legacy {

inline std::atomic<bool> g_dirSet{false};
inline char g_dir[300] = "";  // tests only (SetDirectoryForTest)

// The data directory's name below the app's external files folder. Players' devices hold their gun-hand markers there (weapon_arm.h),
// so the name stays as it is: a module that looked elsewhere would find no marker and treat every profile as new (docs/adr/0046 rule 4).
inline constexpr const char* kDataSubdir = "q15t";

// This process's package name (the first field of /proc/self/cmdline up to a ':'), the part of the game's storage paths it is keyed by. False when it
// cannot be worked out.
inline bool PackageName(char* out, std::size_t n) noexcept {
  char cmd[256] = {};
  std::FILE* f = std::fopen("/proc/self/cmdline", "re");
  if (f == nullptr) return false;
  const std::size_t got = std::fread(cmd, 1, sizeof(cmd) - 1, f);
  std::fclose(f);
  cmd[got] = '\0';
  if (char* colon = std::strchr(cmd, ':')) *colon = '\0';
  if (std::strchr(cmd, '.') == nullptr || std::strchr(cmd, '/') != nullptr || std::strlen(cmd) > 160 || std::strlen(cmd) >= n) return false;
  std::snprintf(out, n, "%.160s", cmd);
  return true;
}

// The data directory, or the test's. False when none can be named.
inline bool Directory(char* out, std::size_t n) noexcept {
  if (g_dirSet.load(std::memory_order_acquire)) {
    std::snprintf(out, n, "%s", g_dir);
    return true;
  }
  char pkg[200] = {};
  if (!PackageName(pkg, sizeof(pkg))) return false;
  std::snprintf(out, n, "/sdcard/Android/data/%.160s/files/%s", pkg, kDataSubdir);
  return true;
}

// True when `file` is in the data directory.
inline bool Present(const char* file) noexcept {
  char dir[300] = {};
  if (!Directory(dir, sizeof(dir))) return false;
  char path[400];
  std::snprintf(path, sizeof(path), "%s/%s", dir, file);
  return access(path, F_OK) == 0;
}

// The directory to look in (nullptr or "" restores production).
inline void SetDirectoryForTest(const char* dir) noexcept {
  std::snprintf(g_dir, sizeof(g_dir), "%s", dir != nullptr ? dir : "");
  g_dirSet.store(dir != nullptr && dir[0] != '\0', std::memory_order_release);
}

}  // namespace nevr_quest::combat::legacy
