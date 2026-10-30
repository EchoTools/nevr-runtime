// A per-module off switch: an empty file `<name>` in the plugin's data directory turns the module off (the source build's
// "off = memsys.off" convention; weapon_arm.cpp's `t26arm.off` is the same idea). The directory is the one weapon_arm.cpp
// uses: /sdcard/Android/data/<package>/files/q15t, the package name read from /proc/self/cmdline.
//
// Header-only and constructor-free (an inline char array is constant-initialised): the repack's static-init check refuses
// a plugin with a namespace-scope constructor. A directory that cannot be worked out (no package name) reads as "no
// switch present": the module stays ON, which is the source build's default for memsys.
#pragma once

#include <unistd.h>

#include <cstdio>
#include <cstring>

namespace q15t {

inline char g_switch_dir_override[256] = "";  // tests only; empty = the real directory

inline void SetModuleSwitchDirForTest(const char* dir) {
  std::snprintf(g_switch_dir_override, sizeof g_switch_dir_override, "%s", dir != nullptr ? dir : "");
}

// The package name of this process (the first field of /proc/self/cmdline up to a ':'), the part of the data directory's path the
// game's storage layout is keyed by. False when it cannot be worked out.
inline bool ModulePackageName(char* out, std::size_t n) {
  char cmd[256] = "";
  FILE* f = std::fopen("/proc/self/cmdline", "re");
  if (f == nullptr) return false;
  const std::size_t r = std::fread(cmd, 1, sizeof cmd - 1, f);
  std::fclose(f);
  cmd[r] = 0;
  if (char* colon = std::strchr(cmd, ':')) *colon = 0;
  if (std::strchr(cmd, '.') == nullptr || std::strchr(cmd, '/') != nullptr || std::strlen(cmd) > 160 || std::strlen(cmd) >= n) return false;
  std::snprintf(out, n, "%.160s", cmd);  // the check above admits at most 160 characters; the precision lets the compiler see it
  return true;
}

// The plugin's data directory: the test override, else /sdcard/Android/data/<package>/files/q15t. False when there is none to name.
inline bool ModuleDataDir(char* out, std::size_t n) {
  if (g_switch_dir_override[0] != 0) {
    std::snprintf(out, n, "%s", g_switch_dir_override);
    return true;
  }
  char pkg[200];
  if (!ModulePackageName(pkg, sizeof pkg)) return false;
  std::snprintf(out, n, "/sdcard/Android/data/%s/files/q15t", pkg);
  return true;
}

// True when `<data dir>/<name>` exists. Never creates anything.
inline bool ModuleSwitchPresent(const char* name) {
  char dir[300] = "";
  if (!ModuleDataDir(dir, sizeof dir)) return false;
  char path[400];
  std::snprintf(path, sizeof path, "%s/%s", dir, name);
  return access(path, F_OK) == 0;
}

}  // namespace q15t
