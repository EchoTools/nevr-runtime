// Host bootstrap diagnostics (runtime/host/bootstrap_diagnostics.h/.cpp): the
// bounded JSONL recorder the stable BugSplat64.dll host uses for its attach,
// runtime-load and crash records. Every line a test reads back is parsed with
// nlohmann::json.

#include "runtime/host/bootstrap_diagnostics.h"

#include "runtime/host/path_buffer.h"
#include "runtime/host/game_targets.h"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <windows.h>

#include <cstdio>
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
