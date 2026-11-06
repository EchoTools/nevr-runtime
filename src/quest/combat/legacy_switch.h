/* The q15t plugin's kill files, which the folded combat modules no longer read (docs/adr/0046 C2): a module that replaces a plugin switch
 * asks here whether the old file is still in the plugin's data directory, so it can warn and name the `combat:` key that replaces it.
 * Header only; the directory is the plugin's own, /sdcard/Android/data/<package>/files/q15t, with the package from /proc/self/cmdline.
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

// The plugin's data directory, or the test's. False when none can be named.
inline bool Directory(char* out, std::size_t n) noexcept {
  if (g_dirSet.load(std::memory_order_acquire)) {
    std::snprintf(out, n, "%s", g_dir);
    return true;
  }
  char cmd[256] = {};
  std::FILE* f = std::fopen("/proc/self/cmdline", "re");
  if (f == nullptr) return false;
  const std::size_t got = std::fread(cmd, 1, sizeof(cmd) - 1, f);
  std::fclose(f);
  cmd[got] = '\0';
  if (char* colon = std::strchr(cmd, ':')) *colon = '\0';
  if (std::strchr(cmd, '.') == nullptr || std::strchr(cmd, '/') != nullptr || std::strlen(cmd) > 160) return false;
  std::snprintf(out, n, "/sdcard/Android/data/%.160s/files/q15t", cmd);
  return true;
}

// True when `file` is in the plugin's data directory.
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
