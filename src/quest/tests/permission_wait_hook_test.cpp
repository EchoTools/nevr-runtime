// Host test for "the game waits for the storage permission it just asked for" (#640): the real handler
// (src/quest/sentinel/permission_wait_hook.cpp), built without exceptions as the sentinel builds it, driven through
// the thunk's entry with a stand-in for libr15's CSysInfo::RequestPermissions and a model of CheckPermission. No GOT
// slot is touched: got_pinned_test.cpp resolves the pinned slot and CheckPermission's prologue in the real library.
//
// Built and run by `just test-quest-hooks`.

#include <elf.h>
#include <pthread.h>
#include <time.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "got_hook.h"
#include "hook_log.h"
#include "permission_wait_hook.h"
#include "quest/tests/test_check.h"

namespace {

namespace pw = nevr_quest::permission_wait;

pw::RequestThunk::Fn RequestEntry() {
  return reinterpret_cast<pw::RequestThunk::Fn>(pw::RequestThunk::EntryAddress());
}
pw::CheckThunk::Fn CheckEntry() {
  return reinterpret_cast<pw::CheckThunk::Fn>(pw::CheckThunk::EntryAddress());
}

const char* const kMic = "android.permission.RECORD_AUDIO";
const char* const kRead = "android.permission.READ_EXTERNAL_STORAGE";
const char* const kWrite = "android.permission.WRITE_EXTERNAL_STORAGE";
const char* const kBoot[3] = {kMic, kRead, kWrite};

std::vector<std::string> g_events;
const char* const* g_namesSeen = nullptr;
std::uint64_t g_countSeen = 0;
pthread_t g_thread;
bool g_checkOnOtherThread = false;
int g_denyChecks = 0;  // the checks that answer "denied" before the grant lands (-1: always denied)
int g_checkCalls = 0;
bool g_micGranted = false;

void RealRequestPermissions(const char* const* names, std::uint64_t count) {
  g_namesSeen = names;
  g_countSeen = count;
  g_events.push_back("request");
}

bool ModelCheck(const char* permission) noexcept {
  ++g_checkCalls;
  if (pthread_equal(pthread_self(), g_thread) == 0) g_checkOnOtherThread = true;
  g_events.push_back(std::string("check:") + permission);
  if (permission == kMic) return g_micGranted;
  if (g_denyChecks < 0) return false;
  if (g_denyChecks > 0) {
    --g_denyChecks;
    return false;
  }
  return true;
}

void Reset(int denyChecks) {
  g_events.clear();
  g_namesSeen = nullptr;
  g_countSeen = 0;
  g_checkOnOtherThread = false;
  g_denyChecks = denyChecks;
  g_checkCalls = 0;
  g_micGranted = false;
  g_thread = pthread_self();
  pw::ResetForTest();
  pw::RequestThunk::Reset();
  pw::CheckThunk::Reset();
  *pw::RequestThunk::OriginalOut() = reinterpret_cast<void*>(&RealRequestPermissions);
  *pw::CheckThunk::OriginalOut() = reinterpret_cast<void*>(&ModelCheck);
}

// ---- the log and the requester (#947) ---------------------------------------------------------------------------

std::vector<std::string> g_lines;
std::vector<std::string> g_warnings;  // the WARN lines only
void Sink(sentinel::LogLevel level, const char* line) {
  g_lines.emplace_back(line);
  if (level == sentinel::LogLevel::kWarn) g_warnings.emplace_back(line);
}
std::vector<std::string> Lines(const char* event) {
  const std::string needle = std::string("\"event\":\"") + event + "\"";
  std::vector<std::string> out;
  for (const std::string& l : g_lines) {
    if (l.find(needle) != std::string::npos) out.push_back(l);
  }
  return out;
}
bool Has(const std::string& line, const char* text) { return line.find(text) != std::string::npos; }

const char* g_requesterResult = "requested";
const char* const* g_requesterNames = nullptr;
// The first name the requester was asked for, read inside the call: the handler's array is gone after it returns.
const char* g_requesterFirst = nullptr;
std::uint64_t g_requesterCount = 0;
int g_requesterCode = -1;
int g_requesterCalls = 0;

const char* Requester(const char* const* names, std::uint64_t count, int requestCode) noexcept {
  ++g_requesterCalls;
  g_requesterNames = names;
  g_requesterFirst = count > 0 && names != nullptr ? names[0] : nullptr;
  g_requesterCount = count;
  g_requesterCode = requestCode;
  g_events.push_back("ours");
  return g_requesterResult;
}

void ResetRequester(const char* result) {
  g_lines.clear();
  g_requesterResult = result;
  g_requesterNames = nullptr;
  g_requesterFirst = nullptr;
  g_requesterCount = 0;
  g_requesterCode = -1;
  g_requesterCalls = 0;
}

long long ElapsedMs(std::chrono::steady_clock::time_point since) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count();
}

void ARequestWithoutTheHookIsJustTheGamesCall() {
  Reset(0);  // thunk not armed: the entry calls straight through
  RequestEntry()(kBoot, 3);
  QCHECK(g_events.size() == 1 && g_events[0] == "request");
  QCHECK(g_checkCalls == 0);
  QCHECK(pw::CurrentCounts().requests == 0);
}

