// Host bootstrap diagnostics (runtime/host/bootstrap_diagnostics.h/.cpp): the
// bounded JSONL recorder the stable BugSplat64.dll host uses for its attach,
// runtime-load and crash records. Every line a test reads back is parsed with
// nlohmann::json.

#include "runtime/host/bootstrap_diagnostics.h"

#include "runtime/host/host_paths.h"
#include "runtime/host/path_buffer.h"
#include "runtime/host/game_targets.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <regex>
#include <set>
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

  unsigned char actual[nevr_host::kGameMainWrapperPrologueSize];
  for (std::size_t at = 0; at < sizeof(actual); ++at) actual[at] = nevr_host::kGameMainWrapperPrologue[at];
  actual[7] = 0x25;  // the SIB byte moved: a different build of the game
  diagnostics.RecordAttach(AttachStep::kImageCheck, AttachOutcome::kAccepted);
  diagnostics.RecordAttach(AttachStep::kPrologueCheck, AttachOutcome::kRejected, 0, actual);
  diagnostics.RecordAttach(AttachStep::kHookInstall, AttachOutcome::kFailed, 9, actual,
                           nevr_host::HookStage::kCreate);
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
  EXPECT_FALSE(records[0].contains("rva"));

  EXPECT_EQ(records[1].at("step"), "prologue_check");
  EXPECT_EQ(records[1].at("outcome"), "mismatch");
  EXPECT_EQ(records[1].at("degraded_action"), "run_original_only");
  EXPECT_EQ(records[1].at("rva"), "0xcd510");
  EXPECT_EQ(records[1].at("expected_prologue"), "4883ec3848c744242000000000");
  EXPECT_EQ(records[1].at("actual_prologue"), "4883ec3848c744252000000000");

  // The hook install carries the same target facts, the MinHook stage that failed and its status.
  EXPECT_EQ(records[2].at("step"), "hook_install");
  EXPECT_EQ(records[2].at("outcome"), "failed");
  EXPECT_EQ(records[2].at("stage"), "create");
  EXPECT_EQ(records[2].at("mh_status"), 9);
  EXPECT_EQ(records[2].at("rva"), "0xcd510");
  EXPECT_EQ(records[2].at("expected_prologue"), "4883ec3848c744242000000000");
  EXPECT_EQ(records[2].at("actual_prologue"), "4883ec3848c744252000000000");
}

TEST(BootstrapDiagnostics, AnInstalledHookIsOneRecordNamingTheEnableStageAndStatusOk) {
  const std::wstring local = MakeTempDir(L"local");
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
  diagnostics.RecordAttach(AttachStep::kPrologueCheck, AttachOutcome::kAccepted, 0,
                           nevr_host::kGameMainWrapperPrologue);
  diagnostics.RecordAttach(AttachStep::kHookInstall, AttachOutcome::kAccepted, 0,
                           nevr_host::kGameMainWrapperPrologue, nevr_host::HookStage::kEnable);
  EXPECT_FALSE(diagnostics.AttachDegraded());
  diagnostics.FlushAttachJournal();

  const std::vector<nlohmann::json> records = ReadRecords(local + kLogsFile);
  ASSERT_EQ(records.size(), 2u);
  int installs = 0;
  for (const nlohmann::json& record : records) installs += record.at("step") == "hook_install" ? 1 : 0;
  EXPECT_EQ(installs, 1);
  EXPECT_EQ(records[1].at("outcome"), "enabled");
  EXPECT_EQ(records[1].at("stage"), "enable");
  EXPECT_EQ(records[1].at("mh_status"), 0);
  EXPECT_EQ(records[1].at("rva"), "0xcd510");
  EXPECT_EQ(records[1].at("actual_prologue"), records[1].at("expected_prologue"));
  EXPECT_EQ(records[1].at("expected_prologue"), "4883ec3848c744242000000000");
}

