// game_folder_redirect_rules.h: which file paths the game folder redirect sends to the nEVR root, and where (#575).
//
// The engine finds its install root once (FUN_1400df410 @0x1400df410, called through fcn.1400e2190 @0x1400e2190, fills
// the buffer at 0x14209d030 that Get @0x1400e2110 returns) and every consumer appends its own folder to it:
// CreateLogFile @0x140100790 writes <root>/_local/r14logs/, HandleCrashDump @0x1401cefe0 creates <root>|_temp|crashes
// and a second _temp|crashes relative to the working directory, CSettingsStore::Initialize @0x141521240 keeps
// <root>\_local and <root>\_temp, fcn.1400dbd50 points dbghelp's symbol cache at <root>\_temp\symbols. The same root
// also locates _data, content, sourcedb and scripts, so it is not moved. What moves is any path under <base>\_local or
// <base>\_temp, rewritten at the file calls every binary of the game reaches the disk through, to <target>\_local and
// <target>\_temp. Nothing else changes.
//
// Pure: no Windows headers, no logging. The platform half (game_folder_redirect.cpp) hands it the paths.

#pragma once

#include <cstddef>
#include <cwctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nevr_platform_compat::game_folder_redirect {

// The two folders that move: the game's settings store and logs (_local), and its scratch and crash folder (_temp).
inline constexpr std::wstring_view kLocalName = L"_local";
inline constexpr std::wstring_view kTempName = L"_temp";

// The folder under the nEVR root that holds them: %LOCALAPPDATA%\nevr\echovr\game\_local and ...\game\_temp.
inline constexpr std::wstring_view kTargetChild = L"game";

// The names the engine stops at when it walks up from echovr.exe to find its root (FUN_1400df410 @0x1400df410
// compares each folder name against the strings at 0x1416cdf4c..0x1416cdf6c): the root is that folder's grandparent.
inline constexpr std::wstring_view kRootMarkers[] = {L"win7", L"win10", L"tools", L"win32", L"wvk"};

// \\?\ and \\?\UNC\ (Win32 file namespace prefixes), and \\.\ (the device namespace, which also takes a drive path).
inline constexpr std::wstring_view kLongPrefix = L"\\\\?\\";
inline constexpr std::wstring_view kLongUncPrefix = L"\\\\?\\UNC\\";
inline constexpr std::wstring_view kDevicePrefix = L"\\\\.\\";

// The game's own settings file in _local, which the redirect copies once from the game folder (LegacyConfigJsonPaths).
inline constexpr std::wstring_view kConfigJsonFile = L"config.json";

inline constexpr wchar_t kSeparator = L'\\';
inline constexpr wchar_t kAltSeparator = L'/';
inline constexpr wchar_t kDriveColon = L':';
// "X:" is two characters; "X:\" three.
inline constexpr std::size_t kDriveLength = 2;
// The root is the marker folder's grandparent: two levels up (FUN_1400df410 takes the parent of the folder above it).
inline constexpr std::size_t kMarkerToRootLevels = 2;
// An exe path has a drive and a file name at least.
inline constexpr std::size_t kMinExeComponents = 2;

struct Plan {
  // Absolute folders whose _local and _temp move (the install root and the exe folder), normalized (Normalize).
  std::vector<std::wstring> bases;
  // The absolute folder that receives them (<nevr root>\game), normalized.
  std::wstring target;
};

namespace detail {

inline wchar_t Fold(wchar_t c) { return static_cast<wchar_t>(std::towlower(static_cast<std::wint_t>(c))); }

inline bool EqualNoCase(std::wstring_view a, std::wstring_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (Fold(a[i]) != Fold(b[i])) return false;
  }
  return true;
}

inline bool StartsWithNoCase(std::wstring_view text, std::wstring_view prefix) {
  return text.size() >= prefix.size() && EqualNoCase(text.substr(0, prefix.size()), prefix);
}

inline bool ContainsNoCase(std::wstring_view text, std::wstring_view needle) {
  if (needle.size() > text.size()) return false;
  for (std::size_t i = 0; i + needle.size() <= text.size(); ++i) {
    if (EqualNoCase(text.substr(i, needle.size()), needle)) return true;
  }
  return false;
}

inline bool IsSeparator(wchar_t c) { return c == kSeparator || c == kAltSeparator; }

// A folder name as the system resolves it: Win32 drops a name's trailing dots and spaces, so "_local." is "_local".
inline std::wstring_view AsResolved(std::wstring_view name) {
  while (!name.empty() && (name.back() == L'.' || name.back() == L' ')) name.remove_suffix(1);
  return name;
}