void TheBootRequestWaitsOnTheCallersThreadUntilTheStoragePermissionsAreGranted() {
  Reset(3);  // the first three checks answer "denied": the first poll's READ, then two more polls
  pw::ArmForTest(&ModelCheck, 5, 5000);
  RequestEntry()(kBoot, 3);
  QCHECK(g_namesSeen == kBoot && g_countSeen == 3);  // the game's own arguments reached it
  QCHECK(!g_events.empty() && g_events[0] == "request");  // the original ran first
  QCHECK(!g_checkOnOtherThread);                       // on the android_main thread, not a helper
  QCHECK(pw::CurrentCounts().waits == 1 && pw::CurrentCounts().granted == 1 && pw::CurrentCounts().timeouts == 0);
  // The final answer is READ granted then WRITE granted; the microphone is never asked about.
  QCHECK(g_events.size() >= 3);
  QCHECK(g_events[g_events.size() - 2] == std::string("check:") + kRead);
  QCHECK(g_events.back() == std::string("check:") + kWrite);
  for (const std::string& e : g_events) QCHECK(e != std::string("check:") + kMic);
  QCHECK(g_checkCalls >= 4);  // it polled more than once
}

void ARequestForPermissionsAlreadyGrantedDoesNotWait() {
  Reset(0);
  pw::ArmForTest(&ModelCheck, 5, 5000);
  const auto start = std::chrono::steady_clock::now();
  RequestEntry()(kBoot, 3);
  QCHECK(ElapsedMs(start) < 4);  // no poll sleep
  QCHECK(pw::CurrentCounts().waits == 0 && pw::CurrentCounts().requests == 1);
  QCHECK(g_checkCalls == 2);  // READ and WRITE, once each
}

void ADeniedPermissionEndsTheWaitAtTheTimeout() {
  Reset(-1);
  pw::ArmForTest(&ModelCheck, 5, 60);
  const auto start = std::chrono::steady_clock::now();
  RequestEntry()(kBoot, 3);
  const long long ms = ElapsedMs(start);
  QCHECK(ms >= 55);    // it did wait
  QCHECK(ms < 2000);   // and gave up
  QCHECK(pw::CurrentCounts().timeouts == 1 && pw::CurrentCounts().granted == 0);
  QCHECK(!g_events.empty() && g_events[0] == "request");  // the game's call was made either way
}

void OnlyTheBootRequestWaits() {
  Reset(-1);
  pw::ArmForTest(&ModelCheck, 5, 40);
  RequestEntry()(kBoot, 3);  // the boot request: waits to the timeout
  const int callsAfterBoot = g_checkCalls;
  const auto start = std::chrono::steady_clock::now();
  const char* const script[1] = {kWrite};
  RequestEntry()(script, 1);  // a script node's request: the game's call only
  QCHECK(ElapsedMs(start) < 20);
  QCHECK(g_checkCalls == callsAfterBoot);
  QCHECK(pw::CurrentCounts().requests == 2 && pw::CurrentCounts().waits == 1);
  QCHECK(g_events.back() == "request");
}

void ARequestWithoutStoragePermissionsDoesNotWait() {
  Reset(-1);
  pw::ArmForTest(&ModelCheck, 5, 5000);
  const char* const mic[1] = {kMic};
  RequestEntry()(mic, 1);
  QCHECK(g_checkCalls == 0);
  QCHECK(pw::CurrentCounts().waits == 0);
}

void WithoutACheckTheGamesCallIsUntouched() {
  Reset(-1);
  pw::ArmForTest(nullptr, 5, 5000);
  const auto start = std::chrono::steady_clock::now();
  RequestEntry()(kBoot, 3);
  QCHECK(ElapsedMs(start) < 20);
  QCHECK(g_events.size() == 1 && g_events[0] == "request");
  QCHECK(pw::CurrentCounts().waits == 0);
}

void ANullNameListIsSafe() {
  Reset(-1);
  pw::ArmForTest(&ModelCheck, 5, 5000);
  RequestEntry()(nullptr, 3);
  QCHECK(g_checkCalls == 0 && pw::CurrentCounts().waits == 0);
}

int g_afterCalls = 0;
std::size_t g_eventsAtAfter = 0;
pw::StorageOutcome g_afterOutcome = pw::StorageOutcome::kCheckUnavailable;

void AfterStorage(pw::StorageOutcome outcome) noexcept {
  ++g_afterCalls;
  g_eventsAtAfter = g_events.size();
  g_afterOutcome = outcome;
}

void TheAfterStorageStepRunsOnceAfterTheWaitAndBeforeTheCallReturns() {
  Reset(3);
  g_afterCalls = 0;
  pw::ArmForTest(&ModelCheck, 5, 5000);
  pw::SetAfterStorage(&AfterStorage);
  RequestEntry()(kBoot, 3);
  QCHECK(g_afterCalls == 1);
  QCHECK(g_eventsAtAfter == g_events.size());  // every check of the wait had been made when it ran
  QCHECK(pw::CurrentCounts().granted == 1);
  QCHECK(g_afterOutcome == pw::StorageOutcome::kGranted && pw::StorageGranted(g_afterOutcome));
  const char* const script[1] = {kWrite};
  RequestEntry()(script, 1);                   // a later request does not run it again
  QCHECK(g_afterCalls == 1);
}

void AGrantThatWasAlreadyThereIsReportedAsAGrant() {
  Reset(0);
  g_afterCalls = 0;
  pw::ArmForTest(&ModelCheck, 5, 5000);
  pw::SetAfterStorage(&AfterStorage);
  RequestEntry()(kBoot, 3);
  QCHECK(g_afterCalls == 1);
  QCHECK(g_afterOutcome == pw::StorageOutcome::kAlreadyGranted && pw::StorageGranted(g_afterOutcome));
}

void ADeniedPermissionIsReportedAsNoGrant() {
  Reset(-1);
  g_afterCalls = 0;
  pw::ArmForTest(&ModelCheck, 5, 40);
  pw::SetAfterStorage(&AfterStorage);
  RequestEntry()(kBoot, 3);
  QCHECK(pw::CurrentCounts().timeouts == 1);
  QCHECK(g_afterCalls == 1);  // the step is told, and the step does nothing without a grant (data_wiring.cpp)
  QCHECK(g_afterOutcome == pw::StorageOutcome::kTimeout && !pw::StorageGranted(g_afterOutcome));
}

