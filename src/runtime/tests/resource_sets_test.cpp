// A downloaded map's resource set on the PC (runtime/patch/resource_sets.h RegisterFileResourceSet; docs/adr/0048 decision 5),
// registered from a file the caller holds by the shared rules (core/resource_set_registry.h RegisterMapFile), next to a plugin's set
// registered the way a plugin's init registers one. Every fixture is built here, in a temporary folder; no game file is read. Part
// of test_behavioral, which links resource_sets.cpp with NEVR_TEST_HOOKS and its Log().

#include <gtest/gtest.h>
#include <winsock2.h>  // before windows.h (tools/tests/test_header_include_order.py)
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/logging.h"
#include "core/resource_shadow.h"
#include "nevr/plugin_interface.h"
#include "quest/data/sha256.h"
#include "runtime/patch/asset_registry.h"
#include "runtime/patch/plugin_assets.h"
#include "runtime/patch/resource_set_registry.h"
#include "runtime/patch/resource_sets.h"
#include "runtime/tests/resource_set_builder.h"

// test_behavioral.cpp's log capture.
bool TestLogContainsAtLevel(EchoVR::LogLevel level, const std::string& needle);
void ClearTestLogs();
std::vector<std::string> TestLogLinesContaining(const std::string& needle);

namespace {

namespace sets = nevr_resource_sets;
using nevr_test_sets::Archive;
using nevr_test_sets::Bytes;
using nevr_test_sets::SetBuilder;
using sets::Outcome;
using sets::Refusal;

constexpr std::uint64_t kMapLevel = 0x4d4150004c56454cULL;
constexpr std::uint64_t kMeshType = 0x1111111111111111ULL;
constexpr std::uint64_t kMapMesh = 0x7777777777777777ULL;
constexpr std::uint64_t kStockMesh = 0x8888888888888888ULL;
constexpr std::uint64_t kScriptName = 0x00ab0000000000cdULL;
// The fixture plugin's level (tests/plugin_resources_test_dll.cpp).
constexpr std::uint64_t kPluginLevel = 0x0123456789abcdefULL;

std::set<std::pair<std::uint64_t, std::uint64_t>> g_stock;
bool StockHas(std::uint64_t type, std::uint64_t name) { return g_stock.count({type, name}) != 0; }
std::uint64_t g_now = 1000000;
std::uint64_t Now() { return g_now; }

// A level: its root, its archive listing the map's mesh and `listed`, and the map's mesh. One chunk.
SetBuilder MapLevel(std::uint64_t level = kMapLevel, std::uint64_t listed = kStockMesh) {
  SetBuilder b;
  const std::uint32_t c = b.AddChunk();
  b.AddRecord(sets::kLevelRootType, level, Bytes(64, 0x11), c);
  b.AddRecord(sets::kArchiveType, level,
              Archive({}, {{kMeshType, kMapMesh}, {kMeshType, listed}, {sets::kLevelRootType, level}}), c);
  b.AddRecord(kMeshType, kMapMesh, Bytes(200, 0x22), c);
  return b;
}

std::vector<unsigned char> WithScript() {
  SetBuilder b = MapLevel();
  b.AddRecord(sets::kScriptType, kScriptName, Bytes(32, 0x33), 0);
  return b.Build();
}

std::string Sha256Of(const std::vector<unsigned char>& bytes) { return nevr_quest::data::Sha256Hex(bytes.data(), bytes.size()); }

sets::MapFileRequest Request(const std::string& owner, const std::string& label, const std::string& sha256 = "",
                             bool rehash = false) {
  sets::MapFileRequest r;
  r.owner = owner;
  r.label = label;
  r.sha256 = sha256;
  r.rehash = rehash;
  return r;
}

std::int64_t AsInt(HANDLE h) { return static_cast<std::int64_t>(reinterpret_cast<intptr_t>(h)); }
HANDLE AsHandle(std::int64_t h) { return reinterpret_cast<HANDLE>(static_cast<intptr_t>(h)); }

std::vector<std::string> g_asked;
bool HoldsMapScript(const std::string& owner, std::uint64_t name) {
  g_asked.push_back(owner);
  return owner == "map:arena" && name == kScriptName;
}
bool HoldsNothing(const std::string& owner, std::uint64_t) {
  g_asked.push_back(owner);
  return false;
}
bool HoldsEverything(const std::string&, std::uint64_t) { return true; }

// What the mount observer saw, as the map channel's observer reads it.
struct Observed {
  int calls = 0;
  std::size_t sets = 0;
  std::vector<std::pair<std::string, std::string>> refusals;  // (owner, reason)
  bool mapLevelListed = false;
  bool servedYet = true;
};
Observed g_observed;
int g_manager = 0;  // a manager the resolver asks about; the fake stock answers for every manager
void Observe(const nevr_resource_sets::MountReport& report) {
  ++g_observed.calls;
  g_observed.sets = report.sets;
  for (const auto& r : report.refusals) {
    g_observed.refusals.emplace_back(r.plugin, nevr_resource_sets::RefusalName(r.outcome.refusal));
  }
  const std::vector<std::uint64_t> levels = nevr_resource_sets::MountedLevelNames();  // no lock is held
  g_observed.mapLevelListed = std::find(levels.begin(), levels.end(), kMapLevel) != levels.end();
  g_observed.servedYet =
      nevr_resource_sets::ManagerFor(&g_manager, nevr_resource_sets::kLevelRootType, kMapLevel) != nullptr;
}

class MapFileSetTest : public ::testing::Test {
 protected:
  void SetUp() override {
    sets::ResetForTest();
    ClearTestLogs();
    g_stock = {{kMeshType, kStockMesh}};
    g_now = 1000000;
    g_asked.clear();
    g_observed = Observed{};
    sets::SetStockForTest(&StockHas, 69651);
    sets::SetClockForTest(&Now);
  }
  void TearDown() override {
    sets::SetScriptWaiver(nullptr);
    sets::ResetForTest();  // closes and unmaps every file the registry holds
    if (plugin_ != nullptr) FreeLibrary(plugin_);
    for (const std::wstring& path : files_) {
      EXPECT_TRUE(DeleteFileW(path.c_str())) << "a file the registry took is still open or mapped";
    }
    ClearTestLogs();
  }

