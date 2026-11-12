// Host minidump capture (runtime/host/crash_capture.h/.cpp): destination
// precomputation, the atomic reentrancy guard, the text-diagnostic fallback, the
// JSON record of every decision, and a real in-process MiniDumpWriteDump through
// the BugSplat_createReport body.

#include "runtime/tests/run_log_test_path.h"
#include "runtime/host/crash_capture.h"

#include "runtime/host/host_paths.h"

#include "runtime/tests/host_crash/minidump.h"

#include <nlohmann/json.hpp>

#include <gtest/gtest.h>

#include "core/nevr_root.h"

#include <windows.h>
#include <dbghelp.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
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
  const std::string text = ReadText(nevr_test::HostLogPath(local));
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
    capture.Prepare(&diagnostics);
  }
};

}  // namespace

// Decision: a minidump goes only to %LOCALAPPDATA%\nevr\echovr\crashes. Never %TEMP%, never the game folder.
TEST(HostCrashCapture, PrepareComputesTheOneCrashDirectory) {
  const std::wstring local = MakeTempDir(L"local");
  const std::wstring temp = MakeTempDir(L"temp");
  Prepared prepared(local, temp);
  ASSERT_TRUE(prepared.capture.LocalReady());
  EXPECT_EQ(std::wstring(prepared.capture.LocalCrashDir()), local + L"\\nevr\\echovr\\crashes");
}

std::wstring FirstFileMatching(const std::wstring& pattern) {
  WIN32_FIND_DATAW data{};
  const HANDLE find = FindFirstFileW(pattern.c_str(), &data);
  if (find == INVALID_HANDLE_VALUE) return {};
  FindClose(find);
  return data.cFileName;
}

// The folder of the executable stands in for "beside the game": the host's directory was the last resort for a dump and its text.
std::wstring ExecutableDir() {
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring dir(path);
  return dir.substr(0, dir.find_last_of(L"\\/"));
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
  // A name of this test's own, so no other process in the wineserver can make it fail or pass.
  static LONG counter = 0;
  const LONG sequence = InterlockedIncrement(&counter);
  wchar_t name[128] = {};
  swprintf(name, 128, L"Local\\nevr-test-log-capture-%lu-%llu-%ld", static_cast<unsigned long>(GetCurrentProcessId()),
           static_cast<unsigned long long>(GetTickCount64()), static_cast<long>(sequence));
  const auto exists = [&name] {
    HANDLE found = OpenMutexW(SYNCHRONIZE, FALSE, name);
    if (found != nullptr) CloseHandle(found);
    return found != nullptr;
  };
  BootstrapDiagnostics::ResetLogLockForTest();
  BootstrapDiagnostics::SetLogMutexNameForTest(name);
  const std::wstring local = MakeTempDir(L"local");
  Prepared prepared(local, MakeTempDir(L"temp"));
  EXPECT_TRUE(exists()) << "attach did not create the log mutex";
  BootstrapDiagnostics::ResetLogLockForTest();  // the crash comes before any attach created it
  ASSERT_FALSE(exists());
  ASSERT_TRUE(prepared.capture.TryEnter());
  EXPECT_FALSE(prepared.capture.Capture(nullptr));
  prepared.capture.Exit();
  EXPECT_FALSE(exists()) << "the crash path created the log mutex";
  EXPECT_EQ(CrashRecords(local).size(), 1u);
  BootstrapDiagnostics::SetLogMutexNameForTest(nullptr);
  prepared.diagnostics.Prepare(GetModuleHandleW(nullptr));  // leave the process attached for later tests
}

TEST(HostCrashCapture, WithoutDbghelpCaptureFailsWritesNoTextFileAndSaysWhy) {
  const std::wstring local = MakeTempDir(L"local");
  const std::wstring temp = MakeTempDir(L"temp");
  Prepared prepared(local, temp);
  // dbghelp.dll is intentionally not loaded: Capture must not LoadLibrary it.
  ASSERT_EQ(GetModuleHandleW(L"dbghelp.dll"), nullptr);
  EXPECT_FALSE(prepared.capture.Capture(nullptr));

  // The crash text is the "failed" record in the main log; no text file is written in the crash folder, %TEMP% or the game folder.
  const std::wstring pid_pattern = L"\\nevr-" + std::to_wstring(GetCurrentProcessId()) + L"-*.txt";
  EXPECT_EQ(FirstFileMatching(local + L"\\nevr\\echovr\\crashes" + pid_pattern), L"");
  EXPECT_EQ(FirstFileMatching(temp + L"\\nevr\\echovr\\crashes" + pid_pattern), L"");
  EXPECT_EQ(FirstFileMatching(ExecutableDir() + pid_pattern), L"");

  const std::vector<nlohmann::json> records = CrashRecords(local);
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0].at("outcome"), "dbghelp_unavailable");
  EXPECT_EQ(records[0].at("win32_error"), ERROR_MOD_NOT_FOUND);
  EXPECT_EQ(records[1].at("outcome"), "failed");
  EXPECT_EQ(records[1].at("thread"), GetCurrentThreadId());
}