TEST(BootstrapDiagnostics, EachMinHookStageHasItsOwnName) {
  const std::wstring local = MakeTempDir(L"local");
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
  diagnostics.RecordAttach(AttachStep::kHookInstall, AttachOutcome::kFailed, 1, nullptr,
                           nevr_host::HookStage::kInitialize);
  diagnostics.RecordAttach(AttachStep::kHookInstall, AttachOutcome::kFailed, 2, nullptr,
                           nevr_host::HookStage::kCreate);
  diagnostics.RecordAttach(AttachStep::kHookInstall, AttachOutcome::kFailed, 3, nullptr,
                           nevr_host::HookStage::kEnable);
  diagnostics.FlushAttachJournal();
  const std::vector<nlohmann::json> records = ReadRecords(local + kLogsFile);
  ASSERT_EQ(records.size(), 3u);
  EXPECT_EQ(records[0].at("stage"), "initialize");
  EXPECT_EQ(records[1].at("stage"), "create");
  EXPECT_EQ(records[2].at("stage"), "enable");
  EXPECT_FALSE(records[0].contains("actual_prologue"));
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

TEST(BootstrapDiagnostics, TheJournalKeepsTheFirstEntriesAndCountsWhatItDropped) {
  const std::wstring local = MakeTempDir(L"local");
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
  for (std::size_t at = 0; at < BootstrapDiagnostics::kJournalCapacity + 3; ++at) {
    diagnostics.RecordAttach(AttachStep::kImageCheck, AttachOutcome::kAccepted);
  }
  EXPECT_TRUE(diagnostics.AttachDegraded());  // dropped decisions are a degraded attach
  diagnostics.FlushAttachJournal();

  // The kept entries, then one record that says three were dropped.
  std::vector<nlohmann::json> records = ReadRecords(local + kLogsFile);
  ASSERT_EQ(records.size(), BootstrapDiagnostics::kJournalCapacity + 1);
  EXPECT_EQ(records.back().at("event"), "host_attach");
  EXPECT_EQ(records.back().at("step"), "journal");
  EXPECT_EQ(records.back().at("outcome"), "overflow");
  EXPECT_EQ(records.back().at("dropped"), 3);

  // Nothing new dropped: nothing new written. A later drop is reported with the new total.
  diagnostics.FlushAttachJournal();
  EXPECT_EQ(ReadRecords(local + kLogsFile).size(), BootstrapDiagnostics::kJournalCapacity + 1);
  diagnostics.RecordAttach(AttachStep::kImageCheck, AttachOutcome::kAccepted);
  diagnostics.FlushAttachJournal();
  records = ReadRecords(local + kLogsFile);
  ASSERT_EQ(records.size(), BootstrapDiagnostics::kJournalCapacity + 2);
  EXPECT_EQ(records.back().at("dropped"), 4);
}

// Two threads flushing while a third records: every entry is written exactly once. A flush that
// moved the written mark backwards after a racing flush would write entries twice.
TEST(BootstrapDiagnostics, ConcurrentFlushesWriteEachEntryExactlyOnce) {
  constexpr int kFlushers = 4;
  for (int round = 0; round < 150; ++round) {
    const std::wstring local = MakeTempDir(L"local");
    BootstrapDiagnostics diagnostics;
    diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
    struct Shared {
      BootstrapDiagnostics* diagnostics;
      volatile LONG stop;
    } shared{&diagnostics, 0};
    auto flusher = [](void* raw) -> DWORD {
      auto* state = static_cast<Shared*>(raw);
      while (InterlockedCompareExchange(&state->stop, 0, 0) == 0) state->diagnostics->FlushAttachJournal();
      return 0;
    };
    HANDLE threads[kFlushers] = {};
    for (HANDLE& thread : threads) {
      thread = CreateThread(nullptr, 0, flusher, &shared, 0, nullptr);
      ASSERT_NE(thread, nullptr);
    }
    for (std::size_t at = 0; at < BootstrapDiagnostics::kJournalCapacity; ++at) {
      diagnostics.RecordAttach(AttachStep::kHookInstall, AttachOutcome::kFailed,
                               static_cast<std::uint32_t>(at), nullptr, nevr_host::HookStage::kCreate);
      SwitchToThread();
    }
    InterlockedExchange(&shared.stop, 1);
    WaitForMultipleObjects(kFlushers, threads, TRUE, 20000);
    for (HANDLE thread : threads) CloseHandle(thread);
    diagnostics.FlushAttachJournal();
    // Counted in memory, not read back from the file: concurrent appenders to one file can lose a
    // line under Wine, which says nothing about whether an entry was claimed once.
    EXPECT_EQ(diagnostics.JournalClaimed(), BootstrapDiagnostics::kJournalCapacity) << "round " << round;
    EXPECT_EQ(diagnostics.JournalFlushed(), BootstrapDiagnostics::kJournalCapacity) << "round " << round;
  }
}

// The decisions that are not about the target: the install wait, the detour defect, a second
// NEVR_SetGameModule, an unread hook record. Each is a record that says what happened and what the
// host did next.
TEST(BootstrapDiagnostics, TheWaitDefectAndRefusalRecordsSayWhatHappenedAndWhatHappensNext) {
  const std::wstring local = MakeTempDir(L"local");
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
  using nevr_host::AttachNote;
  diagnostics.RecordAttach(AttachStep::kInstallWait, AttachOutcome::kAccepted, 0, nullptr,
                           nevr_host::HookStage::kNone, AttachNote::kInstallWaitSignalled);
  EXPECT_FALSE(diagnostics.AttachDegraded());
  diagnostics.RecordAttach(AttachStep::kInstallWait, AttachOutcome::kFailed, 0, nullptr,
                           nevr_host::HookStage::kNone, AttachNote::kInstallWaitUnavailable);
  diagnostics.RecordAttach(AttachStep::kInstallWait, AttachOutcome::kFailed, 0, nullptr,
                           nevr_host::HookStage::kNone, AttachNote::kInstallWaitTimeout);
  diagnostics.RecordAttach(AttachStep::kDetourEntry, AttachOutcome::kFailed, 0, nullptr,
                           nevr_host::HookStage::kNone, AttachNote::kDetourWithoutHook);
  diagnostics.RecordAttach(AttachStep::kSetGameModule, AttachOutcome::kRejected, 0, nullptr,
                           nevr_host::HookStage::kNone, AttachNote::kDuplicateSetGameModule);
  diagnostics.FlushAttachJournal();
  const std::vector<nlohmann::json> records = ReadRecords(local + kLogsFile);
  ASSERT_EQ(records.size(), 5u);
  EXPECT_EQ(records[0].at("step"), "install_wait");
  EXPECT_EQ(records[0].at("outcome"), "signalled");
  EXPECT_FALSE(records[0].contains("next"));
  EXPECT_EQ(records[1].at("outcome"), "unavailable");
  EXPECT_EQ(records[1].at("next"), "load_proceeds_unsynchronized_and_the_journal_is_flushed_again_after_load");
  EXPECT_EQ(records[2].at("outcome"), "timeout");
  EXPECT_EQ(records[2].at("next"), "load_proceeds_unsynchronized_and_the_journal_is_flushed_again_after_load");
  EXPECT_EQ(records[3].at("step"), "detour_entry");
  EXPECT_EQ(records[3].at("outcome"), "defect");
  EXPECT_EQ(records[3].at("next"), "returned_0_without_calling_the_original");
  EXPECT_EQ(records[4].at("step"), "set_game_module");
  EXPECT_EQ(records[4].at("outcome"), "refused");
  EXPECT_EQ(records[4].at("reason"), "already_called");
  EXPECT_EQ(records[4].at("next"), "ignored");
  // degraded_action says the host runs the original only: true of a rejected image or prologue or a
  // failed install, not of a wait that timed out, a defect report or a refused second call.
  for (const nlohmann::json& record : records) EXPECT_FALSE(record.contains("degraded_action")) << record.dump();
}

TEST(BootstrapDiagnostics, ARejectedTargetStepStillSaysTheHostRunsTheOriginalOnly) {
  const std::wstring local = MakeTempDir(L"local");
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
  diagnostics.RecordAttach(AttachStep::kImageCheck, AttachOutcome::kRejected);
  diagnostics.RecordAttach(AttachStep::kPrologueCheck, AttachOutcome::kRejected);
  diagnostics.RecordAttach(AttachStep::kHookInstall, AttachOutcome::kFailed, 9, nullptr,
                           nevr_host::HookStage::kCreate);
  diagnostics.FlushAttachJournal();
  const std::vector<nlohmann::json> records = ReadRecords(local + kLogsFile);
  ASSERT_EQ(records.size(), 3u);
  for (const nlohmann::json& record : records) EXPECT_EQ(record.at("degraded_action"), "run_original_only");
}

TEST(BootstrapDiagnostics, AnUnreadHookRecordIsOnTheInstallRecordAndDegradesTheAttach) {
  const std::wstring local = MakeTempDir(L"local");
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
  diagnostics.RecordAttach(AttachStep::kHookInstall, AttachOutcome::kAccepted, 0,
                           nevr_host::kGameMainWrapperPrologue, nevr_host::HookStage::kEnable,
                           nevr_host::AttachNote::kHookBytesUnread);
  EXPECT_TRUE(diagnostics.AttachDegraded());
  diagnostics.FlushAttachJournal();
  const std::vector<nlohmann::json> records = ReadRecords(local + kLogsFile);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].at("outcome"), "enabled");
  EXPECT_EQ(records[0].at("hook_bytes"), "unread");
  EXPECT_EQ(records[0].at("next"), "context_marks_the_hook_bytes_invalid");
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

// --- the log file under concurrent appenders ---------------------------------------------------

namespace {

struct AppendJob {
  BootstrapDiagnostics* diagnostics;
  int thread;
  int lines;
  const char* prefix;
  std::atomic<int>* dropped;
};

DWORD WINAPI AppendLines(void* raw) {
  const auto* job = static_cast<const AppendJob*>(raw);
  for (int at = 0; at < job->lines; ++at) {
    char line[48];
    const int length = std::snprintf(line, sizeof(line), "%s-T%02d-L%05d\n", job->prefix, job->thread, at);
    const nevr_host::DiagnosticDestination where =
        job->diagnostics->AppendLine(line, static_cast<std::size_t>(length));
    if (where == nevr_host::DiagnosticDestination::kDropped) job->dropped->fetch_add(1);
  }
  return 0;
}

// Runs `threads` appenders of `lines` lines each. `dropped`, when given, counts the appends that reported
// kDropped (no destination took the line).
void AppendConcurrently(BootstrapDiagnostics& diagnostics, int threads, int lines, const char* prefix,
                        std::atomic<int>* dropped = nullptr) {
  std::atomic<int> none{0};
  std::atomic<int>* counter = dropped != nullptr ? dropped : &none;
  std::vector<AppendJob> jobs(static_cast<std::size_t>(threads));
  std::vector<HANDLE> handles;
  for (int t = 0; t < threads; ++t) {
    jobs[static_cast<std::size_t>(t)] = AppendJob{&diagnostics, t, lines, prefix, counter};
    handles.push_back(CreateThread(nullptr, 0, &AppendLines, &jobs[static_cast<std::size_t>(t)], 0, nullptr));
  }
  // A wait that times out would read a file that is still being written: the result is checked, not ignored.
  EXPECT_EQ(WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), TRUE, 120000), WAIT_OBJECT_0);
  for (HANDLE handle : handles) CloseHandle(handle);
}

struct LineCount {
  std::size_t lines = 0;
  std::size_t distinct = 0;
};

LineCount CountLines(const std::string& text) {
  LineCount count;
  std::set<std::string> distinct;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find('\n', start);
    if (end == std::string::npos) break;
    ++count.lines;
    distinct.insert(text.substr(start, end - start));
    start = end + 1;
  }
  count.distinct = distinct.size();
  return count;
}