  // A fixture file, open as the caller holds a map's file: read-only, shared for reading only.
  std::int64_t Held(const std::vector<unsigned char>& bytes) {
    wchar_t folder[MAX_PATH + 1] = {};
    EXPECT_NE(GetTempPathW(MAX_PATH, folder), 0u);
    const std::wstring path = std::wstring(folder) + L"nevr_map_set_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                              std::to_wstring(files_.size()) + L".set";
    {
      std::ofstream out(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
      out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    files_.push_back(path);
    const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_NE(h, INVALID_HANDLE_VALUE);
    return AsInt(h);
  }

  // A refusal leaves the handle with the caller: it still reads, and the caller closes it.
  static void ExpectCallerKeeps(std::int64_t handle) {
    LARGE_INTEGER size = {};
    EXPECT_TRUE(GetFileSizeEx(AsHandle(handle), &size)) << "the refused registration closed the caller's handle";
    EXPECT_TRUE(CloseHandle(AsHandle(handle)));
  }

  static Outcome Register(std::int64_t handle, const std::string& owner = "map:arena",
                          const std::string& label = "map:arena@3") {
    return sets::RegisterFileResourceSet(Request(owner, label), handle);
  }

  // The fixture plugin's set ("fixture levels", its level kPluginLevel), registered through the plugin's own init, as
  // the loader runs it: register_resources is the host's, open for "levels" on this thread.
  void RegisterPluginSet() {
    wchar_t exe[MAX_PATH + 1] = {};
    ASSERT_NE(GetModuleFileNameW(nullptr, exe, MAX_PATH), 0u);
    std::wstring path(exe);
    path.resize(path.find_last_of(L"\\/") + 1);
    path += L"plugins\\test_plugin_resources.dll";
    plugin_ = LoadLibraryW(path.c_str());
    ASSERT_NE(plugin_, nullptr) << "the fixture plugin is built beside the test, under plugins";
    using InitFn = int (*)(const NevrGameContext*, const char*);
    using StatusFn = int32_t (*)(uint32_t);
    const auto init = reinterpret_cast<InitFn>(::GetProcAddress(plugin_, "NevrPluginInitEx"));
    const auto status = reinterpret_cast<StatusFn>(::GetProcAddress(plugin_, "NevrTestPluginResourcesStatus"));
    ASSERT_NE(init, nullptr);
    ASSERT_NE(status, nullptr);
    NevrGameContext ctx{};
    ctx.ctx_size = sizeof(ctx);
    ctx.plugin_id = "levels";
    ctx.register_resources = &sets::HostRegisterResources;
    sets::BeginPluginInit("levels", plugin_);
    const int rc = init(&ctx, "{}");
    sets::EndPluginInit("levels", rc == 0);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(status(0), NEVR_RESOURCES_OK);
  }

  // The mount as the game runs it: armed after the plugin pass, run by the tick once the game asked for a resource.
  static void Mount() {
    sets::MountWhenReady();
    sets::NoteRequest();
    sets::OnFrame(true);
  }

  static bool LevelMounted(std::uint64_t level) {
    const std::vector<std::uint64_t> levels = sets::MountedLevelNames();
    return std::find(levels.begin(), levels.end(), level) != levels.end();
  }

  std::vector<std::wstring> files_;
  HMODULE plugin_ = nullptr;
};

TEST_F(MapFileSetTest, AMapsFileMountsAndItsLevelIsListedAndReported) {
  const std::int64_t file = Held(MapLevel().Build());
  const Outcome outcome = Register(file);
  ASSERT_TRUE(outcome.ok()) << sets::RefusalName(outcome.refusal) << ": " << outcome.detail;
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Info,
                                     "[NEVR.MAPS] map resource set registered owner=map:arena label=\"map:arena@3\" "
                                     "records=3 chunks=1 levels=1 overrides=0"));
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Info, "rehashed=false"));
  EXPECT_TRUE(sets::HoldsKey(sets::kLevelRootType, kMapLevel));
  Mount();
  EXPECT_TRUE(LevelMounted(kMapLevel));
  const std::string report = sets::ReportJson();
  EXPECT_NE(report.find("\"plugin\":\"map:arena\""), std::string::npos) << report;
  EXPECT_NE(report.find("\"label\":\"map:arena@3\""), std::string::npos) << report;
  EXPECT_NE(report.find("\"levels\":[\"0x4d4150004c56454c\"]"), std::string::npos) << report;
}