TEST(HostCrashCapture, PreloadDbgHelpMapsTheSystemLibraryOnceAndWritesNoRecordWhenItSucceeds) {
  const std::wstring local = MakeTempDir(L"local");
  Prepared prepared(local, MakeTempDir(L"temp"));
  // Nothing in the game loads dbghelp.dll, so the host does it ahead of the crash (PreloadDbgHelp), never in Capture.
  ASSERT_EQ(GetModuleHandleW(L"dbghelp.dll"), nullptr);
  EXPECT_TRUE(prepared.capture.PreloadDbgHelp());
  const HMODULE loaded = GetModuleHandleW(L"dbghelp.dll");
  EXPECT_NE(loaded, nullptr);
  EXPECT_TRUE(prepared.capture.PreloadDbgHelp()) << "a second call must find it already loaded";
  EXPECT_EQ(GetModuleHandleW(L"dbghelp.dll"), loaded);
  wchar_t path[MAX_PATH] = {};
  ASSERT_NE(GetModuleFileNameW(loaded, path, MAX_PATH), 0u);
  wchar_t system_dir[MAX_PATH] = {};
  ASSERT_NE(GetSystemDirectoryW(system_dir, MAX_PATH), 0u);
  EXPECT_EQ(_wcsnicmp(path, system_dir, wcslen(system_dir)), 0) << "loaded from outside System32";
}

