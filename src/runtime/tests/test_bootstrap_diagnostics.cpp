// Host bootstrap diagnostics (runtime/host/bootstrap_diagnostics.h/.cpp): the
// bounded JSONL recorder the stable BugSplat64.dll host uses for its attach,
// runtime-load and crash records. Every line a test reads back is parsed with
// nlohmann::json.

#include "runtime/host/bootstrap_diagnostics.h"

#include "runtime/host/path_buffer.h"
#include "runtime/host/preprocess_target.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

using nevr_host::AttachOutcome;
using nevr_host::AttachStep;
using nevr_host::BootstrapDiagnostics;
using nevr_host::DiagnosticDestination;

// A freshly created unique directory under %TEMP%.
std::wstring MakeTempDir(const wchar_t* tag) {
  wchar_t base[MAX_PATH] = {};
  const DWORD base_len = GetTempPathW(MAX_PATH, base);
  EXPECT_GT(base_len, 0u);
  EXPECT_LT(base_len, static_cast<DWORD>(MAX_PATH));

  static LONG counter = 0;
  const LONG sequence = InterlockedIncrement(&counter);
  wchar_t dir[MAX_PATH] = {};
  const int n = swprintf(dir, MAX_PATH, L"%ls\\nevr-diag-%ls-%lu-%llu-%ld", base, tag,
                         static_cast<unsigned long>(GetCurrentProcessId()),
                         static_cast<unsigned long long>(GetTickCount64()),
                         static_cast<long>(sequence));
  EXPECT_GT(n, 0);
  EXPECT_NE(CreateDirectoryW(dir, nullptr), FALSE);
  return std::wstring(dir);
}

std::string ReadAll(const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return {};
  std::string out;
  char buffer[512] = {};
  DWORD read = 0;
  while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) != FALSE && read != 0) {
    out.append(buffer, read);
  }
  CloseHandle(file);
  return out;
}

// Every line of the file, parsed. A line that does not parse fails the test.
std::vector<nlohmann::json> ReadRecords(const std::wstring& path) {
  std::vector<nlohmann::json> records;
  const std::string text = ReadAll(path);
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find('\n', start);
    EXPECT_NE(end, std::string::npos) << "record without a terminating newline";
    if (end == std::string::npos) break;
    records.push_back(nlohmann::json::parse(text.substr(start, end - start)));
    start = end + 1;
  }
  return records;
}

void MakeFile(const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(file, INVALID_HANDLE_VALUE);
  CloseHandle(file);
}

const std::wstring kLogsFile = L"\\nevr\\echovr\\logs\\nevr-bootstrap.jsonl";

}  // namespace

TEST(BootstrapDiagnostics, PrepareBuildsTheThreeDirectoriesFromTheEnvironmentAndTheHostModule) {
  const std::wstring local = MakeTempDir(L"local");
  const std::wstring temp = MakeTempDir(L"temp");
  SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str());
  SetEnvironmentVariableW(L"TEMP", temp.c_str());

  BootstrapDiagnostics diagnostics;
  diagnostics.Prepare(GetModuleHandleW(nullptr));
  ASSERT_TRUE(diagnostics.PrimaryReady());
  ASSERT_TRUE(diagnostics.FallbackReady());
  ASSERT_TRUE(diagnostics.HostDirReady());
  EXPECT_EQ(std::wstring(diagnostics.PrimaryDir()), local + L"\\nevr\\echovr\\logs");
  EXPECT_EQ(std::wstring(diagnostics.FallbackDir()), temp + L"\\nevr\\echovr\\logs");
  EXPECT_EQ(std::wstring(diagnostics.HostDir()).back(), L'\\');
}

TEST(BootstrapDiagnostics, EmitWritesOneParseableStampedLineToThePrimaryDirectory) {
  const std::wstring local = MakeTempDir(L"local");
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), MakeTempDir(L"temp").c_str(), nullptr);

  const nlohmann::json record = {{"event", "runtime_load"}, {"win32_error", 2}};
  EXPECT_EQ(diagnostics.Emit(record), DiagnosticDestination::kPrimary);

  const std::vector<nlohmann::json> records = ReadRecords(local + kLogsFile);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].at("event"), "runtime_load");
  EXPECT_EQ(records[0].at("win32_error"), 2);
  EXPECT_EQ(records[0].at("tag"), "NEVR.HOST");
  EXPECT_EQ(records[0].at("pid"), GetCurrentProcessId());
  EXPECT_EQ(records[0].at("ts").get<std::string>().size(), 24u);
}

