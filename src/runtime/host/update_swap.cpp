#include "runtime/host/update_swap.h"

#include "core/update/engine_start.h"
#include "runtime/ext/plugin_signature.h"
#include "runtime/host/file_digest.h"
#include "runtime/host/runtime_load.h"
#include "runtime/host/update_names.h"
#include "runtime/host/update_seams.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <exception>
#include <memory>
#include <new>
#include <string>
#include <vector>

// The Win32 edges of the self-update start (docs/adr/0024-pc-self-update.md, sections 4 and 6). The start itself is the
// engine's (core/update/engine_start.cpp); this file binds each edge of core/update/edges.h and core/update/fs.h to the
// Win32 call ADR 0024 names, keeps the OS error of a failed call for the records, and owns update\lock. Every path reaches
// this file in UTF-8 and is converted for each call. A path that does not convert names nothing.

namespace nevr_host {
namespace {

namespace engine = nevr_update;
namespace names = nevr_update_names;

constexpr std::size_t kUtcTextLength = 32;
constexpr wchar_t kWideSeparator = L'\\';
constexpr char kNarrowSeparator = '\\';

// UTF-8 to UTF-16, strictly: false when the text is not UTF-8, which names nothing. Out of memory is not a false: std::bad_alloc
// propagates to the caller (update_fs_win32.cpp's Utf8ToWide splits the two the same way).
bool ToWide(const std::string& utf8, std::wstring* out) {
  out->clear();
  if (utf8.empty()) return true;
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  if (size <= 0) return false;
  out->resize(static_cast<std::size_t>(size));
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), out->data(), size) == size;
}

// UTF-16 to UTF-8, strictly: false when the text does not convert. Out of memory propagates, as in ToWide.
bool ToUtf8(const std::wstring& wide, std::string* out) {
  out->clear();
  if (wide.empty()) return true;
  const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
                                       nullptr, nullptr);
  if (size <= 0) return false;
  out->resize(static_cast<std::size_t>(size));
  return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(wide.size()), out->data(), size,
                             nullptr, nullptr) == size;
}

// The attributes of the path, or INVALID_FILE_ATTRIBUTES when nothing is there (or the path does not convert).
DWORD AttributesOf(const std::string& path) {
  std::wstring wide;
  if (!ToWide(path, &wide)) return INVALID_FILE_ATTRIBUTES;
  return GetFileAttributesW(wide.c_str());
}

// The lock file's handle: update\lock held with share mode 0 and delete-on-close (ADR 0024, section 4.1). Closing it releases
// the lock and removes the file.
class HostLock final : public engine::FsLock {
 public:
  explicit HostLock(HANDLE handle) noexcept : handle_(handle) {}
  ~HostLock() override { Release(); }
  void Release() noexcept override {
    if (handle_ == INVALID_HANDLE_VALUE) return;
    CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
  }

 private:
  HANDLE handle_;
};

// The Win32 UpdateFs of one start. It keeps the OS error of its last failed call, which the records carry.
class HostFs final : public engine::UpdateFs {
 public:
  char Separator() const noexcept override { return kNarrowSeparator; }
  int LastOsError() const noexcept override { return os_error_; }

