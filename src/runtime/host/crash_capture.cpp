#include "runtime/host/crash_capture.h"

#include "runtime/host/host_json.h"
#include "runtime/host/host_paths.h"
#include "runtime/host/path_buffer.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace nevr_host {
namespace {

// Process-wide reentrancy guard. A nested createReport (a fault raised while
// this callback is already writing a dump) must not recurse; it is suppressed.
// The flag is cleared when the outermost callback returns, so a later, separate
// crash can still be captured.
volatile LONG g_capturing = 0;
volatile LONG g_dump_counter = 0;

// Test seams (null / 0 in production): a probe run while the guard is held, and
// a flag forcing WriteDumpTo to fail after opening the file (simulating a
// full-disk or write failure for the crash harness).
CaptureProbeFn g_capture_probe = nullptr;
volatile LONG g_fail_writes = 0;

// Bounded wide sprintf into a fixed buffer; returns false on truncation.
bool FormatDumpName(wchar_t* out, std::size_t out_size, const wchar_t* dir, DWORD pid,
                    LONG sequence, const wchar_t* extension) {
  const int n = swprintf(out, out_size, L"%ls\\nevr-%lu-%ld.%ls", dir,
                         static_cast<unsigned long>(pid), static_cast<long>(sequence), extension);
  return n >= 0 && static_cast<std::size_t>(n) < out_size;
}

const char* StageName(DWORD stage) {
  switch (stage) {
    case 0: return "written";
    case 1: return "directory";
    case 2: return "create_file";
    case 3: return "simulated_write_failure";
    case 4: return "write_dump";
  }
  return "unknown";
}

}  // namespace

bool CrashCapture::TryEnter() noexcept {
  return InterlockedExchange(&g_capturing, 1) == 0;
}

void CrashCapture::Exit() noexcept {
  InterlockedExchange(&g_capturing, 0);
}

void CrashCapture::SetCaptureProbe(CaptureProbeFn probe) noexcept { g_capture_probe = probe; }

void CrashCapture::SetFailWrites(bool fail) noexcept {
  InterlockedExchange(&g_fail_writes, fail ? 1 : 0);
}

void CrashCapture::Prepare(HMODULE host_module, BootstrapDiagnostics* diagnostics) {
  diagnostics_ = diagnostics;
  local_ready_ = false;
  temp_ready_ = false;
  host_dir_ready_ = false;

  if (CopyEnv(local_app_data_, MAX_PATH, L"LOCALAPPDATA")) {
    local_ready_ = AppendPath(local_crash_dir_, MAX_PATH, local_app_data_, kCrashesSubdirectory);
  }
  if (CopyEnv(temp_, MAX_PATH, L"TEMP")) {
    temp_ready_ = AppendPath(temp_crash_dir_, MAX_PATH, temp_, kCrashesSubdirectory);
  }
  host_dir_ready_ = DirOfModule(host_dir_, MAX_PATH, host_module);
}

void CrashCapture::Record(const char* outcome, const char* destination, const char* stage,
                          DWORD win32_error, const wchar_t* path, DWORD size,
                          PEXCEPTION_POINTERS exception_pointers) noexcept {
  if (diagnostics_ == nullptr) return;
  char buffer[2048];
  JsonLine line(buffer, sizeof(buffer), "crash_dump");
  line.Str("outcome", outcome);
  if (destination != nullptr) line.Str("destination", destination);
  if (stage != nullptr) line.Str("stage", stage);
  if (win32_error != 0) line.UInt("win32_error", win32_error);
  if (path != nullptr && path[0] != L'\0') line.Wide("path", path);
  if (size != 0) line.UInt("bytes", size);
  line.UInt("thread", GetCurrentThreadId());
  if (exception_pointers != nullptr && exception_pointers->ExceptionRecord != nullptr) {
    line.Hex("code", exception_pointers->ExceptionRecord->ExceptionCode);
    line.Hex("address", static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(
                            exception_pointers->ExceptionRecord->ExceptionAddress)));
  }
  const std::size_t length = line.Finish();
  if (length != 0) diagnostics_->AppendLine(buffer, length);
}

bool CrashCapture::Capture(PEXCEPTION_POINTERS exception_pointers) noexcept {
  if (!TryEnter()) {
    // A fault while the first report is being written: not a second report.
    Record("suppressed", nullptr, "reentrant", 0, nullptr, 0, exception_pointers);
    return false;
  }

  // Test seam: while the reentrancy guard is held, let the harness raise a
  // nested fault to prove the next Capture is suppressed (no recursion or deadlock).
  if (g_capture_probe != nullptr) g_capture_probe();

  bool captured = false;
  const HMODULE dbghelp = GetModuleHandleW(kDbgHelpModule);
  MiniDumpWriteDumpFn write_dump = nullptr;
  if (dbghelp != nullptr) {
    write_dump = reinterpret_cast<MiniDumpWriteDumpFn>(
        GetProcAddress(dbghelp, kMiniDumpWriteDumpProc));
  }
  if (write_dump == nullptr) {
    Record("dbghelp_unavailable", nullptr, nullptr,
           dbghelp == nullptr ? ERROR_MOD_NOT_FOUND : ERROR_PROC_NOT_FOUND, nullptr, 0,
           exception_pointers);
  } else {
    const Attempt attempts[] = {
        {local_crash_dir_, "localappdata", local_ready_, true},
        {temp_crash_dir_, "temp", temp_ready_, true},
        {host_dir_, "host_directory", host_dir_ready_, false},
    };
    for (const Attempt& attempt : attempts) {
      if (!attempt.ready) continue;
      const Outcome outcome = WriteDumpTo(attempt, exception_pointers, write_dump);
      if (outcome.stage == Stage::kWritten) {
        Record("written", attempt.destination, nullptr, 0, outcome.path, outcome.size,
               exception_pointers);
        captured = true;
        break;
      }
      Record("destination_failed", attempt.destination,
             StageName(static_cast<DWORD>(outcome.stage)), outcome.win32_error, outcome.path, 0,
             exception_pointers);
    }
  }

  if (!captured) {
    WriteTextDiagnostic(exception_pointers);
    Record("failed", nullptr, nullptr, 0, nullptr, 0, exception_pointers);
  }
  Exit();
  return captured;
}