TEST(BootstrapDiagnostics, EmitFallsBackToTempWhenThePrimaryIsUnwritable) {
  const std::wstring local = MakeTempDir(L"local");
  const std::wstring temp = MakeTempDir(L"temp");
  // Block the primary: "nevr" is a FILE under %LOCALAPPDATA%, so the logs directory
  // beneath it can be neither created nor written.
  MakeFile(local + L"\\nevr");

  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), temp.c_str(), nullptr);
  EXPECT_EQ(diagnostics.Emit({{"event", "runtime_load"}, {"win32_error", 5}}),
            DiagnosticDestination::kFallback);

  const std::vector<nlohmann::json> records = ReadRecords(temp + kLogsFile);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].at("win32_error"), 5);
}

TEST(BootstrapDiagnostics, EmitFallsBackToTheHostDirectoryWhenBothEnvironmentPathsAreBlocked) {
  const std::wstring local = MakeTempDir(L"local");
  const std::wstring temp = MakeTempDir(L"temp");
  const std::wstring host = MakeTempDir(L"host");
  MakeFile(local + L"\\nevr");
  MakeFile(temp + L"\\nevr");

  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), temp.c_str(), host.c_str());
  EXPECT_EQ(diagnostics.Emit({{"event", "runtime_load"}}), DiagnosticDestination::kHostDirectory);

  const std::vector<nlohmann::json> records = ReadRecords(host + L"\\nevr-bootstrap.jsonl");
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].at("event"), "runtime_load");
}

TEST(BootstrapDiagnostics, EmitReportsDroppedWhenNoDestinationIsAvailable) {
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(L"", L"", L"");
  EXPECT_FALSE(diagnostics.PrimaryReady());
  EXPECT_FALSE(diagnostics.FallbackReady());
  EXPECT_FALSE(diagnostics.HostDirReady());
  EXPECT_EQ(diagnostics.Emit({{"event", "runtime_load"}}), DiagnosticDestination::kDropped);
}

TEST(BootstrapDiagnostics, AnOversizedRecordIsReplacedByAMarkerNotTruncated) {
  const std::wstring local = MakeTempDir(L"local");
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
  nlohmann::json record;
  record["event"] = "runtime_load";
  record["huge"] = std::string(BootstrapDiagnostics::kMaxRecordBytes, 'x');
  EXPECT_EQ(diagnostics.Emit(record), DiagnosticDestination::kPrimary);

  const std::vector<nlohmann::json> records = ReadRecords(local + kLogsFile);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].at("event"), "runtime_load");
  EXPECT_EQ(records[0].at("error"), "record_too_large");
  EXPECT_FALSE(records[0].contains("huge"));
}

TEST(BootstrapDiagnostics, InvalidUtf8InARecordIsReplacedNotThrown) {
  const std::wstring local = MakeTempDir(L"local");
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
  nlohmann::json record;
  record["event"] = "runtime_load";
  record["path"] = std::string("C:\\bad\xff" "name");
  EXPECT_EQ(diagnostics.Emit(record), DiagnosticDestination::kPrimary);

  const std::vector<nlohmann::json> records = ReadRecords(local + kLogsFile);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].at("path"), "C:\\bad\xEF\xBF\xBD" "name");
}

TEST(BootstrapDiagnostics, AppendLineWritesAFormattedLineWithoutTheHeap) {
  const std::wstring local = MakeTempDir(L"local");
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
  const char line[] = "{\"event\":\"fixed\"}\n";
  EXPECT_EQ(diagnostics.AppendLine(line, sizeof(line) - 1), DiagnosticDestination::kPrimary);
  const std::vector<nlohmann::json> records = ReadRecords(local + kLogsFile);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].at("event"), "fixed");
}