void WithoutACheckNothingIsKnownAboutTheGrant() {
  Reset(-1);
  g_afterCalls = 0;
  pw::ArmForTest(nullptr, 5, 5000);
  pw::SetAfterStorage(&AfterStorage);
  RequestEntry()(kBoot, 3);
  QCHECK(g_afterCalls == 1);
  QCHECK(g_afterOutcome == pw::StorageOutcome::kCheckUnavailable && !pw::StorageGranted(g_afterOutcome));
}

void ARequestWithNoStoragePermissionIsNotAGrant() {
  Reset(0);
  g_afterCalls = 0;
  pw::ArmForTest(&ModelCheck, 5, 5000);
  pw::SetAfterStorage(&AfterStorage);
  const char* const mic[1] = {kMic};
  RequestEntry()(mic, 1);
  QCHECK(g_afterCalls == 1);
  QCHECK(g_afterOutcome == pw::StorageOutcome::kNoStoragePermission && !pw::StorageGranted(g_afterOutcome));
}

void NoAfterStorageStepIsTheGamesCallAlone() {
  Reset(0);
  g_afterCalls = 0;
  pw::ArmForTest(&ModelCheck, 5, 5000);
  RequestEntry()(kBoot, 3);
  QCHECK(g_afterCalls == 0);
}

// ---- the CheckPermission hook: the step on a headset that already holds the microphone permission (#758) ----

bool g_storageGranted = true;

bool GameCheck(const char* permission) noexcept {
  ++g_checkCalls;
  g_events.push_back(std::string("check:") + permission);
  if (permission == kMic) return g_micGranted;
  return g_storageGranted;
}

void UseTheGamesCheck(bool mic, bool storage) {
  g_micGranted = mic;
  g_storageGranted = storage;
  *pw::CheckThunk::OriginalOut() = reinterpret_cast<void*>(&GameCheck);
}

void AGrantedMicrophoneRunsTheStepWithTheStorageAnswerBeforeTheCheckReturns() {
  Reset(0);
  g_afterCalls = 0;
  pw::ArmForTest(nullptr, 5, 5000);
  pw::SetAfterStorage(&AfterStorage);
  UseTheGamesCheck(true, true);
  QCHECK(CheckEntry()(kMic));  // the game's answer is returned untouched
  QCHECK(g_afterCalls == 1);
  QCHECK(g_afterOutcome == pw::StorageOutcome::kAlreadyGranted && pw::StorageGranted(g_afterOutcome));
  QCHECK(g_eventsAtAfter == g_events.size());  // the checks were all made before the step ran
  QCHECK(g_events.size() == 3 && g_events[0] == std::string("check:") + kMic);
  QCHECK(g_events[1] == std::string("check:") + kRead && g_events[2] == std::string("check:") + kWrite);
  QCHECK(g_events.back() != "request");  // the request never came
}

void AGrantedMicrophoneWithDeniedStorageIsReportedAsNotRequested() {
  Reset(0);
  g_afterCalls = 0;
  pw::ArmForTest(nullptr, 5, 5000);
  pw::SetAfterStorage(&AfterStorage);
  UseTheGamesCheck(true, false);
  QCHECK(CheckEntry()(kMic));
  QCHECK(g_afterCalls == 1);
  QCHECK(g_afterOutcome == pw::StorageOutcome::kDeniedNotRequested && !pw::StorageGranted(g_afterOutcome));
}

void ADeniedMicrophoneLeavesTheStepToTheRequestPath() {
  Reset(0);
  g_afterCalls = 0;
  pw::ArmForTest(&ModelCheck, 5, 5000);
  pw::SetAfterStorage(&AfterStorage);
  UseTheGamesCheck(false, true);
  QCHECK(!CheckEntry()(kMic));  // the game's "denied" comes back
  QCHECK(g_afterCalls == 0);
  RequestEntry()(kBoot, 3);     // the request the game makes next
  QCHECK(g_afterCalls == 1);
  QCHECK(g_afterOutcome == pw::StorageOutcome::kAlreadyGranted);
}

void OnlyTheFirstMicrophoneCheckDecides() {
  Reset(0);
  g_afterCalls = 0;
  pw::ArmForTest(nullptr, 5, 5000);
  pw::SetAfterStorage(&AfterStorage);
  UseTheGamesCheck(true, true);
  QCHECK(CheckEntry()(kMic));
  QCHECK(g_afterCalls == 1);
  const int calls = g_checkCalls;
  QCHECK(CheckEntry()(kMic));  // a later microphone check: the game's answer, no extra checks, no step
  QCHECK(g_checkCalls == calls + 1 && g_afterCalls == 1);
}

void ACheckForAnotherPermissionIsTheGamesAnswerAndRunsNothing() {
  Reset(0);
  g_afterCalls = 0;
  pw::ArmForTest(nullptr, 5, 5000);
  pw::SetAfterStorage(&AfterStorage);
  UseTheGamesCheck(true, true);
  QCHECK(CheckEntry()(kWrite));
  QCHECK(g_afterCalls == 0 && g_checkCalls == 1);
  QCHECK(CheckEntry()(kMic));  // the microphone check still decides after it
  QCHECK(g_afterCalls == 1);
}

void TheStepRunsOnceWhenTheRequestCameFirstAndTheMicrophoneIsCheckedAfterwards() {
  Reset(0);
  g_afterCalls = 0;
  pw::ArmForTest(&ModelCheck, 5, 5000);
  pw::SetAfterStorage(&AfterStorage);
  RequestEntry()(kBoot, 3);
  QCHECK(g_afterCalls == 1);
  UseTheGamesCheck(true, true);
  QCHECK(CheckEntry()(kMic));
  QCHECK(g_afterCalls == 1);
}

