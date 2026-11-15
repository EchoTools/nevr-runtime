// game_folder_redirect.cpp: the hooks that keep the game's _local and _temp out of the game folder (#575). The
// decisions are game_folder_redirect_rules.h; the reasons and the engine addresses are at the top of that file.

#include <winsock2.h>  // before windows.h (game_folder_redirect.h): core/logging.h pulls winsock2 in

#include "modules/platform-compat/src/game_folder_redirect.h"

#include <atomic>
#include <new>
#include <string>
#include <vector>

#include "core/hooking.h"
#include "core/logging.h"
#include "core/nevr_root.h"
#include "modules/platform-compat/src/game_folder_redirect_rules.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/hook/patching.h"

namespace nevr_platform_compat::game_folder_redirect {
namespace {

// The calls this file hooks (twelve; CreateDirectoryW and CreateDirectoryA are platform_compat.cpp's, which call
// RedirectW and RedirectA too).
constexpr int kOwnHookCount = 12;

Plan g_plan;
std::atomic<bool> g_planReady{false};

// The first rewrite of each moved folder, kept for LogFirstUses. A hook never logs: it runs inside the game's own file
// calls, the game's crash handler (HandleCrashDump @0x1401cefe0 creates _temp|crashes) and the open of the game's network
// log (CreateLogFile @0x140100790) among them, so it only fills a fixed record, and the per-frame work writes the line.
// A record's path holds what the engine's own path buffers hold: 0x200 characters (CreateLogFile @0x140100790 and
// CSettingsStore::Initialize @0x141521240 build every _local and _temp path in 0x200-byte buffers). A longer one is cut
// in the log line only.
constexpr std::size_t kRecordPathChars = 0x200;
enum RecordState : int { kEmpty = 0, kFilling = 1, kReady = 2, kLogged = 3 };
struct FirstUse {
  std::atomic<int> state{kEmpty};
  const char* call = "";  // a call name is a string literal of the hook (static storage), so it is kept, not copied
  wchar_t from[kRecordPathChars] = {};
  wchar_t to[kRecordPathChars] = {};
};
FirstUse g_firstLocal;
FirstUse g_firstTemp;
// Rewrites so far, for the first-use lines (the count when each line is written).
std::atomic<unsigned long> g_rewrites{0};
// FindFirstFileA calls whose rewritten path does not fit the ANSI code page (LogFirstUses reports them once).
std::atomic<unsigned long> g_findAnsiUnfit{0};
std::atomic<bool> g_findAnsiReported{false};
// Calls whose rewrite ran out of memory (std::bad_alloc, caught by type in the hooks): the caller's path was passed on
// unchanged, so that call reached the game folder. LogFirstUses reports them once; a hook never logs and never lets the
// exception leave, because the system's and the game's frames are above it.
std::atomic<unsigned long> g_allocMisses{0};
std::atomic<bool> g_allocMissesReported{false};

template <typename C>
void CopyBounded(C* out, std::size_t capacity, const C* in) {
  std::size_t i = 0;
  for (; in != nullptr && in[i] != 0 && i + 1 < capacity; ++i) out[i] = in[i];
  out[i] = 0;
}

void Record(FirstUse& slot, const char* call, LPCWSTR from, const std::wstring& to) {
  int expected = kEmpty;
  if (!slot.state.compare_exchange_strong(expected, kFilling, std::memory_order_acq_rel)) return;
  slot.call = call;
  CopyBounded(slot.from, kRecordPathChars, from);
  CopyBounded(slot.to, kRecordPathChars, to.c_str());
  slot.state.store(kReady, std::memory_order_release);
}

void LogSlot(FirstUse& slot, std::wstring_view name) {
  int expected = kReady;
  if (!slot.state.compare_exchange_strong(expected, kLogged, std::memory_order_acq_rel)) return;
  Log(EchoVR::LogLevel::Info,
      "[NEVR.PATCH] game folder redirect first use name=%.*ls call=%s from='%ls' to='%ls' rewrites=%lu (the game's "
      "%.*ls folder lives in the nEVR root, not in the game folder)",
      static_cast<int>(name.size()), name.data(), slot.call, slot.from, slot.to,
      g_rewrites.load(std::memory_order_relaxed), static_cast<int>(name.size()), name.data());
}

using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using DeleteFileWFn = BOOL(WINAPI*)(LPCWSTR);
using FindFirstFileWFn = HANDLE(WINAPI*)(LPCWSTR, LPWIN32_FIND_DATAW);
using FindFirstFileExWFn = HANDLE(WINAPI*)(LPCWSTR, FINDEX_INFO_LEVELS, LPVOID, FINDEX_SEARCH_OPS, LPVOID, DWORD);
using FindFirstFileAFn = HANDLE(WINAPI*)(LPCSTR, LPWIN32_FIND_DATAA);
using GetFileAttributesWFn = DWORD(WINAPI*)(LPCWSTR);
using GetFileAttributesAFn = DWORD(WINAPI*)(LPCSTR);
using GetFileAttributesExWFn = BOOL(WINAPI*)(LPCWSTR, GET_FILEEX_INFO_LEVELS, LPVOID);
using SetFileAttributesWFn = BOOL(WINAPI*)(LPCWSTR, DWORD);
using CopyFileWFn = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, BOOL);
using MoveFileExWFn = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, DWORD);

