// Host test for "the game waits for the storage permission it just asked for" (#640): the real handler
// (src/quest/sentinel/permission_wait_hook.cpp), built without exceptions as the sentinel builds it, driven through
// the thunk's entry with a stand-in for libr15's CSysInfo::RequestPermissions and a model of CheckPermission. No GOT
// slot is touched: got_pinned_test.cpp resolves the pinned slot and CheckPermission's prologue in the real library.
//
// Built and run by `just test-quest-hooks`.

#include <pthread.h>
#include <time.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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
void Sink(sentinel::LogLevel, const char* line) { g_lines.emplace_back(line); }
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

void TheWaitIsBoundedOnEveryInput() {
  const pw::OriginalKind kinds[3] = {pw::OriginalKind::kUnknown, pw::OriginalKind::kCallsJava,
                                     pw::OriginalKind::kReturnsAtOnce};
  const char* const results[2] = {"requested", "no_app"};
  const pw::StorageNeededFn neededs[2] = {&Needed, &NotNeeded};
  for (pw::OriginalKind kind : kinds) {
    for (const char* result : results) {
      for (pw::StorageNeededFn needed : neededs) {
        Reset(-1);
        ArrangeC2(result, kind, needed);
        pw::ArmForTest(&ModelCheck, 5, 40);
        g_afterCalls = 0;
        pw::SetAfterStorage(&AfterStorage);
        const auto start = std::chrono::steady_clock::now();
        RequestEntry()(kBoot, 3);
        QCHECK(ElapsedMs(start) < 2000);
        QCHECK(g_afterCalls == 1);
        const pw::Counts c = pw::CurrentCounts();
        QCHECK((c.waits == 1 && c.timeouts == 1) || c.waits == 0);
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
  sentinel::SetLogSink(nullptr);
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "permission_wait_hook_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("permission_wait_hook_test: all checks pass\n");
  return 0;
}