inline bool HasDrive(std::wstring_view p) {
  return p.size() >= kDriveLength && p[1] == kDriveColon &&
         ((p[0] >= L'a' && p[0] <= L'z') || (p[0] >= L'A' && p[0] <= L'Z'));
}

// The components of a path, with "." dropped and ".." taken back (never past the first, the drive or share).
inline std::vector<std::wstring> Components(std::wstring_view p) {
  std::vector<std::wstring> out;
  std::size_t i = 0;
  while (i <= p.size()) {
    std::size_t j = i;
    while (j < p.size() && !IsSeparator(p[j])) ++j;
    const std::wstring_view part = p.substr(i, j - i);
    if (part.empty() || part == L".") {
      // skipped
    } else if (part == L"..") {
      if (out.size() > 1) out.pop_back();
    } else {
      out.emplace_back(part);
    }
    i = j + 1;
  }
  return out;
}

inline std::wstring Join(const std::vector<std::wstring>& parts, std::size_t count) {
  std::wstring out;
  for (std::size_t i = 0; i < count && i < parts.size(); ++i) {
    if (i != 0) out.push_back(kSeparator);
    out += parts[i];
  }
  return out;
}

}  // namespace detail

// An absolute drive path (X:\...) with one backslash between components, no trailing one, "." and ".." resolved.
// Empty for anything else (a relative path, a UNC share, a device path).
inline std::wstring Normalize(std::wstring_view path) {
  if (detail::StartsWithNoCase(path, kLongUncPrefix)) return std::wstring();
  if (detail::StartsWithNoCase(path, kLongPrefix)) path.remove_prefix(kLongPrefix.size());
  if (!detail::HasDrive(path) || path.size() == kDriveLength || !detail::IsSeparator(path[kDriveLength])) {
    return std::wstring();
  }
  return detail::Join(detail::Components(path), static_cast<std::size_t>(-1));
}

// True when the path cannot be under a moved folder: neither name appears in it. Checked before anything is allocated,
// so a call for any other file (every package the game opens) costs one scan.
inline bool CannotMatch(std::wstring_view path) {
  return !detail::ContainsNoCase(path, kLocalName) && !detail::ContainsNoCase(path, kTempName);
}

// Where a path the game passed goes. Nothing (std::nullopt) when it is not under <base>\_local or <base>\_temp of a
// base in the plan, whole folder names only (<base>\_localX stays). `current_dir` resolves a relative path, and a
// relative path behind \\?\ (the game passes one, #1845), as the system would. A \\?\ prefix on an absolute path is
// kept. The result is never under a base again, so rewriting it twice changes nothing.
inline std::optional<std::wstring> Rewrite(const Plan& plan, std::wstring_view path, std::wstring_view current_dir) {
  if (path.empty() || plan.target.empty() || CannotMatch(path)) return std::nullopt;
  if (detail::StartsWithNoCase(path, kLongUncPrefix)) return std::nullopt;

  std::wstring_view kept_prefix;  // \\?\ or \\.\ in front of a drive path, given back on the result
  for (const std::wstring_view prefix : {kLongPrefix, kDevicePrefix}) {
    if (detail::StartsWithNoCase(path, prefix)) {
      path.remove_prefix(prefix.size());
      kept_prefix = prefix;
      break;
    }
  }
  if (kept_prefix == kDevicePrefix && !detail::HasDrive(path)) return std::nullopt;  // a device (\\.\pipe\...)

  std::wstring absolute;
  if (detail::HasDrive(path)) {
    if (path.size() == kDriveLength || !detail::IsSeparator(path[kDriveLength])) {
      // X:relative is relative to the current folder of that drive; only the current drive's is known.
      const std::wstring cwd = Normalize(current_dir);
      if (cwd.empty() || !detail::EqualNoCase(cwd.substr(0, kDriveLength), path.substr(0, kDriveLength))) {
        return std::nullopt;
      }
      absolute = cwd + kSeparator + std::wstring(path.substr(kDriveLength));
    } else {
      absolute.assign(path);
    }
  } else if (path.size() >= kDriveLength && detail::IsSeparator(path[0]) && detail::IsSeparator(path[1])) {
    return std::nullopt;  // a UNC share or a device path
  } else {
    const std::wstring cwd = Normalize(current_dir);
    if (cwd.empty()) return std::nullopt;
    if (detail::IsSeparator(path[0])) {
      absolute = cwd.substr(0, kDriveLength) + std::wstring(path);  // \foo: the root of the current drive
    } else {
      absolute = cwd + kSeparator + std::wstring(path);
    }
    kept_prefix = std::wstring_view();  // the system would not read a relative path behind \\?\ as one; give it a real one
  }

  const std::vector<std::wstring> parts = detail::Components(absolute);
  for (const std::wstring& base : plan.bases) {
    const std::vector<std::wstring> base_parts = detail::Components(base);
    if (base_parts.empty() || parts.size() <= base_parts.size()) continue;
    bool under = true;
    for (std::size_t i = 0; i < base_parts.size() && under; ++i) {
      under = detail::EqualNoCase(parts[i], base_parts[i]);
    }
    if (!under) continue;
    const std::wstring_view folder = detail::AsResolved(parts[base_parts.size()]);
    const bool moved = detail::EqualNoCase(folder, kLocalName) || detail::EqualNoCase(folder, kTempName);
    if (!moved) continue;
    std::wstring out(kept_prefix);
    out += plan.target;
    out.push_back(kSeparator);
    out += folder;
    for (std::size_t i = base_parts.size() + 1; i < parts.size(); ++i) {
      out.push_back(kSeparator);
      out += parts[i];
    }
    return out;
  }
  return std::nullopt;
}