// Each starts as the export itself (of FileApiModule's module); PatchDetour leaves the trampoline in it. A hook that did not install leaves the
// export, so an A hook that forwards to a W original reaches the system either way.
CreateFileWFn g_createFileW = nullptr;
CreateFileAFn g_createFileA = nullptr;
DeleteFileWFn g_deleteFileW = nullptr;
FindFirstFileWFn g_findFirstFileW = nullptr;
FindFirstFileExWFn g_findFirstFileExW = nullptr;
FindFirstFileAFn g_findFirstFileA = nullptr;
GetFileAttributesWFn g_getFileAttributesW = nullptr;
GetFileAttributesAFn g_getFileAttributesA = nullptr;
GetFileAttributesExWFn g_getFileAttributesExW = nullptr;
SetFileAttributesWFn g_setFileAttributesW = nullptr;
CopyFileWFn g_copyFileW = nullptr;
MoveFileExWFn g_moveFileExW = nullptr;

// The export address of each hook that installed, for RetireHooks.
std::vector<void*> g_targets;

// The path the call goes to: the rewritten one, or the caller's.
LPCWSTR Pick(const std::optional<std::wstring>& rewritten, LPCWSTR original) {
  return rewritten ? rewritten->c_str() : original;
}

// The current folder, at any length (GetCurrentDirectoryW tells the size it needs); empty when it cannot be read, and a
// relative path then stays where it is.
std::wstring CurrentDirectory() {
  const DWORD needed = GetCurrentDirectoryW(0, nullptr);  // the size, with its NUL
  if (needed == 0) return std::wstring();
  std::wstring cwd(needed, L'\0');
  const DWORD length = GetCurrentDirectoryW(needed, cwd.data());
  if (length == 0 || length >= needed) return std::wstring();
  cwd.resize(length);
  return cwd;
}

// An ANSI path that names neither folder needs no conversion (most calls).
bool CannotMatchAnsi(LPCSTR path) {
  const auto contains = [path](std::wstring_view name) {
    for (const char* p = path; *p != '\0'; ++p) {
      std::size_t i = 0;
      while (i < name.size() && p[i] != '\0' && detail::Fold(static_cast<wchar_t>(static_cast<unsigned char>(p[i]))) ==
                                                    detail::Fold(name[i])) {
        ++i;
      }
      if (i == name.size()) return true;
    }
    return false;
  };
  return !contains(kLocalName) && !contains(kTempName);
}

HANDLE WINAPI CreateFileWHook(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition,
                              DWORD flags, HANDLE templ) {
  const std::optional<std::wstring> to = RedirectW("CreateFileW", path);
  return g_createFileW(Pick(to, path), access, share, sa, disposition, flags, templ);
}

