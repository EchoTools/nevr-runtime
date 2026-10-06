#include "runtime/host/crash_capture.h"

#include "runtime/host/path_buffer.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace NevrHost {
namespace {

constexpr wchar_t kCrashesSuffix[] = L"EchoVR\\crashes";

// Process-wide reentrancy guard. A nested createReport (a fault raised while
// this callback is already writing a dump) must not recurse; it is suppressed.
// The flag is cleared when the outermost callback returns, so a later, separate
// crash can still be captured.
volatile LONG g_capturing = 0;
volatile LONG g_dump_counter = 0;

// Bounded wide sprintf into a fixed buffer; returns false on truncation.
bool FormatDumpName(wchar_t* out, std::size_t out_size, const wchar_t* dir, DWORD pid,
                    LONG sequence) {
  const int n = swprintf(out, out_size, L"%ls\\nevr-%lu-%ld.dmp", dir,
                         static_cast<unsigned long>(pid), static_cast<long>(sequence));
  return n >= 0 && static_cast<std::size_t>(n) < out_size;
}

}  // namespace

bool CrashCapture::TryEnter() {
  return InterlockedExchange(&g_capturing, 1) == 0;
}

void CrashCapture::Exit() {
  InterlockedExchange(&g_capturing, 0);
}

void CrashCapture::Prepare(HMODULE shim_module) {
  local_ready_ = false;
  temp_ready_ = false;
  shim_ready_ = false;

  if (CopyEnv(local_app_data_, MAX_PATH, L"LOCALAPPDATA")) {
    local_ready_ = AppendPath(local_crash_dir_, MAX_PATH, local_app_data_, kCrashesSuffix);
  }
  if (CopyEnv(temp_, MAX_PATH, L"TEMP")) {
    temp_ready_ = AppendPath(temp_crash_dir_, MAX_PATH, temp_, kCrashesSuffix);
  }
  shim_ready_ = DirOfModule(shim_dir_, MAX_PATH, shim_module);
}

bool CrashCapture::Capture(PEXCEPTION_POINTERS exception_pointers) {
  if (!TryEnter()) return false;

  bool captured = false;
  const HMODULE dbghelp = GetModuleHandleW(kDbgHelpModule);
  if (dbghelp != nullptr) {
    const auto write_dump = reinterpret_cast<MiniDumpWriteDumpFn>(
        GetProcAddress(dbghelp, kMiniDumpWriteDumpProc));
    if (write_dump != nullptr) {
      if (local_ready_ && WriteDumpTo(local_crash_dir_, exception_pointers, write_dump, true)) {
        captured = true;
      } else if (temp_ready_ && WriteDumpTo(temp_crash_dir_, exception_pointers, write_dump, true)) {
        captured = true;
      } else if (shim_ready_ && WriteDumpTo(shim_dir_, exception_pointers, write_dump, false)) {
        captured = true;
      }
    }
  }

  if (!captured) WriteTextDiagnostic(exception_pointers);
  Exit();
  return captured;
}

bool CrashCapture::WriteDumpTo(const wchar_t* dir, PEXCEPTION_POINTERS exception_pointers,
                               MiniDumpWriteDumpFn write_dump, bool create_dir) {
  if (write_dump == nullptr) return false;
  if (create_dir) {
    wchar_t dir_copy[MAX_PATH] = {};
    const std::size_t dir_len = wcslen(dir);
    if (dir_len + 1 > MAX_PATH) return false;
    std::memcpy(dir_copy, dir, (dir_len + 1) * sizeof(wchar_t));
    CreateDirectories(dir_copy);
  }

  wchar_t path[MAX_PATH] = {};
  const DWORD pid = GetCurrentProcessId();
  const LONG sequence = InterlockedIncrement(&g_dump_counter);
  if (!FormatDumpName(path, MAX_PATH, dir, pid, sequence)) return false;

  HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;

  MINIDUMP_EXCEPTION_INFORMATION exception_info = {};
  exception_info.ThreadId = GetCurrentThreadId();
  exception_info.ExceptionPointers = exception_pointers;
  exception_info.ClientPointers = FALSE;

  const BOOL ok = write_dump(GetCurrentProcess(), pid, file, MiniDumpNormal, &exception_info,
                             nullptr, nullptr);
  FlushFileBuffers(file);
  CloseHandle(file);

  if (ok == FALSE) {
    DeleteFileW(path);
    return false;
  }
  return true;
}

void CrashCapture::WriteTextDiagnostic(PEXCEPTION_POINTERS exception_pointers) {
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

  struct Candidate {
    const wchar_t* dir;
    bool ready;
    bool create_dir;
  };
  const Candidate candidates[] = {
      {local_crash_dir_, local_ready_, true},
      {temp_crash_dir_, temp_ready_, true},
      {shim_dir_, shim_ready_, false},
  };
  for (const Candidate& candidate : candidates) {
    if (!candidate.ready) continue;
    if (candidate.create_dir) {
      wchar_t dir_copy[MAX_PATH] = {};
      const std::size_t dir_len = wcslen(candidate.dir);
      if (dir_len + 1 > MAX_PATH) continue;
      std::memcpy(dir_copy, candidate.dir, (dir_len + 1) * sizeof(wchar_t));
      CreateDirectories(dir_copy);
    }
    wchar_t path[MAX_PATH] = {};
    if (swprintf(path, MAX_PATH, L"%ls\\nevr-%lu-%ld.txt", candidate.dir,
                 static_cast<unsigned long>(pid), static_cast<long>(sequence)) < 0) {
      continue;
    }
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

}  // namespace NevrHost