void TheStepRunsOnceWhenTheCheckCameFirstAndARequestFollows() {
  Reset(0);
  g_afterCalls = 0;
  pw::ArmForTest(&ModelCheck, 5, 5000);
  pw::SetAfterStorage(&AfterStorage);
  UseTheGamesCheck(true, true);
  QCHECK(CheckEntry()(kMic));
  QCHECK(g_afterCalls == 1);
  RequestEntry()(kBoot, 3);
  QCHECK(g_afterCalls == 1);
}

void WithoutTheStepTheCheckIsTheGamesAnswerAlone() {
  Reset(0);
  g_afterCalls = 0;
  pw::ArmForTest(nullptr, 5, 5000);
  UseTheGamesCheck(true, false);
  QCHECK(CheckEntry()(kMic));
  QCHECK(g_afterCalls == 0);
}

void TheCheckThunkWithoutTheHookIsTheGamesCall() {
  Reset(0);  // not armed
  UseTheGamesCheck(false, true);
  QCHECK(!CheckEntry()(kMic));
  QCHECK(g_checkCalls == 1);
}

// ---- storage needed or not, and the reference build's no-op original (#1737 C2) --------------------------------------

bool NotNeeded() noexcept { return false; }
bool Needed() noexcept { return true; }

// Sets the requester answer, the original's kind and the storage predicate. Call after Reset, which clears them.
void ArrangeC2(const char* result, pw::OriginalKind kind, pw::StorageNeededFn needed) {
  ResetRequester(result);
  pw::SetRequester(&Requester);
  pw::SetOriginalKind(kind);
  pw::SetStorageNeeded(needed);
}

void ClassifyOriginalWordPinsBothInputs() {
  QCHECK(pw::ClassifyOriginalWord(0xd10103ffu) == pw::OriginalKind::kCallsJava);
  QCHECK(pw::ClassifyOriginalWord(0xd65f03c0u) == pw::OriginalKind::kReturnsAtOnce);
  QCHECK(pw::ClassifyOriginalWord(0u) == pw::OriginalKind::kUnknown);
  QCHECK(pw::ClassifyOriginalWord(0xd10103feu) == pw::OriginalKind::kUnknown);
  QCHECK(std::strcmp(pw::OriginalKindName(pw::OriginalKind::kCallsJava), "calls_java") == 0);
  QCHECK(std::strcmp(pw::OriginalKindName(pw::OriginalKind::kReturnsAtOnce), "returns_at_once") == 0);
  QCHECK(std::strcmp(pw::OriginalKindName(pw::OriginalKind::kUnknown), "unknown") == 0);
}

void StorageNotNeededAsksOnlyTheMicrophoneAndDoesNotWait() {
  Reset(-1);
  ArrangeC2("requested", pw::OriginalKind::kUnknown, &NotNeeded);
  pw::ArmForTest(&ModelCheck, 5, 5000);
  g_afterCalls = 0;
  pw::SetAfterStorage(&AfterStorage);
  const auto start = std::chrono::steady_clock::now();
  RequestEntry()(kBoot, 3);
  QCHECK(g_requesterCalls == 1 && g_requesterCount == 1 && g_requesterFirst == kMic);
  QCHECK(g_namesSeen == nullptr);  // the game's function was not called
  QCHECK(ElapsedMs(start) < 100);
  QCHECK(pw::CurrentCounts().waits == 0);
  QCHECK(g_afterCalls == 1 && g_afterOutcome == pw::StorageOutcome::kDeniedNotRequested);
  const std::vector<std::string> requests = Lines("permission_request");
  QCHECK(requests.size() == 1 && Has(requests[0], "\"asked\":1") && Has(requests[0], "\"storage_needed\":\"no\"") &&
         Has(requests[0], "\"path\":\"ours\""));
  const std::vector<std::string> waits = Lines("permission_wait");
  QCHECK(!waits.empty() && Has(waits.back(), "\"state\":\"not_requested\"") &&
         Has(waits.back(), "\"reason\":\"storage_not_needed\""));
}

void OnlyStorageNamesAndNotNeededIsHisNoOp() {
  const char* const storageOnly[2] = {kRead, kWrite};
  Reset(-1);  // the storage is denied
  ArrangeC2("requested", pw::OriginalKind::kUnknown, &NotNeeded);
  pw::ArmForTest(&ModelCheck, 5, 5000);
  g_afterCalls = 0;
  pw::SetAfterStorage(&AfterStorage);
  RequestEntry()(storageOnly, 2);
  QCHECK(g_requesterCalls == 0);
  QCHECK(g_namesSeen == nullptr);
  QCHECK(pw::CurrentCounts().no_request == 1);
  const std::vector<std::string> requests = Lines("permission_request");
  QCHECK(requests.size() == 1 && Has(requests[0], "\"path\":\"none\"") && Has(requests[0], "\"result\":\"not_needed\""));
  QCHECK(g_afterCalls == 1 && g_afterOutcome == pw::StorageOutcome::kDeniedNotRequested);

  Reset(0);  // the storage is granted already
  ArrangeC2("requested", pw::OriginalKind::kUnknown, &NotNeeded);
  pw::ArmForTest(&ModelCheck, 5, 5000);
  g_afterCalls = 0;
  pw::SetAfterStorage(&AfterStorage);
  RequestEntry()(storageOnly, 2);
  QCHECK(g_namesSeen == nullptr && pw::CurrentCounts().no_request == 1);
  QCHECK(g_afterCalls == 1 && g_afterOutcome == pw::StorageOutcome::kAlreadyGranted);
}

