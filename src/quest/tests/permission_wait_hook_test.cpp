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
#include <string>
#include <vector>

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

}  // namespace

// A player who taps Deny waits out the cap before the game's own error: it is what a person waits (30 s).
void TheProductionCapIsWhatAPlayerWaits() {
  QCHECK(pw::kTimeoutMs == 30000);
  QCHECK(pw::kPollMs <= 500);
}

int main() {
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
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "permission_wait_hook_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("permission_wait_hook_test: all checks pass\n");
  return 0;
}
