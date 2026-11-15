// game_folder_redirect.h: the game's _local and _temp live in the nEVR root, not in the game folder (#575).
//
// The decisions are game_folder_redirect_rules.h. This half builds the plan once, hooks the system's file calls the game's
// binaries reach the disk through (echovr.exe and pnsrad, pnsradmatchmaking, pnsovr, pnsdemo import them from kernel32) and
// rewrites each path under <base>\_local or <base>\_temp before the system sees it. CreateDirectoryW and
// CreateDirectoryA are hooked by platform_compat.cpp (its Wine _temp fix); they call Redirect too.

#pragma once

#include <windows.h>

#include <optional>
#include <string>
#include <string_view>

namespace nevr_platform_compat::game_folder_redirect {

// The module whose file-call exports the hooks patch: kernelbase.dll, where the kernel32.dll exports forward and where a
// DLL that imports the API sets (api-ms-win-core-file-*, the CRT's fopen, dbghelp) calls directly, so one hook sees both
// kinds of caller; kernel32.dll when kernelbase.dll is not loaded. Its name is written to `name`.
HMODULE FileApiModule(const char** name);

// Builds the plan (the install root and the exe folder of this process, %LOCALAPPDATA%\nevr\echovr\game) and installs
// the file-call hooks. Once per process, before the game's first _local or _temp access (boot.cpp calls it before the
// original PreprocessCommandLine; CSettingsStore::Initialize and CreateLogFile run after it returns). Returns the number of hooks
// installed; the line it writes says which failed. With no nEVR root nothing is hooked and no path moves.
int Install();

// Install's body for a given nEVR root and echovr.exe path; Install passes this process's. Called once per process (a test
// calls it in its own process, with folders of its own).
int InstallWith(const std::wstring& root, std::wstring_view exe_path);

// Where a path the game passed goes, for the call named `call`; nothing when it stays. Never logs (it runs inside the
// game's file calls, its crash handler's among them): it records the first rewrite of each moved folder for LogFirstUses.
// Leaves GetLastError as it found it.
std::optional<std::wstring> RedirectW(const char* call, LPCWSTR path);
std::optional<std::wstring> RedirectA(const char* call, LPCSTR path);

// True when a path RedirectW or RedirectA returned is under the moved _temp (any case), so the caller keeps the Wine _temp
// fix's answers for it.
bool IsUnderMovedTemp(const std::wstring& rewritten);

// A folder the redirect moved to the nEVR root, made through `original` (the system's CreateDirectoryW). It never logs:
// the game's crash handler (HandleCrashDump @0x1401cefe0, through Ensure @0x1400eb240) makes its _temp|crashes folder
// through this call. A _temp folder keeps the Wine _temp fix's answers (an existing folder is a success, missing parents
// are made); a _local folder gets the system's own answer, as the game asked for it. The path is absolute (Rewrite
// resolves a relative one).
using CreateDirectoryWFn = BOOL(WINAPI*)(LPCWSTR, LPSECURITY_ATTRIBUTES);
BOOL CreateRedirectedDirectoryW(CreateDirectoryWFn original, const std::wstring& path, LPSECURITY_ATTRIBUTES security);

// The path a call would reach, for a log line that names the file actually read: the rewritten one when the redirect
// moves it, else the caller's. Records nothing.
std::string ResolvedForLog(LPCSTR path);

// Writes, once each, the Info line of the first rewrite of _local and of _temp and the Warning for FindFirstFileA paths
// that did not fit the ANSI code page. Called from the per-frame work (DispatchPerFrameWork, runtime/frame/tick.cpp).
void LogFirstUses();

// Counts a call whose rewrite ran out of memory (the caller's path went on unchanged); LogFirstUses reports the count.
void NoteAllocMiss();

// Forgets the HookGuard record of every hook Install made, before MH_Uninitialize restores the bytes (platform_compat
// shutdown).
void RetireHooks();

}  // namespace nevr_platform_compat::game_folder_redirect