TEST_F(MapFileSetTest, AMapSetMountsBesideAPluginsSetAndBothLevelsAreListed) {
  RegisterPluginSet();
  ASSERT_TRUE(Register(Held(MapLevel().Build())).ok());
  Mount();
  EXPECT_TRUE(LevelMounted(kPluginLevel));
  EXPECT_TRUE(LevelMounted(kMapLevel));
}

TEST_F(MapFileSetTest, ARehashThatDiffersFromThePinIsRefusedAndTheCallerKeepsTheFile) {
  const std::vector<unsigned char> bytes = MapLevel().Build();
  const std::int64_t file = Held(bytes);
  std::string wrong = Sha256Of(bytes);
  ASSERT_EQ(wrong.size(), 64u);
  wrong[0] = wrong[0] == '0' ? '1' : '0';
  const Outcome outcome = sets::RegisterFileResourceSet(Request("map:arena", "map:arena@3", wrong, true), file);
  EXPECT_EQ(outcome.refusal, Refusal::kHash);
  EXPECT_EQ(outcome.detail, "sha256 " + Sha256Of(bytes) + ", pinned " + wrong);
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Warning,
                                     "[NEVR.MAPS] map resource set refused owner=map:arena label=\"map:arena@3\" "
                                     "rehashed=true reason=hash_mismatch"));
  EXPECT_FALSE(sets::HoldsKey(sets::kLevelRootType, kMapLevel));
  ExpectCallerKeeps(file);
}

