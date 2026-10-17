// Host minidump capture (runtime/host/crash_capture.h/.cpp): destination
// precomputation, the atomic reentrancy guard, the text-diagnostic fallback, the
// JSON record of every decision, and a real in-process MiniDumpWriteDump through
// the BugSplat_createReport body.

#include "runtime/host/crash_capture.h"

#include "runtime/tests/host_crash/minidump.h"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include <windows.h>
#include <dbghelp.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using nevr_host::BootstrapDiagnostics;
using nevr_host::CrashCapture;

constexpr DWORD kProbeException = 0xE0000001;

// Captured from a first-chance VEH: real EXCEPTION_RECORD + CONTEXT, copied into
// our own storage so they outlive the exception dispatch.
EXCEPTION_RECORD g_captured_record{};
CONTEXT g_captured_context{};
volatile LONG g_captured = 0;

LONG WINAPI CaptureVeh(PEXCEPTION_POINTERS pointers) {
  if (pointers->ExceptionRecord->ExceptionCode == kProbeException) {
    g_captured_record = *pointers->ExceptionRecord;
    g_captured_context = *pointers->ContextRecord;
    InterlockedExchange(&g_captured, 1);
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

std::wstring MakeTempDir(const wchar_t* tag) {
  wchar_t base[MAX_PATH] = {};
  const DWORD base_len = GetTempPathW(MAX_PATH, base);
  EXPECT_GT(base_len, 0u);
  EXPECT_LT(base_len, static_cast<DWORD>(MAX_PATH));
  static LONG counter = 0;
  const LONG sequence = InterlockedIncrement(&counter);
  wchar_t dir[MAX_PATH] = {};
  const int n = swprintf(dir, MAX_PATH, L"%ls\\nevr-crash-%ls-%lu-%llu-%ld", base, tag,
                         static_cast<unsigned long>(GetCurrentProcessId()),
                         static_cast<unsigned long long>(GetTickCount64()),
                         static_cast<long>(sequence));
  EXPECT_GT(n, 0);
  EXPECT_NE(CreateDirectoryW(dir, nullptr), FALSE);
  return std::wstring(dir);
}

std::vector<std::uint8_t> ReadAllBytes(const wchar_t* path) {
  HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return {};
  std::vector<std::uint8_t> out;
  std::uint8_t buffer[4096] = {};
  DWORD read = 0;
  while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) != FALSE && read != 0) {
    out.insert(out.end(), buffer, buffer + read);
  }
  CloseHandle(file);
  return out;
}

// The newest nevr-<pid>-*.dmp in dir, or empty when none exists.
std::wstring FindNewestDump(const wchar_t* dir, DWORD pid) {
  wchar_t pattern[MAX_PATH] = {};
  swprintf(pattern, MAX_PATH, L"%ls\\nevr-%lu-*.dmp", dir,
           static_cast<unsigned long>(pid));
  WIN32_FIND_DATAW data{};
  HANDLE find = FindFirstFileW(pattern, &data);
  std::wstring newest;
  FILETIME newest_time{};
  if (find != INVALID_HANDLE_VALUE) {
    do {
      if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
      if (newest.empty() || CompareFileTime(&data.ftLastWriteTime, &newest_time) > 0) {
        newest = data.cFileName;
        newest_time = data.ftLastWriteTime;
      }
    } while (FindNextFileW(find, &data) != FALSE);
    FindClose(find);
  }
  if (newest.empty()) return {};
  wchar_t full[MAX_PATH] = {};
  swprintf(full, MAX_PATH, L"%ls\\%ls", dir, newest.c_str());
  return std::wstring(full);
}

std::string ReadText(const std::wstring& path) {
  const std::vector<std::uint8_t> bytes = ReadAllBytes(path.c_str());
  return std::string(bytes.begin(), bytes.end());
}

