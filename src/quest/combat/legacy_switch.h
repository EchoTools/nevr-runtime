/* The q15t plugin's data directory, for the one thing a folded combat module still does with it: look for the plugin's old switch files and
 * say, in a Warning, that they are no longer read (docs/adr/0046 C2). Header only.
 *
 * The directory is /sdcard/Android/data/<package>/files/q15t, the package from /proc/self/cmdline (the plugin's own rule). A module never
 * creates, reads the content of, or acts on a file there.
 */
#pragma once

#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace nevr_quest::combat::legacy {

inline std::atomic<bool> g_dirSet{false};
inline char g_dir[300] = "";  // tests only (SetDirForTest)

// Production: the plugin's data directory. A test: the directory it set. False when none can be named.
inline bool Dir(char* out, std::size_t n) noexcept {
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

// True when `<dir>/<file>` exists. Never creates anything.
inline bool Present(const char* file) noexcept {
  char dir[300] = {};
  if (!Dir(dir, sizeof(dir))) return false;
  char path[400];
  std::snprintf(path, sizeof(path), "%s/%s", dir, file);
  return access(path, F_OK) == 0;
}

// nullptr or "" restores production.
inline void SetDirForTest(const char* dir) noexcept {
  std::snprintf(g_dir, sizeof(g_dir), "%s", dir != nullptr ? dir : "");
  g_dirSet.store(dir != nullptr && dir[0] != '\0', std::memory_order_release);
}

}  // namespace nevr_quest::combat::legacy