  engine::ReadResult ReadSmallFile(const std::string& path, std::string* out) const noexcept override {
    try {
      std::wstring wide;
      if (!ToWide(path, &wide)) return engine::ReadResult::kUnreadable;
      const HANDLE file = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        FailWith(error);
        return (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) ? engine::ReadResult::kAbsent
                                                                                 : engine::ReadResult::kUnreadable;
      }
      LARGE_INTEGER size{};
      if (GetFileSizeEx(file, &size) == FALSE) {
        Fail();
        CloseHandle(file);
        return engine::ReadResult::kUnreadable;
      }
      if (size.QuadPart < 0 || static_cast<unsigned long long>(size.QuadPart) > names::kMaxJsonBytes) {
        CloseHandle(file);
        return engine::ReadResult::kTooBig;
      }
      std::string text(static_cast<std::size_t>(size.QuadPart), '\0');
      DWORD read = 0;
      if (!text.empty() && ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr) == FALSE) {
        Fail();
        CloseHandle(file);
        return engine::ReadResult::kUnreadable;
      }
      CloseHandle(file);
      // Fewer bytes than the file has is not the whole file: it is damaged, as an over-size file is. The engine reads kTooBig
      // as a damaged file (the original host's ReadJsonFile answered kBad for both).
      if (static_cast<std::size_t>(read) != text.size()) return engine::ReadResult::kTooBig;
      *out = std::move(text);
      return engine::ReadResult::kOk;
    } catch (const std::exception&) {
      FailWith(ERROR_OUTOFMEMORY);
      return engine::ReadResult::kUnreadable;
    }
  }

  bool WriteByRename(const std::string& path, const std::string& text) const noexcept override {
    try {
      std::wstring wide;
      if (!ToWide(path, &wide)) {
        FailWith(ERROR_INVALID_NAME);
        return false;
      }
      const std::wstring temporary = wide + L".tmp-" + std::to_wstring(GetCurrentProcessId());
      const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file == INVALID_HANDLE_VALUE) {
        Fail();
        return false;
      }
      DWORD written = 0;
      const bool wrote = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) != FALSE &&
                         written == text.size() && FlushFileBuffers(file) != FALSE;
      if (!wrote) Fail();
      CloseHandle(file);
      if (!wrote || MoveFileExW(temporary.c_str(), wide.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        if (wrote) Fail();
        DeleteFileW(temporary.c_str());
        return false;
      }
      return true;
    } catch (const std::exception&) {
      FailWith(ERROR_OUTOFMEMORY);
      return false;
    }
  }

  bool RemoveFile(const std::string& path) const noexcept override {
    try {
      std::wstring wide;
      if (!ToWide(path, &wide)) {
        FailWith(ERROR_INVALID_NAME);
        return false;
      }
      if (DeleteFileW(wide.c_str()) == FALSE) {
        Fail();
        return false;
      }
      return true;
    } catch (const std::exception&) {
      FailWith(ERROR_OUTOFMEMORY);
      return false;
    }
  }

  // Refused: nothing the start does removes a tree (the engine's check removes its staging folder through its own UpdateFs), and
  // the host stays thin, so no recursive delete is kept here unexercised. The refusal is the last error and nothing is removed.
  void RemoveTree(const std::string& /*path*/) const noexcept override { FailWith(ERROR_CALL_NOT_IMPLEMENTED); }

  bool IsLink(const std::string& path) const noexcept override {
    try {
      std::wstring wide;
      if (!ToWide(path, &wide)) return true;  // cannot tell whether it is a link: the engine then writes nothing through it (fs.h)
      const DWORD attributes = GetFileAttributesW(wide.c_str());
      return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    } catch (const std::bad_alloc&) {
      return true;  // out of memory: cannot tell either
    }
  }

  // Something (a file, a folder or a link) is at the path. A path that does not convert names nothing; out of memory cannot
  // tell, so it answers true and the engine waits (fs.h).
  bool Exists(const std::string& path) const noexcept override {
    try {
      std::wstring wide;
      if (!ToWide(path, &wide)) return false;
      return GetFileAttributesW(wide.c_str()) != INVALID_FILE_ATTRIBUTES;
    } catch (const std::bad_alloc&) {
      return true;
    }
  }

  // A folder, found through a conversion that succeeded. A path that does not convert, or that cannot be converted for lack of
  // memory, is not known to be absent: it answers true, and the engine inspects it instead of skipping it.
  bool IsDirectory(const std::string& path) const noexcept override {
    try {
      std::wstring wide;
      if (!ToWide(path, &wide)) return true;
      const DWORD attributes = GetFileAttributesW(wide.c_str());
      return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    } catch (const std::bad_alloc&) {
      return true;
    }
  }

  // Polls for update\lock: no wait handle exists for a file another process holds open, so the wait is a bounded poll, one
  // open every step, for at most the timeout (ADR 0024, section 4.1).
  std::unique_ptr<engine::FsLock> TakeLock(const std::string& update_dir, std::uint32_t timeout_ms, std::uint32_t step_ms,
                                           int* os_error) const noexcept override {
    try {
      std::wstring folder;
      if (!ToWide(update_dir, &folder) || folder.empty()) {
        if (os_error != nullptr) *os_error = static_cast<int>(ERROR_INVALID_NAME);
        return nullptr;
      }
      const std::wstring path = folder + kWideSeparator + update_names::kLockFile;
      const ULONGLONG started = GetTickCount64();
      for (;;) {
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
          try {
            return std::make_unique<HostLock>(file);
          } catch (const std::exception&) {
            CloseHandle(file);  // no object is left to own the handle: it is released here
            if (os_error != nullptr) *os_error = static_cast<int>(ERROR_OUTOFMEMORY);
            return nullptr;
          }
        }
        if (os_error != nullptr) *os_error = static_cast<int>(GetLastError());
        if (GetTickCount64() - started >= timeout_ms) return nullptr;
        Sleep(step_ms == 0 ? 1 : step_ms);
      }
    } catch (const std::exception&) {
      return nullptr;
    }
  }

  bool Move(const std::string& from, const std::string& to, int* os_error) const noexcept override {
    const bool moved = MoveOrFail(from, to);
    if (!moved && os_error != nullptr) *os_error = LastOsError();
    return moved;
  }

  bool MoveOrFail(const std::string& from, const std::string& to) const noexcept {
    try {
      std::wstring source;
      std::wstring target;
      if (!ToWide(from, &source) || !ToWide(to, &target)) {
        FailWith(ERROR_INVALID_NAME);
        return false;
      }
      // No MOVEFILE_COPY_ALLOWED: a move to another volume fails, and the journal undoes it (ADR 0024, section 4.2). No
      // MOVEFILE_REPLACE_EXISTING: a file where one is going is a failure, never overwritten.
      if (MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH) == FALSE) {
        Fail();
        return false;
      }
      return true;
    } catch (const std::exception&) {
      FailWith(ERROR_OUTOFMEMORY);
      return false;
    }
  }

  bool MakeDir(const std::string& path) const noexcept override {
    try {
      std::wstring wide;
      if (!ToWide(path, &wide)) return false;
      if (CreateDirectoryW(wide.c_str(), nullptr) != FALSE) return true;
      return GetLastError() == ERROR_ALREADY_EXISTS;
    } catch (const std::exception&) {
      return false;
    }
  }

  bool RemoveEmptyDir(const std::string& path) const noexcept override {
    try {
      std::wstring wide;
      if (!ToWide(path, &wide)) return false;
      if (RemoveDirectoryW(wide.c_str()) != FALSE) return true;
      Fail();
      return false;
    } catch (const std::exception&) {
      return false;
    }
  }

  // Every entry of the folder ("<folder>\*"), and the engine keeps the names it needs. A listing that fails leaves *out as it
  // was; one that succeeds replaces it, as the POSIX edge does. The find handle is closed on every path.
  bool ListFolder(const std::string& path, std::vector<engine::DirEntry>* out) const noexcept override {
    HANDLE find = INVALID_HANDLE_VALUE;
    try {
      std::wstring pattern;
      if (!ToWide(path, &pattern)) {
        FailWith(ERROR_INVALID_NAME);
        return false;
      }
      pattern += kWideSeparator;
      pattern += L'*';
      WIN32_FIND_DATAW data{};
      find = FindFirstFileW(pattern.c_str(), &data);
      if (find == INVALID_HANDLE_VALUE) {
        Fail();
        return false;
      }
      std::vector<engine::DirEntry> listed;
      do {
        const std::wstring wide_name = data.cFileName;
        if (wide_name == L"." || wide_name == L"..") continue;
        engine::DirEntry entry;
        if (!ToUtf8(wide_name, &entry.name)) continue;  // a name that does not convert is not one of ours
        entry.is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        listed.push_back(std::move(entry));
      } while (FindNextFileW(find, &data) != FALSE);
      FindClose(find);
      find = INVALID_HANDLE_VALUE;
      *out = std::move(listed);
      return true;
    } catch (const std::exception&) {
      if (find != INVALID_HANDLE_VALUE) FindClose(find);
      FailWith(ERROR_OUTOFMEMORY);
      return false;
    }
  }

 private:
  void Fail() const noexcept { os_error_ = static_cast<int>(GetLastError()); }
  void FailWith(DWORD error) const noexcept { os_error_ = static_cast<int>(error); }

  mutable int os_error_ = 0;
};

