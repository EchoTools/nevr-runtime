// nevr_root.h: where nEVR keeps what it writes and reads on a PC (ADR 0012, #857).
//
// One root per game, %LOCALAPPDATA%\nevr\echovr. The run log and crash record (logs\), the asset cache (cache\), the cosmetic
// packages (cosmetics\v1\packages) and the sign-in cache (.credentials.json) resolve under it; %LOCALAPPDATA%\EchoVR is the original game's folder and nothing of ours is
// written there. config.yaml is read from the root unless the command line names a file (#1255). The names are defined here once. The host
// (runtime/host/host_paths.h) pins its own copies of the root, logs and crashes names to these at compile time, so the host and
// the runtime cannot name different folders.
//
// Header-only and free of writable data (the host links no core state, tools/verify_host_no_core_state.py). The pure functions
// take the folder as an argument so a test supplies its own; the Windows wrappers read %LOCALAPPDATA% and nothing else.
#pragma once

#include <cstddef>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace nevr_root {

template <typename C>
struct Names;

template <>
struct Names<char> {
  static constexpr const char* kRoot = "nevr\\echovr";
  static constexpr const char* kLogs = "logs";
  static constexpr const char* kCrashes = "crashes";
  static constexpr const char* kCache = "cache";
  static constexpr const char* kCosmetics = "cosmetics\\v1\\packages";
  static constexpr const char* kPlugins = "plugins";
  static constexpr const char* kContent = "content";
  static constexpr const char* kCredentials = ".credentials.json";
  static constexpr const char* kConfigYaml = "config.yaml";
  static constexpr const char* kMovedMarker = "MOVED-TO-NEVR-ROOT.txt";
  static constexpr const char* kSeparators = "\\/";
};

template <>
struct Names<wchar_t> {
  static constexpr const wchar_t* kRoot = L"nevr\\echovr";
  static constexpr const wchar_t* kLogs = L"logs";
  static constexpr const wchar_t* kCrashes = L"crashes";
  static constexpr const wchar_t* kCache = L"cache";
  static constexpr const wchar_t* kCosmetics = L"cosmetics\\v1\\packages";
  static constexpr const wchar_t* kPlugins = L"plugins";
  static constexpr const wchar_t* kContent = L"content";
  static constexpr const wchar_t* kCredentials = L".credentials.json";
  static constexpr const wchar_t* kConfigYaml = L"config.yaml";
  static constexpr const wchar_t* kMovedMarker = L"MOVED-TO-NEVR-ROOT.txt";
  static constexpr const wchar_t* kSeparators = L"\\/";
};

// base + one backslash + child; empty when base is empty (an unresolved root stays unresolved all the way down).
template <typename C>
std::basic_string<C> Join(const std::basic_string<C>& base, const C* child) {
  if (base.empty()) return std::basic_string<C>();
  std::basic_string<C> out = base;
  const C last = out.back();
  if (last != static_cast<C>('\\') && last != static_cast<C>('/')) out += static_cast<C>('\\');
  out += child;
  return out;
}

// The root under a Local AppData folder; empty when the folder is empty.
template <typename C>
std::basic_string<C> RootUnder(const std::basic_string<C>& local_app_data) {
  return Join(local_app_data, Names<C>::kRoot);
}

template <typename C>
std::basic_string<C> LogsDir(const std::basic_string<C>& root) {
  return Join(root, Names<C>::kLogs);
}

template <typename C>
std::basic_string<C> CrashesDir(const std::basic_string<C>& root) {
  return Join(root, Names<C>::kCrashes);
}

template <typename C>
std::basic_string<C> CacheDir(const std::basic_string<C>& root) {
  return Join(root, Names<C>::kCache);
}

template <typename C>
std::basic_string<C> CosmeticsDir(const std::basic_string<C>& root) {
  return Join(root, Names<C>::kCosmetics);
}

template <typename C>
std::basic_string<C> PluginsDir(const std::basic_string<C>& root) {
  return Join(root, Names<C>::kPlugins);
}

// Content packs (docs/reference/content-packs.md): <root>\content\<id>\ is one pack's folder, and the serving plugin's own.
template <typename C>
std::basic_string<C> ContentDir(const std::basic_string<C>& root) {
  return Join(root, Names<C>::kContent);
}

template <typename C>
std::basic_string<C> CredentialsPath(const std::basic_string<C>& root) {
  return Join(root, Names<C>::kCredentials);
}

// config.yaml, the one file the runtime reads its settings from (#1255): the root's own copy, unless the command line names another.
template <typename C>
std::basic_string<C> ConfigYamlPath(const std::basic_string<C>& root) {
  return Join(root, Names<C>::kConfigYaml);
}

#ifdef _WIN32

// The root of the Windows user running this process, from %LOCALAPPDATA%. Empty when the variable is unset or does not fit.
inline std::string RootA() {
  char buf[MAX_PATH] = {};
  const DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return std::string();
  return RootUnder(std::string(buf, n));
}

inline std::wstring RootW() {
  wchar_t buf[MAX_PATH * 2] = {};
  const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
  if (n == 0 || n >= sizeof(buf) / sizeof(buf[0])) return std::wstring();
  return RootUnder(std::wstring(buf, n));
}

namespace detail {
inline BOOL MakeDir(const char* p) { return CreateDirectoryA(p, nullptr); }
inline BOOL MakeDir(const wchar_t* p) { return CreateDirectoryW(p, nullptr); }
inline DWORD Attributes(const char* p) { return GetFileAttributesA(p); }
inline DWORD Attributes(const wchar_t* p) { return GetFileAttributesW(p); }
}  // namespace detail

// Creates the folder and every missing parent below the drive. True when the folder exists afterwards. A path that names a
// file where a folder is needed is false.
template <typename C>
bool EnsureDirectoryTree(const std::basic_string<C>& path) {
  if (path.empty()) return false;
  for (std::size_t i = 3; i <= path.size(); ++i) {
    if (i != path.size() && path[i] != static_cast<C>('\\') && path[i] != static_cast<C>('/')) continue;
    const std::basic_string<C> prefix = path.substr(0, i);
    if (!detail::MakeDir(prefix.c_str()) && GetLastError() != ERROR_ALREADY_EXISTS) {
      const DWORD attributes = detail::Attributes(prefix.c_str());
      if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) return false;
    }
  }
  const DWORD attributes = detail::Attributes(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

#endif  // _WIN32

}  // namespace nevr_root
