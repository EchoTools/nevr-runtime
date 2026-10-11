// Host test for the OBB-mount skip (#319): the real handlers (src/quest/sentinel/obb_skip_hook.cpp), built
// without exceptions as the sentinel builds them, driven through the thunks' entries with stand-ins for
// AStorageManager_mountObb, AStorageManager_getMountedObbPath and the game's own callback
// (CSysFile_OnObbStateChange, libr15 0xf86c28) and wait loop (CSysFile::Init 0xf85260), modelled from the
// disassembly described in pinned_targets.h. No GOT slot is touched: got_pinned_test.cpp resolves the two
// pinned slots in the real library, and tests/quest TestHookFramesCarryNoPersonality covers the frames.
//
// Built and run by `just test-quest-hooks`.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "hook_log.h"
#include "hook_report.h"
#include "obb_skip_hook.h"
#include "quest/tests/test_check.h"

namespace {

namespace obb = nevr_quest::obb_skip;
using sentinel::pinned::AStorageManagerOpaque;
using sentinel::pinned::ObbCallbackFn;

// The game's globals this code touches: the completion flag byte (0x37623c0), gOBBPath (0x3762470) and the
// storage manager pointer (0x37623e0).
struct GameState {
  unsigned char complete = 0;
  char obbPath[512] = {};
  AStorageManagerOpaque* manager = nullptr;
  int callbackCalls = 0;
  std::string lastFilename;
  std::int32_t lastState = 0;
  void* lastData = nullptr;
};
GameState g_game;

// What the platform does on Horizon OS: the mount is refused and the callback never runs; no path exists.
int g_realMountCalls = 0;
int g_realPathCalls = 0;
void RealMountRefused(AStorageManagerOpaque*, const char*, const char*, ObbCallbackFn, void*) { ++g_realMountCalls; }
const char* RealPathNone(AStorageManagerOpaque*, const char*) {
  ++g_realPathCalls;
  return nullptr;
}

obb::MountThunk::Fn MountEntry() { return reinterpret_cast<obb::MountThunk::Fn>(obb::MountThunk::EntryAddress()); }
obb::PathThunk::Fn PathEntry() { return reinterpret_cast<obb::PathThunk::Fn>(obb::PathThunk::EntryAddress()); }

// CSysFile_OnObbStateChange, state 1: asks for the mounted path with the manager and the file it recorded,
// copies a non-null result into gOBBPath, and sets the completion flag. Other states set the flag without a
// path. It uses no argument but `state` (0xf86c34: sub w9, w1, #1).
void GameCallback(const char* filename, std::int32_t state, void* data) {
  ++g_game.callbackCalls;
  g_game.lastFilename = filename != nullptr ? filename : "";
  g_game.lastState = state;
  g_game.lastData = data;
  if (state == 1) {
    const char* path = PathEntry()(g_game.manager, "<recorded obb path>");
    if (path != nullptr) {
      std::strncpy(g_game.obbPath, path, sizeof(g_game.obbPath) - 1);
    } else {
      std::memset(g_game.obbPath, 0, sizeof(g_game.obbPath));
    }
  }
  g_game.complete = 1;
}

// CSysFile::Init's wait: clear the flag, mountObb, then poll (a sleep each) while the flag is clear, for at
// most `maxPolls`. Returns the number of polls it slept.
int GameInit(int maxPolls) {
  g_game.complete = 0;
  g_game.manager = reinterpret_cast<AStorageManagerOpaque*>(0x1234);
  MountEntry()(g_game.manager, "/sdcard/Android/obb/com.readyatdawn.r15/main.obb", "37c70a1635a1ad7a", &GameCallback,
               nullptr);
  int polls = 0;
  while (g_game.complete == 0 && polls < maxPolls) ++polls;
  return polls;
}

std::string GetDataRootDir() {
  return g_game.obbPath[0] != '\0' ? std::string(g_game.obbPath) : std::string("/storage/emulated/0/readyatdawn");
}

void Reset() {
  g_game = GameState();
  g_realMountCalls = g_realPathCalls = 0;
  obb::ResetForTest();
  obb::MountThunk::Reset();
  obb::PathThunk::Reset();
  *obb::MountThunk::OriginalOut() = reinterpret_cast<void*>(&RealMountRefused);
  *obb::PathThunk::OriginalOut() = reinterpret_cast<void*>(&RealPathNone);
}

void WithoutTheHookTheGameWaitsOutItsWholePollingBudget() {
  Reset();  // thunks not armed: the entries call straight through to the platform
  const int polls = GameInit(1000);
  QCHECK(polls == 1000);
  QCHECK(g_game.complete == 0);
  QCHECK(g_realMountCalls == 1);
  QCHECK(g_game.callbackCalls == 0);
}

void ASkippedMountCompletesImmediatelyWithTheFallbackRoot() {
  Reset();
  obb::ArmForTest();
  const int polls = GameInit(1000);
  QCHECK(polls == 0);  // the callback ran inside mountObb: the wait loop is never entered
  QCHECK(g_game.complete == 1);
  QCHECK(g_game.callbackCalls == 1);
  QCHECK(g_game.lastState == sentinel::pinned::kObbStateMounted);
  QCHECK(g_game.lastFilename == "/sdcard/Android/obb/com.readyatdawn.r15/main.obb");
  QCHECK(g_game.lastData == nullptr);
  QCHECK(std::strcmp(g_game.obbPath, sentinel::pinned::kObbFallbackDataRoot) == 0);
  QCHECK(GetDataRootDir() == "/storage/emulated/0/readyatdawn");
  QCHECK(g_realMountCalls == 0);  // the platform never saw the refused mount
  QCHECK(g_realPathCalls == 0);
  const obb::Counts counts = obb::CurrentCounts();
  QCHECK(counts.mountSkipped == 1 && counts.pathServed == 1);
}

void AMountWithoutACallbackGoesToThePlatformAndSkipsNothing() {
  Reset();
  obb::ArmForTest();
  MountEntry()(reinterpret_cast<AStorageManagerOpaque*>(0x1), "file", "key", nullptr, nullptr);
  QCHECK(g_realMountCalls == 1);
  QCHECK(obb::CurrentCounts().mountSkipped == 0);
  // No skip happened, so the path request is the platform's.
  QCHECK(PathEntry()(reinterpret_cast<AStorageManagerOpaque*>(0x1), "file") == nullptr);
  QCHECK(g_realPathCalls == 1);
  QCHECK(obb::CurrentCounts().pathServed == 0);
}

void APathRequestBeforeAnySkipIsTheOriginals() {
  Reset();
  obb::ArmForTest();
  QCHECK(PathEntry()(nullptr, "file") == nullptr);
  QCHECK(g_realPathCalls == 1);
}

void EveryLaterPathRequestAfterTheSkipGetsTheFallbackRoot() {
  Reset();
  obb::ArmForTest();
  GameInit(10);
  const char* again = PathEntry()(g_game.manager, "other");
  QCHECK(again != nullptr && std::strcmp(again, sentinel::pinned::kObbFallbackDataRoot) == 0);
  QCHECK(obb::CurrentCounts().pathServed == 2);
}

void ADisarmedHookPassesEverythingThrough() {
  Reset();
  obb::ArmForTest();
  obb::MountThunk::Disarm();
  obb::PathThunk::Disarm();
  const int polls = GameInit(50);
  QCHECK(polls == 50);
  QCHECK(g_realMountCalls == 1);
  QCHECK(obb::CurrentCounts().mountSkipped == 0);
}

void AThunkWithoutAnOriginalIsAFaultAndSkipsNothing() {
  Reset();
  obb::ArmForTest();
  *obb::MountThunk::OriginalOut() = nullptr;
  const int polls = GameInit(7);
  QCHECK(polls == 7);
  QCHECK(g_game.callbackCalls == 0);
  QCHECK(obb::MountThunk::Faults() == 1);
}

void TheFourCountersRegisterAndLeaveTheRestOfTheTable() {
  sentinel::StopReporter();
  QCHECK(obb::RegisterCounters());
  // The four counters are in the table; the rest of it is still free, and a full table refuses a registration
  // rather than dropping it silently.
  int extra = 0;
  static std::atomic<std::uint64_t> spare[sentinel::kMaxReportCounters];
  while (extra < static_cast<int>(sentinel::kMaxReportCounters) &&
         sentinel::RegisterReportCounter("spare", &spare[extra])) {
    ++extra;
  }
  QCHECK(extra == static_cast<int>(sentinel::kMaxReportCounters) - obb::kCounterCount);
  sentinel::StopReporter();
}

}  // namespace

int main() {
  WithoutTheHookTheGameWaitsOutItsWholePollingBudget();
  ASkippedMountCompletesImmediatelyWithTheFallbackRoot();
  AMountWithoutACallbackGoesToThePlatformAndSkipsNothing();
  APathRequestBeforeAnySkipIsTheOriginals();
  EveryLaterPathRequestAfterTheSkipGetsTheFallbackRoot();
  ADisarmedHookPassesEverythingThrough();
  AThunkWithoutAnOriginalIsAFaultAndSkipsNothing();
  TheFourCountersRegisterAndLeaveTheRestOfTheTable();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "obb_skip_hook_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("obb_skip_hook_test: all checks pass\n");
  return 0;
}
