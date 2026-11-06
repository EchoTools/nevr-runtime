// Host test for the Texture Resolution control's hooks (#1012): the five real handlers (src/quest/sentinel/texture_menu_hook.cpp),
// built without exceptions as the sentinel builds it, driven through their thunk entries against stand-ins for the game's
// functions: a CEngine laid out at the offsets the pins name, its resize slot, CVR::RefreshRTSize (which asks the eye-size hook,
// as the game's does), CEngine::Resize, the VR flags, the two CJson::DecodeFrom overloads, ValidateGfxSettings and
// ApplyRuntimeGfxSettings. No GOT slot is touched: got_pinned_test.cpp resolves the pinned slots in the real library, and
// tests/quest TestHookFramesCarryNoPersonality covers the frames.
//
// Built and run by `just test-quest-hooks`.

#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "core/nevr_config.h"
#include "core/nevr_config_testing.h"
#include "hand_flip.h"
#include "hook_log.h"
#include "hook_report.h"
#include "quest/tests/test_check.h"
#include "texture_menu.h"
#include "texture_menu_hook.h"

namespace {

namespace tq = nevr_quest::texture_quality;
namespace tm = nevr_quest::texture_menu;
namespace hf = nevr_quest::hand_flip;
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

// A game directory with RAD/LoneEcho in it: the game's settings file (`settings`), and the player's config.yaml (`config`) at the
// root of the directory, each only when given.
std::string MakeSettingsDirectory(const char* settings, const char* config = nullptr) {
  char dir[] = "/tmp/texmenuXXXXXX";
  if (mkdtemp(dir) == nullptr) return "";
  const std::string root = dir;
  const std::string rad = root + "/RAD";
  const std::string lone = rad + "/LoneEcho";
  if (mkdir(rad.c_str(), 0700) != 0 || mkdir(lone.c_str(), 0700) != 0) return "";
  for (const auto& [name, contents] : {std::pair<std::string, const char*>{lone + "/settings_mp_v2_quest.json", settings},
                                       std::pair<std::string, const char*>{root + "/config.yaml", config}}) {
    if (contents == nullptr) continue;
    std::FILE* f = std::fopen(name.c_str(), "w");
    if (f == nullptr) return "";
    std::fputs(contents, f);
    std::fclose(f);
  }
  return root;
}
std::string ConfigPath(const std::string& root) { return root + "/config.yaml"; }
std::string ReadConfig(const std::string& root) {
  std::FILE* f = std::fopen(ConfigPath(root).c_str(), "r");
  if (f == nullptr) return "";
  char buffer[512] = {};
  const std::size_t n = std::fread(buffer, 1, sizeof(buffer) - 1, f);
  std::fclose(f);
  return std::string(buffer, n);
}
// The level the next launch reads from the directory's config.yaml (graphics.texture_quality), as quest_config does, or
// `fallback` when the key is absent.
tq::Level NextLaunchLevel(const std::string& root, tq::Level fallback) {
  const nevr::NevrConfig config = nevr::NevrConfig::LoadFromFile(ConfigPath(root));
  const std::string word = config.GetString("graphics.texture_quality").value_or("");
  tq::Level level = fallback;
  return tq::ParseLevel(word.c_str(), &level) ? level : fallback;
}
void RemoveSettingsDirectory(const std::string& root) {
  std::remove((root + "/RAD/LoneEcho/settings_mp_v2_quest.json").c_str());
  std::remove(ConfigPath(root).c_str());
  std::remove((ConfigPath(root) + ".tmp").c_str());
  rmdir((root + "/RAD/LoneEcho").c_str());
  rmdir((root + "/RAD").c_str());
  rmdir(root.c_str());
}

// The configuration's level (the key the menu writes) starts the game on the very first eye-size answer, before the game has
// delivered anything; and the first delivery is that level, not the game's slot, which may hold anything.
void TheConfigurationsLevelStartsTheGameWhateverTheGamesSlotHolds() {
  const std::string dir = MakeSettingsDirectory(R"({"graphics": {"msaa": 0, "sharpening": 1.5}})", "graphics:\n  texture_quality: very_high\n");
  QCHECK(!dir.empty());
  Reset(tq::Level::kVeryHigh, true);  // the file says Very High
  *ApplyThunk::OriginalOut() = reinterpret_cast<void*>(&FakeApply);
  g_appDataDir = dir;
  QCHECK(Eye().first == 3584 && Eye().second == 3944);  // Very High
  QCHECK(tq::LatchedLevel() == tq::Level::kVeryHigh);
  alignas(8) unsigned char settings[kSettingsBytes] = {};
  SetMsaa(settings, 0);  // the game's slot says Very Low (an older file): the configuration stands, in the settings the game keeps
  Deliver(settings);
  QCHECK(GetMsaa(settings) == 4 && g_applySawMsaa == 4 && tm::CurrentTotals().configOverrides == 1);
  QCHECK(tq::RequestedLevelValue() == static_cast<int>(tq::Level::kVeryHigh));
  g_order.clear();
  Frame();
  QCHECK(g_resizeCalls == 0);  // asked for what is applied
  RemoveSettingsDirectory(dir);
}

void WithNoKeyTheConfigurationsLevelStarts() {
  // Whatever the game's own file holds (a saved `msaa` included: it is not a choice of the player's), with no key in the file.
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
    QCHECK(start.find("\"source\":\"default\"") != std::string::npos && start.find("\"quality\":\"stock\"") != std::string::npos);
    g_order.clear();
    Frame();
    QCHECK(g_resizeCalls == 0 && Eye().first == kQ3W);  // Low is the stock size: nothing is rebuilt
    tm::PollOnceForTest();
    QCHECK(CountLines("texture_quality_saved") == 0 && ReadConfig(dir).empty());  // nothing was chosen, nothing is remembered
    QCHECK(tm::CurrentTotals().saveWrites == 0);
    RemoveSettingsDirectory(dir);
  }
}