void StorageNeededKeepsOurRequestAndWait() {
  Reset(3);
  ArrangeC2("requested", pw::OriginalKind::kUnknown, &Needed);
  pw::ArmForTest(&ModelCheck, 5, 5000);
  RequestEntry()(kBoot, 3);
  QCHECK(g_requesterCalls == 1 && g_requesterNames == kBoot && g_requesterCount == 3);
  QCHECK(g_requesterCode == pw::kRequestCode);
  QCHECK(g_namesSeen == nullptr);
  QCHECK(pw::CurrentCounts().waits == 1 && pw::CurrentCounts().granted == 1);
  const std::vector<std::string> requests = Lines("permission_request");
  QCHECK(requests.size() == 1 && Has(requests[0], "\"storage_needed\":\"yes\""));
}

void OnRefTheNoOpOriginalIsNotWaitedOn() {
  Reset(-1);
  ArrangeC2("no_app", pw::OriginalKind::kReturnsAtOnce, &Needed);
  pw::ArmForTest(&ModelCheck, 5, 5000);
  g_afterCalls = 0;
  pw::SetAfterStorage(&AfterStorage);
  const auto start = std::chrono::steady_clock::now();
  RequestEntry()(kBoot, 3);
  QCHECK(ElapsedMs(start) < 200);
  QCHECK(pw::CurrentCounts().waits == 0 && pw::CurrentCounts().timeouts == 0);
  QCHECK(g_afterCalls == 1 && g_afterOutcome == pw::StorageOutcome::kDeniedNotRequested);
  bool called = false;
  for (const std::string& e : g_events) {
    if (e == "request") called = true;
  }
  QCHECK(called);  // the game's function ran; on the reference build it is `ret`
  QCHECK(pw::CurrentCounts().game_requests == 0);
  const std::vector<std::string> requests = Lines("permission_request");
  QCHECK(requests.size() == 1 && Has(requests[0], "\"path\":\"game_noop\""));
  const std::vector<std::string> waits = Lines("permission_wait");
  QCHECK(!waits.empty() && Has(waits.back(), "\"reason\":\"original_no_op\""));
}

void OnRefOurRequestStillWaits() {
  Reset(3);
  ArrangeC2("requested", pw::OriginalKind::kReturnsAtOnce, &Needed);
  pw::ArmForTest(&ModelCheck, 5, 5000);
  RequestEntry()(kBoot, 3);
  QCHECK(pw::CurrentCounts().waits == 1 && pw::CurrentCounts().granted == 1);
  QCHECK(g_namesSeen == nullptr);
  QCHECK(pw::CurrentCounts().game_requests == 0);
}

void OnStockTheGamesRequestWaits() {
  Reset(3);
  ArrangeC2("no_app", pw::OriginalKind::kCallsJava, &Needed);
  pw::ArmForTest(&ModelCheck, 5, 5000);
  RequestEntry()(kBoot, 3);
  QCHECK(g_namesSeen == kBoot && g_countSeen == 3);
  QCHECK(pw::CurrentCounts().waits == 1 && pw::CurrentCounts().granted == 1);
  QCHECK(pw::CurrentCounts().game_requests == 1);
  const std::vector<std::string> requests = Lines("permission_request");
  QCHECK(requests.size() == 1 && Has(requests[0], "\"path\":\"game\""));
}

// Every combination of the three inputs (the original's kind, whether the requester asked, whether storage is needed),
// with the storage denied throughout so a wait can only end at its timeout. Each combination's counts are the ones the
// gate `needed && made` predicts, so removing or inverting the gate fails a row.
void TheWaitIsBoundedOnEveryInput() {
  const pw::OriginalKind kinds[3] = {pw::OriginalKind::kUnknown, pw::OriginalKind::kCallsJava,
                                     pw::OriginalKind::kReturnsAtOnce};
  const char* const results[2] = {"requested", "no_app"};
  const pw::StorageNeededFn neededs[2] = {&Needed, &NotNeeded};
  for (pw::OriginalKind kind : kinds) {
    for (const char* result : results) {
      for (pw::StorageNeededFn needed : neededs) {
        const bool ours = std::strcmp(result, "requested") == 0;
        const bool made = ours || kind != pw::OriginalKind::kReturnsAtOnce;  // a request reached Java
        const bool expectWait = needed == &Needed && made;
        Reset(-1);
        ArrangeC2(result, kind, needed);
        pw::ArmForTest(&ModelCheck, 5, 40);
        g_afterCalls = 0;
        pw::SetAfterStorage(&AfterStorage);
        const auto start = std::chrono::steady_clock::now();
        RequestEntry()(kBoot, 3);
        const long long ms = ElapsedMs(start);
        const pw::Counts c = pw::CurrentCounts();
        QCHECK(g_afterCalls == 1);
        QCHECK(c.requests == 1 && c.no_request == 0 && c.granted == 0);
        QCHECK((g_namesSeen != nullptr) == !ours);                    // the game's function runs iff the requester did not ask
        QCHECK(c.game_requests == (!ours && made ? 1u : 0u));
        QCHECK(c.waits == (expectWait ? 1u : 0u));
        QCHECK(c.timeouts == (expectWait ? 1u : 0u));
        QCHECK(g_afterOutcome == (expectWait ? pw::StorageOutcome::kTimeout : pw::StorageOutcome::kDeniedNotRequested));
        if (expectWait) {
          QCHECK(ms >= 35 && ms < 2000);  // it waited to its timeout, and no longer
        } else {
          QCHECK(ms < 200);  // nothing waited
        }
      }
    }
  }
}