HANDLE WINAPI CreateFileAHook(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition,
                              DWORD flags, HANDLE templ) {
  const std::optional<std::wstring> to = RedirectA("CreateFileA", path);
  if (to) return g_createFileW(to->c_str(), access, share, sa, disposition, flags, templ);
  return g_createFileA(path, access, share, sa, disposition, flags, templ);
}

BOOL WINAPI DeleteFileWHook(LPCWSTR path) {
  const std::optional<std::wstring> to = RedirectW("DeleteFileW", path);
  return g_deleteFileW(Pick(to, path));
}

HANDLE WINAPI FindFirstFileWHook(LPCWSTR path, LPWIN32_FIND_DATAW data) {
  const std::optional<std::wstring> to = RedirectW("FindFirstFileW", path);
  return g_findFirstFileW(Pick(to, path), data);
}

HANDLE WINAPI FindFirstFileExWHook(LPCWSTR path, FINDEX_INFO_LEVELS level, LPVOID data, FINDEX_SEARCH_OPS op,
                                   LPVOID filter, DWORD flags) {
  const std::optional<std::wstring> to = RedirectW("FindFirstFileExW", path);
  return g_findFirstFileExW(Pick(to, path), level, data, op, filter, flags);
}

HANDLE WINAPI FindFirstFileAHook(LPCSTR path, LPWIN32_FIND_DATAA data) {
  const std::optional<std::wstring> to = RedirectA("FindFirstFileA", path);
  if (!to) return g_findFirstFileA(path, data);
  // The A call fills an A record, so the rewritten path goes back to the code page. One that does not fit it is not
  // guessed at: the search runs on the caller's path, which only reads.
  BOOL lossy = FALSE;
  const int size = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, to->c_str(), -1, nullptr, 0, nullptr, &lossy);
  if (size <= 0 || lossy) {
    g_findAnsiUnfit.fetch_add(1, std::memory_order_relaxed);
    return g_findFirstFileA(path, data);
  }
  std::string ansi;
  try {
    ansi.assign(static_cast<std::size_t>(size), '\0');
  } catch (const std::bad_alloc&) {
    NoteAllocMiss();
    return g_findFirstFileA(path, data);
  }
  WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, to->c_str(), -1, ansi.data(), size, nullptr, nullptr);
  return g_findFirstFileA(ansi.c_str(), data);
}

DWORD WINAPI GetFileAttributesWHook(LPCWSTR path) {
  const std::optional<std::wstring> to = RedirectW("GetFileAttributesW", path);
  return g_getFileAttributesW(Pick(to, path));
}

DWORD WINAPI GetFileAttributesAHook(LPCSTR path) {
  const std::optional<std::wstring> to = RedirectA("GetFileAttributesA", path);
  if (to) return g_getFileAttributesW(to->c_str());
  return g_getFileAttributesA(path);
}

BOOL WINAPI GetFileAttributesExWHook(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID info) {
  const std::optional<std::wstring> to = RedirectW("GetFileAttributesExW", path);
  return g_getFileAttributesExW(Pick(to, path), level, info);
}

BOOL WINAPI SetFileAttributesWHook(LPCWSTR path, DWORD attributes) {
  const std::optional<std::wstring> to = RedirectW("SetFileAttributesW", path);
  return g_setFileAttributesW(Pick(to, path), attributes);
}

BOOL WINAPI CopyFileWHook(LPCWSTR from, LPCWSTR to_path, BOOL fail_if_exists) {
  const std::optional<std::wstring> src = RedirectW("CopyFileW", from);
  const std::optional<std::wstring> dst = RedirectW("CopyFileW", to_path);
  return g_copyFileW(Pick(src, from), Pick(dst, to_path), fail_if_exists);
}

BOOL WINAPI MoveFileExWHook(LPCWSTR from, LPCWSTR to_path, DWORD flags) {
  const std::optional<std::wstring> src = RedirectW("MoveFileExW", from);
  const std::optional<std::wstring> dst = RedirectW("MoveFileExW", to_path);
  return g_moveFileExW(Pick(src, from), Pick(dst, to_path), flags);
}