// What the player chooses in the menu is remembered, and only that: the next launch starts there. It is remembered as the one key
// graphics.texture_quality in config.yaml, edited in place: the comments and every other key of the file stay.
void AMenuChoicePersistsToTheNextLaunchAsTheConfigKey() {
  const char* const before = "# nevr config\nservices:\n  socket_uri: wss://example.invalid/ws   # kept\n";
  const std::string dir = MakeSettingsDirectory(R"({"graphics": {"msaa": 0}})", before);
  QCHECK(!dir.empty());
  Reset(tq::Level::kStock, false);
  *ApplyThunk::OriginalOut() = reinterpret_cast<void*>(&FakeApply);
  Capture log;
  g_appDataDir = dir;
  tm::SetConfigFile(ConfigPath(dir).c_str());
  Eye();
  alignas(8) unsigned char settings[kSettingsBytes] = {};
  SetMsaa(settings, 0);
  Deliver(settings);  // the load: the default, no choice
  Deliver(settings);  // another setting changed: the same index again is no choice
  tm::PollOnceForTest();
  QCHECK(ReadConfig(dir) == before && tm::CurrentTotals().saveWrites == 0);

  SetMsaa(settings, 4);  // the player picks Very High
  Deliver(settings);
  QCHECK(tm::PollOnceForTest() >= 1);
  QCHECK(ReadConfig(dir) == std::string(before) + "graphics:\n  texture_quality: very_high\n");  // appended; the rest is untouched
  const std::string saved = LineWith("\"event\":\"texture_quality_saved\"");
  QCHECK(saved.find("\"result\":\"written\"") != std::string::npos && saved.find("\"quality\":\"very_high\"") != std::string::npos);
  QCHECK(saved.find("\"menu_index\":4") != std::string::npos);
  QCHECK(tm::CurrentTotals().saveWrites == 1 && tm::CurrentTotals().saveFailures == 0);
  Deliver(settings);  // redelivered unchanged
  tm::PollOnceForTest();
  QCHECK(CountLines("texture_quality_saved") == 1 && tm::CurrentTotals().saveWrites == 1);

  // The next launch reads the key like any other: it starts the game at Very High, and the control shows it whatever the slot holds.
  const tq::Level next = NextLaunchLevel(dir, tq::Level::kStock);
  QCHECK(next == tq::Level::kVeryHigh);
  Reset(next, true);
  *ApplyThunk::OriginalOut() = reinterpret_cast<void*>(&FakeApply);
  g_appDataDir = dir;
  tm::SetConfigFile(ConfigPath(dir).c_str());
  QCHECK(Eye().first == 3584 && Eye().second == 3944);
  SetMsaa(settings, 0);
  Deliver(settings);
  QCHECK(GetMsaa(settings) == 4 && tq::RequestedLevelValue() == static_cast<int>(tq::Level::kVeryHigh));
  g_lines.clear();
  tm::PollOnceForTest();
  const std::string start = LineWith("\"event\":\"texture_quality_start\"");
  QCHECK(start.find("\"source\":\"config\"") != std::string::npos && start.find("\"quality\":\"very_high\"") != std::string::npos);
  QCHECK(CountLines("texture_quality_saved") == 0);  // restoring is not choosing

  // A later choice replaces the value in place (the key is not added twice); Very Low is a choice like any other.
  SetMsaa(settings, 0);
  Deliver(settings);
  tm::PollOnceForTest();
  QCHECK(ReadConfig(dir) == std::string(before) + "graphics:\n  texture_quality: very_low\n");
  QCHECK(NextLaunchLevel(dir, tq::Level::kStock) == tq::Level::kVeryLow);
  Reset(tq::Level::kVeryLow, true);
  g_appDataDir = dir;
  QCHECK(Eye().first == 1344 && Eye().second == 1408);  // Very Low on a Quest 3
  RemoveSettingsDirectory(dir);
}