TEST_F(MapFileSetTest, ARehashThatMatchesThePinRegisters) {
  const std::vector<unsigned char> bytes = MapLevel().Build();
  const Outcome outcome = sets::RegisterFileResourceSet(Request("map:arena", "map:arena@3", Sha256Of(bytes), true), Held(bytes));
  EXPECT_TRUE(outcome.ok()) << outcome.detail;
}

// The rehash reads the handle a step at a time (kRehashStepBytes): a file over two steps hashes as the whole, and a known answer pins
// it.
TEST_F(MapFileSetTest, TheRehashReadsTheWholeFileThroughTheHandle) {
  std::vector<unsigned char> big(2 * sets::kRehashStepBytes + 12345);
  for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<unsigned char>(i * 7 + (i >> 13));
  std::int64_t file = Held(big);
  // Not a set, so a matching hash goes on to kFormat; a hash of only the first MiB would be kHash.
  EXPECT_EQ(sets::RegisterFileResourceSet(Request("map:arena", "map:arena@3", Sha256Of(big), true), file).refusal,
            Refusal::kFormat);
  ExpectCallerKeeps(file);
  file = Held({'a', 'b', 'c'});
  EXPECT_EQ(sets::RegisterFileResourceSet(
                Request("map:arena", "map:arena@3", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", true), file)
                .refusal,
            Refusal::kFormat);
  ExpectCallerKeeps(file);
}

TEST_F(MapFileSetTest, AMapSetWithAnOverrideIsRefused) {
  SetBuilder b = MapLevel();
  b.AddOverride(kMeshType, kStockMesh, Bytes(16, 0x44));
  const std::int64_t file = Held(b.Build());
  const Outcome outcome = Register(file);
  EXPECT_EQ(outcome.refusal, Refusal::kOverride);
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Warning, "reason=override"));
  ExpectCallerKeeps(file);
}

TEST_F(MapFileSetTest, AScriptRecordIsRefusedWithNoWaiverAsAPluginsIs) {
  const std::int64_t file = Held(WithScript());
  const Outcome outcome = Register(file);
  EXPECT_EQ(outcome.refusal, Refusal::kScript);
  EXPECT_EQ(outcome.detail, "0x991421b41de58370:0x00ab0000000000cd");
  ExpectCallerKeeps(file);
}

TEST_F(MapFileSetTest, AScriptRecordTheWaiverDoesNotHoldIsRefused) {
  sets::SetScriptWaiver(&HoldsNothing);
  const std::int64_t file = Held(WithScript());
  const Outcome outcome = Register(file);
  EXPECT_EQ(outcome.refusal, Refusal::kScript);
  EXPECT_EQ(outcome.detail, "0x991421b41de58370:0x00ab0000000000cd");
  EXPECT_EQ(g_asked, std::vector<std::string>{"map:arena"});
  ExpectCallerKeeps(file);
}

TEST_F(MapFileSetTest, AScriptRecordTheWaiverHoldsForThisOwnerPassesAndMounts) {
  sets::SetScriptWaiver(&HoldsMapScript);
  const std::int64_t other = Held(WithScript());
  EXPECT_EQ(Register(other, "map:other", "map:other@1").refusal, Refusal::kScript) << "held for map:arena only";
  ExpectCallerKeeps(other);
  ASSERT_TRUE(Register(Held(WithScript())).ok());
  Mount();
  EXPECT_TRUE(LevelMounted(kMapLevel));
}