CrashCapture::Outcome CrashCapture::WriteDumpTo(const Attempt& attempt,
                                                PEXCEPTION_POINTERS exception_pointers,
                                                MiniDumpWriteDumpFn write_dump) noexcept {
  Outcome outcome;
  if (attempt.create_dir) {
    wchar_t dir_copy[MAX_PATH] = {};
    const std::size_t dir_len = wcslen(attempt.dir);
    if (dir_len + 1 > MAX_PATH) return outcome;
    std::memcpy(dir_copy, attempt.dir, (dir_len + 1) * sizeof(wchar_t));
    CreateDirectories(dir_copy);
  }

  const DWORD pid = GetCurrentProcessId();
  const LONG sequence = InterlockedIncrement(&g_dump_counter);
  if (!FormatDumpName(outcome.path, MAX_PATH, attempt.dir, pid, sequence, L"dmp")) {
    outcome.path[0] = L'\0';
    return outcome;
  }

  HANDLE file = CreateFileW(outcome.path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    outcome.stage = Stage::kCreateFile;
    outcome.win32_error = GetLastError();
    return outcome;
  }

  // Test seam: simulate a full-disk or write failure by failing after the file is
  // opened (the path that MiniDumpWriteDump returning FALSE also exercises), so
  // Capture falls through to the text diagnostic.
  if (g_fail_writes != 0) {
    CloseHandle(file);
    DeleteFileW(outcome.path);
    outcome.stage = Stage::kSimulatedFault;
    outcome.win32_error = ERROR_DISK_FULL;
    return outcome;
  }

  MINIDUMP_EXCEPTION_INFORMATION exception_info = {};
  exception_info.ThreadId = GetCurrentThreadId();
  exception_info.ExceptionPointers = exception_pointers;
  exception_info.ClientPointers = FALSE;

  const BOOL ok = write_dump(GetCurrentProcess(), pid, file, MiniDumpNormal, &exception_info,
                             nullptr, nullptr);
  const DWORD error = ok == FALSE ? GetLastError() : 0;
  FlushFileBuffers(file);
  const DWORD size = GetFileSize(file, nullptr);
  CloseHandle(file);

  if (ok == FALSE) {
    DeleteFileW(outcome.path);
    outcome.stage = Stage::kWriteDump;
    outcome.win32_error = error;
    return outcome;
  }
  outcome.stage = Stage::kWritten;
  outcome.size = size == INVALID_FILE_SIZE ? 0 : size;
  return outcome;
}

void CrashCapture::WriteTextDiagnostic(PEXCEPTION_POINTERS exception_pointers) noexcept {
  char line[512] = {};
  DWORD code = 0;
  unsigned long long address = 0;
  if (exception_pointers != nullptr && exception_pointers->ExceptionRecord != nullptr) {
    code = exception_pointers->ExceptionRecord->ExceptionCode;
    address = static_cast<unsigned long long>(
        reinterpret_cast<std::uintptr_t>(exception_pointers->ExceptionRecord->ExceptionAddress));
  }
  const int n = snprintf(line, sizeof(line),
                         "nevr host: minidump unavailable (dbghelp absent or all destinations "
                         "unwritable) code=0x%08lX addr=0x%llX thread=%lu\n",
                         static_cast<unsigned long>(code), address,
                         static_cast<unsigned long>(GetCurrentThreadId()));
  if (n <= 0) return;
  const std::size_t length =
      static_cast<std::size_t>(n) < sizeof(line) ? static_cast<std::size_t>(n) : sizeof(line) - 1;

  const DWORD pid = GetCurrentProcessId();
  const LONG sequence = InterlockedIncrement(&g_dump_counter);

  const Attempt attempts[] = {
      {local_crash_dir_, "localappdata", local_ready_, true},
      {temp_crash_dir_, "temp", temp_ready_, true},
      {host_dir_, "host_directory", host_dir_ready_, false},
  };
  for (const Attempt& attempt : attempts) {
    if (!attempt.ready) continue;
    if (attempt.create_dir) {
      wchar_t dir_copy[MAX_PATH] = {};
      const std::size_t dir_len = wcslen(attempt.dir);
      if (dir_len + 1 > MAX_PATH) continue;
      std::memcpy(dir_copy, attempt.dir, (dir_len + 1) * sizeof(wchar_t));
      CreateDirectories(dir_copy);
    }
    wchar_t path[MAX_PATH] = {};
    if (!FormatDumpName(path, MAX_PATH, attempt.dir, pid, sequence, L"txt")) continue;
    HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) continue;
    DWORD written = 0;
    WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr);
    FlushFileBuffers(file);
    CloseHandle(file);
    return;
  }
}

}  // namespace nevr_host