// The crash records in the bootstrap log, parsed.
std::vector<nlohmann::json> CrashRecords(const std::wstring& local) {
  std::vector<nlohmann::json> records;
  const std::string text = ReadText(local + L"\\nevr\\echovr\\logs\\nevr-bootstrap.jsonl");
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find('\n', start);
    EXPECT_NE(end, std::string::npos);
    if (end == std::string::npos) break;
    const nlohmann::json record = nlohmann::json::parse(text.substr(start, end - start));
    if (record.at("event") == "crash_dump") records.push_back(record);
    start = end + 1;
  }
  return records;
}

// A prepared capture and the diagnostics that receive its records.
struct Prepared {
  BootstrapDiagnostics diagnostics;
  CrashCapture capture;
  explicit Prepared(const std::wstring& local, const std::wstring& temp) {
    SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str());
    SetEnvironmentVariableW(L"TEMP", temp.c_str());
    diagnostics.Prepare(GetModuleHandleW(nullptr));
    capture.Prepare(GetModuleHandleW(nullptr), &diagnostics);
  }
};

}  // namespace

TEST(HostCrashCapture, PrepareComputesTheThreeDestinations) {
  const std::wstring local = MakeTempDir(L"local");
  const std::wstring temp = MakeTempDir(L"temp");
  Prepared prepared(local, temp);
  ASSERT_TRUE(prepared.capture.LocalReady());
  ASSERT_TRUE(prepared.capture.TempReady());
  ASSERT_TRUE(prepared.capture.HostDirReady());
  EXPECT_EQ(std::wstring(prepared.capture.LocalCrashDir()), local + L"\\nevr\\echovr\\crashes");
  EXPECT_EQ(std::wstring(prepared.capture.TempCrashDir()), temp + L"\\nevr\\echovr\\crashes");
  EXPECT_GE(std::wstring(prepared.capture.HostDir()).size(), 1u);
}

TEST(HostCrashCapture, ReentrancyGuardHoldsAndClears) {
  CrashCapture capture;
  EXPECT_TRUE(capture.TryEnter());
  EXPECT_FALSE(capture.TryEnter());
  EXPECT_FALSE(capture.TryEnter());
  capture.Exit();
  EXPECT_TRUE(capture.TryEnter());
  capture.Exit();
}

TEST(HostCrashCapture, ANestedCaptureIsSuppressedAndRecorded) {
  const std::wstring local = MakeTempDir(L"local");
  Prepared prepared(local, MakeTempDir(L"temp"));
  ASSERT_TRUE(prepared.capture.TryEnter());  // a report is already being written
  EXPECT_FALSE(prepared.capture.Capture(nullptr));
  prepared.capture.Exit();

  const std::vector<nlohmann::json> records = CrashRecords(local);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].at("outcome"), "suppressed");
  EXPECT_EQ(records[0].at("stage"), "reentrant");
  EXPECT_EQ(records[0].at("thread"), GetCurrentThreadId());
}

// A crash before the first flush is the first write in the process. The capture only uses the log
// mutex that attach created: it creates none.
TEST(HostCrashCapture, TheCaptureCreatesNoLogMutexWhenItIsTheFirstToWrite) {
  const std::wstring local = MakeTempDir(L"local");
  Prepared prepared(local, MakeTempDir(L"temp"));
  BootstrapDiagnostics::ResetLogLockForTest();
  ASSERT_TRUE(prepared.capture.TryEnter());
  EXPECT_FALSE(prepared.capture.Capture(nullptr));
  prepared.capture.Exit();
  EXPECT_EQ(BootstrapDiagnostics::LogMutexCreations(), 0u) << "the crash path created the log mutex";
  EXPECT_EQ(CrashRecords(local).size(), 1u);
  prepared.diagnostics.Prepare(GetModuleHandleW(nullptr));  // leave the process attached for later tests
}