// Authenticode (ADR 0010, ADR 0024 section 4.3): the file's signature under the host's policy. The digest check comes first
// when the caller asks for it, with the reasons "unreadable", "size" and "sha256" that the records carry.
class HostVerifier final : public engine::ArtifactVerifier {
 public:
  engine::ArtifactCheck Verify(const std::string& path, const engine::ArtifactExpect& expect) const override {
    engine::ArtifactCheck check;
    std::wstring wide;
    if (!ToWide(path, &wide)) {
      check.reason = "unreadable";
      return check;
    }
    const HANDLE handle = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      check.reason = "unreadable";
      return check;
    }
    std::string why;
    if (expect.check_digest) {
      LARGE_INTEGER size{};
      std::vector<unsigned char> bytes;
      if (GetFileSizeEx(handle, &size) == FALSE || static_cast<std::uint64_t>(size.QuadPart) != expect.size) {
        why = "size";
      } else if (!ReadWholeFile(handle, &bytes) || Sha256Hex(bytes) != expect.sha256) {
        why = "sha256";
      }
    }
    if (why.empty()) {
      const nevr_plugin_signature::Verdict verdict =
          nevr_plugin_signature::VerifyPluginFile(handle, HostVerifyPolicy(), static_cast<std::int64_t>(std::time(nullptr)));
      if (!verdict.accepted()) why = verdict.Describe();
    }
    CloseHandle(handle);  // closed before any rename: a rename needs the share mode this handle refuses
    check.ok = why.empty();
    check.reason = why;
    return check;
  }

  engine::ArtifactFacts Measure(const std::string& path) const override {
    engine::ArtifactFacts facts;
    std::wstring wide;
    if (!ToWide(path, &wide)) return facts;
    const HANDLE file = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      const DWORD error = GetLastError();
      facts.unreadable = error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND;
      return facts;
    }
    facts.present = true;
    std::vector<unsigned char> bytes;
    if (ReadWholeFile(file, &bytes)) {
      facts.sha256 = Sha256Hex(bytes);
      facts.version = BuildVersion(bytes);
    } else {
      facts.unreadable = true;
    }
    CloseHandle(file);
    return facts;
  }
};