TEST(BootstrapDiagnostics, TheAttachJournalIsWrittenInOrderWithTheDecisionsThatMattered) {
  const std::wstring local = MakeTempDir(L"local");
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);

  unsigned char actual[nevr_host::kPreprocessPrologueSize];
  for (std::size_t at = 0; at < sizeof(actual); ++at) actual[at] = nevr_host::kPreprocessPrologue[at];
  actual[7] = 0xb2;  // the lea displacement moved: a different build of the game
  diagnostics.RecordAttach(AttachStep::kImageCheck, AttachOutcome::kAccepted);
  diagnostics.RecordAttach(AttachStep::kPrologueCheck, AttachOutcome::kRejected, 0, actual);
  diagnostics.RecordAttach(AttachStep::kHookInstall, AttachOutcome::kFailed, 9);
  EXPECT_TRUE(diagnostics.AttachDegraded());

  // Nothing is written until the flush: recording is for the loader-lock path.
  EXPECT_TRUE(ReadAll(local + kLogsFile).empty());
  diagnostics.FlushAttachJournal();

  const std::vector<nlohmann::json> records = ReadRecords(local + kLogsFile);
  ASSERT_EQ(records.size(), 3u);
  EXPECT_EQ(records[0].at("event"), "host_attach");
  EXPECT_EQ(records[0].at("step"), "image_check");
  EXPECT_EQ(records[0].at("outcome"), "accepted");
  EXPECT_FALSE(records[0].contains("degraded_action"));

  EXPECT_EQ(records[1].at("step"), "prologue_check");
  EXPECT_EQ(records[1].at("outcome"), "mismatch");
  EXPECT_EQ(records[1].at("degraded_action"), "run_original_only");
  EXPECT_EQ(records[1].at("rva"), "0x116720");
  EXPECT_EQ(records[1].at("expected_prologue"), "405557488d6c24b14881eca8000000488bf9");
  EXPECT_EQ(records[1].at("actual_prologue"), "405557488d6c24b24881eca8000000488bf9");

  EXPECT_EQ(records[2].at("step"), "hook_install");
  EXPECT_EQ(records[2].at("outcome"), "failed");
  EXPECT_EQ(records[2].at("mh_status"), 9);
}

TEST(BootstrapDiagnostics, AFlushWritesEachStepOnceAndPicksUpLaterSteps) {
  const std::wstring local = MakeTempDir(L"local");
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);

  diagnostics.FlushAttachJournal();  // nothing recorded: no line
  EXPECT_TRUE(ReadAll(local + kLogsFile).empty());
  EXPECT_FALSE(diagnostics.AttachDegraded());

  diagnostics.RecordAttach(AttachStep::kImageCheck, AttachOutcome::kAccepted);
  diagnostics.FlushAttachJournal();
  diagnostics.FlushAttachJournal();  // idempotent
  EXPECT_EQ(ReadRecords(local + kLogsFile).size(), 1u);
  EXPECT_EQ(diagnostics.JournalFlushed(), 1u);
  EXPECT_FALSE(diagnostics.AttachDegraded());

  diagnostics.RecordAttach(AttachStep::kHookInstall, AttachOutcome::kAccepted);
  diagnostics.FlushAttachJournal();
  const std::vector<nlohmann::json> records = ReadRecords(local + kLogsFile);
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[1].at("step"), "hook_install");
  EXPECT_EQ(records[1].at("outcome"), "enabled");
}

TEST(BootstrapDiagnostics, TheJournalKeepsTheFirstEntriesWhenItOverflows) {
  const std::wstring local = MakeTempDir(L"local");
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
  for (std::size_t at = 0; at < BootstrapDiagnostics::kJournalCapacity + 3; ++at) {
    diagnostics.RecordAttach(AttachStep::kImageCheck, AttachOutcome::kAccepted);
  }
  diagnostics.FlushAttachJournal();
  EXPECT_EQ(ReadRecords(local + kLogsFile).size(), BootstrapDiagnostics::kJournalCapacity);
}

