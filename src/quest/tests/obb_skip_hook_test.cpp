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
#include <sys/stat.h>
#include <unistd.h>

#include <set>
#include <string>
#include <vector>

#include "hook_log.h"
#include "hook_report.h"
#include "obb_skip_hook.h"
#include "quest/data/plan.h"
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

// CSysInfo::GetDataRootDir with no gOBBPath recorded: the legacy literal (libr15 0xf87250).
int g_realDataRootCalls = 0;
const char* RealDataRoot() {
  ++g_realDataRootCalls;
  return sentinel::pinned::kObbFallbackDataRoot;
}

obb::DataRootThunk::Fn DataRootEntry() {
  return reinterpret_cast<obb::DataRootThunk::Fn>(obb::DataRootThunk::EntryAddress());
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
  g_realMountCalls = g_realPathCalls = g_realDataRootCalls = 0;
  obb::ResetForTest();
  obb::MountThunk::Reset();
  obb::PathThunk::Reset();
  obb::DataRootThunk::Reset();
  *obb::DataRootThunk::OriginalOut() = reinterpret_cast<void*>(&RealDataRoot);
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

// The filesystem, as the resolver sees it (#749): the paths that exist.
std::set<std::string> g_existing;
bool FakeExists(const char* path) noexcept { return g_existing.count(path) != 0; }

std::vector<std::string> g_lines;
void CaptureLine(sentinel::LogLevel, const char* line) { g_lines.emplace_back(line); }

constexpr const char* kMediaDir = "/sdcard/Android/media/com.readyatdawn.r15/files";
constexpr const char* kMediaData = "/sdcard/Android/media/com.readyatdawn.r15/files/_data";
constexpr const char* kLegacyData = "/storage/emulated/0/readyatdawn/_data";
// The file the data step takes as "this version of the data is there" (quest/data/plan.h): a root holding it is complete.
const std::string kMediaManifest = std::string(kMediaDir) + "/" + nevr_quest::data::kManifestPath;
const std::string kLegacyManifest = std::string("/storage/emulated/0/readyatdawn/") + nevr_quest::data::kManifestPath;

bool SomeLineContains(const char* needle) {
  for (const std::string& l : g_lines) {
    if (l.find(needle) != std::string::npos) return true;
  }
  return false;
}

void TheAndroidMediaRootWinsWhenBothHoldTheData() {
  Reset();
  g_existing = {kMediaData, kLegacyData};
  const nevr_quest::data_root::Resolution r = nevr_quest::data_root::Resolve(&FakeExists);
  QCHECK(r.found);
  QCHECK(std::strcmp(r.source, "android_media") == 0);
  QCHECK(std::strcmp(r.root, "/sdcard/Android/media/com.readyatdawn.r15/files") == 0);
}

void TheLegacyRootIsServedWhenOnlyItHoldsTheData() {
  Reset();
  g_existing = {kLegacyData};
  const nevr_quest::data_root::Resolution r = nevr_quest::data_root::Resolve(&FakeExists);
  QCHECK(r.found);
  QCHECK(std::strcmp(r.source, "legacy") == 0);
  QCHECK(std::strcmp(r.root, "/storage/emulated/0/readyatdawn") == 0);
}

void ARootWithoutTheDataDirectoryDoesNotBeatARootThatHasIt() {
  Reset();
  g_existing = {kMediaDir, "/sdcard/Android/media/com.readyatdawn.r15/asset_patches", kLegacyData};
  const nevr_quest::data_root::Resolution r = nevr_quest::data_root::Resolve(&FakeExists);
  QCHECK(std::strcmp(r.source, "legacy") == 0);
}

void TheMediaDirectoryWithoutTheDataDirectoryIsServedAndSaidSo() {
  Reset();
  g_lines.clear();
  g_existing = {kMediaDir};
  const nevr_quest::data_root::Resolution r = nevr_quest::data_root::Resolve(&FakeExists);
  QCHECK(r.found);
  QCHECK(std::strcmp(r.source, "android_media_unmarked") == 0);
  QCHECK(std::strcmp(r.root, kMediaDir) == 0);
  sentinel::LogSink previous = sentinel::SetLogSink(&CaptureLine);
  obb::ResolveRoot(&FakeExists);
  sentinel::SetLogSink(previous);
  QCHECK(SomeLineContains("\"source\":\"android_media_unmarked\""));
  QCHECK(SomeLineContains("\"level\":\"warn\""));
}

// #757: the media directory without _data AND the legacy root without _data. The unmarked tier is the media
// directory, so the legacy directory's existence changes nothing.
void TheMediaDirectoryBeatsALegacyRootThatAlsoHoldsNoData() {
  Reset();
  g_lines.clear();
  g_existing = {kMediaDir, "/storage/emulated/0/readyatdawn"};
  const nevr_quest::data_root::Resolution r = nevr_quest::data_root::Resolve(&FakeExists);
  QCHECK(r.found);
  QCHECK(std::strcmp(r.source, "android_media_unmarked") == 0);
  QCHECK(std::strcmp(r.root, kMediaDir) == 0);
  sentinel::LogSink previous = sentinel::SetLogSink(&CaptureLine);
  obb::ResolveRoot(&FakeExists);
  sentinel::SetLogSink(previous);
  QCHECK(g_lines.size() == 1);
  QCHECK(SomeLineContains("\"source\":\"android_media_unmarked\""));
  QCHECK(SomeLineContains(kMediaDir));
}

void NeitherRootHoldingTheDataIsFoundFalseWithTheGamesOwnFallback() {
  Reset();
  g_existing = {};
  const nevr_quest::data_root::Resolution r = nevr_quest::data_root::Resolve(&FakeExists);
  QCHECK(!r.found);
  QCHECK(std::strcmp(r.source, "none") == 0);
  QCHECK(std::strcmp(r.root, sentinel::pinned::kObbFallbackDataRoot) == 0);
}

void ASkippedMountServesTheResolvedRootAndTheGameReadsFromIt() {
  Reset();
  g_lines.clear();
  sentinel::LogSink previous = sentinel::SetLogSink(&CaptureLine);
  g_existing = {kMediaData};
  obb::ResolveRoot(&FakeExists);
  obb::ArmForTest();
  GameInit(1000);
  sentinel::SetLogSink(previous);
  QCHECK(GetDataRootDir() == "/sdcard/Android/media/com.readyatdawn.r15/files");
  QCHECK(SomeLineContains("\"event\":\"data_root\""));
  QCHECK(SomeLineContains("\"result\":\"found\""));
  QCHECK(SomeLineContains("\"source\":\"android_media\""));
  QCHECK(SomeLineContains("\"level\":\"info\""));  // a root found is an operator line, not a warning (#757)
}

void WithNoRootTheGameKeepsItsFallbackAndOneErrorLineNamesBothPaths() {
  Reset();
  g_lines.clear();
  sentinel::LogSink previous = sentinel::SetLogSink(&CaptureLine);
  g_existing = {};
  obb::ResolveRoot(&FakeExists);
  obb::ArmForTest();
  GameInit(1000);
  sentinel::SetLogSink(previous);
  QCHECK(GetDataRootDir() == "/storage/emulated/0/readyatdawn");
  QCHECK(g_lines.size() == 1);
  QCHECK(SomeLineContains("\"result\":\"not_found\""));
  QCHECK(SomeLineContains("\"level\":\"error\""));  // the one line that says the game will fail is an error (#757)
  QCHECK(SomeLineContains("/sdcard/Android/media/com.readyatdawn.r15/files"));
  QCHECK(SomeLineContains("/storage/emulated/0/readyatdawn"));
}

// ---- the engine's data-root question (#866): no OBB file, so no mount and no path request ever reaches the hooks ----

void NoObbFileTheEngineAsksForTheDataRootAndIsToldTheMediaRootThatHoldsTheData() {
  Reset();
  g_existing = {kMediaData, kMediaManifest, kLegacyData};
  obb::ResolveRoot(&FakeExists);
  obb::ArmForTest();
  const char* root = DataRootEntry()();  // the game's call, and no mountObb before it
  QCHECK(root != nullptr && std::strcmp(root, kMediaDir) == 0);
  QCHECK(g_realDataRootCalls == 0 && g_realMountCalls == 0 && g_realPathCalls == 0);
  QCHECK(obb::CurrentCounts().dataRootServed == 1 && obb::CurrentCounts().mountSkipped == 0);
  QCHECK(obb::DataRootRedirect() != nullptr && std::strcmp(obb::DataRootRedirect(), kMediaDir) == 0);
  // Every later question gets the same answer: the two callers both read it once, at boot.
  QCHECK(std::strcmp(DataRootEntry()(), kMediaDir) == 0 && obb::CurrentCounts().dataRootServed == 2);
}

void TheEngineKeepsItsOwnAnswerWhenOnlyTheLegacyRootHoldsTheData() {
  Reset();
  g_existing = {kLegacyData};
  obb::ResolveRoot(&FakeExists);
  obb::ArmForTest();
  const char* root = DataRootEntry()();
  QCHECK(root != nullptr && std::strcmp(root, "/storage/emulated/0/readyatdawn") == 0);
  QCHECK(g_realDataRootCalls == 1 && obb::CurrentCounts().dataRootServed == 0);
  QCHECK(obb::DataRootRedirect() == nullptr);
}

void TheEngineKeepsItsOwnAnswerForAnUnmarkedMediaDirectoryAndForNoRoot() {
  for (int unmarked = 0; unmarked < 2; ++unmarked) {
    Reset();
    g_existing = unmarked != 0 ? std::set<std::string>{kMediaDir} : std::set<std::string>{};
    obb::ResolveRoot(&FakeExists);
    obb::ArmForTest();
    QCHECK(std::strcmp(DataRootEntry()(), "/storage/emulated/0/readyatdawn") == 0);
    QCHECK(g_realDataRootCalls == 1 && obb::CurrentCounts().dataRootServed == 0);
    QCHECK(obb::DataRootRedirect() == nullptr);
  }
}

void NothingIsRedirectedBeforeTheResolverHasRun() {
  Reset();
  obb::ArmForTest();
  QCHECK(std::strcmp(DataRootEntry()(), "/storage/emulated/0/readyatdawn") == 0);
  QCHECK(g_realDataRootCalls == 1 && obb::DataRootRedirect() == nullptr);
}

// A refused GOT slot leaves the engine reading the legacy root: the data step must not take the media root for the
// engine's, so DataRootRedirect() says nothing until the install took.
void TheRedirectIsNotTheEnginesRootUntilTheHookIsInstalled() {
  Reset();
  g_existing = {kMediaData, kMediaManifest};
  obb::ResolveRoot(&FakeExists);  // resolved; Install() would mark the hook live only when the slot took
  QCHECK(obb::DataRootRedirect() == nullptr);
}

void ADisarmedDataRootHookPassesTheQuestionThrough() {
  Reset();
  g_existing = {kMediaData, kMediaManifest};
  obb::ResolveRoot(&FakeExists);
  obb::ArmForTest();
  obb::DataRootThunk::Disarm();
  QCHECK(std::strcmp(DataRootEntry()(), "/storage/emulated/0/readyatdawn") == 0);
  QCHECK(g_realDataRootCalls == 1 && obb::CurrentCounts().dataRootServed == 0);
}

void ADataRootThunkWithoutAnOriginalIsAFault() {
  Reset();
  g_existing = {kLegacyData};
  obb::ResolveRoot(&FakeExists);
  obb::ArmForTest();
  *obb::DataRootThunk::OriginalOut() = nullptr;
  QCHECK(DataRootEntry()() == nullptr);
  QCHECK(obb::DataRootThunk::Faults() == 1);
}

// With an OBB file the skipped mount records the media root and the engine's later question gets the same answer.
void ASkippedMountAndTheDataRootQuestionAgree() {
  Reset();
  g_existing = {kMediaData, kMediaManifest};
  obb::ResolveRoot(&FakeExists);
  obb::ArmForTest();
  GameInit(10);
  QCHECK(GetDataRootDir() == kMediaDir && std::strcmp(DataRootEntry()(), kMediaDir) == 0);
}

// ---- complete data, not just a `_data` directory (#932) ----

void AStaleMediaDataDirectoryDoesNotTakeTheEngineFromCompleteLegacyData() {
  Reset();
  g_lines.clear();
  sentinel::LogSink previous = sentinel::SetLogSink(&CaptureLine);
  g_existing = {kMediaData, kLegacyData, kLegacyManifest};  // the media copy has no manifest: partial or another build
  obb::ResolveRoot(&FakeExists);
  sentinel::SetLogSink(previous);
  obb::ArmForTest();
  QCHECK(obb::DataRootRedirect() == nullptr);
  QCHECK(std::strcmp(DataRootEntry()(), "/storage/emulated/0/readyatdawn") == 0);
  QCHECK(g_realDataRootCalls == 1 && obb::CurrentCounts().dataRootServed == 0);
  QCHECK(SomeLineContains("\"engine_redirect\":\"media_incomplete\""));  // and the log says why
}

void CompleteMediaDataIsRedirectedToWhetherOrNotLegacyIsComplete() {
  for (int legacyComplete = 0; legacyComplete < 2; ++legacyComplete) {
    Reset();
    g_lines.clear();
    sentinel::LogSink previous = sentinel::SetLogSink(&CaptureLine);
    g_existing = {kMediaData, kMediaManifest};
    if (legacyComplete != 0) g_existing.insert({kLegacyData, kLegacyManifest});
    obb::ResolveRoot(&FakeExists);
    sentinel::SetLogSink(previous);
    obb::ArmForTest();
    QCHECK(obb::DataRootRedirect() != nullptr && std::strcmp(obb::DataRootRedirect(), kMediaDir) == 0);
    QCHECK(std::strcmp(DataRootEntry()(), kMediaDir) == 0);
    QCHECK(SomeLineContains("\"engine_redirect\":\"media_complete\""));
  }
}

void NeitherRootCompleteLeavesTheEngineWhereItWas() {
  Reset();
  g_existing = {kMediaData, kLegacyData};  // both partial
  obb::ResolveRoot(&FakeExists);
  obb::ArmForTest();
  QCHECK(obb::DataRootRedirect() == nullptr);
  QCHECK(std::strcmp(DataRootEntry()(), "/storage/emulated/0/readyatdawn") == 0);
  // A skipped mount is served as before (#749): the first root with a `_data` directory.
  QCHECK(std::strcmp(obb::CurrentRoot(), kMediaDir) == 0);
}

void TheManifestIsLookedUpWithTheFileCheckAndTheDirectoriesWithTheDirectoryCheck() {
  Reset();
  g_existing = {kMediaData};  // directories only
  std::set<std::string> files = {kMediaManifest};
  static std::set<std::string>* filesPtr;
  filesPtr = &files;
  obb::ResolveRoot(&FakeExists, [](const char* path) noexcept { return filesPtr->count(path) != 0; });
  obb::ArmForTest();
  QCHECK(obb::DataRootRedirect() != nullptr);
  files.clear();
  obb::ResolveRoot(&FakeExists, [](const char* path) noexcept { return filesPtr->count(path) != 0; });
  QCHECK(obb::DataRootRedirect() == nullptr);
}

// ---- Install() itself (#932): the path that sets the hook live, driven with the slot installers faked ----

int g_installPathCalls = 0;
int g_installMountCalls = 0;
int g_installDataRootCalls = 0;
sentinel::GotStatus g_pathStatus = sentinel::GotStatus::kOk;
sentinel::GotStatus g_mountStatus = sentinel::GotStatus::kOk;
sentinel::GotStatus g_dataRootStatus = sentinel::GotStatus::kOk;
sentinel::GotStatus FakeInstallPath() noexcept {
  ++g_installPathCalls;
  return g_pathStatus;
}
sentinel::GotStatus FakeInstallMount() noexcept {
  ++g_installMountCalls;
  return g_mountStatus;
}
sentinel::GotStatus FakeInstallDataRoot() noexcept {
  ++g_installDataRootCalls;
  return g_dataRootStatus;
}
obb::InstallEnv FakeEnv() {
  g_installPathCalls = g_installMountCalls = g_installDataRootCalls = 0;
  g_pathStatus = g_mountStatus = g_dataRootStatus = sentinel::GotStatus::kOk;
  return {&FakeExists, &FakeExists, &FakeInstallPath, &FakeInstallMount, &FakeInstallDataRoot};
}

// Not ArmForTest: the answer must come from what InstallWith itself armed and marked.
void InstallWithEveryHookTakingTurnsTheRedirectOnAndAnswersTheEngine() {
  Reset();
  g_lines.clear();
  g_existing = {kMediaData, kMediaManifest};
  sentinel::LogSink previous = sentinel::SetLogSink(&CaptureLine);
  QCHECK(obb::InstallWith(FakeEnv()));
  sentinel::SetLogSink(previous);
  QCHECK(g_installPathCalls == 1 && g_installMountCalls == 1 && g_installDataRootCalls == 1);
  QCHECK(obb::DataRootRedirect() != nullptr && std::strcmp(obb::DataRootRedirect(), kMediaDir) == 0);
  QCHECK(std::strcmp(DataRootEntry()(), kMediaDir) == 0);  // the handler InstallWith armed
  QCHECK(g_realDataRootCalls == 0);
  QCHECK(SomeLineContains("\"result\":\"installed\"") && SomeLineContains("\"data_root\":\"ok\""));
}

void InstallWithARefusedDataRootSlotIsNotLiveAndTheInstallFails() {
  Reset();
  g_lines.clear();
  g_existing = {kMediaData, kMediaManifest};
  obb::InstallEnv env = FakeEnv();
  g_dataRootStatus = sentinel::GotStatus::kModuleNotLoaded;
  sentinel::LogSink previous = sentinel::SetLogSink(&CaptureLine);
  QCHECK(!obb::InstallWith(env));
  sentinel::SetLogSink(previous);
  QCHECK(obb::DataRootRedirect() == nullptr);  // the engine still reads the legacy root, so nothing may say otherwise
  QCHECK(SomeLineContains("\"result\":\"data_root_failed\"") && SomeLineContains("\"data_root\":\"module_not_loaded\""));
  QCHECK(SomeLineContains("\"level\":\"error\""));
}

void InstallWithARefusedPathSlotStillInstallsTheDataRootHookAndSkipsTheMount() {
  Reset();
  g_existing = {kMediaData, kMediaManifest};
  obb::InstallEnv env = FakeEnv();
  g_pathStatus = sentinel::GotStatus::kSlotChanged;
  QCHECK(!obb::InstallWith(env));
  QCHECK(g_installMountCalls == 0 && g_installDataRootCalls == 1);
  QCHECK(obb::DataRootRedirect() != nullptr);
}

void InstallWithAnIncompleteMediaCopyInstallsTheHookButRedirectsNothing() {
  Reset();
  g_existing = {kMediaData};
  QCHECK(obb::InstallWith(FakeEnv()));
  QCHECK(obb::DataRootRedirect() == nullptr);
  QCHECK(std::strcmp(DataRootEntry()(), "/storage/emulated/0/readyatdawn") == 0 && g_realDataRootCalls == 1);
}

void TheRealFilesystemCheckSaysDirectoryOnlyForADirectory() {
  char tmpl[] = "/tmp/obb_skip_rootXXXXXX";
  char* dir = ::mkdtemp(tmpl);
  QCHECK(dir != nullptr);
  if (dir == nullptr) return;
  const std::string base = dir;
  const std::string sub = base + "/_data";
  const std::string file = base + "/afile";
  QCHECK(::mkdir(sub.c_str(), 0700) == 0);
  std::FILE* f = std::fopen(file.c_str(), "w");
  QCHECK(f != nullptr);
  if (f != nullptr) std::fclose(f);
  QCHECK(obb::DirectoryExists(sub.c_str()));
  QCHECK(!obb::DirectoryExists(file.c_str()));  // a file is not a data directory
  QCHECK(!obb::DirectoryExists((base + "/missing").c_str()));
  QCHECK(nevr_quest::data_root::HoldsData(base.c_str(), &obb::DirectoryExists));
  ::unlink(file.c_str());
  ::rmdir(sub.c_str());
  ::rmdir(base.c_str());
}

void TheSixCountersRegisterAndLeaveTheRestOfTheTable() {
  sentinel::StopReporter();
  QCHECK(obb::RegisterCounters());
  // The six counters are in the table; the rest of it is still free, and a full table refuses a registration
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
  TheAndroidMediaRootWinsWhenBothHoldTheData();
  TheLegacyRootIsServedWhenOnlyItHoldsTheData();
  ARootWithoutTheDataDirectoryDoesNotBeatARootThatHasIt();
  TheMediaDirectoryWithoutTheDataDirectoryIsServedAndSaidSo();
  TheMediaDirectoryBeatsALegacyRootThatAlsoHoldsNoData();
  NeitherRootHoldingTheDataIsFoundFalseWithTheGamesOwnFallback();
  ASkippedMountServesTheResolvedRootAndTheGameReadsFromIt();
  WithNoRootTheGameKeepsItsFallbackAndOneErrorLineNamesBothPaths();
  TheRealFilesystemCheckSaysDirectoryOnlyForADirectory();
  NoObbFileTheEngineAsksForTheDataRootAndIsToldTheMediaRootThatHoldsTheData();
  TheEngineKeepsItsOwnAnswerWhenOnlyTheLegacyRootHoldsTheData();
  TheEngineKeepsItsOwnAnswerForAnUnmarkedMediaDirectoryAndForNoRoot();
  NothingIsRedirectedBeforeTheResolverHasRun();
  TheRedirectIsNotTheEnginesRootUntilTheHookIsInstalled();
  ADisarmedDataRootHookPassesTheQuestionThrough();
  ADataRootThunkWithoutAnOriginalIsAFault();
  ASkippedMountAndTheDataRootQuestionAgree();
  AStaleMediaDataDirectoryDoesNotTakeTheEngineFromCompleteLegacyData();
  CompleteMediaDataIsRedirectedToWhetherOrNotLegacyIsComplete();
  NeitherRootCompleteLeavesTheEngineWhereItWas();
  TheManifestIsLookedUpWithTheFileCheckAndTheDirectoriesWithTheDirectoryCheck();
  InstallWithEveryHookTakingTurnsTheRedirectOnAndAnswersTheEngine();
  InstallWithARefusedDataRootSlotIsNotLiveAndTheInstallFails();
  InstallWithARefusedPathSlotStillInstallsTheDataRootHookAndSkipsTheMount();
  InstallWithAnIncompleteMediaCopyInstallsTheHookButRedirectsNothing();
  TheSixCountersRegisterAndLeaveTheRestOfTheTable();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "obb_skip_hook_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("obb_skip_hook_test: all checks pass\n");
  return 0;
}