// The waiver is a map's: a plugin's set carrying a script record is refused whatever the waiver says, and whatever its id.
TEST(MapFileSetRegistry, APluginsSetGetsNoWaiverWhateverItsId) {
  sets::SetScriptWaiver(&HoldsEverything);
  const std::vector<unsigned char> bytes = WithScript();
  sets::Registry registry;
  for (const char* plugin : {"levels", "map:arena"}) {
    const Outcome outcome = registry.Add(plugin, "fixture", bytes.data(), bytes.size(), 7, 4096);
    EXPECT_EQ(outcome.refusal, Refusal::kScript) << plugin;
    EXPECT_EQ(outcome.detail, "0x991421b41de58370:0x00ab0000000000cd") << plugin;
  }
  EXPECT_TRUE(registry.AddMapFile("map:arena", "map:arena@3", bytes.data(), bytes.size(), 7).ok());
  EXPECT_EQ(registry.AddMapFile("arena", "x", bytes.data(), bytes.size(), 8).refusal, Refusal::kFormat);
  sets::SetScriptWaiver(nullptr);
  sets::Registry unwaived;
  EXPECT_EQ(unwaived.AddMapFile("map:arena", "map:arena@3", bytes.data(), bytes.size(), 9).refusal, Refusal::kScript);
}

TEST(MapFileSetRegistry, TheOldRefusalsKeepTheirNames) {
  const std::vector<std::pair<Refusal, std::string>> names = {
      {Refusal::kNone, "none"},         {Refusal::kFormat, "format"},
      {Refusal::kScript, "script_resource"},     {Refusal::kCollides, "collides"},
      {Refusal::kTooLate, "too_late"},  {Refusal::kStockKey, "stock_key"},
      {Refusal::kUnresolved, "unresolved"}, {Refusal::kFrameSize, "frame_size"},
      {Refusal::kHash, "hash_mismatch"}, {Refusal::kOverride, "override"}};
  for (const auto& [refusal, name] : names) EXPECT_EQ(sets::RefusalName(refusal), name);
  EXPECT_EQ(static_cast<int>(Refusal::kHash), static_cast<int>(Refusal::kFrameSize) + 1) << "appended";
}

TEST_F(MapFileSetTest, ARegistrationAfterTheMountIsTooLateAndTheCallerKeepsTheFile) {
  Mount();  // nothing held: registration ends at once
  const std::int64_t file = Held(MapLevel().Build());
  const Outcome outcome = Register(file);
  EXPECT_EQ(outcome.refusal, Refusal::kTooLate);
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Warning, "reason=too_late"));
  EXPECT_FALSE(LevelMounted(kMapLevel));
  ExpectCallerKeeps(file);
}

TEST_F(MapFileSetTest, AMapSetThatTakesAPluginSetsKeyCollidesAndThePluginSetWins) {
  RegisterPluginSet();
  const std::int64_t file = Held(MapLevel(kPluginLevel).Build());
  const Outcome outcome = Register(file);
  EXPECT_EQ(outcome.refusal, Refusal::kCollides);
  EXPECT_NE(outcome.detail.find("levels"), std::string::npos) << outcome.detail;
  ExpectCallerKeeps(file);
  Mount();
  EXPECT_TRUE(LevelMounted(kPluginLevel));
}

// The same claim a plugin's set takes: a plugin asset holds one of the map's keys, so the map's set is refused naming the asset, and
// the caller keeps the file.
TEST_F(MapFileSetTest, AMapSetThatTakesAPluginAssetsKeyCollides) {
  static const unsigned char assetBytes[16] = {};
  ASSERT_EQ(nevr::assets::Global().Register(kMeshType, kMapMesh, assetBytes, sizeof(assetBytes), "asset mesh"),
            nevr::assets::RegisterStatus::Ok);
  const std::int64_t file = Held(MapLevel().Build());
  const Outcome outcome = Register(file);
  nevr::assets::DeregisterPluginAssets(assetBytes, assetBytes + sizeof(assetBytes));
  EXPECT_EQ(outcome.refusal, Refusal::kCollides);
  EXPECT_NE(outcome.detail.find("owner=asset:asset mesh"), std::string::npos) << outcome.detail;
  EXPECT_FALSE(sets::HoldsKey(kMeshType, kMapMesh));
  ExpectCallerKeeps(file);
}

