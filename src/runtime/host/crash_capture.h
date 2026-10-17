#pragma once

// Host-owned minidump capture for the BugSplat_createReport callback
// (docs/adr/0008-bugsplat-host-and-nevr-runtime.md).
//
// The stable host's createReport must write a structurally valid minidump even
// when nevr.dll is absent or failed to load. It therefore:
//   - resolves MiniDumpWriteDump from the ALREADY-LOADED dbghelp.dll
//     (GetModuleHandleW + GetProcAddress; never LoadLibrary: the game's
//     HandleCrashDump has loaded dbghelp before this callback runs);
//   - writes to the first writable of three precomputed destinations:
//       %LOCALAPPDATA%\nevr\echovr\crashes, %TEMP%\nevr\echovr\crashes, the host directory;
//   - guards against re-entry with an atomic flag cleared on return;
//   - on total failure writes a fixed-size text diagnostic;
//   - records every decision as one JSON line through BootstrapDiagnostics::AppendLine
//     (dump written, destination failed and why, capture suppressed, capture failed).
//
// No NEVR logger and no runtime load here: the process is faulting. The records are built in a
// fixed stack buffer by JsonLine and appended through BootstrapDiagnostics::AppendLine. That call
// opens and creates the log file and the crash directories (CreateFileW, CreateDirectoryW), and
// waits on the log mutex that attach created (it creates no mutex). Each wait is bounded by
// kBootstrapLogLockWaitMs, and a wait that times out makes later appends skip the wait; waits that end
// owned are not capped per process. Whether those calls, or MiniDumpWriteDump, allocate is not measured. The reentrancy guard below is an atomic flag, not a lock.

#include "runtime/host/bootstrap_diagnostics.h"

#include <windows.h>
#include <dbghelp.h>

#include <cstdint>

namespace nevr_host {

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                          PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION,
                                          PMINIDUMP_CALLBACK_INFORMATION);

// Invoked once by Capture while the atomic reentrancy guard is held, before any
// dump work. Only the test build can install one (CrashCapture::SetCaptureProbe); the child-process
// crash harness uses it to raise a nested fault and prove the guard suppresses recursion (no deadlock).
using CaptureProbeFn = void (*)();

class CrashCapture {
 public:
  /// Precompute the three destination directories into fixed buffers from
  /// %LOCALAPPDATA%, %TEMP% and the host module path. No file I/O.
  /// `diagnostics` (optional, must outlive this object) receives the JSON records.
  void Prepare(HMODULE host_module, BootstrapDiagnostics* diagnostics);

  /// The BugSplat_createReport body. Returns true when a minidump was written,
  /// false after every destination failed (a text diagnostic is attempted).
  bool Capture(PEXCEPTION_POINTERS exception_pointers) noexcept;

  // Atomic reentrancy guard, exposed for unit tests.
  bool TryEnter() noexcept;
  void Exit() noexcept;

#if defined(NEVR_HOST_TESTING)
  // Test seams, compiled only under NEVR_HOST_TESTING (tools/verify_host_seams.py). The child-process
  // crash harness uses them to inject a nested fault (SetCaptureProbe) and to
  // simulate a full-disk or write failure (SetFailWrites) without touching the
  // crash-safe capture body.
  static void SetCaptureProbe(CaptureProbeFn probe) noexcept;
  static void SetFailWrites(bool fail) noexcept;
#endif

  static constexpr const wchar_t* kDbgHelpModule = L"dbghelp.dll";
  static constexpr const char* kMiniDumpWriteDumpProc = "MiniDumpWriteDump";

  // Test seams.
  const wchar_t* LocalCrashDir() const { return local_crash_dir_; }
  const wchar_t* TempCrashDir() const { return temp_crash_dir_; }
  const wchar_t* HostDir() const { return host_dir_; }
  bool LocalReady() const { return local_ready_; }
  bool TempReady() const { return temp_ready_; }
  bool HostDirReady() const { return host_dir_ready_; }

 private:
  struct Attempt {
    const wchar_t* dir;
    const char* destination;
    bool ready;
    bool create_dir;
  };

  enum class Stage : std::uint32_t {
    kWritten = 0,
    kDirectory,       // the directory path could not be formed
    kCreateFile,      // CreateFileW failed
    kSimulatedFault,  // the SetFailWrites seam
    kWriteDump,       // MiniDumpWriteDump returned FALSE
  };

  struct Outcome {
    Stage stage = Stage::kDirectory;
    DWORD win32_error = 0;
    DWORD size = 0;
    wchar_t path[MAX_PATH] = {};
  };

  Outcome WriteDumpTo(const Attempt& attempt, PEXCEPTION_POINTERS exception_pointers,
                      MiniDumpWriteDumpFn write_dump) noexcept;
  void WriteTextDiagnostic(PEXCEPTION_POINTERS exception_pointers) noexcept;
  void Record(const char* outcome, const char* destination, const char* stage, DWORD win32_error,
              const wchar_t* path, DWORD size, PEXCEPTION_POINTERS exception_pointers) noexcept;

  BootstrapDiagnostics* diagnostics_ = nullptr;
  wchar_t local_app_data_[MAX_PATH] = {};
  wchar_t temp_[MAX_PATH] = {};
  wchar_t local_crash_dir_[MAX_PATH] = {};  // %LOCALAPPDATA%\nevr\echovr\crashes
  wchar_t temp_crash_dir_[MAX_PATH] = {};   // %TEMP%\nevr\echovr\crashes
  wchar_t host_dir_[MAX_PATH] = {};         // directory of BugSplat64.dll
  bool local_ready_ = false;
  bool temp_ready_ = false;
  bool host_dir_ready_ = false;
};

}  // namespace nevr_host