// The lines of `text` that start with `prefix`. A test counts its own appends: once the log lock has degraded the
// file also holds one host_log_lock record, and that record is not a line the test appended (#1219).
LineCount CountLinesWithPrefix(const std::string& text, char prefix) {
  LineCount count;
  std::set<std::string> distinct;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find('\n', start);
    if (end == std::string::npos) break;
    if (end > start && text[start] == prefix) {
      ++count.lines;
      distinct.insert(text.substr(start, end - start));
    }
    start = end + 1;
  }
  count.distinct = distinct.size();
  return count;
}

}  // namespace

// The lock records the file holds and the process's lock state, printed when a line is missing (#840): a short
// file with no host_log_lock record and no unlocked writes points at the append itself, not at the mutex.
std::string LockDiagnosis(const std::string& text) {
  std::string records;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find('\n', start);
    if (end == std::string::npos) break;
    const std::string line = text.substr(start, end - start);
    if (line.find("host_log_lock") != std::string::npos) records += line + "\n";
    start = end + 1;
  }
  const bool unreliable = BootstrapDiagnostics::LogLockUnreliableForTest();
  std::string summary = std::string("lock_unreliable=") + (unreliable ? "true" : "false") +
                        " unlocked_writes=" + std::to_string(BootstrapDiagnostics::UnlockedWritesForTest()) +
                        "\nhost_log_lock records in the file:\n";
  return summary + (records.empty() ? std::string("(none)\n") : records);
}

// The lines of `text` that are neither an appended `H-T<tt>-L<nnnnn>` line nor a host_log_lock record, with their
// line numbers: a line split, merged or garbled by an unlocked write shows here, where the counts only show its count (#1304).
std::string StrayLines(const std::string& text) {
  static const std::regex kAppended("H-T[0-9]{2}-L[0-9]{5}");
  std::string strays;
  std::size_t number = 0;
  std::size_t start = 0;
  while (start < text.size()) {
    const std::size_t end = text.find('\n', start);
    if (end == std::string::npos) break;
    ++number;
    const std::string line = text.substr(start, end - start);
    if (!std::regex_match(line, kAppended) && line.find("host_log_lock") == std::string::npos) {
      strays += "line " + std::to_string(number) + ": " + line + "\n";
    }
    start = end + 1;
  }
  return strays.empty() ? std::string("stray lines: (none)\n") : "stray lines:\n" + strays;
}

TEST(BootstrapDiagnostics, ConcurrentAppendersToOneFileLoseNoLine) {
  for (const int threads : {1, 2, 4, 8}) {
    const std::wstring local = MakeTempDir(L"local");
    BootstrapDiagnostics diagnostics;
    diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
    constexpr int kLines = 300;
    std::atomic<int> dropped{0};
    AppendConcurrently(diagnostics, threads, kLines, "A", &dropped);
    const std::string text = ReadAll(local + kLogsFile);
    const LineCount count = CountLinesWithPrefix(text, 'A');
    const std::size_t expected = static_cast<std::size_t>(threads * kLines);
    // The record of a short file: the lock state at the end, the dropped appends, and the lock records.
    EXPECT_EQ(count.lines, expected) << threads << " threads, dropped=" << dropped.load() << "\n"
                                     << LockDiagnosis(text);
    EXPECT_EQ(count.distinct, expected) << threads << " threads, dropped=" << dropped.load() << "\n"
                                        << LockDiagnosis(text);
  }
}

// The same test as a helper for a second process: it appends only when the parent asks through
// the environment, so the normal run of this binary skips it.
TEST(BootstrapDiagnostics, AppendHelperForTheTwoProcessTest) {
  wchar_t dir[MAX_PATH] = {};
  wchar_t id[16] = {};
  if (GetEnvironmentVariableW(L"NEVR_TEST_APPEND_DIR", dir, MAX_PATH) == 0) GTEST_SKIP() << "helper process only";
  GetEnvironmentVariableW(L"NEVR_TEST_APPEND_ID", id, 16);
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(dir, nullptr, nullptr);
  char prefix[16] = {};
  std::snprintf(prefix, sizeof(prefix), "P%ls", id);
  AppendConcurrently(diagnostics, 4, 100, prefix);
}

TEST(BootstrapDiagnostics, TwoProcessesAppendingToOneFileLoseNoLine) {
  const std::wstring local = MakeTempDir(L"local");
  SetEnvironmentVariableW(L"NEVR_TEST_APPEND_DIR", local.c_str());
  wchar_t self[MAX_PATH] = {};
  ASSERT_GT(GetModuleFileNameW(nullptr, self, MAX_PATH), 0u);
  HANDLE children[2] = {};
  for (int child = 0; child < 2; ++child) {
    SetEnvironmentVariableW(L"NEVR_TEST_APPEND_ID", child == 0 ? L"1" : L"2");
    std::wstring command = L"\"" + std::wstring(self) +
                           L"\" --gtest_filter=BootstrapDiagnostics.AppendHelperForTheTwoProcessTest";
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    ASSERT_NE(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                             &process),
              FALSE);
    CloseHandle(process.hThread);
    children[child] = process.hProcess;
  }
  EXPECT_EQ(WaitForMultipleObjects(2, children, TRUE, 120000), WAIT_OBJECT_0);
  for (HANDLE child : children) CloseHandle(child);
  SetEnvironmentVariableW(L"NEVR_TEST_APPEND_DIR", nullptr);
  SetEnvironmentVariableW(L"NEVR_TEST_APPEND_ID", nullptr);
  const LineCount count = CountLines(ReadAll(local + kLogsFile));
  EXPECT_EQ(count.lines, 800u);
  EXPECT_EQ(count.distinct, 800u);
}

// --- the log lock, and a flush that must not read an entry before it is written -----------------

namespace {

std::wstring g_holder_mutex_name;

DWORD WINAPI HoldLogLock(void* raw) {
  HANDLE* handles = static_cast<HANDLE*>(raw);  // [0] ready, [1] release
  HANDLE mutex = CreateMutexW(nullptr, FALSE, g_holder_mutex_name.c_str());
  WaitForSingleObject(mutex, 5000);
  SetEvent(handles[0]);
  WaitForSingleObject(handles[1], 10000);
  ReleaseMutex(mutex);
  CloseHandle(mutex);
  return 0;
}

// A mutex name no other test process shares, so two runs under one wineserver cannot hold each other's lock.
std::wstring UniqueMutexName(const wchar_t* tag) {
  wchar_t name[128] = {};
  swprintf(name, 128, L"Local\\nevr-test-log-%ls-%lu-%llu", tag, static_cast<unsigned long>(GetCurrentProcessId()),
           static_cast<unsigned long long>(GetTickCount64()));
  return std::wstring(name);
}

// The log mutex of this process is named `name` for the scope; the old one is closed first and the
// production name is restored (and the test's mutex closed) at the end.
struct ScopedLogMutexName {
  explicit ScopedLogMutexName(const std::wstring& name) {
    BootstrapDiagnostics::ResetLogLockForTest();
    BootstrapDiagnostics::SetLogMutexNameForTest(name.c_str());
    g_holder_mutex_name = name;
  }
  ~ScopedLogMutexName() {
    BootstrapDiagnostics::ResetLogLockForTest();
    BootstrapDiagnostics::SetLogMutexNameForTest(nullptr);
  }
};

// Whether a mutex called `name` exists in this session (opened, not created).
bool MutexExists(const wchar_t* name) {
  HANDLE found = OpenMutexW(SYNCHRONIZE, FALSE, name);
  if (found == nullptr) return false;
  CloseHandle(found);
  return true;
}

}  // namespace

namespace {

struct HoldFor {
  HANDLE ready;
  DWORD milliseconds;
};

// Takes the log mutex, says so, and keeps it for the given time: longer than an append's bounded wait.
DWORD WINAPI HoldLogLockFor(void* raw) {
  const auto* hold = static_cast<const HoldFor*>(raw);
  HANDLE mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, g_holder_mutex_name.c_str());
  if (mutex == nullptr) return 1;
  WaitForSingleObject(mutex, 5000);
  SetEvent(hold->ready);
  Sleep(hold->milliseconds);
  ReleaseMutex(mutex);
  CloseHandle(mutex);
  return 0;
}

}  // namespace