// A choice that cannot be written is loud, with the reason and what it costs, and counted; the file it could not edit is left as it was.
void AChoiceThatCannotBeWrittenIsLoud() {
  // A file nevr_config will not edit in place (the graphics block is flow style): refused, the file untouched.
  const std::string dir = MakeSettingsDirectory(nullptr, "graphics: {texture_quality: low}\n");
  Reset(tq::Level::kStock, false);
  *ApplyThunk::OriginalOut() = reinterpret_cast<void*>(&FakeApply);
  Capture log;
  g_appDataDir = dir;
  tm::SetConfigFile(ConfigPath(dir).c_str());
  Eye();
  alignas(8) unsigned char settings[kSettingsBytes] = {};
  SetMsaa(settings, 1);
  Deliver(settings);
  SetMsaa(settings, 3);
  Deliver(settings);
  QCHECK(tm::PollOnceForTest() >= 1);
  const std::string refused = LineWith("\"event\":\"texture_quality_saved\"");
  QCHECK(refused.find("\"level\":\"warn\"") != std::string::npos && refused.find("\"result\":\"refused\"") != std::string::npos);
  QCHECK(refused.find("\"reason\":\"section_flow_style\"") != std::string::npos && refused.find("\"consequence\"") != std::string::npos);
  QCHECK(ReadConfig(dir) == "graphics: {texture_quality: low}\n");
  QCHECK(tm::CurrentTotals().saveFailures == 1 && tm::CurrentTotals().saveWrites == 0);
  RemoveSettingsDirectory(dir);

  // A file in a folder that is not there: the write fails with the errno.
  Reset(tq::Level::kStock, false);
  *ApplyThunk::OriginalOut() = reinterpret_cast<void*>(&FakeApply);
  g_lines.clear();
  tm::SetConfigFile("/nonexistent/texmenu/config.yaml");
  SetMsaa(settings, 1);
  Deliver(settings);
  SetMsaa(settings, 3);
  Deliver(settings);
  tm::PollOnceForTest();
  const std::string failed = LineWith("\"event\":\"texture_quality_saved\"");
  QCHECK(failed.find("\"reason\":\"write_failed\"") != std::string::npos && failed.find("\"errno\":2") != std::string::npos);
  QCHECK(tm::CurrentTotals().saveFailures == 1 && tm::CurrentTotals().saveWrites == 0);

  // No config path was given (SetConfigFile never ran): there is nothing to write to, and it says so.
  Reset(tq::Level::kStock, false);
  *ApplyThunk::OriginalOut() = reinterpret_cast<void*>(&FakeApply);
  g_lines.clear();
  SetMsaa(settings, 1);
  Deliver(settings);
  SetMsaa(settings, 3);
  Deliver(settings);
  tm::PollOnceForTest();
  QCHECK(LineWith("\"event\":\"texture_quality_saved\"").find("\"result\":\"no_config_file\"") != std::string::npos);
  QCHECK(tm::CurrentTotals().saveFailures == 1);
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
  QCHECK(g_parsed[0].find("\"name\":\"NEVR\"") != std::string::npos);
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
  const std::string dir = MakeSettingsDirectory(nullptr, "graphics:\n  texture_quality: medium\n");
  Reset(tq::Level::kMedium, true);
  Capture log;
  g_appDataDir = dir;
  Eye();  // the first query: the configured level is Medium
  QCHECK(tm::PollOnceForTest() >= 1);
  const std::string start = LineWith("\"event\":\"texture_quality_start\"");
  QCHECK(start.find("\"source\":\"config\"") != std::string::npos && start.find("\"quality\":\"medium\"") != std::string::npos);
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

// On the reference libr15, with a `stock` configuration, the start is his rule when the settings file names an `msaa`
// index V (level kLow + V). With no file or no key it is the platform default, the headset's own size. Neither is the player's choice,
// so neither is written to config.yaml (#1988).
void ReferenceRuleWithNoSettingsFileStartsAtThePlatformDefaultAndSavesNothing() {
  for (const char* settingsFile : {static_cast<const char*>(nullptr), "{}", R"({"graphics": {"sharpening": 1.5}})"}) {
    Reset(tq::Level::kStock, true);
    tq::SetInputForTest(tq::EyeInput::kReference);
    *ApplyThunk::OriginalOut() = reinterpret_cast<void*>(&FakeApply);
    const std::string dir = MakeSettingsDirectory(settingsFile);
    QCHECK(!dir.empty());
    g_appDataDir = dir;
    Capture log;
    QCHECK(Eye().first == kQ3W && Eye().second == kQ3H);  // the headset's own size, not High
    QCHECK(tq::LatchedLevel() == tq::Level::kStock);
    alignas(8) unsigned char settings[kSettingsBytes] = {};
    SetMsaa(settings, 0);
    Deliver(settings);
    QCHECK(GetMsaa(settings) == tq::kDefaultMenuIndex);  // the control shows Low
    QCHECK(tm::PollOnceForTest() > 0);
    const std::string start = LineWith("\"event\":\"texture_quality_start\"");
    QCHECK(start.find("\"source\":\"platform_default\"") != std::string::npos);
    QCHECK(start.find("\"input\":\"reference\"") != std::string::npos && start.find("\"quality\":\"stock\"") != std::string::npos);
    QCHECK(CountLines("\"source\":\"reference_rule\"") == 0);
    tm::PollOnceForTest();
    QCHECK(CountLines("texture_quality_saved") == 0 && ReadConfig(dir).empty());  // nothing written to config.yaml
    QCHECK(tm::CurrentTotals().saveWrites == 0);
    RemoveSettingsDirectory(dir);
  }
}

void ReferenceRuleReadsTheMsaaIndexOfTheSettingsFileAndSavesNothing() {
  Reset(tq::Level::kStock, true);
  tq::SetInputForTest(tq::EyeInput::kReference);
  const std::string dir = MakeSettingsDirectory(R"({"graphics": {"msaa": 1, "sharpening": 0.5}})");
  QCHECK(!dir.empty());
  g_appDataDir = dir;
  Capture log;
  QCHECK(Eye().first == 2376 && Eye().second == 2568);  // Medium on a Quest 3: kLow + 1
  QCHECK(tq::LatchedLevel() == tq::Level::kMedium);
  QCHECK(tm::PollOnceForTest() > 0);
  QCHECK(CountLines("\"source\":\"reference_rule\"") == 1);
  QCHECK(CountLines("\"reference_file\":\"read\"") == 1);
  tm::PollOnceForTest();
  QCHECK(ReadConfig(dir).empty() && tm::CurrentTotals().saveWrites == 0);  // derived, not chosen
  RemoveSettingsDirectory(dir);
}

// On either input a choice in the menu is written as the config key, and the next launch honors it: the reference rule does not run
// once the key names a level.
void AMenuChoiceIsWrittenAndHonoredOnTheNextLaunchOnEveryInput() {
  for (const tq::EyeInput input : {tq::EyeInput::kReference, tq::EyeInput::kStock}) {
    const std::string dir = MakeSettingsDirectory(R"({"graphics": {"sharpening": 1.5}})");
    QCHECK(!dir.empty());
    Reset(tq::Level::kStock, false);
    tq::SetInputForTest(input);
    *ApplyThunk::OriginalOut() = reinterpret_cast<void*>(&FakeApply);
    Capture log;
    g_appDataDir = dir;
    tm::SetConfigFile(ConfigPath(dir).c_str());
    QCHECK(Eye().first == kQ3W && Eye().second == kQ3H);  // not High
    QCHECK(tq::LatchedLevel() == tq::Level::kStock);
    alignas(8) unsigned char settings[kSettingsBytes] = {};
    SetMsaa(settings, 3);  // what the game's slot may still hold
    Deliver(settings);
    QCHECK(GetMsaa(settings) == tq::kDefaultMenuIndex && tq::RequestedLevelValue() == static_cast<int>(tq::Level::kLow));
    QCHECK(tm::PollOnceForTest() > 0);
    QCHECK(ReadConfig(dir).empty() && tm::CurrentTotals().saveWrites == 0);  // left alone until a real change

    SetMsaa(settings, 2);  // the player picks Medium
    Deliver(settings);
    QCHECK(tm::PollOnceForTest() >= 1);
    QCHECK(ReadConfig(dir) == "graphics:\n  texture_quality: medium\n");

    const tq::Level next = NextLaunchLevel(dir, tq::Level::kStock);  // the next launch
    QCHECK(next == tq::Level::kMedium);
    Reset(next, true);
    tq::SetInputForTest(input);
    g_lines.clear();
    g_appDataDir = dir;
    QCHECK(Eye().first == 2376 && Eye().second == 2568);  // Medium: the key is honored
    tm::PollOnceForTest();
    const std::string again = LineWith("\"event\":\"texture_quality_start\"");
    QCHECK(again.find("\"source\":\"config\"") != std::string::npos && again.find("\"quality\":\"medium\"") != std::string::npos);
    RemoveSettingsDirectory(dir);
  }
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

void AConfiguredVeryLowOutranksTheReferenceRule() {
  Reset(tq::Level::kVeryLow, true);
  tq::SetInputForTest(tq::EyeInput::kReference);
  const std::string dir = MakeSettingsDirectory(R"({"graphics": {"msaa": 3}})", "graphics:\n  texture_quality: very_low\n");
  QCHECK(!dir.empty());
  g_appDataDir = dir;
  Capture log;
  QCHECK(Eye().first == 1344 && Eye().second == 1408);  // Very Low: the key, not msaa 3 (High)
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

// ---- the Controller Hand Flip row (KNOWN-WRONG #2043) -------------------------------------------------------------------

std::uint32_t GetFlags(const unsigned char* settings) {
  std::uint32_t v;
  std::memcpy(&v, settings + hf::kCarrierOffset, sizeof(v));
  return v;
}
void SetFlags(unsigned char* settings, std::uint32_t v) { std::memcpy(settings + hf::kCarrierOffset, &v, sizeof(v)); }

std::uint32_t g_applySawFlags = 0;
void FakeApplyFlags(const unsigned char* settings, void*, std::uint32_t) {
  ++g_applyCalls;
  if (settings != nullptr) g_applySawFlags = GetFlags(settings);
}

// What the Quest's start hands over before the hooks exist (entry.cpp, from the resolved configuration): config.yaml's path and what the
// reader found under graphics.hand_flip. Called after Reset, which clears it.
void ConfigureHandFlip(const std::string& dir, bool fromConfig, bool flip) { hf::Configure(ConfigPath(dir).c_str(), fromConfig, flip); }

// The game's own value of the carrier bit is 1, and that is Off. The first delivery is the load: with no stored choice the bit is
// the default (Off), which is the value the game holds, so nothing is written to it and the poller says so once
// (hand_flip_carrier before=1 after=1). A later delivery with another bit is the player's choice, and the flag the tracking handler
// reads takes it at once: no restart, no re-enumeration, no poll. The choice is written by the poller, as the key graphics.hand_flip
// of config.yaml, and the texture level is not saved. A redelivery of the same bit is no choice.
void AMenuChangeOfTheHandFlipRowAppliesLiveAndIsRemembered() {
  const std::string dir = MakeSettingsDirectory(nullptr);
  QCHECK(!dir.empty());
  Reset(tq::Level::kLow);
  ConfigureHandFlip(dir, false, false);
  Capture log;
  *ApplyThunk::OriginalOut() = reinterpret_cast<void*>(&FakeApplyFlags);
  g_appDataDir = dir;
  Eye();
  alignas(8) unsigned char settings[kSettingsBytes] = {};
  SetMsaa(settings, 1);
  SetFlags(settings, 0x5);  // the game's own flags: bit 0 set (the game's value), TAA set
  Deliver(settings);
  QCHECK(!hf::Enabled() && hf::CurrentBy() == hf::kByDefault);
  QCHECK(GetFlags(settings) == 0x5 && g_applySawFlags == 0x5);  // Off is the game's own value: nothing written to its bit
  Deliver(settings);                                             // another setting changed: no choice
  QCHECK(!hf::Enabled() && hf::Poll() == 2);                     // the start line and the carrier line; nothing to save
  QCHECK(LineWith("hand_flip_carrier").find("\"before\":1,\"after\":1,") != std::string::npos);

  SetFlags(settings, GetFlags(settings) & ~hf::kCarrierBit);  // the player picks On: the bit is cleared
  Deliver(settings);
  QCHECK(hf::Enabled() && hf::CurrentBy() == hf::kByMenu);   // live, before any poll
  QCHECK(GetFlags(settings) == 0x4 && g_applySawFlags == 0x4);  // the game's own function sees what the menu holds
  QCHECK(ReadConfig(dir).empty());  // the start and the load wrote nothing
  QCHECK(hf::Poll() == 1);
  QCHECK(ReadConfig(dir) == "graphics:\n  hand_flip: true\n");
  QCHECK(tm::CurrentTotals().saveWrites == 0 && tm::CurrentTotals().saveFailures == 0);  // the texture level is not saved
  Deliver(settings);                                         // redelivered unchanged
  QCHECK(hf::Enabled() && hf::Poll() == 0);

  SetFlags(settings, GetFlags(settings) | hf::kCarrierBit);  // and Off again: the bit is set
  Deliver(settings);
  QCHECK(!hf::Enabled());
  QCHECK(hf::Poll() == 1 && ReadConfig(dir) == "graphics:\n  hand_flip: false\n");
  QCHECK(tq::RequestedLevelValue() == static_cast<int>(tq::Level::kLow));  // and the texture control did not move
  QCHECK(access((dir + "/RAD/LoneEcho/nevr_hand_flip.json").c_str(), F_OK) != 0);  // no file of the earlier build is written
  RemoveSettingsDirectory(dir);
}

// A game whose own file held the bit clear (not the game's value) is put on Off at the load: the one write a player who never
// touched the toggle can get, and it is logged.
void ADefaultStartPutsAClearedBitBackToTheGamesValue() {
  const std::string dir = MakeSettingsDirectory(nullptr);
  QCHECK(!dir.empty());
  Reset(tq::Level::kLow);
  ConfigureHandFlip(dir, false, false);
  Capture log;
  *ApplyThunk::OriginalOut() = reinterpret_cast<void*>(&FakeApplyFlags);
  g_appDataDir = dir;
  Eye();
  alignas(8) unsigned char settings[kSettingsBytes] = {};
  SetMsaa(settings, 1);
  SetFlags(settings, 0x4);
  Deliver(settings);
  QCHECK(!hf::Enabled() && GetFlags(settings) == 0x5 && g_applySawFlags == 0x5);
  QCHECK(hf::Poll() == 2);
  QCHECK(LineWith("hand_flip_carrier").find("\"before\":0,\"after\":1,") != std::string::npos);
  QCHECK(!hf::Enabled() && ReadConfig(dir).empty());  // a default is never saved
  RemoveSettingsDirectory(dir);
}

// The game can put the stock settings back without the menu (UpdateSettings re-reads the settings JSON, InitCI copies the UI rows
// back). The stock bit is 1, which is Off: a restore after On reads as the player turning it Off, is saved once, and a further
// delivery of the same bit is no change.
void AStockRestoreAfterOnReadsAsOffAndIsSavedOnce() {
  const std::string dir = MakeSettingsDirectory(nullptr, "graphics:\n  hand_flip: true\n");
  QCHECK(!dir.empty());
  ResetDocuments();
  ConfigureHandFlip(dir, true, true);
  Capture log;
  *ApplyThunk::OriginalOut() = reinterpret_cast<void*>(&FakeApplyFlags);
  g_appDataDir = dir;
  Eye();
  alignas(8) unsigned char settings[kSettingsBytes] = {};
  SetMsaa(settings, 1);
  SetFlags(settings, 0x5);
  Deliver(settings);  // the load: On, so the bit is cleared
  QCHECK(hf::Enabled() && GetFlags(settings) == 0x4 && g_applySawFlags == 0x4);
  QCHECK(hf::Poll() == 2);  // the start line and the carrier line (before=1 after=0)
  QCHECK(LineWith("hand_flip_carrier").find("\"before\":1,\"after\":0,") != std::string::npos);
  SetFlags(settings, 0x5);  // the game restores its stock flags
  Deliver(settings);
  QCHECK(!hf::Enabled() && hf::CurrentBy() == hf::kByMenu);
  QCHECK(hf::Poll() == 1 && ReadConfig(dir) == "graphics:\n  hand_flip: false\n");
  Deliver(settings);  // and delivers it again
  QCHECK(!hf::Enabled() && hf::Poll() == 0);
  RemoveSettingsDirectory(dir);
}

// The next launch: the configured choice (graphics.hand_flip: true) is resolved on the first eye-size query, the menu document starts
// the row on it, and the first delivery puts the carrier bit on it whatever the game's own file held; restoring it is not choosing it.
void AStoredChoiceStartsTheRowAndTheFlagAndIsNotSavedAgain() {
  const std::string dir = MakeSettingsDirectory(nullptr, "graphics:\n  hand_flip: true\n");
  QCHECK(!dir.empty());
  ResetDocuments();
  ConfigureHandFlip(dir, true, true);
  Capture log;
  *ApplyThunk::OriginalOut() = reinterpret_cast<void*>(&FakeApplyFlags);
  g_appDataDir = dir;
  Eye();  // the start is resolved here, before the menu document is read
  QCHECK(hf::Started() && hf::Enabled() && hf::CurrentBy() == hf::kByConfig);
  Read("/d/json/r14/config/uisettings/menu_settings.json", kMenuDocument);
  QCHECK(g_parsed.size() == 1);
  QCHECK(g_parsed[0].find(R"("path":"graphics|fullscreen","name":"Controller Hand Flip","value":false,"type":"NAMED FLAG","truestr":"Off","falsestr":"On")") !=
         std::string::npos);
  alignas(8) unsigned char settings[kSettingsBytes] = {};
  SetMsaa(settings, 1);
  SetFlags(settings, 1);  // the game's own value
  Deliver(settings);
  QCHECK(GetFlags(settings) == 0 && g_applySawFlags == 0 && hf::Enabled());
  QCHECK(hf::Poll() == 2);  // the start line and the carrier line, and nothing saved
  QCHECK(ReadConfig(dir) == "graphics:\n  hand_flip: true\n");
  RemoveSettingsDirectory(dir);
}

// A second read of the menu document in one run serves the row's current choice, not the one it was first rewritten with; a read
// with nothing changed in between is still served the kept text.
void AReReadOfTheMenuServesTheCurrentHandFlipChoice() {
  const std::string dir = MakeSettingsDirectory(nullptr);
  QCHECK(!dir.empty());
  ResetDocuments();
  Capture log;
  g_appDataDir = dir;
  Eye();
  const char* const path = "/d/json/r14/config/uisettings/menu_settings.json";
  const char* const off = R"("path":"graphics|fullscreen","name":"Controller Hand Flip","value":true,"type":"NAMED FLAG","truestr":"Off","falsestr":"On")";
  const char* const on = R"("path":"graphics|fullscreen","name":"Controller Hand Flip","value":false,"type":"NAMED FLAG","truestr":"Off","falsestr":"On")";
  Read(path, kMenuDocument);
  Read(path, kMenuDocument);  // nothing changed: the kept text
  QCHECK(g_parsed.size() == 2 && g_parsed[0].find(off) != std::string::npos && g_parsed[0] == g_parsed[1]);
  QCHECK(tm::CurrentTotals().jsonRewrites == 1);
  hf::SetFromMenu(1);  // the player chose On since
  Read(path, kMenuDocument);
  QCHECK(g_parsed.size() == 3 && g_parsed[2].find(on) != std::string::npos && g_parsed[2].find(off) == std::string::npos);
  QCHECK(tm::CurrentTotals().jsonRewrites == 2);
  Read(path, kMenuDocument);
  QCHECK(g_parsed.size() == 4 && g_parsed[3] == g_parsed[2] && tm::CurrentTotals().jsonRewrites == 2);
  hf::SetFromMenu(0);
  Read(path, kMenuDocument);
  QCHECK(g_parsed.size() == 5 && g_parsed[4].find(off) != std::string::npos && g_parsed[4] == g_parsed[0]);
  // The settings document has no row for the choice to start: a change of the choice does not make a re-read rewrite it again.
  const char* const settingsPath = "/d/json/r14/config/gamesettings_config.json";
  Read(settingsPath, kSettingsDocument);
  const unsigned long long rewritten = tm::CurrentTotals().jsonRewrites;
  hf::SetFromMenu(1);
  Read(settingsPath, kSettingsDocument);
  QCHECK(g_parsed.size() == 7 && g_parsed[6] == g_parsed[5] && tm::CurrentTotals().jsonRewrites == rewritten);
  hf::SetFromMenu(0);
  hf::Poll();
  RemoveSettingsDirectory(dir);
}

// ---- the start race, and a row that is not offered (KNOWN-WRONG #2043) ----------------------------------------------------

std::atomic<int> g_raceSeamEntered{0};
std::atomic<int> g_raceSeamReleased{0};
alignas(8) unsigned char g_raceSettings[kSettingsBytes];
void RaceSeam() noexcept {
  g_raceSeamEntered.store(1);
  for (int i = 0; i < 10000 && g_raceSeamReleased.load() == 0; ++i) usleep(1000);
}
void* RaceStartThread(void*) {
  hf::EnsureStarted();  // the tracking handler's first call: it resolves the configured choice, then the seam holds it
  return nullptr;
}
void* RaceDeliverThread(void*) {
  Deliver(g_raceSettings);
  return nullptr;
}
bool RaceWaitFor(const std::atomic<int>& word, int want) {
  for (int i = 0; i < 5000 && word.load() != want; ++i) usleep(1000);
  return word.load() == want;
}

// The tracking thread is inside the start (it has read the stored On and not yet stored it) when the first settings delivery
// arrives: the delivery waits for the start and then writes the bit from On (cleared), where an unsynchronised delivery would have
// read Off and written the game's value over the stored choice.
void AFirstDeliveryWaitsForAStartAnotherThreadIsResolving() {
  const std::string dir = MakeSettingsDirectory(nullptr, "graphics:\n  hand_flip: true\n");
  QCHECK(!dir.empty());
  ResetDocuments();
  ConfigureHandFlip(dir, true, true);
  Capture log;
  *ApplyThunk::OriginalOut() = reinterpret_cast<void*>(&FakeApplyFlags);
  g_appDataDir = dir;
  std::memset(g_raceSettings, 0, sizeof(g_raceSettings));
  SetMsaa(g_raceSettings, 1);
  SetFlags(g_raceSettings, 0x5);
  g_raceSeamEntered.store(0);
  g_raceSeamReleased.store(0);
  hf::g_startSeam.store(&RaceSeam);
  pthread_t starter, deliverer;
  QCHECK(pthread_create(&starter, nullptr, &RaceStartThread, nullptr) == 0);
  QCHECK(RaceWaitFor(g_raceSeamEntered, 1));
  QCHECK(!hf::Started());
  QCHECK(pthread_create(&deliverer, nullptr, &RaceDeliverThread, nullptr) == 0);
  QCHECK(RaceWaitFor(hf::g_startWaiters, 1));   // the delivery is waiting for the start
  QCHECK(GetFlags(g_raceSettings) == 0x5);       // and has written nothing yet
  g_raceSeamReleased.store(1);
  pthread_join(starter, nullptr);
  pthread_join(deliverer, nullptr);
  hf::g_startSeam.store(nullptr);
  QCHECK(hf::Enabled() && GetFlags(g_raceSettings) == 0x4 && g_applySawFlags == 0x4);  // On: the bit cleared
  QCHECK(hf::Poll() == 2);
  QCHECK(LineWith("hand_flip_carrier").find("\"before\":1,\"after\":0,") != std::string::npos);
  RemoveSettingsDirectory(dir);
}

// A delivery that gives up waiting touches nothing and leaves the load to the next delivery.
void AFirstDeliveryThatTimesOutLeavesTheLoadToTheNextOne() {
  const std::string dir = MakeSettingsDirectory(nullptr, "graphics:\n  hand_flip: true\n");
  QCHECK(!dir.empty());
  ResetDocuments();
  ConfigureHandFlip(dir, true, true);
  Capture log;
  *ApplyThunk::OriginalOut() = reinterpret_cast<void*>(&FakeApplyFlags);
  g_appDataDir = dir;
  std::memset(g_raceSettings, 0, sizeof(g_raceSettings));
  SetMsaa(g_raceSettings, 1);
  SetFlags(g_raceSettings, 0x5);
  g_raceSeamEntered.store(0);
  g_raceSeamReleased.store(0);
  hf::g_startSeam.store(&RaceSeam);
  hf::g_startWaitMs.store(50);
  pthread_t starter;
  QCHECK(pthread_create(&starter, nullptr, &RaceStartThread, nullptr) == 0);
  QCHECK(RaceWaitFor(g_raceSeamEntered, 1));
  Deliver(g_raceSettings);
  QCHECK(GetFlags(g_raceSettings) == 0x5 && g_applySawFlags == 0x5 && !hf::Enabled());  // untouched
  g_raceSeamReleased.store(1);
  pthread_join(starter, nullptr);
  hf::g_startSeam.store(nullptr);
  Deliver(g_raceSettings);  // the next delivery is the load
  QCHECK(hf::Enabled() && GetFlags(g_raceSettings) == 0x4 && g_applySawFlags == 0x4);
  QCHECK(hf::Poll() == 3);  // the start line, the timeout line, the carrier line
  QCHECK(LineWith("hand_flip_start_wait").find("\"level\":\"warn\"") != std::string::npos);
  RemoveSettingsDirectory(dir);
}

const char* const kSettingsWithFullscreen =
    R"json({"quest": {"settings": [{"path": "graphics|msaa", "operator": "remove"},
      {"path": "graphics|fullscreen", "value": true, "operator": "remove"}], "pages": ["GRAPHICS (1/3)"]}})json";