TEST_F(MapFileSetTest, TheSameMapRegisteredTwiceCollides) {
  ASSERT_TRUE(Register(Held(MapLevel().Build())).ok());
  const std::int64_t again = Held(MapLevel().Build());
  EXPECT_EQ(Register(again).refusal, Refusal::kCollides);
  ExpectCallerKeeps(again);
}

TEST_F(MapFileSetTest, AMapSetWithAStockKeyIsRefusedAtTheMountAndThePluginSetStillMounts) {
  RegisterPluginSet();
  g_stock.insert({kMeshType, kMapMesh});
  ASSERT_TRUE(Register(Held(MapLevel().Build())).ok());
  Mount();
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Warning,
                                     "[NEVR.RESOURCE] resource set refused at mount plugin=map:arena "
                                     "label=\"map:arena@3\" reason=stock_key"));
  EXPECT_FALSE(LevelMounted(kMapLevel));
  EXPECT_TRUE(LevelMounted(kPluginLevel));
  EXPECT_EQ(sets::ReportJson().find("map:arena"), std::string::npos);
}

TEST_F(MapFileSetTest, AMapLevelListingWhatNothingServesIsRefusedAtTheMountAndThePluginSetStillMounts) {
  RegisterPluginSet();
  ASSERT_TRUE(Register(Held(MapLevel(kMapLevel, 0x9999999999999999ULL).Build())).ok());
  Mount();
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Warning, "plugin=map:arena label=\"map:arena@3\" reason=unresolved"));
  EXPECT_FALSE(LevelMounted(kMapLevel));
  EXPECT_TRUE(LevelMounted(kPluginLevel));
}

TEST_F(MapFileSetTest, FilesThatAreNotSetsAreBadFormat) {
  for (const std::vector<unsigned char>& bytes : {std::vector<unsigned char>{}, Bytes(47, 0)}) {
    const std::int64_t file = Held(bytes);
    EXPECT_EQ(Register(file).refusal, Refusal::kFormat) << bytes.size() << " bytes";
    ExpectCallerKeeps(file);
  }
  const HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  const Outcome notFile = Register(AsInt(event));
  EXPECT_EQ(notFile.refusal, Refusal::kFormat) << "a handle that is not a file";
  EXPECT_EQ(notFile.detail, "not_a_file");
  CloseHandle(event);
  EXPECT_EQ(Register(AsInt(INVALID_HANDLE_VALUE)).detail, "bad_handle");
  EXPECT_EQ(Register(0).detail, "bad_handle");
}

TEST_F(MapFileSetTest, AnOwnerOrLabelOutsideTheRulesIsRefusedBeforeTheFileIsRead) {
  const std::int64_t file = Held(MapLevel().Build());
  EXPECT_EQ(Register(file, "arena").refusal, Refusal::kFormat) << "not map:<id>";
  EXPECT_EQ(Register(file, "map:").refusal, Refusal::kFormat) << "no id";
  EXPECT_EQ(Register(file, "map:arena", "two\nlines").refusal, Refusal::kFormat);
  EXPECT_EQ(Register(file, "map:arena", std::string(129, 'a')).refusal, Refusal::kFormat);
  EXPECT_EQ(sets::RegisterFileResourceSet(Request("map:arena", "map:arena@3", "ABC", true), file).refusal, Refusal::kFormat);
  EXPECT_FALSE(TestLogContainsAtLevel(EchoVR::LogLevel::Warning, "two\nlines")) << "a label's newline never reaches the log";
  const std::string longest = "map:" + std::string(55, 'a') + "@999999999.999999999.999999999.999999999";
  ASSERT_EQ(longest.size(), sets::kMaxMapLabelBytes);
  EXPECT_EQ(Register(file, "map:" + std::string(55, 'a'), longest + "9").refusal, Refusal::kFormat);
  EXPECT_TRUE(Register(file, "map:" + std::string(55, 'a'), longest).ok());
}