TEST(BootstrapDiagnostics, AttachDegradedIsTrueForAnyRejectionOrFailureAndFalseOtherwise) {
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(L"", L"", L"");
  EXPECT_FALSE(diagnostics.AttachDegraded());
  diagnostics.RecordAttach(AttachStep::kImageCheck, AttachOutcome::kAccepted);
  EXPECT_FALSE(diagnostics.AttachDegraded());

  BootstrapDiagnostics rejected;
  rejected.PrepareForTest(L"", L"", L"");
  rejected.RecordAttach(AttachStep::kImageCheck, AttachOutcome::kAccepted);
  rejected.RecordAttach(AttachStep::kPrologueCheck, AttachOutcome::kRejected);
  EXPECT_TRUE(rejected.AttachDegraded());

  BootstrapDiagnostics failed;
  failed.PrepareForTest(L"", L"", L"");
  failed.RecordAttach(AttachStep::kHookInstall, AttachOutcome::kFailed, 7);
  EXPECT_TRUE(failed.AttachDegraded());
}

TEST(HostPathBuffer, AppendPathJoinsWithExactlyOneSeparatorAndRefusesTruncation) {
  wchar_t out[64] = {};
  ASSERT_TRUE(nevr_host::AppendPath(out, 64, L"C:\\base", L"leaf"));
  EXPECT_EQ(std::wstring(out), L"C:\\base\\leaf");
  ASSERT_TRUE(nevr_host::AppendPath(out, 64, L"C:\\base\\", L"leaf"));
  EXPECT_EQ(std::wstring(out), L"C:\\base\\leaf");
  EXPECT_FALSE(nevr_host::AppendPath(out, 64, L"", L"leaf"));
  wchar_t small[10] = {};
  EXPECT_FALSE(nevr_host::AppendPath(small, 10, L"C:\\base", L"leaf"));   // needs 13
  wchar_t exact[13] = {};
  EXPECT_TRUE(nevr_host::AppendPath(exact, 13, L"C:\\base", L"leaf"));  // fits with no spare
}

TEST(HostPathBuffer, CopyEnvYieldsAnEmptyBufferForAnUnsetOrTooLongVariable) {
  wchar_t out[8] = {L'x'};
  SetEnvironmentVariableW(L"NEVR_HOST_TEST_VALUE", L"short");
  EXPECT_TRUE(nevr_host::CopyEnv(out, 8, L"NEVR_HOST_TEST_VALUE"));
  EXPECT_EQ(std::wstring(out), L"short");
  EXPECT_FALSE(nevr_host::CopyEnv(out, 4, L"NEVR_HOST_TEST_VALUE"));  // truncation
  EXPECT_EQ(out[0], L'\0');
  SetEnvironmentVariableW(L"NEVR_HOST_TEST_VALUE", nullptr);
  EXPECT_FALSE(nevr_host::CopyEnv(out, 8, L"NEVR_HOST_TEST_VALUE"));
  EXPECT_EQ(out[0], L'\0');
}

TEST(HostPathBuffer, CreateDirectoriesMakesEveryMissingLevelAndRestoresThePath) {
  const std::wstring root = MakeTempDir(L"mkdir");
  std::wstring deep = root + L"\\a\\b\\c";
  std::vector<wchar_t> buffer(deep.begin(), deep.end());
  buffer.push_back(L'\0');
  ASSERT_TRUE(nevr_host::CreateDirectories(buffer.data()));
  EXPECT_EQ(std::wstring(buffer.data()), deep);  // separators put back
  EXPECT_NE(GetFileAttributesW(deep.c_str()), INVALID_FILE_ATTRIBUTES);
  wchar_t empty[1] = {L'\0'};
  EXPECT_FALSE(nevr_host::CreateDirectories(empty));
  EXPECT_FALSE(nevr_host::CreateDirectories(nullptr));
}

TEST(HostPathBuffer, DirOfModuleEndsInABackslash) {
  wchar_t dir[MAX_PATH] = {};
  ASSERT_TRUE(nevr_host::DirOfModule(dir, MAX_PATH, GetModuleHandleW(nullptr)));
  const std::wstring text(dir);
  EXPECT_EQ(text.back(), L'\\');
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  EXPECT_EQ(std::wstring(exe).substr(0, text.size()), text);
  wchar_t tiny[4] = {};
  EXPECT_FALSE(nevr_host::DirOfModule(tiny, 4, GetModuleHandleW(nullptr)));
  EXPECT_EQ(tiny[0], L'\0');
}
