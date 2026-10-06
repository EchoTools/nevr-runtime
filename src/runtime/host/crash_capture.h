#pragma once

// Host-owned minidump capture from the BugSplat_createReport callback
// (docs/design/2026-10-05-bugsplat-nevr-split.md step 3).
//
// The stable host's createReport must write a structurally valid minidump even
// when nevr.dll is absent or failed to load. It therefore:
//   - resolves MiniDumpWriteDump from the ALREADY-LOADED dbghelp.dll
//     (GetModuleHandleW + GetProcAddress; never LoadLibrary — the game's
//     HandleCrashDump has already loaded dbghelp before this callback);
//   - writes to the first writable precomputed destination:
//       %LOCALAPPDATA%\EchoVR\crashes, %TEMP%\EchoVR\crashes, shim module dir;
//   - guards against re-entry with an atomic flag cleared on return;
//   - on total failure writes a fixed-size text diagnostic.
//
// No NEVR logger, no locks, no heap-owned path state, no runtime load here.

#include <windows.h>
#include <dbghelp.h>

#include <cstdint>

namespace NevrHost {

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                          PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION,
                                          PMINIDUMP_CALLBACK_INFORMATION);

class CrashCapture {
 public:
  // Precompute the three destination directories into fixed buffers from
  // %LOCALAPPDATA%, %TEMP% and the shim module path. No allocation, no I/O.
  void Prepare(HMODULE shim_module);

  // The BugSplat_createReport body. Returns true when a minidump was written,
  // false after every destination failed (a text diagnostic is attempted).
  bool Capture(PEXCEPTION_POINTERS exception_pointers);

  // Atomic reentrancy guard, exposed for unit tests.
  bool TryEnter();
  void Exit();

  static constexpr const wchar_t* kDbgHelpModule = L"dbghelp.dll";
  static constexpr const char* kMiniDumpWriteDumpProc = "MiniDumpWriteDump";

  // Test seams.
  const wchar_t* LocalCrashDir() const { return local_crash_dir_; }
  const wchar_t* TempCrashDir() const { return temp_crash_dir_; }
  const wchar_t* ShimDir() const { return shim_dir_; }
  bool LocalReady() const { return local_ready_; }
  bool TempReady() const { return temp_ready_; }
  bool ShimReady() const { return shim_ready_; }

 private:
  // create_dir: whether to create the destination (and intermediate levels) on
  // demand. The %LOCALAPPDATA% and %TEMP% crash directories are created on
  // demand; the shim module directory is not (it is the module's own directory
  // and is expected to already exist and be writable).
  bool WriteDumpTo(const wchar_t* dir, PEXCEPTION_POINTERS exception_pointers,
                   MiniDumpWriteDumpFn write_dump, bool create_dir);
  void WriteTextDiagnostic(PEXCEPTION_POINTERS exception_pointers);

  wchar_t local_app_data_[MAX_PATH] = {};
  wchar_t temp_[MAX_PATH] = {};
  wchar_t local_crash_dir_[MAX_PATH] = {};  // %LOCALAPPDATA%\EchoVR\crashes
  wchar_t temp_crash_dir_[MAX_PATH] = {};   // %TEMP%\EchoVR\crashes
  wchar_t shim_dir_[MAX_PATH] = {};         // directory of BugSplat64.dll
  bool local_ready_ = false;
  bool temp_ready_ = false;
  bool shim_ready_ = false;
};

}  // namespace NevrHost