// With the hand pose hooks not installed the row is not offered: the menu has none, the settings document keeps the stock hiding
// entry, the configured choice is ignored, the game's bit is never written (not at the load, not on a change), nothing is saved, and
// the texture poller says so once.
void ARowThatIsNotOfferedIsNotInTheMenuAndTheBitIsLeftAlone() {
  const std::string dir = MakeSettingsDirectory(nullptr, "graphics:\n  hand_flip: true\n");
  QCHECK(!dir.empty());
  ResetDocuments();
  ConfigureHandFlip(dir, true, true);
  hf::SetOffered(false);
  Capture log;
  *ApplyThunk::OriginalOut() = reinterpret_cast<void*>(&FakeApplyFlags);
  g_appDataDir = dir;
  Eye();
  QCHECK(hf::Started() && !hf::Enabled() && hf::CurrentBy() == hf::kByDefault);  // the configured On is not used
  Read("/d/json/r14/config/uisettings/menu_settings.json", kMenuDocument);
  Read("/d/json/r14/config/gamesettings_config.json", kSettingsWithFullscreen);
  QCHECK(g_parsed.size() == 2);
  QCHECK(g_parsed[0].find("Texture Resolution") != std::string::npos && g_parsed[0].find("Controller Hand Flip") == std::string::npos);
  QCHECK(g_parsed[1].find("graphics|fullscreen") != std::string::npos && g_parsed[1].find("graphics|msaa") == std::string::npos);
  alignas(8) unsigned char settings[kSettingsBytes] = {};
  SetMsaa(settings, 1);
  SetFlags(settings, 0x5);
  Deliver(settings);
  QCHECK(GetFlags(settings) == 0x5 && g_applySawFlags == 0x5);  // the load does not write the bit
  SetFlags(settings, 0x4);                                      // the game's bit changes, and no row is there to have changed it
  Deliver(settings);
  QCHECK(!hf::Enabled() && hf::CurrentBy() == hf::kByDefault);
  hf::Poll();  // (the start line, if the poller runs)
  QCHECK(LineWith("hand_flip_carrier").empty() && LineWith("hand_flip_saved").empty());  // no carrier write, no save
  QCHECK(ReadConfig(dir) == "graphics:\n  hand_flip: true\n");  // the file is as it was
  tm::PollOnceForTest();
  tm::PollOnceForTest();
  QCHECK(CountLines("\"event\":\"hand_flip_row\"") == 1);
  QCHECK(LineWith("\"document\":\"menu_settings\"").find("\"hand_flip_rows\":0}") != std::string::npos);
  RemoveSettingsDirectory(dir);
}