// The game's settings move with _local: the first game-folder _local\config.json there is (LegacyConfigJsonPaths) is copied
// to <target>\_local\config.json once, when the target has none. The old file is left where it is and no longer read.
void CopyLegacyConfigJson() {
  const std::wstring local = g_plan.target + kSeparator + std::wstring(kLocalName);
  const std::wstring to = local + kSeparator + std::wstring(kConfigJsonFile);
  if (GetFileAttributesW(to.c_str()) != INVALID_FILE_ATTRIBUTES) return;  // already there: the target's own wins
  for (const std::wstring& from : LegacyConfigJsonPaths(g_plan)) {
    if (GetFileAttributesW(from.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
    const bool copied = nevr_root::EnsureDirectoryTree(local) && CopyFileW(from.c_str(), to.c_str(), TRUE) != FALSE;
    Log(copied ? EchoVR::LogLevel::Info : EchoVR::LogLevel::Warning,
        "[NEVR.PATCH] game folder redirect: config.json %s from='%ls' to='%ls' error=%lu (%s)",
        copied ? "copied" : "NOT copied", from.c_str(), to.c_str(), copied ? 0UL : static_cast<unsigned long>(GetLastError()),
        copied ? "the game reads it there from now on; the old file is left in place and no longer read"
               : "the game starts with its built-in settings; copy the file there by hand to keep them");
    return;
  }
}

template <typename Fn>
bool Hook(HMODULE module, const char* name, Fn* original, Fn detour, std::string* failed) {
  *original = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(module, name)));
  if (*original == nullptr) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.PATCH] game folder redirect hook not installed name=%s reason=no_export error=%lu (paths this call "
        "takes stay in the game folder)",
        name, static_cast<unsigned long>(GetLastError()));
    if (!failed->empty()) *failed += ",";
    *failed += name;
    return false;
  }
  void* const target = reinterpret_cast<void*>(*original);
  if (!PatchDetour(original, reinterpret_cast<PVOID>(detour), name)) {
    // PatchDetour wrote the Warning with the MinHook stage and status (N126, N128).
    if (!failed->empty()) *failed += ",";
    *failed += name;
    return false;
  }
  g_targets.push_back(target);
  return true;
}

}  // namespace

HMODULE FileApiModule(const char** name) {
  HMODULE module = GetModuleHandleA("kernelbase.dll");
  if (module != nullptr && GetProcAddress(module, "CreateDirectoryW") != nullptr) {
    *name = "kernelbase.dll";
    return module;
  }
  *name = "kernel32.dll";
  return GetModuleHandleA("kernel32.dll");
}

bool IsUnderMovedTemp(const std::wstring& rewritten) { return MovedName(g_plan, rewritten) == kTempName; }

BOOL CreateRedirectedDirectoryW(CreateDirectoryWFn original, const std::wstring& path, LPSECURITY_ATTRIBUTES security) {
  if (!IsUnderMovedTemp(path)) return original(path.c_str(), security);
  if (original(path.c_str(), security)) return TRUE;
  DWORD lastError = GetLastError();
  if (lastError == ERROR_PATH_NOT_FOUND || lastError == ERROR_FILE_NOT_FOUND) {
    const std::size_t lastSlash = path.find_last_of(L'\\');
    if (lastSlash != std::wstring::npos && lastSlash != 0) {
      try {
        CreateRedirectedDirectoryW(original, path.substr(0, lastSlash), security);
      } catch (const std::bad_alloc&) {
        NoteAllocMiss();  // the parent is not made; the call below gets the system's answer
      }
      if (original(path.c_str(), security)) return TRUE;
      lastError = GetLastError();
    }
  }
  if (lastError == ERROR_ALREADY_EXISTS) {
    SetLastError(ERROR_SUCCESS);
    return TRUE;
  }
  SetLastError(lastError);
  return FALSE;
}

// ResolvedForLog's conversion and rewrite; it allocates, and ResolvedForLog catches what it throws.
std::string ResolvedForLogOrThrow(LPCSTR path);

