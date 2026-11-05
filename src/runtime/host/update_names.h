#pragma once

// The closed vocabulary of the host's self-update (docs/adr/0024-pc-self-update.md, section 4.2, "Paths are confined").
// The host composes every path under update\ and the nevr root itself, from these names only; a name read from a file
// that is outside this vocabulary makes that file unusable. Pure: no I/O, no allocation beyond the caller's string.
//
// The vocabulary is ADR 0024's, made strict where Windows path rules would otherwise reach outside the folder:
//   * a version is [0-9A-Za-z.+-]{1,64};
//   * a plugin name is [A-Za-z0-9._-]{1,64}, the file is plugins\<name>.dll;
//   * neither starts or ends with '.', so "." and ".." are never a name, and Windows cannot strip a trailing dot to
//     make two names one;
//   * neither is a Windows device name (CON, PRN, AUX, NUL, COM0-9, LPT0-9, CONIN$, CONOUT$) before its first dot,
//     in any case, because Windows opens the device for "plugins\CON.dll".
// The files a release carries are written in update\staged.json, update\swap.json and update\state.json as member
// paths: "nevr-runtime.dll" or "plugins/<name>.dll", the spelling of the core's NevrPcStage result
// (installer/src/stage.rs, staged_member).

#include <cstddef>
#include <string>
#include <string_view>

namespace nevr_host::update_names {

inline constexpr wchar_t kUpdateDir[] = L"update";
inline constexpr wchar_t kLockFile[] = L"lock";
inline constexpr wchar_t kStagedJson[] = L"staged.json";
inline constexpr wchar_t kSwapJson[] = L"swap.json";
inline constexpr wchar_t kPendingJson[] = L"pending.json";
inline constexpr wchar_t kStateJson[] = L"state.json";
inline constexpr wchar_t kCrashedFile[] = L"crashed";
inline constexpr wchar_t kStagingDir[] = L"staging";
inline constexpr wchar_t kPreviousDir[] = L"previous";
inline constexpr wchar_t kPluginsDir[] = L"plugins";
inline constexpr wchar_t kRuntimeFile[] = L"nevr-runtime.dll";
inline constexpr char kRuntimeMember[] = "nevr-runtime.dll";
inline constexpr char kPluginPrefix[] = "plugins/";
inline constexpr char kPluginSuffix[] = ".dll";
inline constexpr std::size_t kMaxNameLength = 64;
/// Every JSON file the host reads from update\ is capped at this many bytes (ADR 0024, section 4.3).
inline constexpr std::size_t kMaxJsonBytes = 64 * 1024;
/// ... and at this many levels of nested arrays and objects. The files of update\ are at most 3 deep; a deeper one is
/// damage, and is never parsed into a tree (copying or writing one recurses once per level).
inline constexpr std::size_t kMaxJsonDepth = 16;

/// True for a Windows device name, in any case, compared on the part before the first '.'.
bool IsDeviceName(std::string_view name) noexcept;

/// [0-9A-Za-z.+-]{1,64}, no leading or trailing '.', not a device name.
bool IsVersionName(std::string_view version) noexcept;

/// [A-Za-z0-9._-]{1,64}, no leading or trailing '.', not a device name.
bool IsPluginName(std::string_view name) noexcept;

/// One file of a release: the runtime, or one plugin.
struct Member {
  bool runtime = false;
  std::string plugin;  ///< the plugin's name (no "plugins/", no ".dll"); empty for the runtime
};

/// "nevr-runtime.dll" or "plugins/<name>.dll" with a name IsPluginName accepts. False for anything else: another
/// separator, a backslash, a drive, "..", an absolute path, another extension.
bool ParseMember(std::string_view path, Member* out) noexcept;

/// The member's path as the files spell it ("nevr-runtime.dll", "plugins/<name>.dll").
std::string MemberPath(const Member& member);

/// The member's path relative to a folder, with backslashes ("nevr-runtime.dll", "plugins\<name>.dll").
std::wstring MemberRelativePath(const Member& member);

}  // namespace nevr_host::update_names