TEST(HostCrashCapture, WithoutDbghelpCaptureFailsWritesATextDiagnosticAndSaysWhy) {
  const std::wstring local = MakeTempDir(L"local");
  Prepared prepared(local, MakeTempDir(L"temp"));
  // dbghelp.dll is intentionally not loaded: Capture must not LoadLibrary it.
  ASSERT_EQ(GetModuleHandleW(L"dbghelp.dll"), nullptr);
  EXPECT_FALSE(prepared.capture.Capture(nullptr));

  wchar_t pattern[MAX_PATH] = {};
  swprintf(pattern, MAX_PATH, L"%ls\\nevr-%lu-*.txt",
           (local + L"\\nevr\\echovr\\crashes").c_str(),
           static_cast<unsigned long>(GetCurrentProcessId()));
  WIN32_FIND_DATAW data{};
  const HANDLE find = FindFirstFileW(pattern, &data);
  EXPECT_NE(find, INVALID_HANDLE_VALUE);
  if (find != INVALID_HANDLE_VALUE) FindClose(find);

  const std::vector<nlohmann::json> records = CrashRecords(local);
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0].at("outcome"), "dbghelp_unavailable");
  EXPECT_EQ(records[0].at("win32_error"), ERROR_MOD_NOT_FOUND);
  EXPECT_EQ(records[1].at("outcome"), "failed");
}

TEST(HostCrashCapture, WritesAStructurallyValidMinidumpNamingTheThreadAndRecordsIt) {
  const std::wstring local = MakeTempDir(L"local");
  Prepared prepared(local, MakeTempDir(L"temp"));
  ASSERT_TRUE(prepared.capture.LocalReady());

  // The game loads dbghelp before its crash path (HandleCrashDump); the host only
  // resolves it, never loads it. Simulate that here.
  ASSERT_NE(LoadLibraryW(L"dbghelp.dll"), nullptr);

  // Capture real exception pointers from a benign first-chance exception.
  void* handler = AddVectoredExceptionHandler(1, CaptureVeh);
  ASSERT_NE(handler, nullptr);
  RaiseException(kProbeException, 0, 0, nullptr);
  RemoveVectoredExceptionHandler(handler);
  ASSERT_EQ(g_captured, 1L);

  EXCEPTION_POINTERS pointers{};
  pointers.ExceptionRecord = &g_captured_record;
  pointers.ContextRecord = &g_captured_context;

  const DWORD thread = GetCurrentThreadId();
  const DWORD pid = GetCurrentProcessId();
  EXPECT_TRUE(prepared.capture.Capture(&pointers));

  const std::wstring crashes = local + L"\\nevr\\echovr\\crashes";
  const std::wstring dump = FindNewestDump(crashes.c_str(), pid);
  ASSERT_FALSE(dump.empty());
  const std::vector<std::uint8_t> bytes = ReadAllBytes(dump.c_str());
  ASSERT_FALSE(bytes.empty());

  nevr_host_test::MinidumpView view(bytes.data(), bytes.size());
  ASSERT_TRUE(view.Ok());
  EXPECT_TRUE(view.HasSignature());
  EXPECT_TRUE(view.ValidVersion());
  std::uint32_t thread_id = 0;
  std::uint32_t code = 0;
  ASSERT_TRUE(view.ExceptionInfo(&thread_id, &code, nullptr));
  EXPECT_EQ(thread_id, thread);
  EXPECT_EQ(code, kProbeException);
  EXPECT_TRUE(view.ContainsThread(thread));

  const std::vector<nlohmann::json> records = CrashRecords(local);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].at("outcome"), "written");
  EXPECT_EQ(records[0].at("destination"), "localappdata");
  EXPECT_EQ(records[0].at("thread"), thread);
  EXPECT_EQ(records[0].at("code"), "0xe0000001");
  EXPECT_EQ(records[0].at("bytes"), bytes.size());
  EXPECT_EQ(records[0].at("path").get<std::string>(),
            std::string(dump.begin(), dump.end()));  // ASCII temp path
}