// The tracking handler can need the flag before the eye-size query has resolved it: it resolves the start itself, from the same
// configuration, with no game call and no file read.
void TheTrackingSideResolvesTheStartFromTheConfiguration() {
  const std::string dir = MakeSettingsDirectory(nullptr, "graphics:\n  hand_flip: true\n");
  Reset(tq::Level::kLow);
  ConfigureHandFlip(dir, true, true);
  g_appDataDir = dir;
  QCHECK(!hf::Started());
  hf::EnsureStarted();
  QCHECK(hf::Started() && hf::Enabled() && hf::CurrentBy() == hf::kByConfig);
  RemoveSettingsDirectory(dir);
}

// The texture poller and the hand pose poller both edit config.yaml (NevrConfig::Set reads the file, splices and replaces it through
// one temporary file): they take turns. While one is inside the rename that publishes its edit the other has not started its own, and
// afterwards the file holds both keys (a second writer that read the file before the first published would have dropped the first's).
std::atomic<int> g_renames{0};
std::atomic<int> g_renameRelease{0};
int BlockingRename(const char* from, const char* to) {
  if (g_renames.fetch_add(1) == 0) {
    for (int i = 0; i < 10000 && g_renameRelease.load() == 0; ++i) usleep(1000);
  }
  return std::rename(from, to) == 0 ? 0 : errno;
}
void* HandFlipPollThread(void*) {
  hf::Poll();
  return nullptr;
}
void* TexturePollThread(void*) {
  tm::PollOnceForTest();
  return nullptr;
}