// Another process runs the root's runtime when a write-open of it meets a sharing violation: a mapped image refuses it
// (ADR 0024, section 4.4).
class HostInUseCheck final : public engine::InUseCheck {
 public:
  engine::InUseAnswer Query(const std::string& runtime_path) const override {
    engine::InUseAnswer answer;
    std::wstring wide;
    if (!ToWide(runtime_path, &wide)) {
      answer.os_error = static_cast<int>(ERROR_INVALID_NAME);
      answer.state = engine::InUse::kError;  // a path that does not convert is not known to be free
      return answer;
    }
    const HANDLE file = CreateFileW(wide.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
      CloseHandle(file);
      return answer;
    }
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return answer;
    answer.os_error = static_cast<int>(error);
    answer.state = error == ERROR_SHARING_VIOLATION ? engine::InUse::kInUse : engine::InUse::kError;
    return answer;
  }
};

// The previous release's runtime is the rollback target: there, and it verifies under the host's policy (ADR 0024, section 6).
class HostRollbackTarget final : public engine::RollbackTarget {
 public:
  explicit HostRollbackTarget(const engine::ArtifactVerifier& verifier) noexcept : verifier_(verifier) {}

  engine::RollbackCheck Check(const std::string& runtime_path) const override {
    engine::RollbackCheck check;
    if (AttributesOf(runtime_path) == INVALID_FILE_ATTRIBUTES) {
      check.state = engine::RollbackState::kAbsent;
      return check;
    }
    const engine::ArtifactCheck verdict = verifier_.Verify(runtime_path, engine::ArtifactExpect{});
    if (!verdict.ok) {
      check.state = engine::RollbackState::kUnverified;
      check.reason = verdict.reason;
      return check;
    }
    check.state = engine::RollbackState::kUsable;
    return check;
  }