// The lock is held for longer than an append's bounded wait while the appenders run. The wait times out, and the
// appends that follow write without the lock. Every appended line must still be in the file.
TEST(BootstrapDiagnostics, ConcurrentAppendersKeepEveryLineWhileTheLockIsHeldPastItsWait) {
  g_holder_mutex_name = UniqueMutexName(L"held-past-wait");
  ScopedLogMutexName scoped(g_holder_mutex_name);
  const std::wstring local = MakeTempDir(L"local");
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
  HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HoldFor hold{ready, 3000};
  HANDLE holder = CreateThread(nullptr, 0, &HoldLogLockFor, &hold, 0, nullptr);
  ASSERT_NE(holder, nullptr);
  ASSERT_EQ(WaitForSingleObject(ready, 5000), WAIT_OBJECT_0);

  constexpr int kThreads = 8;
  constexpr int kLines = 300;
  AppendConcurrently(diagnostics, kThreads, kLines, "H");
  WaitForSingleObject(holder, 10000);
  CloseHandle(holder);
  CloseHandle(ready);

  EXPECT_TRUE(BootstrapDiagnostics::LogLockUnreliableForTest())
      << "the appends did not time out on the lock; the test forces nothing";
  const std::string text = ReadAll(local + kLogsFile);
  const std::size_t expected = static_cast<std::size_t>(kThreads * kLines);
  const LineCount own = CountLinesWithPrefix(text, 'H');
  // A count that is off prints the lines that are neither an appended line nor the lock record, so the odd line is in
  // the failure output (#1304).
  EXPECT_EQ(own.lines, expected) << LockDiagnosis(text) << StrayLines(text);
  EXPECT_EQ(own.distinct, expected) << LockDiagnosis(text) << StrayLines(text);
  // The degraded lock leaves one record in the file, which a count of every line would add to the appended ones.
  EXPECT_EQ(CountLines(text).lines, expected + 1) << LockDiagnosis(text) << StrayLines(text);
}

// The first appends of a process run the one-time initialization of the local lock. This helper is that first use:
// it is the only test the child process runs, and eight threads log for the first time at once.
TEST(BootstrapDiagnostics, FirstAppendsHelperInAFreshProcess) {
  wchar_t dir[MAX_PATH] = {};
  wchar_t name[128] = {};
  if (GetEnvironmentVariableW(L"NEVR_TEST_FIRST_USE_DIR", dir, MAX_PATH) == 0) GTEST_SKIP() << "helper process only";
  GetEnvironmentVariableW(L"NEVR_TEST_FIRST_USE_MUTEX", name, 128);
  BootstrapDiagnostics::SetLogMutexNameForTest(name);
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(dir, nullptr, nullptr);
  AppendConcurrently(diagnostics, 8, 300, "F");
}

// Eight threads log for the first time at once, in a process of their own, so the one-time initialization of the
// local lock is raced, not run by an earlier test. Every line must be in the file.
TEST(BootstrapDiagnostics, FirstConcurrentAppendsInAFreshProcessLoseNoLine) {
  const std::wstring local = MakeTempDir(L"first");
  const std::wstring name = UniqueMutexName(L"first");
  SetEnvironmentVariableW(L"NEVR_TEST_FIRST_USE_DIR", local.c_str());
  SetEnvironmentVariableW(L"NEVR_TEST_FIRST_USE_MUTEX", name.c_str());
  wchar_t self[MAX_PATH] = {};
  ASSERT_GT(GetModuleFileNameW(nullptr, self, MAX_PATH), 0u);
  std::wstring command = L"\"" + std::wstring(self) +
                         L"\" --gtest_filter=BootstrapDiagnostics.FirstAppendsHelperInAFreshProcess";
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};
  ASSERT_NE(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process),
            FALSE);
  CloseHandle(process.hThread);
  EXPECT_EQ(WaitForSingleObject(process.hProcess, 120000), WAIT_OBJECT_0);
  DWORD code = 1;
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hProcess);
  SetEnvironmentVariableW(L"NEVR_TEST_FIRST_USE_DIR", nullptr);
  SetEnvironmentVariableW(L"NEVR_TEST_FIRST_USE_MUTEX", nullptr);
  EXPECT_EQ(code, 0u) << "the helper process failed";
  EXPECT_EQ(CountLinesWithPrefix(ReadAll(local + kLogsFile), 'F').distinct, static_cast<std::size_t>(8 * 300));
}

