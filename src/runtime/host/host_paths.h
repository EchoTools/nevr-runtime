#pragma once

// Where the stable host keeps what it writes: in its own per-game folder, never in
// the game's own %LOCALAPPDATA%\EchoVR:
//   %LOCALAPPDATA%\nevr\echovr\logs\nevr-bootstrap.jsonl
//   %LOCALAPPDATA%\nevr\echovr\crashes\nevr-<pid>-<n>.dmp
// with %TEMP%\nevr\echovr\{logs,crashes} as the fallback and the host's own
// directory as the last resort.

namespace nevr_host {

inline constexpr wchar_t kLogsSubdirectory[] = L"nevr\\echovr\\logs";
inline constexpr wchar_t kCrashesSubdirectory[] = L"nevr\\echovr\\crashes";
inline constexpr wchar_t kBootstrapLogName[] = L"nevr-bootstrap.jsonl";

}  // namespace nevr_host