 private:
  const engine::ArtifactVerifier& verifier_;
};

// The PC load maps the DLL under the loader lock, so its DllMain has run when the load outcome is known (runtime_load.h).
class HostLoaderEdge final : public engine::RuntimeLoaderEdge {
 public:
  bool CodeBeforeEntry() const noexcept override { return kPeCodeBeforeEntry; }
};

// The host's records: one JSON record per decision, in the run's main log (tag NEVR.HOST).
class HostLog final : public engine::UpdateLog {
 public:
  explicit HostLog(BootstrapDiagnostics& diagnostics) noexcept : diagnostics_(diagnostics) {}
  void Emit(const nlohmann::json& record) override { diagnostics_.Emit(record); }

 private:
  BootstrapDiagnostics& diagnostics_;
};

class HostClock final : public engine::UpdateClock {
 public:
  std::uint64_t TickMs() const noexcept override { return GetTickCount64(); }
  std::string UtcIso() const override {
    SYSTEMTIME now{};
    GetSystemTime(&now);
    char text[kUtcTextLength] = {};
    std::snprintf(text, sizeof(text), "%04u-%02u-%02uT%02u:%02u:%02uZ", now.wYear, now.wMonth, now.wDay, now.wHour,
                  now.wMinute, now.wSecond);
    return text;
  }
};

// Test seams, read by the engine through function pointers (update_seams.h): production answers are the constants.
bool MoveFails(int move, int* os_error) {
  DWORD error = 0;
  const bool fails = SwapMoveFails(move, &error);
  *os_error = static_cast<int>(error);
  return fails;
}

// The edges of one start. A start owns one; the engine reads them through the context it is given.
class PcEdges {
 public:
  explicit PcEdges(BootstrapDiagnostics& diagnostics) : rollback_(verifier_), log_(diagnostics) {}
  PcEdges(const PcEdges&) = delete;
  PcEdges& operator=(const PcEdges&) = delete;

  HostFs fs_;
  HostVerifier verifier_;
  HostInUseCheck in_use_;
  HostRollbackTarget rollback_;
  HostLoaderEdge loader_;
  HostLog log_;
  HostClock clock_;
};

// The engine's context for one start: the root, the bound edges, the seams and the host's lock slot.
engine::StartContext ContextFor(const std::string& root, PcEdges& edges, std::unique_ptr<engine::FsLock>* lock) {
  engine::StartHooks hooks;
  hooks.step_ends = &SwapStepEnds;
  hooks.move_fails = &MoveFails;
  return engine::StartContext{
      root,
      engine::StartEdges{edges.fs_, edges.verifier_, edges.in_use_, edges.rollback_, edges.loader_, edges.log_, edges.clock_},
      hooks,
      engine::LockTiming{static_cast<std::uint32_t>(SwapLockTimeoutMs()), static_cast<std::uint32_t>(SwapLockStepMs())},
      lock};
}

