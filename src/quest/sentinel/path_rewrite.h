/* The data-path rewrite for a pinned service-only libr15 (#1737).
 *
 * The reference build's libr15 (the one file in kLibR15ServiceOnlySha256) names its data directory by a literal,
 * /sdcard/Android/media/com.readyatdawn.afk/...: CSysInfo::GetDataRootDir (0xf87250) and CSysJava::InternalStoragePath
 * (0xf7fbb4) return it, and the engine builds every data path from those. When that file runs under another package
 * (com.readyatdawn.r15), the app's media directory is the running package's, so those paths name a directory the game does not
 * own. The rewrite turns the prefix /sdcard/Android/media/com.readyatdawn.afk into /sdcard/Android/media/<running package> on
 * the path argument of the file-system calls libr15 makes (path_rewrite_hook.h), into a caller-owned buffer; the caller's
 * string is never written. Pure: no file system, no log.
 *
 * The match is exact on the package segment: the next character after the prefix is '/' or the end of the string, so
 * .../com.readyatdawn.afkx/... is not rewritten. With the running package equal to com.readyatdawn.afk, nothing is rewritten.
 */
#pragma once

#include <cstddef>
#include <cstring>

namespace nevr_quest::path_rewrite {

inline constexpr const char* kMediaDir = "/sdcard/Android/media/";
inline constexpr const char* kFromPackage = "com.readyatdawn.afk";
// Room for any path the engine builds (its CFixedStringT<512>) plus the longest package the prefix may grow by.
inline constexpr std::size_t kBufferSize = 640;

// The package name from the first NUL-terminated field of /proc/self/cmdline (`cmdline`, `length` bytes): the text before
// the first NUL or ':' (a process of the app named "<package>:<service>"). True and the name in out[cap] when it is
// non-empty, fits and holds only [A-Za-z0-9._]; false otherwise (out is then "").
inline bool PackageFromCmdline(const char* cmdline, std::size_t length, char* out, std::size_t cap) noexcept {
  if (out == nullptr || cap == 0) return false;
  out[0] = '\0';
  if (cmdline == nullptr) return false;
  std::size_t n = 0;
  while (n < length && cmdline[n] != '\0' && cmdline[n] != ':') {
    const char c = cmdline[n];
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_';
    if (!ok || n + 1 >= cap) {
      out[0] = '\0';
      return false;
    }
    out[n] = c;
    ++n;
  }
  out[n] = '\0';
  return n > 0;
}

// `path` with kMediaDir + kFromPackage replaced by kMediaDir + `runningPackage`, written into out[cap], and `out`
// returned; `path` itself when it does not start with that prefix followed by '/' or the end, when `runningPackage` is
// null, empty or kFromPackage (the identity), or when the result does not fit `cap`.
inline const char* Rewrite(const char* path, const char* runningPackage, char* out, std::size_t cap) noexcept {
  if (path == nullptr || runningPackage == nullptr || runningPackage[0] == '\0' || out == nullptr) return path;
  if (std::strcmp(runningPackage, kFromPackage) == 0) return path;
  const std::size_t dirLen = std::strlen(kMediaDir);
  const std::size_t fromLen = std::strlen(kFromPackage);
  if (std::strncmp(path, kMediaDir, dirLen) != 0) return path;
  if (std::strncmp(path + dirLen, kFromPackage, fromLen) != 0) return path;
  const char* const rest = path + dirLen + fromLen;
  if (*rest != '/' && *rest != '\0') return path;
  const std::size_t pkgLen = std::strlen(runningPackage);
  const std::size_t restLen = std::strlen(rest);
  if (dirLen + pkgLen + restLen + 1 > cap) return path;
  std::memcpy(out, kMediaDir, dirLen);
  std::memcpy(out + dirLen, runningPackage, pkgLen);
  std::memcpy(out + dirLen + pkgLen, rest, restLen + 1);
  return out;
}

}  // namespace nevr_quest::path_rewrite