TEST_F(MapFileSetTest, TheObserverSeesTheMountBeforeTheShadowIsServed) {
  sets::SetMountObserver(&Observe);
  ASSERT_TRUE(Register(Held(MapLevel().Build())).ok());
  Mount();
  EXPECT_EQ(g_observed.calls, 1);
  EXPECT_EQ(g_observed.sets, 1u);
  EXPECT_TRUE(g_observed.mapLevelListed);
  EXPECT_FALSE(g_observed.servedYet) << "the observer runs before the first request is answered from the shadow";
  EXPECT_NE(sets::ManagerFor(&g_manager, sets::kLevelRootType, kMapLevel), nullptr) << "served after it";
  sets::OnFrame(true);
  EXPECT_EQ(g_observed.calls, 1) << "once a run";
}

TEST_F(MapFileSetTest, TheObserverSeesAStockKeyRefusalWithItsOwner) {
  sets::SetMountObserver(&Observe);
  RegisterPluginSet();
  g_stock.insert({kMeshType, kMapMesh});
  ASSERT_TRUE(Register(Held(MapLevel().Build())).ok());
  Mount();
  ASSERT_EQ(g_observed.calls, 1);
  EXPECT_EQ(g_observed.refusals,
            (std::vector<std::pair<std::string, std::string>>{{"map:arena", "stock_key"}}));
  EXPECT_EQ(g_observed.sets, 1u) << "the plugin's set";
  EXPECT_TRUE(LevelMounted(kPluginLevel));
}

// A run that abandons registration (the resolver hook is not installed) tells the observer, with an empty report.
TEST_F(MapFileSetTest, AbandonCallsTheObserverWithAnEmptyReport) {
  sets::SetMountObserver(&Observe);
  sets::SetNoResolverHookForTest(true);
  ASSERT_TRUE(Register(Held(MapLevel().Build())).ok());
  sets::MountWhenReady();
  EXPECT_EQ(g_observed.calls, 1);
  EXPECT_EQ(g_observed.sets, 0u);
  EXPECT_TRUE(g_observed.refusals.empty());
  EXPECT_TRUE(TestLogContainsAtLevel(EchoVR::LogLevel::Warning, "reason=no_resolver_hook"));
}

TEST_F(MapFileSetTest, ARunWithNothingHeldTellsTheObserverToo) {
  sets::SetMountObserver(&Observe);
  Mount();
  EXPECT_EQ(g_observed.calls, 1);
  EXPECT_EQ(g_observed.sets, 0u);
}

// The same plugin mount with and without an observer writes the same lines: the observer adds none and moves none.
TEST_F(MapFileSetTest, WithNoObserverTheMountTraceIsUnchanged) {
  std::vector<std::vector<std::string>> traces;
  for (const bool observe : {false, true}) {
    sets::ResetForTest();
    // Chunk indices are never reused within a process, so each pass starts the counter afresh, as a new process
    // would; otherwise the second summary's chunk_base is one past the first's.
    nevr_shadow::Shadow::ResetChunkCounterForTest();
    sets::SetStockForTest(&StockHas, 69651);
    sets::SetClockForTest(&Now);
    if (plugin_ != nullptr) {
      FreeLibrary(plugin_);
      plugin_ = nullptr;
    }
    ClearTestLogs();
    if (observe) sets::SetMountObserver(&Observe);
    RegisterPluginSet();
    Mount();
    traces.push_back(TestLogLinesContaining("[NEVR.RESOURCE] resource set"));
  }
  EXPECT_FALSE(traces[0].empty());
  EXPECT_EQ(traces[0], traces[1]);
  EXPECT_EQ(g_observed.calls, 1);
}

}  // namespace