void TheTwoPollersNeverEditConfigYamlAtTheSameTime() {
  const std::string dir = MakeSettingsDirectory(nullptr, "graphics:\n  texture_quality: low\n");
  QCHECK(!dir.empty());
  Reset(tq::Level::kLow);
  ConfigureHandFlip(dir, false, false);
  tm::SetConfigFile(ConfigPath(dir).c_str());
  Capture log;
  *ApplyThunk::OriginalOut() = reinterpret_cast<void*>(&FakeApplyFlags);
  g_appDataDir = dir;
  Eye();
  alignas(8) unsigned char settings[kSettingsBytes] = {};
  SetMsaa(settings, 1);
  SetFlags(settings, 0x5);
  Deliver(settings);  // the load
  hf::Poll();
  SetMsaa(settings, 4);                                       // the player picks Very High ...
  SetFlags(settings, GetFlags(settings) & ~hf::kCarrierBit);  // ... and the hand flip On, in the same delivery
  Deliver(settings);
  QCHECK(hf::Enabled());
  g_renames.store(0);
  g_renameRelease.store(0);
  nevr::config_testing::SetRenameForTest(&BlockingRename);
  pthread_t first, second;
  QCHECK(pthread_create(&first, nullptr, &HandFlipPollThread, nullptr) == 0);
  QCHECK(RaceWaitFor(g_renames, 1));  // the hand pose poller is inside its rename
  QCHECK(pthread_create(&second, nullptr, &TexturePollThread, nullptr) == 0);
  usleep(300 * 1000);                 // the texture poller has had time to reach its own rename, were it let
  QCHECK(g_renames.load() == 1);
  g_renameRelease.store(1);
  pthread_join(first, nullptr);
  pthread_join(second, nullptr);
  nevr::config_testing::SetRenameForTest(nullptr);
  QCHECK(g_renames.load() == 2);
  QCHECK(ReadConfig(dir) == "graphics:\n  texture_quality: very_high\n  hand_flip: true\n");
  QCHECK(access((ConfigPath(dir) + ".tmp").c_str(), F_OK) != 0);
  RemoveSettingsDirectory(dir);
}