void OnlyTheBootRequestDecidesWhenNotNeeded() {
  Reset(-1);
  ArrangeC2("requested", pw::OriginalKind::kUnknown, &NotNeeded);
  pw::ArmForTest(&ModelCheck, 5, 40);
  g_afterCalls = 0;
  pw::SetAfterStorage(&AfterStorage);
  RequestEntry()(kBoot, 3);
  const int callsAfterBoot = g_checkCalls;
  const char* const script[1] = {kWrite};
  RequestEntry()(script, 1);  // a script node's request: storage only, so nothing is asked and nothing is checked
  QCHECK(pw::CurrentCounts().requests == 2);
  QCHECK(pw::CurrentCounts().waits == 0);
  QCHECK(g_afterCalls == 1);
  QCHECK(g_checkCalls == callsAfterBoot);
}

// ---- install-time classification of libr15's RequestPermissions, on a synthetic libr15 (#1737 C2) ----------------------
// InstallAtWith reads the live module through a lookup. The lookup here returns an in-memory image that carries the build
// ID note and the word at the pinned RequestPermissions address, so the classification and its log line are pinned
// without libr15. The slots are absent from the image, so the install itself fails by design; what is read is the kind.

constexpr std::size_t kNoteAt = 0x100;

struct SyntheticLibr15 {
  unsigned char* bytes = nullptr;
  std::size_t size = 0;
  Elf64_Phdr phdr[2] = {};
  sentinel::ElfImage image;
};

SyntheticLibr15* g_synthetic = nullptr;

bool SyntheticLookup(const char* module, sentinel::ElfImage* out) {
  if (g_synthetic == nullptr || std::strcmp(module, sentinel::pinned::kLibR15) != 0) return false;
  *out = g_synthetic->image;
  return true;
}

bool NoLibr15(const char*, sentinel::ElfImage*) { return false; }

int HexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

// Decodes `hex` (2n lowercase digits) into `out`; false on any other shape.
bool DecodeHex(const char* hex, unsigned char* out, std::size_t n) {
  if (std::strlen(hex) != 2 * n) return false;
  for (std::size_t i = 0; i < n; ++i) {
    const int hi = HexNibble(hex[2 * i]);
    const int lo = HexNibble(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = static_cast<unsigned char>((hi << 4) | lo);
  }
  return true;
}

// Builds the image: a PT_LOAD over the whole buffer, a PT_NOTE with the GNU build ID `buildId` (20 bytes of hex), and
// `wordAtRequest` at the pinned RequestPermissions address.
void BuildSyntheticLibr15(SyntheticLibr15* s, const char* buildId, std::uint32_t wordAtRequest) {
  constexpr std::size_t kDescBytes = 20;
  s->size = static_cast<std::size_t>(sentinel::pinned::kRequestPermissionsVaddr) + 64;
  s->bytes = static_cast<unsigned char*>(std::calloc(1, s->size));
  QCHECK(s->bytes != nullptr);
  if (s->bytes == nullptr) return;
  unsigned char desc[kDescBytes] = {};
  QCHECK(DecodeHex(buildId, desc, kDescBytes));
  const Elf64_Nhdr nh{4, kDescBytes, NT_GNU_BUILD_ID};
  std::memcpy(s->bytes + kNoteAt, &nh, sizeof(nh));
  std::memcpy(s->bytes + kNoteAt + sizeof(nh), "GNU", 4);
  std::memcpy(s->bytes + kNoteAt + sizeof(nh) + 4, desc, kDescBytes);
  std::memcpy(s->bytes + sentinel::pinned::kRequestPermissionsVaddr, &wordAtRequest, sizeof(wordAtRequest));
  s->phdr[0].p_type = PT_LOAD;
  s->phdr[0].p_vaddr = 0;
  s->phdr[0].p_filesz = s->size;
  s->phdr[0].p_memsz = s->size;
  s->phdr[1].p_type = PT_NOTE;
  s->phdr[1].p_vaddr = kNoteAt;
  s->phdr[1].p_filesz = sizeof(nh) + 4 + kDescBytes;
  s->phdr[1].p_memsz = sizeof(nh) + 4 + kDescBytes;
  s->image.base = reinterpret_cast<std::uintptr_t>(s->bytes);
  s->image.phdr = s->phdr;
  s->image.phnum = 2;
}

sentinel::GotTarget AbsentSlot() {
  return sentinel::GotTarget{"absent.so", "absent_symbol", sentinel::RelocKind::kJumpSlot, nullptr, std::nullopt, nullptr};
}

// Installs over a synthetic image (or none) and returns the kind the handler now acts on. `*line` gets the one
// permission_wait_original line; `warnings` the WARN lines of the install.
pw::OriginalKind InstallOver(sentinel::ImageLookup lookup, const char* buildId, std::uint32_t word, std::string* line,
                             std::vector<std::string>* warnings) {
  SyntheticLibr15 s;
  if (lookup == &SyntheticLookup) {
    BuildSyntheticLibr15(&s, buildId, word);
    g_synthetic = &s;
  }
  Reset(0);
  g_lines.clear();
  g_warnings.clear();
  const bool installed = pw::InstallAtWith(lookup, AbsentSlot(), AbsentSlot(), &ModelCheck);
  g_synthetic = nullptr;
  QCHECK(!installed);  // the absent slots are not found, so the install is refused; the classification is what is read
  const std::vector<std::string> lines = Lines("permission_wait_original");
  QCHECK(lines.size() == 1);
  if (lines.size() == 1 && line != nullptr) *line = lines[0];
  if (warnings != nullptr) {  // only this install's kind lines: the install may log its own WARN lines too
    warnings->clear();
    for (const std::string& w : g_warnings) {
      if (Has(w, "\"event\":\"permission_wait_original\"")) warnings->push_back(w);
    }
  }
  std::free(s.bytes);
  return pw::CurrentOriginalKind();
}

// The pinned build ID of the patched libr15 (the one the live check compares against).
const char* PinnedId() { return sentinel::pinned::LibR15PatchedBuildId(); }
const char* OtherId() { return "0000000000000000000000000000000000000000"; }

void InstallClassifiesTheStockOriginalAsCallingJava() {
  std::string line;
  std::vector<std::string> warnings;
  const pw::OriginalKind kind = InstallOver(&SyntheticLookup, PinnedId(), sentinel::pinned::kRequestPermissionsStockWord, &line, &warnings);
  QCHECK(kind == pw::OriginalKind::kCallsJava);
  QCHECK(Has(line, "\"kind\":\"calls_java\"") && Has(line, "\"reason\":\"resolved\""));
  QCHECK(warnings.empty());  // the stock build is the expected one: INFO, not WARN
}

void InstallClassifiesTheReferenceOriginalAsReturningAtOnce() {
  std::string line;
  std::vector<std::string> warnings;
  const pw::OriginalKind kind = InstallOver(&SyntheticLookup, PinnedId(), sentinel::pinned::kRequestPermissionsReturnWord, &line, &warnings);
  QCHECK(kind == pw::OriginalKind::kReturnsAtOnce);
  QCHECK(Has(line, "\"kind\":\"returns_at_once\"") && Has(line, "\"reason\":\"resolved\""));
  QCHECK(Has(line, "\"effect\":"));  // Category G: the line says what the kind does
  QCHECK(warnings.empty());
}

void InstallClassifiesAnyOtherWordAsUnknownWithAWarning() {
  std::string line;
  std::vector<std::string> warnings;
  const pw::OriginalKind kind = InstallOver(&SyntheticLookup, PinnedId(), 0xd10103feu, &line, &warnings);
  QCHECK(kind == pw::OriginalKind::kUnknown);
  QCHECK(Has(line, "\"kind\":\"unknown\"") && Has(line, "\"reason\":\"prologue_mismatch\""));
  QCHECK(warnings.size() == 1);  // the unknown kind is a WARN, with the address and both expected words and the actual one
  if (warnings.size() == 1) {
    QCHECK(Has(warnings[0], "\"va\":\"0x0000000000f873ec\""));
    QCHECK(Has(warnings[0], "\"expected_calls_java\":\"0x00000000d10103ff\""));
    QCHECK(Has(warnings[0], "\"expected_returns_at_once\":\"0x00000000d65f03c0\""));
    QCHECK(Has(warnings[0], "\"actual\":\"0x00000000d10103fe\""));
  }
}

void AWrongBuildIdIsNotTrustedEvenWithAPinnedWord() {
  std::string line;
  std::vector<std::string> warnings;
  const pw::OriginalKind kind = InstallOver(&SyntheticLookup, OtherId(), sentinel::pinned::kRequestPermissionsStockWord, &line, &warnings);
  QCHECK(kind == pw::OriginalKind::kUnknown);
  QCHECK(Has(line, "\"reason\":\"build_id_mismatch\""));
  QCHECK(warnings.size() == 1);
}

void AModuleThatIsNotLoadedIsUnknown() {
  std::string line;
  std::vector<std::string> warnings;
  const pw::OriginalKind kind = InstallOver(&NoLibr15, nullptr, 0, &line, &warnings);
  QCHECK(kind == pw::OriginalKind::kUnknown);
  QCHECK(Has(line, "\"reason\":\"module_not_loaded\""));
  QCHECK(warnings.size() == 1);
}

}  // namespace