// A holder that never lets go must delay an append by the bounded wait at most; the line is still written.
TEST(BootstrapDiagnostics, AnAppendIsNotBlockedForeverByAnotherThreadHoldingTheLogLock) {
  ScopedLogMutexName scoped(UniqueMutexName(L"blocked"));
  const std::wstring local = MakeTempDir(L"local");
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
  HANDLE handles[2] = {CreateEventW(nullptr, TRUE, FALSE, nullptr), CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  HANDLE holder = CreateThread(nullptr, 0, &HoldLogLock, handles, 0, nullptr);
  ASSERT_NE(holder, nullptr);
  ASSERT_EQ(WaitForSingleObject(handles[0], 5000), WAIT_OBJECT_0);

  const auto start = std::chrono::steady_clock::now();
  const char line[] = "{\"event\":\"blocked\"}\n";
  const DiagnosticDestination where = diagnostics.AppendLine(line, sizeof(line) - 1);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
  SetEvent(handles[1]);
  WaitForSingleObject(holder, 10000);
  CloseHandle(holder);
  CloseHandle(handles[0]);
  CloseHandle(handles[1]);

  EXPECT_EQ(where, DiagnosticDestination::kPrimary);
  EXPECT_LT(elapsed.count(), 4000);  // bounded, not the holder's 10 s
  EXPECT_GE(elapsed.count(), 300);   // it did wait for the lock first
  EXPECT_EQ(CountLines(ReadAll(local + kLogsFile)).lines, 2u);  // the line, then the host_log_lock record
}

// A crash inside an append runs the crash callback on the thread that holds the lock: the mutex is
// recursive for its owner, so that record is written at once instead of waiting for itself.
TEST(BootstrapDiagnostics, AnAppendOnTheThreadThatHoldsTheLogLockIsImmediate) {
  const std::wstring name = UniqueMutexName(L"owner");
  ScopedLogMutexName scoped(name);
  const std::wstring local = MakeTempDir(L"local");
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
  HANDLE mutex = CreateMutexW(nullptr, FALSE, name.c_str());
  ASSERT_NE(mutex, nullptr);
  ASSERT_EQ(WaitForSingleObject(mutex, 5000), WAIT_OBJECT_0);
  const auto start = std::chrono::steady_clock::now();
  const char line[] = "{\"event\":\"crash_while_holding\"}\n";
  diagnostics.AppendLine(line, sizeof(line) - 1);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
  ReleaseMutex(mutex);
  CloseHandle(mutex);
  EXPECT_LT(elapsed.count(), 200);
  EXPECT_EQ(CountLines(ReadAll(local + kLogsFile)).lines, 1u);
}

// The mutex is created at attach (Prepare) and never by an append: the crash callback is an append, and
// it must not be the first to create it. These tests name the log mutex uniquely through the seam and look
// for it with OpenMutexW, so no other process (another worktree's test run in the same wineserver) can make
// them fail or pass. That no other line of src/runtime/host/ creates a mutex, under any name, is checked in
// tools/tests/test_host_log_mutex_creation.py.
TEST(BootstrapDiagnostics, PrepareCreatesTheLogMutex) {
  const std::wstring name = UniqueMutexName(L"prepare");
  ScopedLogMutexName scoped(name);
  ASSERT_FALSE(MutexExists(name.c_str()));
  BootstrapDiagnostics attached;
  attached.Prepare(GetModuleHandleW(nullptr));
  EXPECT_TRUE(MutexExists(name.c_str())) << "attach did not create the log mutex";
  attached.Prepare(GetModuleHandleW(nullptr));  // idempotent
  EXPECT_TRUE(MutexExists(name.c_str()));
  BootstrapDiagnostics::ResetLogLockForTest();
  EXPECT_FALSE(MutexExists(name.c_str()));
}

TEST(BootstrapDiagnostics, AnAppendBeforeAttachCreatesNoLogMutex) {
  const std::wstring name = UniqueMutexName(L"absent");
  ScopedLogMutexName scoped(name);
  const std::wstring local = MakeTempDir(L"local");
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
  // Nothing has attached in this process (the log mutex is forgotten), and the first write is an
  // append, as a crash before the first flush is.
  BootstrapDiagnostics::ResetLogLockForTest();
  ASSERT_FALSE(MutexExists(name.c_str()));
  const char line[] = "{\"event\":\"first_write_is_a_crash\"}\n";
  EXPECT_EQ(diagnostics.AppendLine(line, sizeof(line) - 1), DiagnosticDestination::kPrimary);
  EXPECT_FALSE(MutexExists(name.c_str())) << "an append created the log mutex";
  EXPECT_EQ(CountLines(ReadAll(local + kLogsFile)).lines, 1u);
}

// One crash writes up to four records. With a holder that never releases, the first record waits and
// the lock is then marked unreliable: the rest write at once, so the whole crash costs one wait.
TEST(BootstrapDiagnostics, ACrashOfFourRecordsPaysOneLockWaitWhenAHolderNeverReleases) {
  ScopedLogMutexName scoped(UniqueMutexName(L"four"));
  const std::wstring local = MakeTempDir(L"local");
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
  HANDLE handles[2] = {CreateEventW(nullptr, TRUE, FALSE, nullptr), CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  HANDLE holder = CreateThread(nullptr, 0, &HoldLogLock, handles, 0, nullptr);
  ASSERT_NE(holder, nullptr);
  ASSERT_EQ(WaitForSingleObject(handles[0], 5000), WAIT_OBJECT_0);

  const auto start = std::chrono::steady_clock::now();
  const char line[] = "{\"event\":\"crash_dump\"}\n";
  for (int record = 0; record < 4; ++record) diagnostics.AppendLine(line, sizeof(line) - 1);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
  const bool unreliable = BootstrapDiagnostics::LogLockUnreliableForTest();
  SetEvent(handles[1]);
  WaitForSingleObject(holder, 10000);
  CloseHandle(holder);
  CloseHandle(handles[0]);
  CloseHandle(handles[1]);

  EXPECT_TRUE(unreliable);
  EXPECT_GE(elapsed.count(), 300);  // the first record did wait
  EXPECT_LT(elapsed.count(), 900);  // four waits would be about 2000
  EXPECT_EQ(CountLines(ReadAll(local + kLogsFile)).lines, 5u);  // four records and the one host_log_lock record
}

namespace {

DWORD WINAPI ReleasingHolder(void* raw) {
  HANDLE* events = static_cast<HANDLE*>(raw);  // [0] held, [1] next
  HANDLE mutex = CreateMutexW(nullptr, FALSE, g_holder_mutex_name.c_str());
  for (int round = 0; round < 4; ++round) {
    WaitForSingleObject(mutex, 5000);
    SetEvent(events[0]);
    Sleep(350);  // releases inside the append's 500 ms wait
    ReleaseMutex(mutex);
    WaitForSingleObject(events[1], 5000);
  }
  CloseHandle(mutex);
  return 0;
}

}  // namespace

// What the docs say: each wait is bounded, and only a wait that does not end owned marks the lock
// unreliable. A holder that releases inside every wait makes each of four appends wait, with no mark.
TEST(BootstrapDiagnostics, WaitsThatEndOwnedAreNotCountedAgainstTheLock) {
  ScopedLogMutexName scoped(UniqueMutexName(L"releasing"));
  const std::wstring local = MakeTempDir(L"local");
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
  HANDLE events[2] = {CreateEventW(nullptr, FALSE, FALSE, nullptr), CreateEventW(nullptr, FALSE, FALSE, nullptr)};
  HANDLE holder = CreateThread(nullptr, 0, &ReleasingHolder, events, 0, nullptr);
  ASSERT_NE(holder, nullptr);
  const char line[] = "{\"event\":\"owned_wait\"}\n";
  long long total_ms = 0;
  for (int round = 0; round < 4; ++round) {
    ASSERT_EQ(WaitForSingleObject(events[0], 5000), WAIT_OBJECT_0);
    const auto start = std::chrono::steady_clock::now();
    diagnostics.AppendLine(line, sizeof(line) - 1);
    total_ms += std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    SetEvent(events[1]);
  }
  WaitForSingleObject(holder, 10000);
  CloseHandle(holder);
  CloseHandle(events[0]);
  CloseHandle(events[1]);
  EXPECT_FALSE(BootstrapDiagnostics::LogLockUnreliableForTest());
  EXPECT_EQ(CountLines(ReadAll(local + kLogsFile)).lines, 4u);  // no host_log_lock record
  EXPECT_GE(total_ms, 800);   // each append waited for the holder: there is no per-process budget
  EXPECT_LT(total_ms, 4 * static_cast<long long>(nevr_host::kBootstrapLogLockWaitMs) + 1000);
}

// The wait that times out says so, once, in the log: the stage, the wait result and what happens next.
TEST(BootstrapDiagnostics, ATimedOutLogLockWaitIsRecordedOnceWithWhatHappensNext) {
  ScopedLogMutexName scoped(UniqueMutexName(L"record"));
  const std::wstring local = MakeTempDir(L"local");
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
  HANDLE handles[2] = {CreateEventW(nullptr, TRUE, FALSE, nullptr), CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  HANDLE holder = CreateThread(nullptr, 0, &HoldLogLock, handles, 0, nullptr);
  ASSERT_NE(holder, nullptr);
  ASSERT_EQ(WaitForSingleObject(handles[0], 5000), WAIT_OBJECT_0);
  const char line[] = "{\"event\":\"x\"}\n";
  for (int record = 0; record < 3; ++record) diagnostics.AppendLine(line, sizeof(line) - 1);
  SetEvent(handles[1]);
  WaitForSingleObject(holder, 10000);
  CloseHandle(holder);
  CloseHandle(handles[0]);
  CloseHandle(handles[1]);

  std::vector<nlohmann::json> locks;
  for (const nlohmann::json& record : ReadRecords(local + kLogsFile)) {
    if (record.value("event", "") == "host_log_lock") locks.push_back(record);
  }
  ASSERT_EQ(locks.size(), 1u);
  EXPECT_EQ(locks[0].at("stage"), "wait");
  EXPECT_EQ(locks[0].at("outcome"), "timeout");
  EXPECT_EQ(locks[0].at("wait_result"), WAIT_TIMEOUT);
  EXPECT_EQ(locks[0].at("wait_ms"), nevr_host::kBootstrapLogLockWaitMs);
  EXPECT_EQ(locks[0].at("next"), "later_appends_write_without_the_lock");
  EXPECT_EQ(locks[0].at("tag"), "NEVR.HOST");
}

// A mutex that cannot be created at attach is journalled (the loader lock may be held: nothing is written
// then) and written when the journal is flushed; the appends still work, without the lock.
TEST(BootstrapDiagnostics, AFailedLogMutexCreationIsJournalledAndAppendsStillWork) {
  ScopedLogMutexName scoped(L"Local\\nevr-test\\not\\a\\valid\\mutex\\name");
  const std::wstring local = MakeTempDir(L"local");
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
  ASSERT_EQ(diagnostics.JournalSize(), 1u);
  EXPECT_FALSE(diagnostics.AttachDegraded()) << "a missing log lock is not a failed attach";
  diagnostics.FlushAttachJournal();
  const char line[] = "{\"event\":\"after\"}\n";
  EXPECT_EQ(diagnostics.AppendLine(line, sizeof(line) - 1), DiagnosticDestination::kPrimary);

  const std::vector<nlohmann::json> records = ReadRecords(local + kLogsFile);
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0].at("event"), "host_attach");
  EXPECT_EQ(records[0].at("step"), "log_mutex");
  EXPECT_EQ(records[0].at("outcome"), "unavailable");
  EXPECT_NE(records[0].at("win32_error").get<std::uint32_t>(), 0u);
  EXPECT_EQ(records[0].at("next"), "log_appends_run_without_the_lock");
  EXPECT_EQ(records[1].at("event"), "after");
}

namespace {

std::mutex g_sink_lock;
std::vector<nlohmann::json>* g_sink_records = nullptr;

void CollectingSink(const nlohmann::json& record) {
  std::lock_guard<std::mutex> guard(g_sink_lock);
  if (g_sink_records != nullptr) g_sink_records->push_back(record);
}

struct ScopedSink {
  explicit ScopedSink(std::vector<nlohmann::json>* records) {
    g_sink_records = records;
    BootstrapDiagnostics::SetEmitSinkForTest(&CollectingSink);
  }
  ~ScopedSink() {
    BootstrapDiagnostics::SetEmitSinkForTest(nullptr);
    g_sink_records = nullptr;
  }
};

struct FlushState {
  BootstrapDiagnostics* diagnostics;
  volatile LONG stop;
};

DWORD WINAPI FlushUntilStopped(void* raw) {
  auto* state = static_cast<FlushState*>(raw);
  while (InterlockedCompareExchange(&state->stop, 0, 0) == 0) state->diagnostics->FlushAttachJournal();
  return 0;
}

}  // namespace

// RecordAttach reserves a slot before it fills it. A flush that trusts the count emits a slot that is
// still empty: a record whose step is "unknown", and the real one lost.
TEST(BootstrapDiagnostics, AFlushNeverEmitsAnEntryBeforeItIsWritten) {
  std::vector<nlohmann::json> records;
  ScopedSink sink(&records);
  const unsigned char bytes[nevr_host::kGameMainWrapperPrologueSize] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
  for (int round = 0; round < 2000; ++round) {
    records.clear();
    BootstrapDiagnostics diagnostics;
    diagnostics.PrepareForTest(nullptr, nullptr, nullptr);
    FlushState state{&diagnostics, 0};
    HANDLE flushers[4];
    for (HANDLE& flusher : flushers) flusher = CreateThread(nullptr, 0, &FlushUntilStopped, &state, 0, nullptr);
    for (std::size_t at = 0; at < BootstrapDiagnostics::kJournalCapacity; ++at) {
      diagnostics.RecordAttach(AttachStep::kHookInstall, AttachOutcome::kFailed,
                               static_cast<std::uint32_t>(1000 + at), bytes, nevr_host::HookStage::kCreate);
    }
    InterlockedExchange(&state.stop, 1);
    WaitForMultipleObjects(4, flushers, TRUE, 20000);
    for (HANDLE flusher : flushers) CloseHandle(flusher);
    diagnostics.FlushAttachJournal();

    std::set<int> seen;
    for (const nlohmann::json& record : records) {
      ASSERT_EQ(record.value("step", ""), "hook_install") << "round " << round << ": " << record.dump();
      seen.insert(record.at("mh_status").get<int>());
    }
    ASSERT_EQ(records.size(), BootstrapDiagnostics::kJournalCapacity) << "round " << round;
    ASSERT_EQ(seen.size(), BootstrapDiagnostics::kJournalCapacity) << "round " << round;
  }
}

namespace {

HANDLE g_reserved_event = nullptr;
HANDLE g_release_event = nullptr;
volatile LONG g_pause_armed = 0;

// Called from RecordAttach after it reserved its slot and wrote part of it. Pauses only the armed call.
void PauseInsideRecordAttach() {
  if (InterlockedExchange(&g_pause_armed, 0) == 0) return;
  SetEvent(g_reserved_event);
  WaitForSingleObject(g_release_event, 20000);
}

struct RecordJob {
  BootstrapDiagnostics* diagnostics;
  std::uint32_t detail;
};

DWORD WINAPI RecordOne(void* raw) {
  const auto* job = static_cast<const RecordJob*>(raw);
  job->diagnostics->RecordAttach(AttachStep::kHookInstall, AttachOutcome::kFailed, job->detail, nullptr,
                                 nevr_host::HookStage::kCreate);
  return 0;
}

}  // namespace

namespace {

// Releases, joins and closes the paused recorder on every exit from the test, a failed ASSERT
// included: the thread is inside RecordAttach on the test's own `diagnostics`, so it must be done
// before that goes out of scope. Declare it after `diagnostics` and the RecordJob (it is destroyed first).
struct PausedRecorder {
  HANDLE thread = nullptr;
  ~PausedRecorder() {
    SetEvent(g_release_event);
    if (thread != nullptr) {
      WaitForSingleObject(thread, INFINITE);
      CloseHandle(thread);
    }
    BootstrapDiagnostics::SetRecordPauseForTest(nullptr);
    CloseHandle(g_reserved_event);
    CloseHandle(g_release_event);
  }
};

}  // namespace

// Deterministic form of the race above: an entry that is reserved and half written is not emitted, and
// the mark does not skip it; once it is complete a later flush writes it, with its own fields.
TEST(BootstrapDiagnostics, AnEntryThatIsReservedButNotWrittenIsNotEmittedUntilItIsComplete) {
  std::vector<nlohmann::json> records;
  ScopedSink sink(&records);
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(nullptr, nullptr, nullptr);
  g_reserved_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  g_release_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  BootstrapDiagnostics::SetRecordPauseForTest(&PauseInsideRecordAttach);

  RecordJob first{&diagnostics, 1000};
  RecordOne(&first);  // complete: the pause is not armed
  InterlockedExchange(&g_pause_armed, 1);
  RecordJob second{&diagnostics, 1001};
  PausedRecorder paused;
  paused.thread = CreateThread(nullptr, 0, &RecordOne, &second, 0, nullptr);
  ASSERT_NE(paused.thread, nullptr);
  ASSERT_EQ(WaitForSingleObject(g_reserved_event, 10000), WAIT_OBJECT_0);

  diagnostics.FlushAttachJournal();  // entry 0 is complete; entry 1 is reserved and half written
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].at("mh_status"), 1000);
  EXPECT_EQ(diagnostics.JournalFlushed(), 1u);

  SetEvent(g_release_event);
  WaitForSingleObject(paused.thread, 10000);
  diagnostics.FlushAttachJournal();
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[1].at("step"), "hook_install");
  EXPECT_EQ(records[1].at("mh_status"), 1001);
  EXPECT_EQ(records[1].at("stage"), "create");
}