// The name of the moved folder a rewritten path is under (_local or _temp), for the first-use line; empty otherwise.
inline std::wstring_view MovedName(const Plan& plan, std::wstring_view rewritten) {
  for (const std::wstring_view prefix : {kLongPrefix, kDevicePrefix}) {
    if (detail::StartsWithNoCase(rewritten, prefix)) {
      rewritten.remove_prefix(prefix.size());
      break;
    }
  }
  if (!detail::StartsWithNoCase(rewritten, plan.target)) return std::wstring_view();
  rewritten.remove_prefix(plan.target.size());
  if (rewritten.empty() || !detail::IsSeparator(rewritten.front())) return std::wstring_view();
  rewritten.remove_prefix(1);
  for (const std::wstring_view name : {kLocalName, kTempName}) {
    if (detail::StartsWithNoCase(rewritten, name) &&
        (rewritten.size() == name.size() || detail::IsSeparator(rewritten[name.size()]))) {
      return name;
    }
  }
  return std::wstring_view();
}

// The bases for an echovr.exe at `exe_path`: the engine's own root (the grandparent of the nearest ancestor folder
// named in kRootMarkers, FUN_1400df410) and the exe's folder, normalized, without duplicates. Only the exe's folder
// when no ancestor carries a marker (the engine then falls back to "." and the exe's folder is what that names).
inline std::vector<std::wstring> BasesForExe(std::wstring_view exe_path) {
  std::vector<std::wstring> bases;
  const std::wstring normalized = Normalize(exe_path);
  if (normalized.empty()) return bases;
  const std::vector<std::wstring> parts = detail::Components(normalized);
  if (parts.size() < kMinExeComponents) return bases;
  const std::size_t exe_dir_count = parts.size() - 1;
  for (std::size_t n = exe_dir_count; n > 0; --n) {
    bool marker = false;
    for (const std::wstring_view name : kRootMarkers) marker = marker || detail::EqualNoCase(parts[n - 1], name);
    if (!marker) continue;
    // parts[n - 1] is the marker folder; its grandparent is the first n - 2 components.
    if (n > kMarkerToRootLevels) bases.push_back(detail::Join(parts, n - kMarkerToRootLevels));
    break;
  }
  const std::wstring exe_dir = detail::Join(parts, exe_dir_count);
  bool seen = false;
  for (const std::wstring& b : bases) seen = seen || detail::EqualNoCase(b, exe_dir);
  if (!seen) bases.push_back(exe_dir);
  return bases;
}

// The game folder's own <base>\_local\config.json files, in the order the runtime reads them (LoadEarlyConfig in
// src/runtime/lifecycle/config.cpp: the exe folder's first, then the install root's): the redirect copies the first that
// exists to <target>\_local\config.json once, when the target has none, so a player's game settings move with _local.
inline std::vector<std::wstring> LegacyConfigJsonPaths(const Plan& plan) {
  std::vector<std::wstring> out;
  for (auto it = plan.bases.rbegin(); it != plan.bases.rend(); ++it) {
    out.push_back(*it + kSeparator + std::wstring(kLocalName) + kSeparator + std::wstring(kConfigJsonFile));
  }
  return out;
}

}  // namespace nevr_platform_compat::game_folder_redirect