std::string ResolvedForLog(LPCSTR path) {
  if (path == nullptr) return std::string();
  if (!g_planReady.load(std::memory_order_acquire) || CannotMatchAnsi(path)) return std::string(path);
  try {
    return ResolvedForLogOrThrow(path);
  } catch (const std::bad_alloc&) {
    return std::string(path);  // a log line's name only: the caller's path, as written
  }
}

void NoteAllocMiss() { g_allocMisses.fetch_add(1, std::memory_order_relaxed); }

std::string ResolvedForLogOrThrow(LPCSTR path) {
  const int size = MultiByteToWideChar(CP_ACP, 0, path, -1, nullptr, 0);
  if (size <= 0) return std::string(path);
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(CP_ACP, 0, path, -1, wide.data(), size);
  wide.resize(static_cast<std::size_t>(size) - 1);  // the converted NUL
  const std::optional<std::wstring> to = Rewrite(g_plan, wide, CurrentDirectory());
  if (!to) return std::string(path);
  const int out = WideCharToMultiByte(CP_UTF8, 0, to->c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (out <= 0) return std::string(path);
  std::string utf8(static_cast<std::size_t>(out), '\0');
  WideCharToMultiByte(CP_UTF8, 0, to->c_str(), -1, utf8.data(), out, nullptr, nullptr);
  utf8.resize(static_cast<std::size_t>(out) - 1);
  return utf8;
}

void LogFirstUses() {
  if (!g_planReady.load(std::memory_order_acquire)) return;
  LogSlot(g_firstLocal, kLocalName);
  LogSlot(g_firstTemp, kTempName);
  const unsigned long unfit = g_findAnsiUnfit.load(std::memory_order_relaxed);
  if (unfit != 0 && !g_findAnsiReported.exchange(true)) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.PATCH] game folder redirect: FindFirstFileA calls=%lu with a rewritten path that does not fit the ANSI "
        "code page; the caller's path was passed on, and its W form is redirected where the system converts it "
        "(kernelbase.dll)",
        unfit);
  }
  const unsigned long misses = g_allocMisses.load(std::memory_order_relaxed);
  if (misses != 0 && !g_allocMissesReported.exchange(true)) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.PATCH] game folder redirect: calls=%lu ran out of memory while rewriting a path; the caller's path was "
        "passed on unchanged, so those calls reached the game folder",
        misses);
  }
}

std::optional<std::wstring> RedirectW(const char* call, LPCWSTR path) {
  if (path == nullptr || !g_planReady.load(std::memory_order_acquire) || CannotMatch(path)) return std::nullopt;
  const DWORD saved = GetLastError();
  std::optional<std::wstring> to;
  try {
    to = Rewrite(g_plan, path, CurrentDirectory());
    if (to) {
      g_rewrites.fetch_add(1, std::memory_order_relaxed);
      Record(MovedName(g_plan, *to) == kLocalName ? g_firstLocal : g_firstTemp, call, path, *to);
    }
  } catch (const std::bad_alloc&) {
    NoteAllocMiss();
    to.reset();
  }
  SetLastError(saved);
  return to;
}

std::optional<std::wstring> RedirectA(const char* call, LPCSTR path) {
  if (path == nullptr || !g_planReady.load(std::memory_order_acquire) || CannotMatchAnsi(path)) return std::nullopt;
  const DWORD saved = GetLastError();
  const int size = MultiByteToWideChar(CP_ACP, 0, path, -1, nullptr, 0);
  if (size <= 0) {
    SetLastError(saved);
    return std::nullopt;
  }
  std::wstring wide;
  try {
    wide.assign(static_cast<std::size_t>(size), L'\0');
  } catch (const std::bad_alloc&) {
    NoteAllocMiss();
    SetLastError(saved);
    return std::nullopt;
  }
  MultiByteToWideChar(CP_ACP, 0, path, -1, wide.data(), size);
  wide.resize(static_cast<std::size_t>(size) - 1);  // the converted NUL
  SetLastError(saved);
  return RedirectW(call, wide.c_str());
}