namespace {

// A sink that holds the flushing thread before its first record is stored, the way a thread killed mid-write
// leaves the entries it claimed unwritten.
std::mutex g_kill_lock;
std::vector<nlohmann::json>* g_kill_records = nullptr;
DWORD g_kill_thread = 0;
HANDLE g_kill_held_event = nullptr;
HANDLE g_kill_release_event = nullptr;
bool g_kill_held_once = false;

void KilledFlushSink(const nlohmann::json& record) {
  if (GetCurrentThreadId() == g_kill_thread && !g_kill_held_once) {
    g_kill_held_once = true;
    SetEvent(g_kill_held_event);
    WaitForSingleObject(g_kill_release_event, 10000);
  }
  std::lock_guard<std::mutex> guard(g_kill_lock);
  if (g_kill_records != nullptr) g_kill_records->push_back(record);
}

DWORD WINAPI FlushOnceOnAThread(void* raw) {
  static_cast<BootstrapDiagnostics*>(raw)->FlushAttachJournal();
  return 0;
}

}  // namespace

// The flush thread of a failed attach is killed when the process ends at once, after it claimed the entries and
// before it wrote them. The last flush writes them, the failing step first.
TEST(BootstrapDiagnostics, TheLastFlushWritesWhatAKilledFlushClaimedAndTheFailingStepFirst) {
  std::vector<nlohmann::json> records;
  g_kill_records = &records;
  g_kill_held_once = false;
  g_kill_held_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  g_kill_release_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  BootstrapDiagnostics::SetEmitSinkForTest(&KilledFlushSink);

  BootstrapDiagnostics diagnostics;
  diagnostics.RecordAttach(AttachStep::kImageCheck, AttachOutcome::kAccepted);
  diagnostics.RecordAttach(AttachStep::kPrologueCheck, AttachOutcome::kRejected);
  diagnostics.RecordAttach(AttachStep::kHookInstall, AttachOutcome::kFailed);

  HANDLE flusher = CreateThread(nullptr, 0, &FlushOnceOnAThread, &diagnostics, 0, &g_kill_thread);
  ASSERT_NE(flusher, nullptr);
  ASSERT_EQ(WaitForSingleObject(g_kill_held_event, 10000), WAIT_OBJECT_0);
  EXPECT_EQ(diagnostics.JournalFlushed(), 3u);  // all three claimed, none written
  EXPECT_TRUE(records.empty());

  diagnostics.FlushAttachJournalAtExit();  // the flusher is still held: it stands for the killed thread
  ASSERT_EQ(records.size(), 3u);
  EXPECT_EQ(records[0].at("step"), "prologue_check");  // the failing steps first, in journal order
  EXPECT_EQ(records[1].at("step"), "hook_install");
  EXPECT_EQ(records[2].at("step"), "image_check");

  SetEvent(g_kill_release_event);
  WaitForSingleObject(flusher, 10000);
  CloseHandle(flusher);
  BootstrapDiagnostics::SetEmitSinkForTest(nullptr);
  g_kill_records = nullptr;
  CloseHandle(g_kill_held_event);
  CloseHandle(g_kill_release_event);
}

