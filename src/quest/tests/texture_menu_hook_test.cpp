// Host test for the Texture Resolution control's hooks (#1012): the five real handlers (src/quest/sentinel/texture_menu_hook.cpp),
// built without exceptions as the sentinel builds it, driven through their thunk entries against stand-ins for the game's
// functions: a CEngine laid out at the offsets the pins name, its resize slot, CVR::RefreshRTSize (which asks the eye-size hook,
// as the game's does), CEngine::Resize, the VR flags, the two CJson::DecodeFrom overloads, ValidateGfxSettings and
// ApplyRuntimeGfxSettings. No GOT slot is touched: got_pinned_test.cpp resolves the pinned slots in the real library, and
// tests/quest TestHookFramesCarryNoPersonality covers the frames.
//
// Built and run by `just test-quest-hooks`.

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "hook_log.h"
#include "hook_report.h"
#include "quest/tests/test_check.h"
#include "texture_menu.h"
#include "texture_menu_hook.h"

namespace {

namespace tq = nevr_quest::texture_quality;
namespace tm = nevr_quest::texture_menu;
namespace pin = sentinel::pinned;

using SysThunk = tq::SystemPropertyIntThunk;
using CheckThunk = pin::LibR15CheckForResizeThunk;
using ApplyThunk = pin::LibR15ApplyRuntimeGfxSettingsThunk;
using ValidateThunk = pin::LibR15ValidateGfxSettingsThunk;
using FileThunk = pin::LibR15JsonDecodeFromFileThunk;
using TextThunk = pin::LibR15JsonDecodeFromTextThunk;

const pin::OvrJavaOpaque* const kJava = reinterpret_cast<const pin::OvrJavaOpaque*>(0x5eed);

// ---- the runtime and the game, as stand-ins -------------------------------------------------------------------------------

constexpr int kQ3W = 1680, kQ3H = 1760;  // a Quest 3's stock size: the table above Low is the Quest 3's
int g_natW = kQ3W, g_natH = kQ3H;
int FakeRuntime(const pin::OvrJavaOpaque*, int id) { return id == 5 ? g_natW : (id == 6 ? g_natH : id * 10 + 7); }

std::vector<std::string> g_order;

// The engine at the pinned offsets, with its vtable: slot +0x30 is CEngine::Resize.
alignas(16) unsigned char g_engine[0x17000];
void* g_vtable[16];

bool g_refreshAsks = true;  // CVR::RefreshRTSize asks the runtime (false: the stored size override is set)
int g_seenW = 0, g_seenH = 0;
void FakeRefresh() {
  g_order.push_back("refresh");
  if (g_refreshAsks) {
    g_seenW = SysThunk::EntryFn()(kJava, 5);
    g_seenH = SysThunk::EntryFn()(kJava, 6);
  }
}

int g_resizeRc = 0;
int g_resizeCalls = 0;
void* g_resizeEngine = nullptr;
std::uint32_t g_resizeW = 0, g_resizeH = 0;
std::int32_t g_resizeMode = 0;
std::vector<std::pair<int, int>> g_sizesAtResize;
int FakeResize(void* engine, std::uint32_t w, std::uint32_t h, std::int32_t mode) {
  g_order.push_back("resize");
  ++g_resizeCalls;
  g_resizeEngine = engine;
  g_resizeW = w;
  g_resizeH = h;
  g_resizeMode = mode;
  g_sizesAtResize.emplace_back(g_seenW, g_seenH);
  return g_resizeRc;
}
int OtherResize(void*, std::uint32_t, std::uint32_t, std::int32_t) { return 0; }

int g_vrInitialized = 1, g_vrGraphics = 1, g_vrLost = 0;
int FakeInitialized() { return g_vrInitialized; }
int FakeGraphicsInitialized() { return g_vrGraphics; }
int FakeDisplayLost() { return g_vrLost; }

std::string g_appDataDir;
void FakeAppData(void* fixedString) { std::snprintf(static_cast<char*>(fixedString), 512, "%s", g_appDataDir.c_str()); }

std::uint64_t FakeCheck(void*) {
  g_order.push_back("check");
  return 7;
}

constexpr std::uint32_t kDisplayW = 1024, kDisplayH = 1024;
constexpr std::int32_t kDisplayMode = 2;

void SetUpEngine() {
  std::memset(g_engine, 0, sizeof(g_engine));
  for (void*& slot : g_vtable) slot = nullptr;
  g_vtable[pin::engine_layout::kResizeVtableOffset / sizeof(void*)] = reinterpret_cast<void*>(&FakeResize);
  void* const* table = g_vtable;
  std::memcpy(g_engine, &table, sizeof(table));
  g_engine[pin::engine_layout::kReadyFlags] = pin::engine_layout::kReadyBit;
  std::memcpy(g_engine + pin::engine_layout::kDisplayWidth, &kDisplayW, sizeof(kDisplayW));
  std::memcpy(g_engine + pin::engine_layout::kDisplayHeight, &kDisplayH, sizeof(kDisplayH));
  std::memcpy(g_engine + pin::engine_layout::kDisplayMode, &kDisplayMode, sizeof(kDisplayMode));
}

tm::Game TheGame() {
  tm::Game game;
  game.refreshRtSize = &FakeRefresh;
  game.vrInitialized = &FakeInitialized;
  game.vrGraphicsInitialized = &FakeGraphicsInitialized;
  game.vrDisplayLost = &FakeDisplayLost;
  game.localAppDataDirectory = &FakeAppData;
  game.engineResize = &FakeResize;
  return game;
}

// ---- the log ------------------------------------------------------------------------------------------------------------

std::vector<std::string> g_lines;
void CaptureLine(sentinel::LogLevel, const char* line) { g_lines.emplace_back(line); }
struct Capture {
  sentinel::LogSink previous;
  Capture() : previous(sentinel::SetLogSink(&CaptureLine)) { g_lines.clear(); }
  ~Capture() { sentinel::SetLogSink(previous); }
};
std::size_t CountLines(const char* needle) {
  std::size_t n = 0;
  for (const std::string& l : g_lines) n += l.find(needle) != std::string::npos ? 1U : 0U;
  return n;
}
std::string LineWith(const char* needle) {
  for (const std::string& l : g_lines) {
    if (l.find(needle) != std::string::npos) return l;
  }
  return "";
}

// ---- the world ----------------------------------------------------------------------------------------------------------

const char* g_property = "";
int FakeProperty(const char*, char* out) noexcept {
  std::strcpy(out, g_property);
  return static_cast<int>(std::strlen(g_property));
}

// Everything back to nothing, then the eye-size hook latched at `configLevel` with the runtime behind it, the five handlers armed
// with the stand-ins, and the thunks' originals set. The Quest 3 stock size is the native answer.
void Reset(tq::Level configLevel = tq::Level::kLow, bool fromConfig = true) {
  g_order.clear();
  g_natW = kQ3W;
  g_natH = kQ3H;
  g_refreshAsks = true;
  g_seenW = g_seenH = 0;
  g_resizeRc = 0;
  g_resizeCalls = 0;
  g_resizeEngine = nullptr;
  g_sizesAtResize.clear();
  g_vrInitialized = 1;
  g_vrGraphics = 1;
  g_vrLost = 0;
  g_appDataDir.clear();
  g_property = "";
  tm::ResetForTest();
  tq::ResetForTest();
  SysThunk::Reset();
  CheckThunk::Reset();
  ApplyThunk::Reset();
  ValidateThunk::Reset();
  FileThunk::Reset();
  TextThunk::Reset();
  *SysThunk::OriginalOut() = reinterpret_cast<void*>(&FakeRuntime);
  *CheckThunk::OriginalOut() = reinterpret_cast<void*>(&FakeCheck);
  SetUpEngine();
  QCHECK(tq::ArmForTest(configLevel));
  tm::ArmForTest(TheGame(), configLevel, fromConfig, &FakeProperty);
}

std::uint64_t Frame() { return CheckThunk::EntryFn()(g_engine); }

// The runtime answers at the eye-size hook's current level (the pair, as the game asks for it).
std::pair<int, int> Eye() { return {SysThunk::EntryFn()(kJava, 5), SysThunk::EntryFn()(kJava, 6)}; }

// ---- the apply ----------------------------------------------------------------------------------------------------------

// A request is applied at the next frame: the engine's own check first, then the eye size is refreshed, with the new level
// already answering it, then the engine's resize with the display it keeps; the result of the check is the one returned.
void ALevelAskedForIsAppliedAtTheNextFrameInTheEnginesOrder() {
  Reset(tq::Level::kLow);
  QCHECK(Eye().first == 1680);  // Low
  tq::RequestLevel(tq::Level::kHigh, tq::RequestSource::kMenu);
  QCHECK(Eye().first == 1680);  // asked for, not applied: no answer has changed
  g_order.clear();
  QCHECK(Frame() == 7);
  QCHECK((g_order == std::vector<std::string>{"check", "refresh", "resize"}));
  QCHECK(g_resizeCalls == 1 && g_resizeEngine == g_engine);
  QCHECK(g_resizeW == kDisplayW && g_resizeH == kDisplayH && g_resizeMode == kDisplayMode);
  // The size refreshed before the resize is already the new level's (the render-target manager is built after it).
  QCHECK(g_sizesAtResize.size() == 1 && g_sizesAtResize[0].first == 3072 && g_sizesAtResize[0].second == 3380);
  QCHECK(tq::LatchedLevel() == tq::Level::kHigh);
  QCHECK(Eye().first == 3072 && Eye().second == 3380);
  const tm::Totals totals = tm::CurrentTotals();
  QCHECK(totals.applies == 1 && totals.failed == 0 && totals.refused == 0 && !totals.disabled);
  // The next frame has nothing to do.
  g_order.clear();
  QCHECK(Frame() == 7);
  QCHECK((g_order == std::vector<std::string>{"check"}));
  QCHECK(g_resizeCalls == 1);
}

void TheSizesFollowTheHeadset() {
  Reset(tq::Level::kLow);
  g_natW = 1440;
  g_natH = 1584;  // a Quest 2
  QCHECK(Eye().first == 1440);
  tq::RequestLevel(tq::Level::kUltra, tq::RequestSource::kMenu);
  Frame();
  QCHECK(g_sizesAtResize.size() == 1 && g_sizesAtResize[0].first == 2592 && g_sizesAtResize[0].second == 2848);
  tq::RequestLevel(tq::Level::kVeryLow, tq::RequestSource::kMenu);
  Frame();
  QCHECK(g_sizesAtResize.size() == 2 && g_sizesAtResize[1].first == 1152 && g_sizesAtResize[1].second == 1264);
}

void ARequestForTheAppliedLevelDoesNothing() {
  Reset(tq::Level::kHigh);
  Eye();
  tq::RequestLevel(tq::Level::kHigh, tq::RequestSource::kMenu);
  g_order.clear();
  Frame();
  QCHECK((g_order == std::vector<std::string>{"check"}));
  QCHECK(tm::CurrentTotals().applies == 0 && tm::CurrentTotals().equalCommits == 0);
}

void RequestsBetweenFramesCoalesceIntoOneApplyOfTheLast() {
  Reset(tq::Level::kLow);
  Eye();
  tq::RequestLevel(tq::Level::kMedium, tq::RequestSource::kMenu);
  tq::RequestLevel(tq::Level::kUltra, tq::RequestSource::kMenu);
  tq::RequestLevel(tq::Level::kHigh, tq::RequestSource::kProperty);
  Frame();
  QCHECK(g_resizeCalls == 1);
  QCHECK(tq::LatchedLevel() == tq::Level::kHigh && Eye().first == 3072);
}

// Two levels that give the same eye size (`stock` and Low on a headset) are committed without a rebuild.
void LevelsWithTheSameEyeSizeNeedNoRebuild() {
  Reset(tq::Level::kStock);
  QCHECK(Eye().first == kQ3W);  // the runtime's own, which also tells the hook the native size
  tq::RequestLevel(tq::Level::kLow, tq::RequestSource::kMenu);
  g_order.clear();
  Frame();
  QCHECK((g_order == std::vector<std::string>{"check"}));
  QCHECK(g_resizeCalls == 0);
  QCHECK(tq::LatchedLevel() == tq::Level::kLow);
  QCHECK(tm::CurrentTotals().equalCommits == 1 && tm::CurrentTotals().applies == 0);
  // And from Low back to a level with another size is a rebuild.
  tq::RequestLevel(tq::Level::kMedium, tq::RequestSource::kMenu);
  Frame();
  QCHECK(g_resizeCalls == 1 && tq::LatchedLevel() == tq::Level::kMedium);
}

// ---- what it waits for ---------------------------------------------------------------------------------------------------

void ItWaitsForTheEngineAndTheVrLayerAndDoesNotGiveUp() {
  Reset(tq::Level::kLow);
  Eye();
  tq::RequestLevel(tq::Level::kHigh, tq::RequestSource::kMenu);

  g_engine[pin::engine_layout::kReadyFlags] = 0;  // the engine is not ready
  Frame();
  QCHECK(g_resizeCalls == 0 && tq::LatchedLevel() == tq::Level::kLow);
  g_engine[pin::engine_layout::kReadyFlags] = pin::engine_layout::kReadyBit;

  g_vrInitialized = 0;
  Frame();
  QCHECK(g_resizeCalls == 0 && tq::LatchedLevel() == tq::Level::kLow);
  g_vrInitialized = 1;
  g_vrGraphics = 0;
  Frame();
  QCHECK(g_resizeCalls == 0 && tq::LatchedLevel() == tq::Level::kLow);
  g_vrGraphics = 1;
  g_vrLost = 1;  // the display was lost: the engine recovers it, then we apply
  Frame();
  QCHECK(g_resizeCalls == 0 && tq::LatchedLevel() == tq::Level::kLow);
  g_vrLost = 0;

  // Nothing was refused for good, and the request was still pending: the next frame applies it.
  QCHECK(!tm::CurrentTotals().disabled && tm::CurrentTotals().refused == 0);
  Frame();
  QCHECK(g_resizeCalls == 1 && tq::LatchedLevel() == tq::Level::kHigh);
}

void AnEngineWhoseResizeSlotIsNotTheEnginesResizeIsRefusedForGood() {
  Reset(tq::Level::kLow);
  Eye();
  g_vtable[pin::engine_layout::kResizeVtableOffset / sizeof(void*)] = reinterpret_cast<void*>(&OtherResize);
  tq::RequestLevel(tq::Level::kHigh, tq::RequestSource::kMenu);
  Frame();
  QCHECK(g_resizeCalls == 0 && tq::LatchedLevel() == tq::Level::kLow);
  const tm::Totals totals = tm::CurrentTotals();
  QCHECK(totals.disabled && totals.refused == 1);
  QCHECK(std::strcmp(totals.disabledWhy, "resize_slot_is_not_engine_resize") == 0);
  // Put right, it still does not apply: the process has been refused.
  g_vtable[pin::engine_layout::kResizeVtableOffset / sizeof(void*)] = reinterpret_cast<void*>(&FakeResize);
  Frame();
  QCHECK(g_resizeCalls == 0 && tm::CurrentTotals().refused == 1);
}

void WhenTheRuntimeIsNeverAskedTheLevelIsPutBackAndRefusedForGood() {
  Reset(tq::Level::kLow);
  Eye();
  g_refreshAsks = false;  // the stored size override is set: RefreshRTSize does not ask
  tq::RequestLevel(tq::Level::kHigh, tq::RequestSource::kMenu);
  g_order.clear();
  Frame();
  QCHECK(g_resizeCalls == 0);  // no rebuild was started
  QCHECK(tq::LatchedLevel() == tq::Level::kLow);
  QCHECK((g_order == std::vector<std::string>{"check", "refresh", "refresh"}));  // the second put the size back
  const tm::Totals totals = tm::CurrentTotals();
  QCHECK(totals.disabled && totals.refused == 1);
  QCHECK(std::strcmp(totals.disabledWhy, "eye_size_override_is_set") == 0);
}

void AMissingGameFunctionRefusesForGood() {
  Reset(tq::Level::kLow);
  Eye();
  tm::Game game = TheGame();
  game.refreshRtSize = nullptr;
  tm::ArmForTest(game, tq::Level::kLow, true, nullptr);
  tq::RequestLevel(tq::Level::kHigh, tq::RequestSource::kMenu);
  Frame();
  QCHECK(g_resizeCalls == 0 && tm::CurrentTotals().disabled);
  QCHECK(std::strcmp(tm::CurrentTotals().disabledWhy, "game_functions_missing") == 0);
}

// ---- a rebuild that fails ------------------------------------------------------------------------------------------------

void AFailedRebuildIsUndoneAndTheFailedLevelIsNotAskedForAgain() {
  Reset(tq::Level::kLow);
  Eye();
  g_resizeRc = 5;
  tq::RequestLevel(tq::Level::kUltra, tq::RequestSource::kMenu);
  g_order.clear();
  QCHECK(Frame() == 7);
  // The rebuild at Ultra, the eye size put back to Low, and the engine rebuilt at it.
  QCHECK((g_order == std::vector<std::string>{"check", "refresh", "resize", "refresh", "resize"}));
  QCHECK(g_sizesAtResize.size() == 2);
  QCHECK(g_sizesAtResize[0].first == 4096 && g_sizesAtResize[0].second == 4506);
  QCHECK(g_sizesAtResize[1].first == 1680 && g_sizesAtResize[1].second == 1760);
  QCHECK(tq::LatchedLevel() == tq::Level::kLow);
  QCHECK(tq::RequestedLevelValue() == static_cast<int>(tq::Level::kLow));  // the failed level is no longer asked for
  const tm::Totals totals = tm::CurrentTotals();
  QCHECK(totals.failed >= 1 && totals.applies == 0 && !totals.disabled);
  // The next frames do nothing; a later, different request is tried.
  g_order.clear();
  Frame();
  QCHECK((g_order == std::vector<std::string>{"check"}));
  g_resizeRc = 0;
  tq::RequestLevel(tq::Level::kHigh, tq::RequestSource::kMenu);
  Frame();
  QCHECK(tq::LatchedLevel() == tq::Level::kHigh && tm::CurrentTotals().applies == 1);
}

// ---- the settings the game delivers -------------------------------------------------------------------------------------

constexpr std::size_t kSettingsBytes = 0x70;
void Deliver(unsigned char* settings) { ApplyThunk::EntryFn()(settings, nullptr, 0); }

int g_applyCalls = 0;
std::int32_t g_applySawMsaa = -1;
void FakeApply(const unsigned char* settings, void*, std::uint32_t) {
  ++g_applyCalls;
  if (settings == nullptr) return;
  std::memcpy(&g_applySawMsaa, settings + pin::gfx_layout::kMsaa, sizeof(g_applySawMsaa));
}
void SetMsaa(unsigned char* settings, std::int32_t v) { std::memcpy(settings + pin::gfx_layout::kMsaa, &v, sizeof(v)); }
std::int32_t GetMsaa(const unsigned char* settings) {
  std::int32_t v;
  std::memcpy(&v, settings + pin::gfx_layout::kMsaa, sizeof(v));
  return v;
}

void EveryDeliveryAsksForTheLevelTheMenuHolds() {
  Reset(tq::Level::kLow);
  g_applyCalls = 0;
  *ApplyThunk::OriginalOut() = reinterpret_cast<void*>(&FakeApply);
  tm::ArmForTest(TheGame(), tq::Level::kLow, true, nullptr);
  alignas(8) unsigned char settings[kSettingsBytes] = {};
  SetMsaa(settings, 1);
  Deliver(settings);  // the load
  QCHECK(tq::RequestedLevelValue() == static_cast<int>(tq::Level::kLow));
  SetMsaa(settings, 4);
  Deliver(settings);
  QCHECK(tq::RequestedLevelValue() == static_cast<int>(tq::Level::kVeryHigh));
  QCHECK(tq::LastRequestSource() == tq::RequestSource::kMenu);
  SetMsaa(settings, 0);
  Deliver(settings);
  QCHECK(tq::RequestedLevelValue() == static_cast<int>(tq::Level::kVeryLow));
  QCHECK(g_applyCalls == 3 && tm::CurrentTotals().deliveries == 3);
  // The game's own function saw the value the menu holds: nothing is hidden from it.
  QCHECK(g_applySawMsaa == 0 && GetMsaa(settings) == 0);
  // A value outside the menu is clamped into it.
  SetMsaa(settings, 77);
  Deliver(settings);
  QCHECK(tq::RequestedLevelValue() == static_cast<int>(tq::Level::kUltra));
  // A null settings pointer is the game's problem, not ours: the original is called, nothing is asked.
  g_applyCalls = 0;
  ApplyThunk::EntryFn()(nullptr, nullptr, 0);
  QCHECK(g_applyCalls == 1);
}

// With nothing saved, the configuration's level is the player's level: the first delivery is changed to it, so the menu shows
// it and the next save keeps it. Later deliveries are the player's.
void TheFirstDeliveryTakesTheConfigurationsLevelWhenNothingIsSaved() {
  Reset(tq::Level::kHigh);
  *ApplyThunk::OriginalOut() = reinterpret_cast<void*>(&FakeApply);
  alignas(8) unsigned char settings[kSettingsBytes] = {};
  SetMsaa(settings, 1);  // what the game holds when nothing is saved: Low
  Deliver(settings);
  QCHECK(GetMsaa(settings) == 3 && g_applySawMsaa == 3);  // High, in the settings the game keeps and the call it makes
  QCHECK(tq::RequestedLevelValue() == static_cast<int>(tq::Level::kHigh));
  QCHECK(tm::CurrentTotals().configOverrides == 1);
  SetMsaa(settings, 5);  // the player picks Ultra
  Deliver(settings);
  QCHECK(GetMsaa(settings) == 5 && tq::RequestedLevelValue() == static_cast<int>(tq::Level::kUltra));
  QCHECK(tm::CurrentTotals().configOverrides == 1);
}

void AStockConfigurationChangesNothingAtTheFirstDelivery() {
  Reset(tq::Level::kStock);
  *ApplyThunk::OriginalOut() = reinterpret_cast<void*>(&FakeApply);
  alignas(8) unsigned char settings[kSettingsBytes] = {};
  SetMsaa(settings, 1);
  Deliver(settings);
  QCHECK(GetMsaa(settings) == 1 && tm::CurrentTotals().configOverrides == 0);
  QCHECK(tq::RequestedLevelValue() == static_cast<int>(tq::Level::kLow));  // Low is the menu's name for stock
}

// ---- the saved value -----------------------------------------------------------------------------------------------------

// A game directory with RAD/LoneEcho in it: the game's settings file (`settings`), and the player's marker (`marker`), each only
// when given.
std::string MakeSettingsDirectory(const char* settings, const char* marker = nullptr) {
  char dir[] = "/tmp/texmenuXXXXXX";
  if (mkdtemp(dir) == nullptr) return "";
  const std::string root = dir;
  const std::string rad = root + "/RAD";
  const std::string lone = rad + "/LoneEcho";
  if (mkdir(rad.c_str(), 0700) != 0 || mkdir(lone.c_str(), 0700) != 0) return "";
  for (const auto& [name, contents] : {std::pair<const char*, const char*>{"settings_mp_v2_quest.json", settings},
                                       std::pair<const char*, const char*>{tm::kMarkerFile, marker}}) {
    if (contents == nullptr) continue;
    std::FILE* f = std::fopen((lone + "/" + name).c_str(), "w");
    if (f == nullptr) return "";
    std::fputs(contents, f);
    std::fclose(f);
  }
  return root;
}
std::string ReadMarker(const std::string& root) {
  std::FILE* f = std::fopen((root + "/RAD/LoneEcho/" + tm::kMarkerFile).c_str(), "r");
  if (f == nullptr) return "";
  char buffer[256] = {};
  const std::size_t n = std::fread(buffer, 1, sizeof(buffer) - 1, f);
  std::fclose(f);
  return std::string(buffer, n);
}
void RemoveSettingsDirectory(const std::string& root) {
  std::remove((root + "/RAD/LoneEcho/settings_mp_v2_quest.json").c_str());
  std::remove((root + "/RAD/LoneEcho/" + tm::kMarkerFile).c_str());
  std::remove((root + "/RAD/LoneEcho/" + tm::kMarkerFile + ".tmp").c_str());
  rmdir((root + "/RAD/LoneEcho").c_str());
  rmdir((root + "/RAD").c_str());
  rmdir(root.c_str());
}

// What the player chose (the marker) starts the game at that level, on the very first eye-size answer, before the game has
// delivered anything; and the first delivery is the player's, not the configuration's. The game's own slot may hold anything.
void ASavedLevelStartsTheGameAndOutranksTheConfiguration() {
  const std::string dir = MakeSettingsDirectory(R"({"graphics": {"msaa": 0, "sharpening": 1.5}})", R"({"menu_index":4})");
  QCHECK(!dir.empty());
  Reset(tq::Level::kHigh);  // the configuration says High
  *ApplyThunk::OriginalOut() = reinterpret_cast<void*>(&FakeApply);
  g_appDataDir = dir;
  QCHECK(Eye().first == 3584 && Eye().second == 3944);  // Very High, the marked level
  QCHECK(tq::LatchedLevel() == tq::Level::kVeryHigh);
  alignas(8) unsigned char settings[kSettingsBytes] = {};
  SetMsaa(settings, 0);  // the game's slot says Very Low (an older file): the player's choice stands, in the settings the game keeps
  Deliver(settings);
  QCHECK(GetMsaa(settings) == 4 && g_applySawMsaa == 4 && tm::CurrentTotals().configOverrides == 0);
  QCHECK(tq::RequestedLevelValue() == static_cast<int>(tq::Level::kVeryHigh));
  g_order.clear();
  Frame();
  QCHECK(g_resizeCalls == 0);  // asked for what is applied
  RemoveSettingsDirectory(dir);
}

void WithNothingSavedTheConfigurationsLevelStarts() {
  // Whatever the game's own file holds (a saved `msaa` included: it is not a choice of the player's), with no marker.
  for (const char* contents : {static_cast<const char*>(nullptr), "{}", "{\"graphics\": {\"sharpening\": 1.5}}", "not json at all",
                               "{\"graphics\": {\"msaa\": 0}}", "{\"graphics\": {\"msaa\": 4}}"}) {
    const std::string dir = MakeSettingsDirectory(contents);
    QCHECK(!dir.empty());
    Reset(tq::Level::kMedium);
    g_appDataDir = dir;
    QCHECK(Eye().first == 2376 && Eye().second == 2568);
    QCHECK(tq::LatchedLevel() == tq::Level::kMedium);
    RemoveSettingsDirectory(dir);
  }
  // A marker that holds no choice is no marker.
  for (const char* marker : {"", "{}", "not json at all", R"({"menu_index": "4"})", R"({"menu_index": 2.5})"}) {
    const std::string dir = MakeSettingsDirectory(nullptr, marker);
    QCHECK(!dir.empty());
    Reset(tq::Level::kMedium);
    g_appDataDir = dir;
    QCHECK(Eye().first == 2376 && Eye().second == 2568);
    RemoveSettingsDirectory(dir);
  }
  // No directory at all (the game's function wrote nothing): the same.
  Reset(tq::Level::kMedium);
  g_appDataDir = "";
  QCHECK(Eye().first == 2376);
  // A directory that is not there: the same.
  Reset(tq::Level::kMedium);
  g_appDataDir = "/nonexistent/texmenu";
  QCHECK(Eye().first == 2376);
}

// The launch bug: a settings file that never held the setting reads 0 in the game's slot, and the slot was read as "the player
// chose Very Low". A player who never chose starts at the default, and the control shows it.
void AFreshSettingsFileStartsAtTheDefaultNotVeryLow() {
  for (const char* settingsFile : {static_cast<const char*>(nullptr), "{}", R"({"graphics": {"msaa": 0}})"}) {
    const std::string dir = MakeSettingsDirectory(settingsFile);
    QCHECK(!dir.empty());
    Reset(tq::Level::kStock, /*fromConfig=*/false);  // the default configuration: `stock`, source default
    *ApplyThunk::OriginalOut() = reinterpret_cast<void*>(&FakeApply);
    Capture log;
    g_appDataDir = dir;
    QCHECK(Eye().first == kQ3W && Eye().second == kQ3H);  // the headset's own size on the first answer
    QCHECK(tq::LatchedLevel() == tq::Level::kStock);
    alignas(8) unsigned char settings[kSettingsBytes] = {};
    SetMsaa(settings, 0);  // what the game holds when nothing was ever saved
    Deliver(settings);
    QCHECK(GetMsaa(settings) == tq::kDefaultMenuIndex && g_applySawMsaa == tq::kDefaultMenuIndex);  // Low, not Very Low
    QCHECK(tq::RequestedLevelValue() == static_cast<int>(tq::Level::kLow));
    QCHECK(tm::CurrentTotals().configOverrides == 1);
    QCHECK(tm::PollOnceForTest() >= 1);  // the poller runs seconds before the first frame applies anything
    const std::string start = LineWith("\"event\":\"texture_quality_start\"");
    QCHECK(start.find("\"source\":\"config\"") != std::string::npos && start.find("\"quality\":\"stock\"") != std::string::npos);
    g_order.clear();
    Frame();
    QCHECK(g_resizeCalls == 0 && Eye().first == kQ3W);  // Low is the stock size: nothing is rebuilt
    tm::PollOnceForTest();
    QCHECK(CountLines("texture_quality_saved") == 0 && ReadMarker(dir).empty());  // nothing was chosen, nothing is remembered
    QCHECK(tm::CurrentTotals().markerWrites == 0);
    RemoveSettingsDirectory(dir);
  }
}

// What the player chooses in the menu is remembered, and only that: the next launch starts there.
void AMenuChoicePersistsToTheNextLaunch() {
  const std::string dir = MakeSettingsDirectory(R"({"graphics": {"msaa": 0}})");
  QCHECK(!dir.empty());
  Reset(tq::Level::kStock, false);
  *ApplyThunk::OriginalOut() = reinterpret_cast<void*>(&FakeApply);
  Capture log;
  g_appDataDir = dir;
  Eye();
  alignas(8) unsigned char settings[kSettingsBytes] = {};
  SetMsaa(settings, 0);
  Deliver(settings);  // the load: the default, no choice
  Deliver(settings);  // another setting changed: the same index again is no choice
  tm::PollOnceForTest();
  QCHECK(ReadMarker(dir).empty() && tm::CurrentTotals().markerWrites == 0);

  SetMsaa(settings, 4);  // the player picks Very High
  Deliver(settings);
  QCHECK(tm::PollOnceForTest() >= 1);
  QCHECK(ReadMarker(dir) == R"({"menu_index":4})");
  const std::string saved = LineWith("\"event\":\"texture_quality_saved\"");
  QCHECK(saved.find("\"result\":\"written\"") != std::string::npos && saved.find("\"quality\":\"very_high\"") != std::string::npos);
  QCHECK(saved.find("\"menu_index\":4") != std::string::npos);
  QCHECK(tm::CurrentTotals().markerWrites == 1 && tm::CurrentTotals().markerFailures == 0);
  Deliver(settings);  // redelivered unchanged
  tm::PollOnceForTest();
  QCHECK(CountLines("texture_quality_saved") == 1 && tm::CurrentTotals().markerWrites == 1);

  // The next launch: the marker starts the game at Very High, and the control shows it whatever the game's slot holds.
  Reset(tq::Level::kStock, false);
  *ApplyThunk::OriginalOut() = reinterpret_cast<void*>(&FakeApply);
  g_appDataDir = dir;
  QCHECK(Eye().first == 3584 && Eye().second == 3944);
  SetMsaa(settings, 0);
  Deliver(settings);
  QCHECK(GetMsaa(settings) == 4 && tq::RequestedLevelValue() == static_cast<int>(tq::Level::kVeryHigh));
  g_lines.clear();
  tm::PollOnceForTest();
  const std::string start = LineWith("\"event\":\"texture_quality_start\"");
  QCHECK(start.find("\"source\":\"player_marker\"") != std::string::npos && start.find("\"quality\":\"very_high\"") != std::string::npos);
  QCHECK(CountLines("texture_quality_saved") == 0);  // restoring is not choosing

  // Choosing Very Low is a choice like any other, and is told apart from never having chosen.
  SetMsaa(settings, 0);
  Deliver(settings);
  tm::PollOnceForTest();
  QCHECK(ReadMarker(dir) == R"({"menu_index":0})");
  Reset(tq::Level::kStock, false);
  g_appDataDir = dir;
  QCHECK(Eye().first == 1344 && Eye().second == 1408);  // Very Low on a Quest 3
  QCHECK(tq::LatchedLevel() == tq::Level::kVeryLow);
  RemoveSettingsDirectory(dir);
}

// A choice that cannot be written is loud, with what it costs, and counted.
void AChoiceThatCannotBeWrittenIsLoud() {
  Reset(tq::Level::kStock, false);
  *ApplyThunk::OriginalOut() = reinterpret_cast<void*>(&FakeApply);
  Capture log;
  g_appDataDir = "/nonexistent/texmenu";
  Eye();
  alignas(8) unsigned char settings[kSettingsBytes] = {};
  SetMsaa(settings, 1);
  Deliver(settings);
  SetMsaa(settings, 3);
  Deliver(settings);
  QCHECK(tm::PollOnceForTest() >= 1);
  const std::string saved = LineWith("\"event\":\"texture_quality_saved\"");
  QCHECK(saved.find("\"level\":\"error\"") != std::string::npos && saved.find("\"result\":\"write_failed\"") != std::string::npos);
  QCHECK(saved.find("\"errno\":2") != std::string::npos && saved.find("\"consequence\"") != std::string::npos);
  QCHECK(tm::CurrentTotals().markerFailures == 1 && tm::CurrentTotals().markerWrites == 0);

  // The start resolver never ran (no eye-size query): there is no directory to write to, and it says so.
  Reset(tq::Level::kStock, false);
  *ApplyThunk::OriginalOut() = reinterpret_cast<void*>(&FakeApply);
  g_lines.clear();
  SetMsaa(settings, 1);
  Deliver(settings);
  SetMsaa(settings, 3);
  Deliver(settings);
  tm::PollOnceForTest();
  QCHECK(LineWith("\"event\":\"texture_quality_saved\"").find("\"result\":\"no_directory\"") != std::string::npos);
  QCHECK(tm::CurrentTotals().markerFailures == 1);
}

// ---- ValidateGfxSettings -------------------------------------------------------------------------------------------------

// The game's clamp of `graphics|msaa` to 0..2, as the pinned function does it (and nothing else of its work).
int g_validateCalls = 0;
void FakeValidate(unsigned char* settings) {
  ++g_validateCalls;
  if (settings == nullptr) return;
  std::int32_t v;
  std::memcpy(&v, settings + pin::gfx_layout::kMsaa, sizeof(v));
  v = v < 0 ? 0 : v;
  v = v < 2 ? v : 2;
  std::memcpy(settings + pin::gfx_layout::kMsaa, &v, sizeof(v));
  settings[0x30] = 0x5a;  // another field the original touches stays what the original made it
}

void TheSixLevelsSurviveTheGamesClamp() {
  Reset(tq::Level::kLow);
  *ValidateThunk::OriginalOut() = reinterpret_cast<void*>(&FakeValidate);
  g_validateCalls = 0;
  alignas(8) unsigned char settings[kSettingsBytes] = {};
  for (int menu = 0; menu < 6; ++menu) {
    SetMsaa(settings, menu);
    ValidateThunk::EntryFn()(settings);
    QCHECK(GetMsaa(settings) == menu);
  }
  QCHECK(g_validateCalls == 6 && settings[0x30] == 0x5a);
  SetMsaa(settings, 6);
  ValidateThunk::EntryFn()(settings);
  QCHECK(GetMsaa(settings) == 5);
  SetMsaa(settings, 4000000);
  ValidateThunk::EntryFn()(settings);
  QCHECK(GetMsaa(settings) == 5);
  SetMsaa(settings, -4);
  ValidateThunk::EntryFn()(settings);
  QCHECK(GetMsaa(settings) == 0);
  g_validateCalls = 0;
  ValidateThunk::EntryFn()(nullptr);
  QCHECK(g_validateCalls == 1);
}

// ---- the two documents ---------------------------------------------------------------------------------------------------

const char* kMenuDocument =
    R"json({"pages": [{"name": "GRAPHICS (1/3)", "rows": [{"path": "graphics|qualitylevel", "name": "Graphics Quality"},)json"
    R"( {"path": "graphics|msaa", "name": "Anti-aliasing", "idx": 0, "type": "STRING ARRAY", "values": ["None", "2x", "4x"]}]}]})";
const char* kSettingsDocument =
    R"json({"quest": {"settings": [{"path": "graphics|msaa", "value": 1, "operator": "remove"}], "pages": ["GRAPHICS (1/3)", "DISPLAY"]}})json";

std::vector<std::string> g_parsed;  // the texts the game's parser was given
int FakeDecodeText(pin::CJsonOpaque*, const char* text, unsigned long long length) {
  g_parsed.emplace_back(text, static_cast<std::size_t>(length));
  return 0;
}
// The game's file read: it strips the comments and parses the text, here the document the test hands it.
const char* g_documentOnDisk = "";
std::vector<std::string> g_pathsRead;
int FakeDecodeFile(pin::CJsonOpaque* self, const char* path, std::uint32_t) {
  g_pathsRead.emplace_back(path);
  return TextThunk::EntryFn()(self, g_documentOnDisk, std::strlen(g_documentOnDisk));
}

void ResetDocuments() {
  Reset(tq::Level::kLow);
  g_parsed.clear();
  g_pathsRead.clear();
  *FileThunk::OriginalOut() = reinterpret_cast<void*>(&FakeDecodeFile);
  *TextThunk::OriginalOut() = reinterpret_cast<void*>(&FakeDecodeText);
}

int Read(const char* path, const char* document) {
  g_documentOnDisk = document;
  return FileThunk::EntryFn()(nullptr, path, 1);
}

void TheMenuDocumentIsParsedAsTheControl() {
  ResetDocuments();
  QCHECK(Read("/data/sourcedb/rad15/json/r14/config/uisettings/menu_settings.json", kMenuDocument) == 0);
  QCHECK(g_parsed.size() == 1);
  QCHECK(g_parsed[0].find("\"name\":\"Texture Resolution\"") != std::string::npos);
  QCHECK(g_parsed[0].find("\"name\":\"GRAPHICS\"") != std::string::npos);
  QCHECK(g_parsed[0].find("Anti-aliasing") == std::string::npos);
  QCHECK(g_parsed[0].find("Graphics Quality") == std::string::npos);
  QCHECK(g_parsed[0].find("\"idx\":1") != std::string::npos);
  QCHECK(tm::CurrentTotals().jsonRewrites == 1);
}

void TheSettingsDocumentIsParsedWithoutTheRemovals() {
  ResetDocuments();
  QCHECK(Read("/data/sourcedb/rad15/json/r14/config/gamesettings_config.json", kSettingsDocument) == 0);
  QCHECK(g_parsed.size() == 1);
  QCHECK(g_parsed[0].find("graphics|msaa") == std::string::npos);
  QCHECK(g_parsed[0].find("GRAPHICS (1/3)") == std::string::npos);
  QCHECK(g_parsed[0].find("DISPLAY") != std::string::npos);
}

void TheMenuDefaultIsTheConfigurationsLevel() {
  Reset(tq::Level::kHigh);
  g_parsed.clear();
  *FileThunk::OriginalOut() = reinterpret_cast<void*>(&FakeDecodeFile);
  *TextThunk::OriginalOut() = reinterpret_cast<void*>(&FakeDecodeText);
  Read("/x/json/r14/config/uisettings/menu_settings.json", kMenuDocument);
  QCHECK(g_parsed.size() == 1 && g_parsed[0].find("\"idx\":3") != std::string::npos);
  Reset(tq::Level::kStock);
  g_parsed.clear();
  *FileThunk::OriginalOut() = reinterpret_cast<void*>(&FakeDecodeFile);
  *TextThunk::OriginalOut() = reinterpret_cast<void*>(&FakeDecodeText);
  Read("/x/json/r14/config/uisettings/menu_settings.json", kMenuDocument);
  QCHECK(g_parsed.size() == 1 && g_parsed[0].find("\"idx\":1") != std::string::npos);  // stock is shown as Low
}

void AnyOtherFileOrParseIsPassedThroughUntouched() {
  ResetDocuments();
  // The same document under another name is not ours.
  for (const char* path : {"/data/sourcedb/rad15/json/r14/config/uisettings/other_settings.json",
                           "/data/sourcedb/rad15/json/r14/config/matchsettings_config.json",
                           "/data/menu_settings.json", "/data/sourcedb/rad15/json/r14/config/uisettings/menu_settings.json.bak",
                           ""}) {
    g_parsed.clear();
    QCHECK(Read(path, kMenuDocument) == 0);
    QCHECK(g_parsed.size() == 1 && g_parsed[0] == kMenuDocument);
  }
  // A parse outside any file read of ours is passed through, whatever the text (the game parses network replies this way).
  g_parsed.clear();
  QCHECK(TextThunk::EntryFn()(nullptr, kMenuDocument, std::strlen(kMenuDocument)) == 0);
  QCHECK(g_parsed.size() == 1 && g_parsed[0] == kMenuDocument);
  QCHECK(tm::CurrentTotals().jsonRewrites == 0);
}

void TheThreadIsDisarmedWhenTheReadEnds() {
  ResetDocuments();
  Read("/d/json/r14/config/uisettings/menu_settings.json", kMenuDocument);
  g_parsed.clear();
  // A parse after the read, on the same thread, is not rewritten.
  TextThunk::EntryFn()(nullptr, kMenuDocument, std::strlen(kMenuDocument));
  QCHECK(g_parsed.size() == 1 && g_parsed[0] == kMenuDocument);
  // A second parse inside one read is not rewritten either (the first one was the document).
  g_parsed.clear();
  static const char* const kTwice = kMenuDocument;
  struct Twice {
    static int Read(pin::CJsonOpaque* self, const char*, std::uint32_t) {
      TextThunk::EntryFn()(self, kTwice, std::strlen(kTwice));
      return TextThunk::EntryFn()(self, kTwice, std::strlen(kTwice));
    }
  };
  *FileThunk::OriginalOut() = reinterpret_cast<void*>(&Twice::Read);
  FileThunk::EntryFn()(nullptr, "/d/json/r14/config/uisettings/menu_settings.json", 1);
  QCHECK(g_parsed.size() == 2 && g_parsed[0] != kMenuDocument && g_parsed[1] == kMenuDocument);
}

void ADocumentOfAnotherShapeIsPassedThroughAndSaysSo() {
  ResetDocuments();
  Capture log;
  Read("/d/json/r14/config/uisettings/menu_settings.json", R"({"pages": [{"name": "P", "rows": []}]})");
  QCHECK(g_parsed.size() == 1 && g_parsed[0] == R"({"pages": [{"name": "P", "rows": []}]})");
  Read("/d/json/r14/config/gamesettings_config.json", "{ not json");
  QCHECK(g_parsed.size() == 2 && g_parsed[1] == "{ not json");
  QCHECK(tm::CurrentTotals().jsonRewrites == 0);
  QCHECK(tm::PollOnceForTest() >= 2);
  const std::string menu = LineWith("\"document\":\"menu_settings\"");
  QCHECK(menu.find("\"result\":\"shape_mismatch\"") != std::string::npos && menu.find("\"level\":\"warn\"") != std::string::npos);
  const std::string settings = LineWith("\"document\":\"gamesettings_config\"");
  QCHECK(settings.find("\"result\":\"not_json\"") != std::string::npos);
}

void TheSameDocumentReadAgainIsServedTheSameRewrite() {
  ResetDocuments();
  Read("/d/json/r14/config/uisettings/menu_settings.json", kMenuDocument);
  Read("/d/json/r14/config/uisettings/menu_settings.json", kMenuDocument);
  QCHECK(g_parsed.size() == 2 && g_parsed[0] == g_parsed[1]);
  QCHECK(tm::CurrentTotals().jsonRewrites == 1);  // rewritten once
}

// ---- the log -----------------------------------------------------------------------------------------------------------

void ThePollerWritesTheStartTheRequestsAndTheChanges() {
  const std::string dir = MakeSettingsDirectory(nullptr, R"({"menu_index":2})");
  Reset(tq::Level::kLow);
  Capture log;
  g_appDataDir = dir;
  Eye();  // the first query: the marked level is Medium
  QCHECK(tm::PollOnceForTest() >= 1);
  const std::string start = LineWith("\"event\":\"texture_quality_start\"");
  QCHECK(start.find("\"source\":\"player_marker\"") != std::string::npos && start.find("\"quality\":\"medium\"") != std::string::npos);
  QCHECK(start.find("\"menu_index\":2") != std::string::npos);
  QCHECK(CountLines("texture_quality_start") == 1);
  tm::PollOnceForTest();
  QCHECK(CountLines("texture_quality_start") == 1);  // once

  // A request from the development property: one request line, from the property.
  g_property = "high";
  tm::PollOnceForTest();
  const std::string request = LineWith("\"event\":\"texture_quality_request\"");
  QCHECK(request.find("\"source\":\"property\"") != std::string::npos);
  QCHECK(request.find("\"from\":\"medium\"") != std::string::npos && request.find("\"to\":\"high\"") != std::string::npos);
  tm::PollOnceForTest();
  QCHECK(CountLines("texture_quality_request") == 1);  // not again for the same request

  // Applied at the next frame; the poller writes the change with the size and the engine's result.
  Frame();
  tm::PollOnceForTest();
  const std::string applied = LineWith("\"event\":\"texture_quality_applied\"");
  QCHECK(applied.find("\"from\":\"medium\"") != std::string::npos && applied.find("\"to\":\"high\"") != std::string::npos);
  QCHECK(applied.find("\"w\":3072") != std::string::npos && applied.find("\"h\":3380") != std::string::npos);
  QCHECK(applied.find("\"rc\":0") != std::string::npos && applied.find("\"applies\":1") != std::string::npos);
  QCHECK(CountLines("texture_quality_applied") == 1);

  // The property takes words and menu indices; a word that is not a level is not a request.
  g_property = "banana";
  tm::PollOnceForTest();
  QCHECK(CountLines("texture_quality_request") == 1);
  g_property = "0";
  tm::PollOnceForTest();
  QCHECK(tq::RequestedLevelValue() == static_cast<int>(tq::Level::kVeryLow));
  g_property = "7";
  tm::PollOnceForTest();
  QCHECK(tq::RequestedLevelValue() == static_cast<int>(tq::Level::kVeryLow));  // no entry 7
  RemoveSettingsDirectory(dir);
}

void ARefusedApplyAndAFailedRebuildAreLoudLinesWithTheirConsequence() {
  Reset(tq::Level::kLow);
  Capture log;
  Eye();
  g_vtable[pin::engine_layout::kResizeVtableOffset / sizeof(void*)] = reinterpret_cast<void*>(&OtherResize);
  tq::RequestLevel(tq::Level::kHigh, tq::RequestSource::kMenu);
  Frame();
  tm::PollOnceForTest();
  const std::string refused = LineWith("\"event\":\"texture_quality_apply_refused\"");
  QCHECK(refused.find("\"level\":\"error\"") != std::string::npos);
  QCHECK(refused.find("resize_slot_is_not_engine_resize") != std::string::npos);
  QCHECK(refused.find("applies at the next launch") != std::string::npos);

  Reset(tq::Level::kLow);
  Capture log2;
  Eye();
  g_resizeRc = 9;
  tq::RequestLevel(tq::Level::kHigh, tq::RequestSource::kMenu);
  Frame();
  tm::PollOnceForTest();
  const std::string failed = LineWith("\"event\":\"texture_quality_apply_failed\"");
  QCHECK(failed.find("\"level\":\"error\"") != std::string::npos && failed.find("\"rc\":9") != std::string::npos);
  QCHECK(failed.find("previous level was put back") != std::string::npos);
}

void TheInstallRefusesAndSaysWhyOnAHostWithoutLibr15() {
  Reset(tq::Level::kLow);
  Capture log;
  QCHECK(!tm::Install(tq::Level::kHigh, true));
  const std::string line = LineWith("\"event\":\"texture_menu_install\"");
  QCHECK(line.find("\"result\":\"skipped\"") != std::string::npos && line.find("\"why\":\"module_not_loaded\"") != std::string::npos);
  QCHECK(line.find("\"quality\":\"high\"") != std::string::npos && line.find("\"source\":\"config\"") != std::string::npos);
  QCHECK(line.find("\"level\":\"error\"") != std::string::npos);
  QCHECK(line.find("no Texture Resolution control") != std::string::npos);
}

void TheCountersRegisterAndLeaveTheRestOfTheTable() {
  sentinel::StopReporter();
  Capture log;
  QCHECK(tm::RegisterCounters());
  QCHECK(CountLines("texture_menu_counters") == 0);
  int extra = 0;
  static std::atomic<std::uint64_t> spare[sentinel::kMaxReportCounters];
  while (extra < static_cast<int>(sentinel::kMaxReportCounters) && sentinel::RegisterReportCounter("spare", &spare[extra])) ++extra;
  QCHECK(extra == static_cast<int>(sentinel::kMaxReportCounters) - tm::kCounterCount);
  sentinel::StopReporter();

  constexpr unsigned kFill = sentinel::kMaxReportCounters - 2;
  static std::atomic<std::uint64_t> filler[kFill];
  for (unsigned i = 0; i < kFill; ++i) QCHECK(sentinel::RegisterReportCounter("filler", &filler[i]));
  g_lines.clear();
  QCHECK(!tm::RegisterCounters());
  QCHECK(CountLines("\"event\":\"texture_menu_counters\"") == 1);
  sentinel::StopReporter();
}

// ---- the reference input's start rule (#1737 C4) ------------------------------------------------------------------------------

// On the reference libr15, with a `stock` configuration and no marker, the start is his rule: the settings file's msaa index V,
// at level kLow + V (no file or no key: V=2, High). The rule is queued as the player's marker, so the next launch reads it there.
void ReferenceRuleWithNoSettingsFileStartsAtHigh() {
  Reset(tq::Level::kStock, true);
  tq::SetInputForTest(tq::EyeInput::kReference);
  const std::string dir = MakeSettingsDirectory(nullptr);
  QCHECK(!dir.empty());
  g_appDataDir = dir;
  Capture log;
  QCHECK(Eye().first == 3072 && Eye().second == 3380);  // High on a Quest 3
  QCHECK(tq::LatchedLevel() == tq::Level::kHigh);
  QCHECK(tm::PollOnceForTest() > 0);
  QCHECK(CountLines("\"source\":\"reference_rule\"") == 1);
  QCHECK(CountLines("\"reference_file\":\"missing\"") == 1);
  QCHECK(ReadMarker(dir) == R"({"menu_index":3})");  // High, queued as the player's marker
  RemoveSettingsDirectory(dir);
}

void ReferenceRuleReadsTheMsaaIndexOfTheSettingsFile() {
  Reset(tq::Level::kStock, true);
  tq::SetInputForTest(tq::EyeInput::kReference);
  const std::string dir = MakeSettingsDirectory(R"({"graphics": {"msaa": 1, "sharpening": 0.5}})");
  QCHECK(!dir.empty());
  g_appDataDir = dir;
  Capture log;
  QCHECK(Eye().first == 2376 && Eye().second == 2568);  // Medium on a Quest 3: kLow + 1
  QCHECK(tq::LatchedLevel() == tq::Level::kMedium);
  QCHECK(tm::PollOnceForTest() > 0);
  QCHECK(CountLines("\"reference_file\":\"read\"") == 1);
  QCHECK(ReadMarker(dir) == R"({"menu_index":2})");
  RemoveSettingsDirectory(dir);
}

void AConfiguredLowOutranksTheReferenceRule() {
  Reset(tq::Level::kLow, true);
  tq::SetInputForTest(tq::EyeInput::kReference);
  const std::string dir = MakeSettingsDirectory(R"({"graphics": {"msaa": 3}})");
  QCHECK(!dir.empty());
  g_appDataDir = dir;
  Capture log;
  QCHECK(Eye().first == 1680 && Eye().second == 1760);  // Low: the configuration stands, the rule does not run
  QCHECK(tq::LatchedLevel() == tq::Level::kLow);
  tm::PollOnceForTest();
  QCHECK(CountLines("\"source\":\"reference_rule\"") == 0);
  RemoveSettingsDirectory(dir);
}

void APlayersMarkerOutranksTheReferenceRule() {
  Reset(tq::Level::kStock, true);
  tq::SetInputForTest(tq::EyeInput::kReference);
  const std::string dir = MakeSettingsDirectory(R"({"graphics": {"msaa": 3}})", R"({"menu_index":0})");
  QCHECK(!dir.empty());
  g_appDataDir = dir;
  Capture log;
  QCHECK(Eye().first == 1344 && Eye().second == 1408);  // Very Low: the marker, not msaa 3 (High)
  QCHECK(tq::LatchedLevel() == tq::Level::kVeryLow);
  QCHECK(CountLines("\"source\":\"reference_rule\"") == 0);
  RemoveSettingsDirectory(dir);
}

void TheStockInputNeverUsesTheReferenceRule() {
  Reset(tq::Level::kStock, true);
  tq::SetInputForTest(tq::EyeInput::kStock);
  const std::string dir = MakeSettingsDirectory(R"({"graphics": {"msaa": 3}})");
  QCHECK(!dir.empty());
  g_appDataDir = dir;
  Capture log;
  QCHECK(Eye().first == kQ3W && Eye().second == kQ3H);  // stock: the runtime's own size
  QCHECK(tq::LatchedLevel() == tq::Level::kStock);
  QCHECK(CountLines("\"source\":\"reference_rule\"") == 0);
  RemoveSettingsDirectory(dir);
}

}  // namespace