int Install() {
  static bool s_done = false;
  static int s_installed = 0;
  if (s_done) return s_installed;
  s_done = true;

  const std::wstring root = nevr_root::RootW();
  WCHAR exe[MAX_PATH] = {};
  const DWORD exe_length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  if (root.empty() || exe_length == 0 || exe_length >= MAX_PATH) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.PATCH] game folder redirect not installed reason=%s error=%lu (the game writes _local and _temp in the "
        "game folder this run)",
        root.empty() ? "no_root" : "exe_path_unreadable", static_cast<unsigned long>(GetLastError()));
    return 0;
  }
  s_installed = InstallWith(root, std::wstring_view(exe, exe_length));
  return s_installed;
}

int InstallWith(const std::wstring& root, std::wstring_view exe_path) {
  g_plan.bases = BasesForExe(exe_path);
  g_plan.target = Normalize(nevr_root::Join(root, std::wstring(kTargetChild).c_str()));
  if (g_plan.bases.empty() || g_plan.target.empty() || !nevr_root::EnsureDirectoryTree(g_plan.target)) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.PATCH] game folder redirect not installed reason=target_unusable target='%ls' error=%lu (the game "
        "writes _local and _temp in the game folder this run)",
        g_plan.target.c_str(), static_cast<unsigned long>(GetLastError()));
    return 0;
  }
  CopyLegacyConfigJson();
  g_planReady.store(true, std::memory_order_release);

  const char* module_name = "";
  HMODULE file_module = FileApiModule(&module_name);
  std::string failed;
  int installed = 0;
  if (file_module != nullptr) {
    installed += Hook(file_module, "CreateFileW", &g_createFileW, &CreateFileWHook, &failed) ? 1 : 0;
    installed += Hook(file_module, "CreateFileA", &g_createFileA, &CreateFileAHook, &failed) ? 1 : 0;
    installed += Hook(file_module, "DeleteFileW", &g_deleteFileW, &DeleteFileWHook, &failed) ? 1 : 0;
    installed += Hook(file_module, "FindFirstFileW", &g_findFirstFileW, &FindFirstFileWHook, &failed) ? 1 : 0;
    installed += Hook(file_module, "FindFirstFileExW", &g_findFirstFileExW, &FindFirstFileExWHook, &failed) ? 1 : 0;
    installed += Hook(file_module, "FindFirstFileA", &g_findFirstFileA, &FindFirstFileAHook, &failed) ? 1 : 0;
    installed += Hook(file_module, "GetFileAttributesW", &g_getFileAttributesW, &GetFileAttributesWHook, &failed) ? 1 : 0;
    installed += Hook(file_module, "GetFileAttributesA", &g_getFileAttributesA, &GetFileAttributesAHook, &failed) ? 1 : 0;
    installed +=
        Hook(file_module, "GetFileAttributesExW", &g_getFileAttributesExW, &GetFileAttributesExWHook, &failed) ? 1 : 0;
    installed += Hook(file_module, "SetFileAttributesW", &g_setFileAttributesW, &SetFileAttributesWHook, &failed) ? 1 : 0;
    installed += Hook(file_module, "CopyFileW", &g_copyFileW, &CopyFileWHook, &failed) ? 1 : 0;
    installed += Hook(file_module, "MoveFileExW", &g_moveFileExW, &MoveFileExWHook, &failed) ? 1 : 0;
  } else {
    failed = "file-call module not loaded";
  }

  std::wstring bases;
  for (const std::wstring& base : g_plan.bases) {
    if (!bases.empty()) bases += L"','";
    bases += base;
  }
  Log(installed == kOwnHookCount ? EchoVR::LogLevel::Info : EchoVR::LogLevel::Warning,
      "[NEVR.PATCH] game folder redirect installed hooks=%d of %d module=%s bases='%ls' target='%ls'%s%s (_local and "
      "_temp under the game folder live in the nEVR root)",
      installed, kOwnHookCount, module_name, bases.c_str(), g_plan.target.c_str(), failed.empty() ? "" : " failed=",
      failed.c_str());
  return installed;
}

void RetireHooks() {
  for (void* target : g_targets) nevr_hook_guard::Retire(target);
  g_targets.clear();
}

}  // namespace nevr_platform_compat::game_folder_redirect