TEST(BootstrapDiagnostics, TheLastFlushWritesNothingTwiceAndNothingWhenEverythingIsWritten) {
  std::vector<nlohmann::json> records;
  ScopedSink sink(&records);
  BootstrapDiagnostics diagnostics;
  diagnostics.RecordAttach(AttachStep::kImageCheck, AttachOutcome::kAccepted);
  diagnostics.RecordAttach(AttachStep::kPrologueCheck, AttachOutcome::kRejected);
  diagnostics.FlushAttachJournal();
  ASSERT_EQ(records.size(), 2u);
  diagnostics.FlushAttachJournalAtExit();
  diagnostics.FlushAttachJournalAtExit();
  EXPECT_EQ(records.size(), 2u);

  diagnostics.RecordAttach(AttachStep::kHookInstall, AttachOutcome::kFailed);  // recorded after the last flush
  diagnostics.FlushAttachJournalAtExit();
  ASSERT_EQ(records.size(), 3u);
  EXPECT_EQ(records[2].at("step"), "hook_install");
}

// A flush thread killed after its record reached the file and before it marked the entry leaves the entry
// "claimed": the last flush finds the record by (run, seq) and does not write it again. A claimed entry that is not
// in the file is written.
TEST(BootstrapDiagnostics, TheLastFlushDoesNotRepeatAClaimedStepThatIsAlreadyInTheLogAndWritesOneThatIsNot) {
  const std::wstring local = MakeTempDir(L"local");
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
  diagnostics.RecordAttach(AttachStep::kImageCheck, AttachOutcome::kAccepted);
  diagnostics.RecordAttach(AttachStep::kPrologueCheck, AttachOutcome::kRejected);
  diagnostics.RecordAttach(AttachStep::kHookInstall, AttachOutcome::kFailed);
  diagnostics.FlushAttachJournal();
  ASSERT_EQ(ReadRecords(local + kLogsFile).size(), 3u);

  diagnostics.SetEntryStateForTest(1, BootstrapDiagnostics::kEntryClaimed);  // written, not marked
  diagnostics.FlushAttachJournalAtExit();
  std::vector<nlohmann::json> records = ReadRecords(local + kLogsFile);
  EXPECT_EQ(records.size(), 3u);

  const std::wstring other = MakeTempDir(L"local");
  BootstrapDiagnostics lost;
  lost.PrepareForTest(other.c_str(), nullptr, nullptr);
  lost.RecordAttach(AttachStep::kImageCheck, AttachOutcome::kAccepted);
  lost.RecordAttach(AttachStep::kPrologueCheck, AttachOutcome::kRejected);
  lost.SetEntryStateForTest(1, BootstrapDiagnostics::kEntryClaimed);  // claimed, never written
  lost.FlushAttachJournalAtExit();
  records = ReadRecords(other + kLogsFile);
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0].at("step"), "prologue_check");
  EXPECT_EQ(records[1].at("step"), "image_check");
}

// Every journal record carries its journal index as `seq`; with the record's `run` it is a key no step shares.
TEST(BootstrapDiagnostics, EachJournalRecordCarriesItsSeqAndNoPidSeqPairRepeats) {
  const std::wstring local = MakeTempDir(L"local");
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
  for (int at = 0; at < 5; ++at) diagnostics.RecordAttach(AttachStep::kImageCheck, AttachOutcome::kAccepted);
  diagnostics.FlushAttachJournal();
  diagnostics.FlushAttachJournalAtExit();
  const std::vector<nlohmann::json> records = ReadRecords(local + kLogsFile);
  ASSERT_EQ(records.size(), 5u);
  std::set<std::pair<std::string, unsigned long long>> keys;
  for (std::size_t at = 0; at < records.size(); ++at) {
    EXPECT_EQ(records[at].at("seq").get<std::size_t>(), at);
    keys.insert({records[at].at("run").get<std::string>(), records[at].at("seq").get<unsigned long long>()});
    EXPECT_EQ(records[at].at("run").get<std::string>().size(), 16u);
  }
  EXPECT_EQ(keys.size(), 5u);
}

// Wine reuses pids: the game is pid 32 on every launch. An older process with the same pid wrote seq 0 and 1 to this
// log; the new process claimed its entry 1 (its decisive step) and was killed before the write. The last flush must
// still write it: the older process's (pid, seq 1) line is not this process's.
TEST(BootstrapDiagnostics, ThePidOfAnOlderProcessDoesNotHideAClaimedStepOfThisOne) {
  const std::wstring local = MakeTempDir(L"local");
  const std::uint64_t mine = BootstrapDiagnostics::RunTokenForTest();

  // The older process: same pid, another run token.
  BootstrapDiagnostics::SetRunTokenForTest(mine ^ 0x5A5A5A5A5A5A5A5Aull);
  BootstrapDiagnostics older;
  older.PrepareForTest(local.c_str(), nullptr, nullptr);
  older.RecordAttach(AttachStep::kImageCheck, AttachOutcome::kAccepted);
  older.RecordAttach(AttachStep::kPrologueCheck, AttachOutcome::kAccepted);
  older.FlushAttachJournal();
  ASSERT_EQ(ReadRecords(local + kLogsFile).size(), 2u);

  // This process: its entry 1 is claimed and never written.
  BootstrapDiagnostics::SetRunTokenForTest(mine);
  BootstrapDiagnostics now;
  now.PrepareForTest(local.c_str(), nullptr, nullptr);
  now.RecordAttach(AttachStep::kImageCheck, AttachOutcome::kAccepted);
  now.RecordAttach(AttachStep::kPrologueCheck, AttachOutcome::kRejected);
  now.SetEntryStateForTest(1, BootstrapDiagnostics::kEntryClaimed);
  now.SetEntryStateForTest(0, BootstrapDiagnostics::kEntryWritten);
  now.FlushAttachJournalAtExit();

  const std::vector<nlohmann::json> records = ReadRecords(local + kLogsFile);
  ASSERT_EQ(records.size(), 3u);  // the decisive record landed
  EXPECT_EQ(records[2].at("step"), "prologue_check");
  EXPECT_EQ(records[2].at("outcome"), "mismatch");
  EXPECT_NE(records[2].at("run"), records[1].at("run"));  // another run, the same pid
  EXPECT_EQ(records[2].at("pid"), records[1].at("pid"));
  BootstrapDiagnostics::SetRunTokenForTest(0);
}

