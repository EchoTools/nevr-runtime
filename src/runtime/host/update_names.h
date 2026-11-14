#pragma once

// The PC edge of the self-update vocabulary. The names are core/update/names.h (namespace nevr_update_names), shared with
// the Quest runtime; this header re-exports them for the host and the PC runtime, and adds what only the Win32 calls
// take: the wide spellings of the twelve folder and file names, and MemberRelativePath. Every wide constant is pinned to
// its narrow twin by the static_asserts below, so a drift is a compile error. Pure: no I/O, no allocation beyond the
// caller's string.

#include "core/update/names.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace nevr_host::update_names {

using nevr_update_names::IsDeviceName;
using nevr_update_names::IsPluginName;
using nevr_update_names::IsVersionName;
using nevr_update_names::kMaxJsonBytes;
using nevr_update_names::kMaxJsonDepth;
using nevr_update_names::kMaxNameLength;
using nevr_update_names::kPluginPrefix;
using nevr_update_names::kPluginSuffix;
using nevr_update_names::kRuntimeMember;
using nevr_update_names::Member;
using nevr_update_names::MemberPath;
using nevr_update_names::ParseMember;

inline constexpr wchar_t kUpdateDir[] = L"update";
inline constexpr wchar_t kLockFile[] = L"lock";
inline constexpr wchar_t kStagedJson[] = L"staged.json";
inline constexpr wchar_t kSwapJson[] = L"swap.json";
inline constexpr wchar_t kPendingJson[] = L"pending.json";
inline constexpr wchar_t kStateJson[] = L"state.json";
inline constexpr wchar_t kCrashedFile[] = L"crashed";
inline constexpr wchar_t kStagingDir[] = L"staging";
inline constexpr wchar_t kPreviousDir[] = L"previous";
inline constexpr wchar_t kFailedDir[] = L"failed";
inline constexpr wchar_t kPluginsDir[] = L"plugins";
inline constexpr wchar_t kRuntimeFile[] = L"nevr-runtime.dll";

// Each wide name spells its narrow twin: same length including the terminating NUL, same character at every index.
template <std::size_t N, std::size_t M>
constexpr bool SameSpelling(const wchar_t (&wide)[N], const char (&narrow)[M]) noexcept {
  if (N != M) return false;
  for (std::size_t i = 0; i < N; ++i) {
    if (static_cast<char>(wide[i]) != narrow[i]) return false;
  }
  return true;
}

static_assert(SameSpelling(kUpdateDir, nevr_update_names::kUpdateDir), "update folder: wide and narrow differ");
static_assert(SameSpelling(kLockFile, nevr_update_names::kLockFile), "lock file: wide and narrow differ");
static_assert(SameSpelling(kStagedJson, nevr_update_names::kStagedJson), "staged.json: wide and narrow differ");
static_assert(SameSpelling(kSwapJson, nevr_update_names::kSwapJson), "swap.json: wide and narrow differ");
static_assert(SameSpelling(kPendingJson, nevr_update_names::kPendingJson), "pending.json: wide and narrow differ");
static_assert(SameSpelling(kStateJson, nevr_update_names::kStateJson), "state.json: wide and narrow differ");
static_assert(SameSpelling(kCrashedFile, nevr_update_names::kCrashedFile), "crashed: wide and narrow differ");
static_assert(SameSpelling(kStagingDir, nevr_update_names::kStagingDir), "staging: wide and narrow differ");
static_assert(SameSpelling(kPreviousDir, nevr_update_names::kPreviousDir), "previous: wide and narrow differ");
static_assert(SameSpelling(kFailedDir, nevr_update_names::kFailedDir), "failed: wide and narrow differ");
static_assert(SameSpelling(kPluginsDir, nevr_update_names::kPluginsDir), "plugins folder: wide and narrow differ");
static_assert(SameSpelling(kRuntimeFile, nevr_update_names::kRuntimeFile), "runtime file: wide and narrow differ");

/// The member's path relative to a folder, with backslashes ("nevr-runtime.dll", "plugins\<name>.dll").
inline std::wstring MemberRelativePath(const Member& member) {
  if (member.runtime) return kRuntimeFile;
  std::wstring out = kPluginsDir;
  out += L'\\';
  for (const char c : member.plugin) out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
  out += L".dll";
  return out;
}

}  // namespace nevr_host::update_names