// The settings document stops hiding the new row's setting along with the control's, and the poller says how many rows each
// document got.
void TheSettingsDocumentStopsHidingTheHandFlipSetting() {
  ResetDocuments();
  Capture log;
  Read("/d/json/r14/config/gamesettings_config.json",
       R"json({"quest": {"settings": [{"path": "graphics|msaa", "operator": "remove"}, {"path": "graphics|fullscreen", "operator": "remove"}], "pages": ["GRAPHICS (1/3)"]}})json");
  QCHECK(g_parsed.size() == 1 && g_parsed[0].find("graphics|fullscreen") == std::string::npos);
  Read("/d/json/r14/config/uisettings/menu_settings.json", kMenuDocument);
  QCHECK(tm::PollOnceForTest() >= 2);
  const std::string menu = LineWith("\"document\":\"menu_settings\"");
  QCHECK(menu.find("\"result\":\"rewritten\",\"rows\":1,\"pages\":1,\"hand_flip_rows\":1}") != std::string::npos);
  const std::string settings = LineWith("\"document\":\"gamesettings_config\"");
  QCHECK(settings.find("\"result\":\"rewritten\",\"rows\":1,\"pages\":1,\"hand_flip_rows\":1}") != std::string::npos);
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
  TheConfigurationsLevelStartsTheGameWhateverTheGamesSlotHolds();
  WithNoKeyTheConfigurationsLevelStarts();
  AFreshSettingsFileStartsAtTheDefaultNotVeryLow();
  AMenuChoicePersistsToTheNextLaunchAsTheConfigKey();
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
  ReferenceRuleWithNoSettingsFileStartsAtThePlatformDefaultAndSavesNothing();
  AMenuChoiceIsWrittenAndHonoredOnTheNextLaunchOnEveryInput();
  ReferenceRuleReadsTheMsaaIndexOfTheSettingsFileAndSavesNothing();
  AConfiguredLowOutranksTheReferenceRule();
  AConfiguredVeryLowOutranksTheReferenceRule();
  AMenuChangeOfTheHandFlipRowAppliesLiveAndIsRemembered();
  ADefaultStartPutsAClearedBitBackToTheGamesValue();
  AStockRestoreAfterOnReadsAsOffAndIsSavedOnce();
  AReReadOfTheMenuServesTheCurrentHandFlipChoice();
  AFirstDeliveryWaitsForAStartAnotherThreadIsResolving();
  AFirstDeliveryThatTimesOutLeavesTheLoadToTheNextOne();
  ARowThatIsNotOfferedIsNotInTheMenuAndTheBitIsLeftAlone();
  AStoredChoiceStartsTheRowAndTheFlagAndIsNotSavedAgain();
  TheTrackingSideResolvesTheStartFromTheConfiguration();
  TheSettingsDocumentStopsHidingTheHandFlipSetting();
  TheTwoPollersNeverEditConfigYamlAtTheSameTime();
  TheStockInputNeverUsesTheReferenceRule();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "texture_menu_hook_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("texture_menu_hook_test: all checks pass\n");
  return 0;
}