// The same run is found: a record this process wrote (claimed, not marked) is not written again even when an older
// process's lines with the same pid and seq stand later in the file.
TEST(BootstrapDiagnostics, ARecordOfThisRunIsFoundAmongOlderLinesWithTheSamePid) {
  const std::wstring local = MakeTempDir(L"local");
  const std::uint64_t mine = BootstrapDiagnostics::RunTokenForTest();
  BootstrapDiagnostics now;
  now.PrepareForTest(local.c_str(), nullptr, nullptr);
  now.RecordAttach(AttachStep::kImageCheck, AttachOutcome::kAccepted);
  now.RecordAttach(AttachStep::kPrologueCheck, AttachOutcome::kRejected);
  now.FlushAttachJournal();

  BootstrapDiagnostics::SetRunTokenForTest(mine ^ 0x00FF00FF00FF00FFull);
  BootstrapDiagnostics older;
  older.PrepareForTest(local.c_str(), nullptr, nullptr);
  older.RecordAttach(AttachStep::kImageCheck, AttachOutcome::kAccepted);
  older.RecordAttach(AttachStep::kPrologueCheck, AttachOutcome::kAccepted);
  older.FlushAttachJournal();
  BootstrapDiagnostics::SetRunTokenForTest(mine);

  now.SetEntryStateForTest(1, BootstrapDiagnostics::kEntryClaimed);
  now.FlushAttachJournalAtExit();
  EXPECT_EQ(ReadRecords(local + kLogsFile).size(), 4u);  // nothing added
  BootstrapDiagnostics::SetRunTokenForTest(0);
}

// The claim is stored before the write, so a flush thread killed anywhere inside the write leaves the entry claimed.
namespace {
BootstrapDiagnostics* g_order_diagnostics = nullptr;
std::vector<std::uint32_t>* g_order_states = nullptr;
void StateAtWriteSink(const nlohmann::json& record) {
  if (g_order_diagnostics != nullptr && g_order_states != nullptr && record.contains("seq")) {
    g_order_states->push_back(g_order_diagnostics->EntryStateForTest(record.at("seq").get<std::size_t>()));
  }
}
}  // namespace

TEST(BootstrapDiagnostics, AnEntryIsClaimedBeforeItsRecordIsWrittenByEitherFlush) {
  std::vector<std::uint32_t> states;
  BootstrapDiagnostics diagnostics;
  g_order_diagnostics = &diagnostics;
  g_order_states = &states;
  BootstrapDiagnostics::SetEmitSinkForTest(&StateAtWriteSink);
  diagnostics.RecordAttach(AttachStep::kImageCheck, AttachOutcome::kAccepted);
  diagnostics.RecordAttach(AttachStep::kPrologueCheck, AttachOutcome::kRejected);
  diagnostics.FlushAttachJournal();
  diagnostics.RecordAttach(AttachStep::kHookInstall, AttachOutcome::kFailed);
  diagnostics.FlushAttachJournalAtExit();
  BootstrapDiagnostics::SetEmitSinkForTest(nullptr);
  g_order_diagnostics = nullptr;
  g_order_states = nullptr;
  ASSERT_EQ(states.size(), 3u);
  for (const std::uint32_t state : states) EXPECT_EQ(state, BootstrapDiagnostics::kEntryClaimed);
}

// The log mutex is named and shared by every process on the prefix: a live process can hold it. The last flush waits
// at most kBootstrapExitLogLockWaitMs and then writes nothing (an unlocked line could interleave with the holder's);
// a holder that never lets go must not hold the exiting process.
TEST(BootstrapDiagnostics, TheLastFlushIsNotHeldForeverByAnotherThreadHoldingTheLogLockAndWritesNothingUnlocked) {
  ScopedLogMutexName scoped(UniqueMutexName(L"exitheld"));
  const std::wstring local = MakeTempDir(L"local");
  BootstrapDiagnostics diagnostics;
  diagnostics.PrepareForTest(local.c_str(), nullptr, nullptr);
  diagnostics.RecordAttach(AttachStep::kImageCheck, AttachOutcome::kAccepted);
  diagnostics.RecordAttach(AttachStep::kPrologueCheck, AttachOutcome::kRejected);
  diagnostics.RecordAttach(AttachStep::kHookInstall, AttachOutcome::kFailed);
  HANDLE handles[2] = {CreateEventW(nullptr, TRUE, FALSE, nullptr), CreateEventW(nullptr, TRUE, FALSE, nullptr)};
  HANDLE holder = CreateThread(nullptr, 0, &HoldLogLock, handles, 0, nullptr);
  ASSERT_NE(holder, nullptr);
  ASSERT_EQ(WaitForSingleObject(handles[0], 5000), WAIT_OBJECT_0);

  const auto start = std::chrono::steady_clock::now();
  diagnostics.FlushAttachJournalAtExit();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
  EXPECT_LT(elapsed.count(), 1500);  // one bounded wait, not one per record and not the holder's 10 s
  EXPECT_GE(elapsed.count(), 200);   // it did wait for the lock first
  EXPECT_EQ(BootstrapDiagnostics::ExitFlushSkippedForTest(), 1u);
  EXPECT_TRUE(ReadAll(local + kLogsFile).empty());  // nothing was written without the lock

  SetEvent(handles[1]);
  WaitForSingleObject(holder, 10000);
  CloseHandle(holder);
  CloseHandle(handles[0]);
  CloseHandle(handles[1]);

  diagnostics.FlushAttachJournalAtExit();  // the lock is free now: everything that was skipped is written, once
  const std::vector<nlohmann::json> records = ReadRecords(local + kLogsFile);
  ASSERT_EQ(records.size(), 3u);
  EXPECT_EQ(records[0].at("step"), "prologue_check");
}

// The last flush reports entries the journal had no room for, once.
TEST(BootstrapDiagnostics, TheLastFlushReportsTheDroppedEntriesOnce) {
  std::vector<nlohmann::json> records;
  ScopedSink sink(&records);
  BootstrapDiagnostics diagnostics;
  for (std::size_t at = 0; at < BootstrapDiagnostics::kJournalCapacity + 2; ++at) {
    diagnostics.RecordAttach(AttachStep::kImageCheck, AttachOutcome::kAccepted);
  }
  diagnostics.FlushAttachJournalAtExit();
  diagnostics.FlushAttachJournalAtExit();
  std::size_t overflow = 0;
  for (const nlohmann::json& record : records) overflow += record.value("step", "") == "journal" ? 1u : 0u;
  EXPECT_EQ(overflow, 1u);
}

// A flush on the thread inside DllMain attach is counted, so the attach path cannot write the journal itself.
TEST(BootstrapDiagnostics, AFlushOnTheAttachThreadIsCounted) {
  std::vector<nlohmann::json> records;
  ScopedSink sink(&records);
  BootstrapDiagnostics diagnostics;
  const std::size_t before = BootstrapDiagnostics::FlushesOnAttachThread();
  diagnostics.FlushAttachJournal();  // no attach thread named: not counted
  EXPECT_EQ(BootstrapDiagnostics::FlushesOnAttachThread(), before);
  BootstrapDiagnostics::MarkAttachThread(GetCurrentThreadId());
  diagnostics.FlushAttachJournal();
  diagnostics.FlushAttachJournalAtExit();
  BootstrapDiagnostics::MarkAttachThread(0);
  EXPECT_EQ(BootstrapDiagnostics::FlushesOnAttachThread(), before + 2);
  diagnostics.FlushAttachJournal();
  EXPECT_EQ(BootstrapDiagnostics::FlushesOnAttachThread(), before + 2);
}