// A player who taps Deny waits out the cap before the game's own error: it is what a person waits (30 s).
void TheProductionCapIsWhatAPlayerWaits() {
  QCHECK(pw::kTimeoutMs == 30000);
  QCHECK(pw::kPollMs <= 500);
}

// #947: the game's RequestPermissions passes no request code, so the request is made through the requester with
// kRequestCode, and the game's function is not called.
void WithARequesterTheRequestCarriesTheHooksCodeAndTheGamesCallIsNotMade() {
  Reset(0);
  ResetRequester("requested");
  pw::ArmForTest(&ModelCheck, 5, 5000);
  pw::SetRequester(&Requester);
  RequestEntry()(kBoot, 3);
  QCHECK(g_requesterCalls == 1);
  QCHECK(g_requesterNames == kBoot && g_requesterCount == 3);  // the game's own arguments
  QCHECK(g_requesterCode == pw::kRequestCode);
  QCHECK(g_namesSeen == nullptr);                              // the game's function never ran
  QCHECK(!g_events.empty() && g_events[0] == "ours");
  for (const std::string& e : g_events) QCHECK(e != "request");
  QCHECK(g_checkCalls == 2);                                   // the boot wait still ran after the request
  QCHECK(pw::CurrentCounts().requests == 1 && pw::CurrentCounts().game_requests == 0);
  const std::vector<std::string> lines = Lines("permission_request");
  QCHECK(lines.size() == 1);
  if (lines.size() == 1) {
    QCHECK(Has(lines[0], "\"result\":\"requested\""));
    QCHECK(Has(lines[0], "\"path\":\"ours\""));
    QCHECK(Has(lines[0], "\"permissions\":3"));
    QCHECK(Has(lines[0], "\"request_code\":1"));
  }
}