// The nevr root is the folder the runtime sits in: the runtime's path without its last element. False when it does not
// convert to UTF-8, the engine's spelling of a path: such a root names no update folder the engine can reach. Out of memory
// is not a false: it propagates to the start, which stops (RunUpdateAtStart, RollBackInProcess, MarkPendingFailed).
bool RootOf(const wchar_t* runtime_path, std::string* root) {
  std::wstring folder = runtime_path;
  const std::size_t separator = folder.find_last_of(L"\\/");
  folder = separator == std::wstring::npos ? std::wstring() : folder.substr(0, separator);
  return ToUtf8(folder, root);
}

}  // namespace

UpdateLock::~UpdateLock() { Release(); }

bool UpdateLock::Acquire(const wchar_t* update_dir, DWORD timeout_ms, DWORD step_ms) noexcept {
  if (Held()) return true;
  if (update_dir == nullptr || update_dir[0] == L'\0') return false;
  try {
    std::string utf8;
    if (!ToUtf8(std::wstring(update_dir), &utf8)) return false;
    HostFs fs;
    lock_ = fs.TakeLock(utf8, static_cast<std::uint32_t>(timeout_ms), static_cast<std::uint32_t>(step_ms), nullptr);
    return lock_ != nullptr;
  } catch (const std::exception&) {
    return false;
  }
}

void UpdateLock::Release() noexcept { lock_.reset(); }

UpdateResult RunUpdateAtStart(const wchar_t* runtime_path, BootstrapDiagnostics& diagnostics, UpdateLock& lock) noexcept {
  if (runtime_path == nullptr || runtime_path[0] == L'\0') return {};
  try {
    std::string root;
    if (!RootOf(runtime_path, &root)) {
      // The root has no UTF-8 spelling, so the engine reaches no update folder through it: the start does no update work and
      // the runtime loads. The record is a Warning, and its reason is the word the runtime's check gives the same root.
      diagnostics.Emit(nlohmann::json{{"event", "update_swap"},
                                      {"outcome", "skipped"},
                                      {"reason", "root_unconvertible"},
                                      {"level", "warn"}});
      return {};
    }
    PcEdges edges(diagnostics);
    return engine::RunStart(ContextFor(root, edges, lock.Slot()));
  } catch (const std::exception&) {
    // Not even the start could be set up, or its record written: nothing is known about the root, so nothing loads.
    return {UpdateAction::kStop, "internal_error", false};
  }
}

UpdateResult RollBackInProcess(const wchar_t* runtime_path, BootstrapDiagnostics& diagnostics, UpdateLock& lock,
                               const char* reason) noexcept {
  if (runtime_path == nullptr || runtime_path[0] == L'\0' || reason == nullptr) return {UpdateAction::kStop, "no_previous", false};
  try {
    PcEdges edges(diagnostics);
    std::string root;
    if (!RootOf(runtime_path, &root)) return {UpdateAction::kStop, "internal_error", false};
    return engine::RollBackNowIn(ContextFor(root, edges, lock.Slot()), reason);
  } catch (const std::exception&) {
    return {UpdateAction::kStop, "internal_error", false};
  }
}

bool MarkPendingFailed(const wchar_t* runtime_path, BootstrapDiagnostics& diagnostics, UpdateLock& lock, const char* reason) noexcept {
  if (runtime_path == nullptr || runtime_path[0] == L'\0' || reason == nullptr) return false;
  bool marked = false;
  try {
    PcEdges edges(diagnostics);
    std::string root;
    if (RootOf(runtime_path, &root)) marked = engine::MarkFailedIn(ContextFor(root, edges, lock.Slot()), reason);
  } catch (const std::exception&) {
    marked = false;
  }
  lock.Release();  // the load released it before initialize; this start took it only to write the record
  return marked;
}

}  // namespace nevr_host