int main() {
  ALevelAskedForIsAppliedAtTheNextFrameInTheEnginesOrder();
  TheSizesFollowTheHeadset();
  ARequestForTheAppliedLevelDoesNothing();
  RequestsBetweenFramesCoalesceIntoOneApplyOfTheLast();
  LevelsWithTheSameEyeSizeNeedNoRebuild();
  ItWaitsForTheEngineAndTheVrLayerAndDoesNotGiveUp();
  AnEngineWhoseResizeSlotIsNotTheEnginesResizeIsRefusedForGood();
  WhenTheRuntimeIsNeverAskedTheLevelIsPutBackAndRefusedForGood();
  AMissingGameFunctionRefusesForGood();
  AFailedRebuildIsUndoneAndTheFailedLevelIsNotAskedForAgain();
  EveryDeliveryAsksForTheLevelTheMenuHolds();
  TheFirstDeliveryTakesTheConfigurationsLevelWhenNothingIsSaved();
  AStockConfigurationChangesNothingAtTheFirstDelivery();
  ASavedLevelStartsTheGameAndOutranksTheConfiguration();
  WithNothingSavedTheConfigurationsLevelStarts();
  AFreshSettingsFileStartsAtTheDefaultNotVeryLow();
  AMenuChoicePersistsToTheNextLaunch();
  AChoiceThatCannotBeWrittenIsLoud();
  TheSixLevelsSurviveTheGamesClamp();
  TheMenuDocumentIsParsedAsTheControl();
  TheSettingsDocumentIsParsedWithoutTheRemovals();
  TheMenuDefaultIsTheConfigurationsLevel();
  AnyOtherFileOrParseIsPassedThroughUntouched();
  TheThreadIsDisarmedWhenTheReadEnds();
  ADocumentOfAnotherShapeIsPassedThroughAndSaysSo();
  TheSameDocumentReadAgainIsServedTheSameRewrite();
  ThePollerWritesTheStartTheRequestsAndTheChanges();
  ARefusedApplyAndAFailedRebuildAreLoudLinesWithTheirConsequence();
  TheInstallRefusesAndSaysWhyOnAHostWithoutLibr15();
  TheCountersRegisterAndLeaveTheRestOfTheTable();
  ReferenceRuleWithNoSettingsFileStartsAtHigh();
  ReferenceRuleReadsTheMsaaIndexOfTheSettingsFile();
  AConfiguredLowOutranksTheReferenceRule();
  APlayersMarkerOutranksTheReferenceRule();
  TheStockInputNeverUsesTheReferenceRule();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "texture_menu_hook_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("texture_menu_hook_test: all checks pass\n");
  return 0;
}
