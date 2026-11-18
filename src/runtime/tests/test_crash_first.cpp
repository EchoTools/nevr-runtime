// The PC crash-first filter (#1605): the game's SetUnhandledExceptionFilter request is remembered and not installed, ours runs
// first on the fatal exception, then the game's. The logic (lifecycle/crash_first_filter.cpp) is driven here with
// the exception records the filter is handed; the hook and the report writer are not part of it (crash_first_win.cpp).

#include <windows.h>

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/crash_first.h"
#include "core/crash_report.h"
#include "runtime/lifecycle/crash_first_filter.h"
#include "runtime/lifecycle/crash_first_report.h"

namespace {

using nevr_crash_first_win::Filter;

std::vector<std::string> g_events;
EXCEPTION_POINTERS* g_reportInfo = nullptr;
bool g_noteChained = false;
long g_noteRequests = 0;

void Report(EXCEPTION_POINTERS* info) noexcept {
  g_events.emplace_back("report");
  g_reportInfo = info;
}
void Note(EXCEPTION_POINTERS*, bool chained, long requests) noexcept {
  g_events.emplace_back("note");
  g_noteChained = chained;
  g_noteRequests = requests;
}
LONG WINAPI GameFilter(EXCEPTION_POINTERS*) {
  g_events.emplace_back("game");
  return EXCEPTION_EXECUTE_HANDLER;
}
LONG WINAPI OtherFilter(EXCEPTION_POINTERS*) { return EXCEPTION_CONTINUE_EXECUTION; }

EXCEPTION_RECORD g_record;
CONTEXT g_context;
EXCEPTION_POINTERS g_pointers;

EXCEPTION_POINTERS* AccessViolation() {
  g_record = {};
  g_context = {};
  g_record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
  g_pointers.ExceptionRecord = &g_record;
  g_pointers.ContextRecord = &g_context;
  return &g_pointers;
}

class CrashFirstFilter : public ::testing::Test {
 protected:
  void SetUp() override {
    nevr_crash_first_win::ResetForTest();
    g_events.clear();
    g_reportInfo = nullptr;
    g_noteChained = false;
    g_noteRequests = 0;
    nevr_crash_first_win::SetActions(&Report, &Note);
  }
  void TearDown() override { nevr_crash_first_win::ResetForTest(); }
};

TEST_F(CrashFirstFilter, TheGamesRequestIsRememberedAndTheFilterItDisplacedIsHandedBack) {
  EXPECT_EQ(nevr_crash_first_win::RememberGameFilter(&GameFilter), nullptr);
  EXPECT_EQ(nevr_crash_first_win::RememberGameFilter(&OtherFilter), &GameFilter);
  EXPECT_EQ(nevr_crash_first_win::RememberGameFilter(nullptr), &OtherFilter);
  EXPECT_EQ(nevr_crash_first_win::RequestCount(), 3);
}

TEST_F(CrashFirstFilter, TheFilterInstalledBeforeOursIsWhatTheGameThinksItDisplaced) {
  nevr_crash_first_win::SeedPrior(&OtherFilter);
  EXPECT_EQ(nevr_crash_first_win::RememberGameFilter(&GameFilter), &OtherFilter);
}

TEST_F(CrashFirstFilter, SeedingNeverOverwritesTheGamesOwnRequest) {
  nevr_crash_first_win::RememberGameFilter(&GameFilter);  // the game asked between the hook and the seed
  nevr_crash_first_win::SeedPrior(&OtherFilter);
  EXPECT_EQ(nevr_crash_first_win::RememberGameFilter(&OtherFilter), &GameFilter);
}

TEST_F(CrashFirstFilter, OursIsNeverRememberedAsTheGames) {
  nevr_crash_first_win::RememberGameFilter(&GameFilter);
  nevr_crash_first_win::RememberGameFilter(&nevr_crash_first_win::TopLevelFilter);
  EXPECT_EQ(nevr_crash_first_win::RememberGameFilter(nullptr), &GameFilter);  // a chain that called itself would recurse
}

TEST_F(CrashFirstFilter, OursRunsFirstThenTheGamesAndItsAnswerIsReturned) {
  nevr_crash_first_win::RememberGameFilter(&GameFilter);
  EXCEPTION_POINTERS* info = AccessViolation();
  EXPECT_EQ(nevr_crash_first_win::TopLevelFilter(info), EXCEPTION_EXECUTE_HANDLER);
  EXPECT_EQ(g_events, (std::vector<std::string>{"report", "game", "note"}));
  EXPECT_EQ(g_reportInfo, info);  // the report got the exception the system raised
  EXPECT_TRUE(g_noteChained);
  EXPECT_EQ(g_noteRequests, 1);
  EXPECT_EQ(nevr_crash_first_win::ChainedCount(), 1);
}

TEST_F(CrashFirstFilter, WithoutAGameFilterTheExceptionIsPassedOn) {
  EXPECT_EQ(nevr_crash_first_win::TopLevelFilter(AccessViolation()), EXCEPTION_CONTINUE_SEARCH);
  EXPECT_EQ(g_events, (std::vector<std::string>{"report", "note"}));
  EXPECT_FALSE(g_noteChained);
}

TEST_F(CrashFirstFilter, AGameThatClearedItsFilterHasNoneRunAfterOurs) {
  nevr_crash_first_win::RememberGameFilter(&GameFilter);
  nevr_crash_first_win::RememberGameFilter(nullptr);
  EXPECT_EQ(nevr_crash_first_win::TopLevelFilter(AccessViolation()), EXCEPTION_CONTINUE_SEARCH);
  EXPECT_EQ(g_events, (std::vector<std::string>{"report", "note"}));
}

TEST_F(CrashFirstFilter, OnlyTheFirstExceptionWritesTheReport) {
  nevr_crash_first_win::RememberGameFilter(&GameFilter);
  nevr_crash_first_win::TopLevelFilter(AccessViolation());
  g_events.clear();
  nevr_crash_first_win::TopLevelFilter(AccessViolation());
  EXPECT_EQ(g_events, (std::vector<std::string>{"game", "note"}));
}

std::atomic<bool> g_slowReportStarted{false};
std::mutex g_threadEventsMutex;
std::vector<std::string> g_threadEvents;
void ThreadEvent(const char* name) {
  std::lock_guard<std::mutex> lock(g_threadEventsMutex);
  g_threadEvents.emplace_back(name);
}
void SlowReport(EXCEPTION_POINTERS*) noexcept {
  g_slowReportStarted.store(true);
  Sleep(200);
  ThreadEvent("report_done");
}
LONG WINAPI RecordingGame(EXCEPTION_POINTERS*) {
  ThreadEvent("game");
  return EXCEPTION_CONTINUE_SEARCH;
}

void RaiseOnThisThread() {
  EXCEPTION_RECORD record = {};
  CONTEXT context = {};
  record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
  EXCEPTION_POINTERS pointers = {&record, &context};
  nevr_crash_first_win::TopLevelFilter(&pointers);
}

// A second thread that faults while the first writes the report waits for it: the game's filter, which may end the process,
// never runs before the report is written.
TEST_F(CrashFirstFilter, ASecondThreadWaitsForTheReportBeforeRunningTheGamesFilter) {
  nevr_crash_first_win::SetActions(&SlowReport, nullptr);
  nevr_crash_first_win::RememberGameFilter(&RecordingGame);
  g_slowReportStarted.store(false);
  g_threadEvents.clear();
  std::thread first(RaiseOnThisThread);
  while (!g_slowReportStarted.load()) Sleep(1);
  std::thread second(RaiseOnThisThread);
  first.join();
  second.join();
  ASSERT_EQ(g_threadEvents.size(), 3u);
  EXPECT_EQ(g_threadEvents.front(), "report_done");  // no "game" before it, though the second thread arrived during the report
}



// What the filter hands the report writer and writes to the crash record (crash_first_report.h).
constexpr std::uint64_t kThreadId = 4242;
constexpr std::uint64_t kExceptionAddress = 0x140123456;
constexpr std::uint64_t kContextRip = 0x140654321;
constexpr std::uint64_t kFaultedOn = 0xDEADBEEF00;
constexpr std::uint64_t kAccessKindRead = 0;
constexpr ULONG_PTR kFaultParameters = 2;
constexpr ULONG_PTR kOneParameter = 1;
constexpr long kDisplacedRequests = 3;

TEST(CrashFirstReport, AnAccessViolationCarriesItsCodeAddressFaultAddressAndContext) {
  EXCEPTION_RECORD record = {};
  CONTEXT context = {};
  record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
  record.ExceptionAddress = reinterpret_cast<PVOID>(static_cast<std::uintptr_t>(kExceptionAddress));
  record.NumberParameters = kFaultParameters;
  record.ExceptionInformation[0] = static_cast<ULONG_PTR>(kAccessKindRead);
  record.ExceptionInformation[1] = static_cast<ULONG_PTR>(kFaultedOn);
  EXCEPTION_POINTERS pointers = {&record, &context};
  const nevr_crash::CrashInfo crash = nevr_crash_first_win::BuildFirstFatalInfo(&pointers, kThreadId);
  EXPECT_EQ(crash.code, static_cast<std::int64_t>(EXCEPTION_ACCESS_VIOLATION));
  EXPECT_EQ(crash.fault_address, kFaultedOn);
  EXPECT_EQ(crash.thread_id, kThreadId);
  EXPECT_EQ(crash.context, &pointers);
  EXPECT_EQ(crash.pc, 0u);  // the context's Rip is 0 here and wins over the record's address
}

TEST(CrashFirstReport, TheContextsInstructionPointerIsThePcWhenThereIsOne) {
  EXCEPTION_RECORD record = {};
  CONTEXT context = {};
  record.ExceptionCode = EXCEPTION_ILLEGAL_INSTRUCTION;
  record.ExceptionAddress = reinterpret_cast<PVOID>(static_cast<std::uintptr_t>(kExceptionAddress));
  context.Rip = kContextRip;
  EXCEPTION_POINTERS pointers = {&record, &context};
  EXPECT_EQ(nevr_crash_first_win::BuildFirstFatalInfo(&pointers, kThreadId).pc, kContextRip);
  pointers.ContextRecord = nullptr;  // no context: the record's address is all there is
  EXPECT_EQ(nevr_crash_first_win::BuildFirstFatalInfo(&pointers, kThreadId).pc, kExceptionAddress);
}

TEST(CrashFirstReport, OnlyAnAccessViolationOrInPageErrorHasAFaultAddress) {
  EXCEPTION_RECORD record = {};
  CONTEXT context = {};
  record.NumberParameters = kFaultParameters;
  record.ExceptionInformation[1] = static_cast<ULONG_PTR>(kFaultedOn);
  EXCEPTION_POINTERS pointers = {&record, &context};
  record.ExceptionCode = EXCEPTION_IN_PAGE_ERROR;
  EXPECT_EQ(nevr_crash_first_win::BuildFirstFatalInfo(&pointers, kThreadId).fault_address, kFaultedOn);
  record.ExceptionCode = EXCEPTION_INT_DIVIDE_BY_ZERO;
  EXPECT_EQ(nevr_crash_first_win::BuildFirstFatalInfo(&pointers, kThreadId).fault_address, 0u);
  record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
  record.NumberParameters = kOneParameter;  // too few parameters: the second one is not there to read
  EXPECT_EQ(nevr_crash_first_win::BuildFirstFatalInfo(&pointers, kThreadId).fault_address, 0u);
}

TEST(CrashFirstReport, NothingToReadStillNamesTheThread) {
  const nevr_crash::CrashInfo none = nevr_crash_first_win::BuildFirstFatalInfo(nullptr, kThreadId);
  EXPECT_EQ(none.thread_id, kThreadId);
  EXPECT_EQ(none.code, 0);
  EXPECT_EQ(none.pc, 0u);
  EXPECT_EQ(none.context, nullptr);
  EXCEPTION_POINTERS empty = {nullptr, nullptr};
  const nevr_crash::CrashInfo hollow = nevr_crash_first_win::BuildFirstFatalInfo(&empty, kThreadId);
  EXPECT_EQ(hollow.code, 0);
  EXPECT_EQ(hollow.context, &empty);
}

TEST(CrashFirstReport, TheRecordLineNamesTheCodeWhetherTheGameFilterRanAndTheDisplacedRequests) {
  EXCEPTION_RECORD record = {};
  record.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
  EXCEPTION_POINTERS pointers = {&record, nullptr};
  char line[nevr_crash_first_win::kRecordLineSize];
  nevr_crash_first_win::FormatFirstHandlerLine(line, sizeof(line), &pointers, true, kDisplacedRequests);
  EXPECT_STREQ(line, "[NEVR.CRASH] first_handler code=0xC0000005 game_filter_called=1 displaced_requests=3");
  nevr_crash_first_win::FormatFirstHandlerLine(line, sizeof(line), nullptr, false, 0);
  EXPECT_STREQ(line, "[NEVR.CRASH] first_handler code=0x00000000 game_filter_called=0 displaced_requests=0");
}

TEST(CrashFirstReport, TheRecordLineNeverWritesPastItsBuffer) {
  EXCEPTION_POINTERS pointers = {nullptr, nullptr};
  constexpr size_t kSmall = 16;
  char line[kSmall + 1] = {};
  line[kSmall] = 'X';  // a guard byte after the buffer
  nevr_crash_first_win::FormatFirstHandlerLine(line, kSmall, &pointers, false, 0);
  EXPECT_EQ(line[kSmall], 'X');
  EXPECT_EQ(std::string(line).size(), kSmall - 1);
}

}  // namespace