void ARequesterThatCouldNotAskLeavesTheRequestToTheGame() {
  Reset(0);
  ResetRequester("no_app");
  pw::ArmForTest(&ModelCheck, 5, 5000);
  pw::SetRequester(&Requester);
  RequestEntry()(kBoot, 3);
  QCHECK(g_requesterCalls == 1);
  QCHECK(g_namesSeen == kBoot && g_countSeen == 3);  // the game's call, with its own arguments
  QCHECK(g_events.size() >= 2 && g_events[0] == "ours" && g_events[1] == "request");
  QCHECK(pw::CurrentCounts().requests == 1 && pw::CurrentCounts().game_requests == 1);
  const std::vector<std::string> lines = Lines("permission_request");
  QCHECK(lines.size() == 1);
  if (lines.size() == 1) {
    QCHECK(Has(lines[0], "\"result\":\"no_app\""));
    QCHECK(Has(lines[0], "\"path\":\"game\""));
    QCHECK(!Has(lines[0], "request_code"));  // the game's call carries no code of ours
  }
}

void WithoutARequesterTheGameAsksAndTheLineSaysSo() {
  Reset(0);
  ResetRequester("requested");
  pw::ArmForTest(&ModelCheck, 5, 5000);
  RequestEntry()(kBoot, 3);
  QCHECK(g_requesterCalls == 0);
  QCHECK(g_namesSeen == kBoot);
  const std::vector<std::string> lines = Lines("permission_request");
  QCHECK(lines.size() == 1 && Has(lines[0], "\"result\":\"no_requester\"") && Has(lines[0], "\"path\":\"game\""));
}

void EveryRequestGoesThroughTheRequesterAndOnlyTheFirstFewAreLogged() {
  Reset(0);
  ResetRequester("requested");
  pw::ArmForTest(&ModelCheck, 5, 5000);
  pw::SetRequester(&Requester);
  const char* const script[1] = {kMic};
  for (unsigned i = 0; i < pw::kMaxLoggedRequests + 3; ++i) RequestEntry()(i == 0 ? kBoot : script, i == 0 ? 3 : 1);
  QCHECK(g_requesterCalls == static_cast<int>(pw::kMaxLoggedRequests + 3));  // the script nodes' requests too
  QCHECK(g_requesterCode == pw::kRequestCode);
  QCHECK(g_namesSeen == nullptr);
  QCHECK(Lines("permission_request").size() == pw::kMaxLoggedRequests);
  QCHECK(pw::CurrentCounts().requests == pw::kMaxLoggedRequests + 3);
}

void TheRequestCodeIsOneTheActivityAccepts() {
  QCHECK(pw::kRequestCode >= 0);  // Activity.requestPermissions throws for a negative code
}

int main() {
  sentinel::SetLogSink(&Sink);
  TheProductionCapIsWhatAPlayerWaits();
  ARequestWithoutTheHookIsJustTheGamesCall();
  TheBootRequestWaitsOnTheCallersThreadUntilTheStoragePermissionsAreGranted();
  ARequestForPermissionsAlreadyGrantedDoesNotWait();
  ADeniedPermissionEndsTheWaitAtTheTimeout();
  OnlyTheBootRequestWaits();
  ARequestWithoutStoragePermissionsDoesNotWait();
  WithoutACheckTheGamesCallIsUntouched();
  ANullNameListIsSafe();
  TheAfterStorageStepRunsOnceAfterTheWaitAndBeforeTheCallReturns();
  AGrantThatWasAlreadyThereIsReportedAsAGrant();
  ADeniedPermissionIsReportedAsNoGrant();
  WithoutACheckNothingIsKnownAboutTheGrant();
  ARequestWithNoStoragePermissionIsNotAGrant();
  NoAfterStorageStepIsTheGamesCallAlone();
  AGrantedMicrophoneRunsTheStepWithTheStorageAnswerBeforeTheCheckReturns();
  AGrantedMicrophoneWithDeniedStorageIsReportedAsNotRequested();
  ADeniedMicrophoneLeavesTheStepToTheRequestPath();
  OnlyTheFirstMicrophoneCheckDecides();
  ACheckForAnotherPermissionIsTheGamesAnswerAndRunsNothing();
  TheStepRunsOnceWhenTheRequestCameFirstAndTheMicrophoneIsCheckedAfterwards();
  TheStepRunsOnceWhenTheCheckCameFirstAndARequestFollows();
  WithoutTheStepTheCheckIsTheGamesAnswerAlone();
  TheCheckThunkWithoutTheHookIsTheGamesCall();
  WithARequesterTheRequestCarriesTheHooksCodeAndTheGamesCallIsNotMade();
  ARequesterThatCouldNotAskLeavesTheRequestToTheGame();
  WithoutARequesterTheGameAsksAndTheLineSaysSo();
  EveryRequestGoesThroughTheRequesterAndOnlyTheFirstFewAreLogged();
  TheRequestCodeIsOneTheActivityAccepts();
  ClassifyOriginalWordPinsBothInputs();
  StorageNotNeededAsksOnlyTheMicrophoneAndDoesNotWait();
  OnlyStorageNamesAndNotNeededIsHisNoOp();
  StorageNeededKeepsOurRequestAndWait();
  OnRefTheNoOpOriginalIsNotWaitedOn();
  OnRefOurRequestStillWaits();
  OnStockTheGamesRequestWaits();
  TheWaitIsBoundedOnEveryInput();
  OnlyTheBootRequestDecidesWhenNotNeeded();
  InstallClassifiesTheStockOriginalAsCallingJava();
  InstallClassifiesTheReferenceOriginalAsReturningAtOnce();
  InstallClassifiesAnyOtherWordAsUnknownWithAWarning();
  AWrongBuildIdIsNotTrustedEvenWithAPinnedWord();
  AModuleThatIsNotLoadedIsUnknown();
  sentinel::SetLogSink(nullptr);
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "permission_wait_hook_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("permission_wait_hook_test: all checks pass\n");
  return 0;
}
