// Host minidump capture (runtime/host/crash_capture.h/.cpp): destination
// precomputation, the atomic reentrancy guard, the text-diagnostic fallback, and
// a real in-process MiniDumpWriteDump through the BugSplat_createReport body.

#include "runtime/host/crash_capture.h"

#include "runtime/tests/host_crash/minidump.h"

#include <gtest/gtest.h>

#include <windows.h>
#include <dbghelp.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using NevrHost::CrashCapture;

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
  const int n = swprintf(dir, MAX_PATH, L"%ls\\nevr-crash-%ls-%lu-%ld", base, tag,
                         static_cast<unsigned long>(GetCurrentProcessId()),
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

}  // namespace

TEST(HostCrashCapture, PrepareComputesTheThreeDestinations) {
  const std::wstring local = MakeTempDir(L"local");
  const std::wstring temp = MakeTempDir(L"temp");
  SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str());
  SetEnvironmentVariableW(L"TEMP", temp.c_str());

  CrashCapture capture;
  capture.Prepare(GetModuleHandleW(nullptr));
  ASSERT_TRUE(capture.LocalReady());
  ASSERT_TRUE(capture.TempReady());
  ASSERT_TRUE(capture.ShimReady());
  EXPECT_EQ(std::wstring(capture.LocalCrashDir()), local + L"\\EchoVR\\crashes");
  EXPECT_EQ(std::wstring(capture.TempCrashDir()), temp + L"\\EchoVR\\crashes");
  EXPECT_TRUE(std::wstring(capture.ShimDir()).size() >= 1);
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

TEST(HostCrashCapture, WithoutDbghelpCaptureFailsAndWritesATextDiagnostic) {
  const std::wstring local = MakeTempDir(L"local");
  SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str());
  SetEnvironmentVariableW(L"TEMP", MakeTempDir(L"temp").c_str());

  CrashCapture capture;
  capture.Prepare(GetModuleHandleW(nullptr));
  // dbghelp.dll is intentionally not loaded: Capture must not LoadLibrary it.
  EXPECT_FALSE(capture.Capture(nullptr));

  wchar_t pattern[MAX_PATH] = {};
  swprintf(pattern, MAX_PATH, L"%ls\\nevr-%lu-*.txt",
           (local + L"\\EchoVR\\crashes").c_str(),
           static_cast<unsigned long>(GetCurrentProcessId()));
  WIN32_FIND_DATAW data{};
  const HANDLE find = FindFirstFileW(pattern, &data);
  EXPECT_NE(find, INVALID_HANDLE_VALUE);
  if (find != INVALID_HANDLE_VALUE) FindClose(find);
}

TEST(HostCrashCapture, WritesAStructurallyValidMinidumpNamingTheThread) {
  const std::wstring local = MakeTempDir(L"local");
  SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str());
  SetEnvironmentVariableW(L"TEMP", MakeTempDir(L"temp").c_str());

  // The game loads dbghelp before its crash path (HandleCrashDump); the shim only
  // resolves it, never loads it. Simulate that here.
  ASSERT_NE(LoadLibraryW(L"dbghelp.dll"), nullptr);

  CrashCapture capture;
  capture.Prepare(GetModuleHandleW(nullptr));
  ASSERT_TRUE(capture.LocalReady());

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
  EXPECT_TRUE(capture.Capture(&pointers));

  const std::wstring dump = FindNewestDump((local + L"\\EchoVR\\crashes").c_str(), pid);
  ASSERT_FALSE(dump.empty());
  const std::vector<std::uint8_t> bytes = ReadAllBytes(dump.c_str());
  ASSERT_FALSE(bytes.empty());

  NevrHostTest::MinidumpView view(bytes.data(), bytes.size());
  ASSERT_TRUE(view.Ok());
  EXPECT_TRUE(view.HasSignature());
  EXPECT_TRUE(view.ValidVersion());
  std::uint32_t thread_id = 0;
  std::uint32_t code = 0;
  ASSERT_TRUE(view.ExceptionInfo(&thread_id, &code, nullptr));
  EXPECT_EQ(thread_id, thread);
  EXPECT_EQ(code, kProbeException);
  EXPECT_TRUE(view.ContainsThread(thread));
}