// Runs after the tests that need dbghelp.dll unloaded: it preloads it.
TEST(HostCrashCapture, WhenTheCrashDirectoryCannotBeUsedNoDumpOrTextIsWrittenAnywhereElse) {
  const std::wstring local = MakeTempDir(L"local");
  const std::wstring temp = MakeTempDir(L"temp");
  // "crashes" is a FILE under the nevr root: the crashes directory can be neither made nor written. The logs folder is fine.
  ASSERT_TRUE(nevr_root::EnsureDirectoryTree(local + L"\\nevr\\echovr\\logs"));
  HANDLE blocker = CreateFileW((local + L"\\nevr\\echovr\\crashes").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(blocker, INVALID_HANDLE_VALUE);
  CloseHandle(blocker);
  Prepared prepared(local, temp);
  ASSERT_TRUE(prepared.capture.PreloadDbgHelp());

  void* handler = AddVectoredExceptionHandler(1, CaptureVeh);
  ASSERT_NE(handler, nullptr);
  RaiseException(kProbeException, 0, 0, nullptr);
  RemoveVectoredExceptionHandler(handler);
  ASSERT_EQ(g_captured, 1L);
  EXCEPTION_POINTERS pointers{};
  pointers.ExceptionRecord = &g_captured_record;
  pointers.ContextRecord = &g_captured_context;
  const std::wstring pid_pattern = L"\\nevr-" + std::to_wstring(GetCurrentProcessId()) + L"-*";

  EXPECT_FALSE(prepared.capture.Capture(&pointers));

  EXPECT_EQ(FirstFileMatching(temp + L"\\nevr\\echovr\\crashes" + pid_pattern), L"") << "nothing in %TEMP%";
  EXPECT_EQ(GetFileAttributesW((temp + L"\\nevr").c_str()), INVALID_FILE_ATTRIBUTES);
  EXPECT_EQ(FirstFileMatching(ExecutableDir() + pid_pattern), L"") << "nothing beside the game";
  // The failure is in the main log, with the exception's code and address and the thread: that record is the crash text.
  const std::vector<nlohmann::json> records = CrashRecords(local);
  ASSERT_FALSE(records.empty());
  EXPECT_EQ(records.back().at("outcome"), "failed");
  EXPECT_EQ(records.back().at("code"), "0xe0000001");
  EXPECT_EQ(records.back().at("thread"), GetCurrentThreadId());
  EXPECT_TRUE(records.back().contains("address"));
}

TEST(HostCrashCapture, WritesAStructurallyValidMinidumpNamingTheThreadAndRecordsIt) {
  const std::wstring local = MakeTempDir(L"local");
  Prepared prepared(local, MakeTempDir(L"temp"));
  ASSERT_TRUE(prepared.capture.LocalReady());

  // dbghelp.dll is mapped by the host ahead of the crash, not by Capture.
  ASSERT_TRUE(prepared.capture.PreloadDbgHelp());

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

// ---- self-update's crash marker (docs/adr/0024-pc-self-update.md, section 6; F11, F21) -----------------------------

namespace {

bool PathExists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

void WriteSmallFile(const std::wstring& path, const char* text) {
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(file, INVALID_HANDLE_VALUE) << GetLastError();
  DWORD written = 0;
  ASSERT_TRUE(WriteFile(file, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr));
  CloseHandle(file);
}

// Every record of the host log with the event `event`.
std::vector<nlohmann::json> RecordsOf(const std::wstring& local, const char* event) {
  std::vector<nlohmann::json> out;
  const std::string text = ReadText(nevr_test::HostLogPath(local));
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find('\n', start);
    if (end == std::string::npos) break;
    const nlohmann::json record = nlohmann::json::parse(text.substr(start, end - start), nullptr, false);
    if (record.is_object() && record.value("event", "") == event) out.push_back(record);
    start = end + 1;
  }
  return out;
}

}  // namespace

// F11: the swapped release crashes during boot. The host's capture leaves update\crashed because update\pending.json
// exists at the moment of the crash, and the next start rolls back. The marker is one empty file.
TEST(HostCrashMarker, ACrashWhileAReleaseIsPendingLeavesTheMarker) {
  const std::wstring local = MakeTempDir(L"local");
  const std::wstring temp = MakeTempDir(L"temp");
  Prepared prepared(local, temp);
  const std::wstring update = local + L"\\nevr\\echovr\\update";
  ASSERT_NE(CreateDirectoryW((local + L"\\nevr").c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS, FALSE);
  CreateDirectoryW((local + L"\\nevr\\echovr").c_str(), nullptr);
  ASSERT_NE(CreateDirectoryW(update.c_str(), nullptr), FALSE) << GetLastError();
  WriteSmallFile(update + L"\\pending.json", "{\"to\":{\"version\":\"1.2.3\"}}");
  EXPECT_EQ(std::wstring(prepared.capture.LocalUpdateDir()), update);

  prepared.capture.Capture(nullptr);
  EXPECT_TRUE(PathExists(update + L"\\crashed")) << "update\\crashed marks a crash while the release was pending";
  EXPECT_TRUE(ReadText(update + L"\\crashed").empty()) << "an empty file";
  EXPECT_TRUE(PathExists(update + L"\\pending.json")) << "the capture changes nothing else";

  const std::vector<nlohmann::json> marks = RecordsOf(local, "crash_marker");
  ASSERT_EQ(marks.size(), 1u);
  EXPECT_EQ(marks[0].at("outcome"), "written");
  EXPECT_EQ(marks[0].at("tag"), "NEVR.HOST");
}

// F21: after the mark, UpdateConfirm has removed pending.json; a crash then is the release's own and leaves nothing.
TEST(HostCrashMarker, ACrashWithNoPendingReleaseLeavesNothingAndMakesNoUpdateFolder) {
  const std::wstring local = MakeTempDir(L"local");
  const std::wstring temp = MakeTempDir(L"temp");
  Prepared prepared(local, temp);
  const std::wstring update = local + L"\\nevr\\echovr\\update";
  prepared.capture.Capture(nullptr);
  EXPECT_FALSE(PathExists(update)) << "a crash capture creates no update folder";
  EXPECT_TRUE(RecordsOf(local, "crash_marker").empty());

  // The folder is there (a confirmed release left it), pending.json is not.
  CreateDirectoryW((local + L"\\nevr").c_str(), nullptr);
  CreateDirectoryW((local + L"\\nevr\\echovr").c_str(), nullptr);
  ASSERT_NE(CreateDirectoryW(update.c_str(), nullptr), FALSE) << GetLastError();
  WriteSmallFile(update + L"\\state.json", "{}");
  prepared.capture.Capture(nullptr);
  EXPECT_FALSE(PathExists(update + L"\\crashed")) << "no pending.json at the crash: no marker";
  EXPECT_TRUE(RecordsOf(local, "crash_marker").empty());
}

// A marker that cannot be made is a record with the error, not silence (a directory where the file goes).
TEST(HostCrashMarker, AMarkerThatCannotBeMadeIsRecordedWithTheError) {
  const std::wstring local = MakeTempDir(L"local");
  const std::wstring temp = MakeTempDir(L"temp");
  Prepared prepared(local, temp);
  const std::wstring update = local + L"\\nevr\\echovr\\update";
  CreateDirectoryW((local + L"\\nevr").c_str(), nullptr);
  CreateDirectoryW((local + L"\\nevr\\echovr").c_str(), nullptr);
  ASSERT_NE(CreateDirectoryW(update.c_str(), nullptr), FALSE);
  WriteSmallFile(update + L"\\pending.json", "{}");
  ASSERT_NE(CreateDirectoryW((update + L"\\crashed").c_str(), nullptr), FALSE);
  prepared.capture.Capture(nullptr);
  const std::vector<nlohmann::json> marks = RecordsOf(local, "crash_marker");
  ASSERT_EQ(marks.size(), 1u);
  EXPECT_EQ(marks[0].at("outcome"), "failed");
  EXPECT_GT(marks[0].value("win32_error", 0u), 0u);
}
