// Host test of the shield colour module (src/quest/combat/shieldgamma.cpp, #2186, row R58): the real handler through the thunk, installed by the
// real GotHook into the JUMP_SLOT of a libr15 image built in memory (combat_libr15_image.h) whose slot holds base + the pinned function, and
// called the way the game calls it: through the slot. The fake CSetMaterialPropertyNode::Update records every call.
//
// The checks 1 to 10 are the combat plugin's shieldgamma_test (#1591): the assertions are the same and the fixture is the manager's (the image, the
// slot and the log lines are the combat area's, and the module acts in a combat level, which the test puts the game in through the real combat
// level answer, combat_scope.h). The rest is new: the arena path (no combat level loaded: the game's function with the same arguments, no read of
// the data, no write, no second call, no line), the combat lobby and the lobbies arena players use, and each refusal.
//
// Built and run by `just test-quest-combat`.

#include "quest/combat/shieldgamma.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "quest/combat/combat_level.h"
#include "quest/combat/combat_scope.h"
#include "quest/combat/combat_tick.h"
#include "quest/combat/combat_tick_targets.h"
#include "quest/combat/safe_read.h"
#include "quest/combat/shieldgamma_targets.h"
#include "quest/tests/combat_game_model.h"
#include "quest/tests/combat_libr15_image.h"
#include "quest/tests/test_check.h"

namespace {

namespace sg = nevr_quest::combat::shieldgamma;
namespace scope = nevr_quest::combat::scope;
namespace tick = nevr_quest::combat::tick;
namespace pinned = sentinel::pinned;
using combat_test::Image;

struct Line {
  sentinel::LogLevel level;
  std::string text;
};
std::vector<Line> g_lines;
void Capture(sentinel::LogLevel level, const char* line) { g_lines.push_back({level, line}); }
bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }
int Count(const char* event, const char* needle = "", int level = -1) {
  int n = 0;
  const std::string key = std::string("\"event\":\"") + event + "\"";
  for (const Line& l : g_lines) {
    if (Has(l.text, key) && Has(l.text, needle) && (level < 0 || static_cast<int>(l.level) == level)) ++n;
  }
  return n;
}
constexpr int kInfo = static_cast<int>(sentinel::LogLevel::kInfo);
constexpr int kError = static_cast<int>(sentinel::LogLevel::kError);

constexpr std::uint64_t kMat = sg::kMaterial, kTube = sg::kTubeMaterial, kRing = sg::kRingProperty;
constexpr std::uint64_t kOtherProp = 0xd1b26f18aaaaaaaaULL;

struct Call {
  const void* node;
  const void* data;
  std::uint8_t bytes[0x58];
};
std::vector<Call> g_calls;
bool g_reads = true;
bool g_record = true;  // false: the cost loop, which must not grow a vector
std::atomic<std::uint64_t> g_safeReads{0};  // every SafeRead the module makes (safe_read.h's test counter)
std::uint64_t FakeUpdate(void* node, const void* data) {
  if (!g_record) return 0x71;
  Call c{node, data, {}};
  if (g_reads) std::memcpy(c.bytes, data, sizeof c.bytes);
  g_calls.push_back(c);
  return 0x70 + g_calls.size();
}
using UpdateFn = std::uint64_t (*)(void*, const void*);

struct alignas(16) Data {
  std::uint8_t b[0x60];
};
Data MakeData(std::uint64_t mat, std::uint64_t prop, std::int32_t type, float r, float g, float b, float a, std::int32_t space) {
  Data d;
  std::memset(d.b, 0x5c, sizeof d.b);
  std::memcpy(d.b + 0x20, &mat, 8);
  std::memcpy(d.b + 0x28, &prop, 8);
  std::memcpy(d.b + 0x30, &type, 4);
  const float v[4] = {r, g, b, a};
  std::memcpy(d.b + 0x38, v, 16);
  std::memcpy(d.b + 0x4c, &space, 4);
  return d;
}
void Rgba(const std::uint8_t* b, float* out) { std::memcpy(out, b + 0x38, 16); }
std::uint64_t Mat(const std::uint8_t* b) {
  std::uint64_t m;
  std::memcpy(&m, b + 0x20, 8);
  return m;
}
float Dec(float x) { return std::pow(x < 0 ? 0 : (x > 1 ? 1 : x), 2.2f); }

Image g_image;
combat_test::GameModel g_model;
std::int64_t g_nowMs = 1000;
std::int64_t Clock() noexcept { return g_nowMs; }
bool Lookup(const char* module, sentinel::ElfImage* out) {
  if (!g_image.Present() || std::strcmp(module, pinned::kLibR15) != 0) return false;
  *out = g_image.Elf();
  return true;
}
UpdateFn Slot() { return reinterpret_cast<UpdateFn>(*g_image.Slot(sg::kSlotVaddr)); }
bool Hooked() { return *g_image.Slot(sg::kSlotVaddr) == sg::Thunk::EntryAddress(); }

void FakeTickFrame(void*) {}
void Build() {
  namespace tt = nevr_quest::combat::tick::targets;
  g_image.Build({{sg::kSymbol, sg::kSlotVaddr, false, false, sg::kFnVaddr}, {tt::kSymbol, tt::kSlotVaddr, false, false, tt::kStockVaddr}});
  *g_image.Slot(tt::kSlotVaddr) = reinterpret_cast<void*>(&FakeTickFrame);  // the game's function: any address in an executable mapping
  g_image.Real(sg::kFnVaddr, reinterpret_cast<void*>(&FakeUpdate));
  *g_image.Slot(sg::kSlotVaddr) = reinterpret_cast<void*>(g_image.Base() + sg::kFnVaddr);
}
// `scopeLive`: the combat tick is installed on the image and the `scope` row has registered the refresh (what the shipped table does before this module).
void Begin(bool scopeLive = true) {
  g_lines.clear();
  g_calls.clear();
  g_reads = true;
  g_nowMs = 1000;
  g_model.Clear();
  tick::ResetForTest();
  scope::ResetForTest();
  scope::SetClockForTest(&Clock);
  sg::ResetForTest();
  sentinel::SetLogSink(&Capture);
  Build();
  if (scopeLive) {
    QCHECK(tick::InstallAt(&Lookup));
    QCHECK(scope::InstallNormal().installed && scope::Live());
    g_lines.clear();
  }
}
void End() {
  sg::ResetForTest();
  scope::ResetForTest();
  tick::ResetForTest();
  g_image.Drop();
  sentinel::SetLogSink(nullptr);
}
void Levels(std::initializer_list<const char*> names) {
  g_model.Only(names);
  g_nowMs += scope::kLookEveryMs;
  scope::Refresh(g_model.Address());
}
void LevelsOf(const char* first, const char* second) {
  g_model.Clear();
  int row = 0;
  for (const char* name : {first, second}) {
    if (name != nullptr) g_model.PutLevel(0, row++, EchoVR::CSymbol64Hash(name));
  }
  g_nowMs += scope::kLookEveryMs;
  scope::Refresh(g_model.Address());
}
void EnterCombat() { Levels({"mpl_combat_dyson"}); }

void ThePinsAreTheReadelfAndDisassemblyPins() {
  QCHECK(sg::kSlotVaddr == 0x36d2020 && sg::kFnVaddr == 0x272d55c);
  QCHECK(std::strcmp(sg::kSymbol, "_ZN10NRadEngine24CSetMaterialPropertyNode6UpdateERKNS_28SSetMaterialPropertyNodeDataE") == 0);
  QCHECK(kMat == 0xdbde1b05fa8f6200ULL && kTube == 0xb28e940eb21a5798ULL && kRing == 0xdd35e419672ee0baULL);
  QCHECK(sg::kGamma == 2.2f && sg::kVec4 == 1 && sg::kLinear == 0 && sg::kDataSize == 0x58);
  QCHECK(sg::kDataMaterial == 0x20 && sg::kDataProperty == 0x28 && sg::kDataType == 0x30 && sg::kDataValue == 0x38 && sg::kDataSpace == 0x4c);
}

void TheModuleActsInACombatLevelAsThePluginDid() {
  Begin();
  void* node = reinterpret_cast<void*>(0x6000);
  // 1. gamma only (the reference build's shieldgamma.on, tube mirror off)
  nevr_quest::combat::ModuleResult ir = sg::InstallAt(&Lookup, true, false);
  QCHECK(ir.installed && std::strcmp(ir.status, "installed") == 0 && Hooked());
  QCHECK(Count("combat_shieldgamma_install", "\"result\":\"installed\"", kInfo) == 1);
  QCHECK(Count("combat_shieldgamma_install", "\"gamma\":1,\"tube_mirror\":0") == 1);
  QCHECK(Count("got_hook", "\"owner\":\"combat_shieldgamma\"") >= 1);  // the manager's line names who installed the hook (what a refusal on a shared slot shows)
  EnterCombat();
  UpdateFn update = Slot();
  Data d = MakeData(kMat, kOtherProp, 1, 0.941f, 0.466f, 0.054f, 0.25f, 0);
  std::uint64_t r = update(node, &d);
  QCHECK(g_calls.size() == 1 && g_calls[0].node == node && g_calls[0].data == &d);
  float seen[4];
  Rgba(g_calls[0].bytes, seen);
  QCHECK(seen[0] == Dec(0.941f) && seen[1] == Dec(0.466f) && seen[2] == Dec(0.054f));
  QCHECK(seen[3] == 0.25f);
  float after[4];
  Rgba(d.b, after);
  QCHECK(after[0] == 0.941f && after[1] == 0.466f && after[2] == 0.054f && after[3] == 0.25f);
  QCHECK(r == 0x71);
  QCHECK(Count("combat_shieldgamma_decode") == 1 && Count("combat_shieldgamma_decode", "\"n\":1,") == 1);
  QCHECK(std::fabs(seen[0] - 0.8738f) < 0.01f);  // the decode of the orange team colour

  // 2. the clamp: sat() before pow
  g_calls.clear();
  d = MakeData(kMat, kOtherProp, 1, -0.5f, 0.5f, 2.0f, 1.0f, 0);
  update(node, &d);
  Rgba(g_calls[0].bytes, seen);
  QCHECK(seen[0] == 0.0f && seen[1] == Dec(0.5f) && seen[2] == 1.0f);

  // 3. what is not a linear vec4 write to the shield material passes with the original pointer and bytes
  struct Case {
    const char* what;
    Data d;
  } cases[] = {
      {"other material", MakeData(kMat + 1, kOtherProp, 1, 0.5f, 0.5f, 0.5f, 0.5f, 0)},
      {"float type", MakeData(kMat, kOtherProp, 0, 0.5f, 0.5f, 0.5f, 0.5f, 0)},
      {"sRGB colourspace", MakeData(kMat, kOtherProp, 1, 0.5f, 0.5f, 0.5f, 0.5f, 1)},
  };
  for (auto& c : cases) {
    g_calls.clear();
    update(node, &c.d);
    QCHECK(g_calls.size() == 1 && g_calls[0].data == &c.d);
    float v[4];
    Rgba(g_calls[0].bytes, v);
    QCHECK(v[0] == 0.5f && v[1] == 0.5f && v[2] == 0.5f && v[3] == 0.5f);
  }
  // the ring colour to the shield material with the mirror off: no second call
  g_calls.clear();
  d = MakeData(kMat, kRing, 1, 0.5f, 0.5f, 0.5f, 0.5f, 0);
  update(node, &d);
  QCHECK(g_calls.size() == 1);

  // 4. thinning: decode lines on the 1st, 2nd, 4th, 8th ...
  g_lines.clear();
  for (int i = 0; i < 6; ++i) update(node, &d);  // decodes so far: 3 (cases 1, 2 and the ring write); these are n = 4..9
  QCHECK(Count("combat_shieldgamma_decode") == 2);  // n=4 and n=8

  // 5. no per-call logging for calls that are not decoded
  g_lines.clear();
  Data other = MakeData(kMat + 5, kOtherProp, 1, 0.1f, 0.2f, 0.3f, 0.4f, 0);
  for (int i = 0; i < 2000; ++i) update(node, &other);
  QCHECK(g_lines.empty());

  // 6. unreadable data: the real function, same arguments, nothing written
  g_calls.clear();
  g_reads = false;
  const unsigned un = sg::CurrentStats().unreadable;
  r = update(node, nullptr);
  QCHECK(g_calls.size() == 1 && g_calls[0].data == nullptr && g_calls[0].node == node && r == 0x71);
  QCHECK(sg::CurrentStats().unreadable == un + 1);
  g_reads = true;
  End();

  // 7. mirror only (the reference build's default: tube mirror ON, gamma off)
  Begin();
  ir = sg::InstallAt(&Lookup, false, true);
  QCHECK(ir.installed && Count("combat_shieldgamma_install", "\"gamma\":0,\"tube_mirror\":1") == 1);
  EnterCombat();
  update = Slot();
  d = MakeData(kMat, kRing, 1, 0.941f, 0.466f, 0.054f, 0.25f, 0);
  r = update(node, &d);
  QCHECK(g_calls.size() == 2);
  if (g_calls.size() == 2) {
    QCHECK(g_calls[0].data != &d && g_calls[0].node == node);
    QCHECK(Mat(g_calls[0].bytes) == kTube);
    Rgba(g_calls[0].bytes, seen);
    QCHECK(seen[0] == Dec(0.941f) && seen[1] == Dec(0.466f) && seen[2] == Dec(0.054f) && seen[3] == 0.25f);
    // every other byte of the copy is the original's
    Data o = MakeData(kMat, kRing, 1, 0.941f, 0.466f, 0.054f, 0.25f, 0);
    std::uint8_t cmp[0x58];
    std::memcpy(cmp, o.b, sizeof cmp);
    std::memcpy(cmp + 0x20, &kTube, 8);
    float dv[4] = {Dec(0.941f), Dec(0.466f), Dec(0.054f), 0.25f};
    std::memcpy(cmp + 0x38, dv, 16);
    QCHECK(std::memcmp(cmp, g_calls[0].bytes, sizeof cmp) == 0);
    QCHECK(g_calls[1].data == &d && Mat(g_calls[1].bytes) == kMat);
    Rgba(g_calls[1].bytes, seen);
    QCHECK(seen[0] == 0.941f && seen[1] == 0.466f && seen[2] == 0.054f);  // gamma off: the original call carries the raw rgb
  }
  QCHECK(r == 0x72);  // the original call's result is the one returned
  QCHECK(Count("combat_shieldgamma_mirror") == 1);
  float orig[4];
  Rgba(d.b, orig);
  QCHECK(orig[0] == 0.941f && Mat(d.b) == kMat);
  // a non-ring property, or a material other than the shield's: no mirror
  g_calls.clear();
  d = MakeData(kMat, kOtherProp, 1, 0.5f, 0.5f, 0.5f, 0.5f, 0);
  update(node, &d);
  QCHECK(g_calls.size() == 1);
  g_calls.clear();
  d = MakeData(kMat + 1, kRing, 1, 0.5f, 0.5f, 0.5f, 0.5f, 0);
  update(node, &d);
  QCHECK(g_calls.size() == 1);
  g_calls.clear();
  d = MakeData(kMat, kRing, 1, 0.5f, 0.5f, 0.5f, 0.5f, 1);
  update(node, &d);
  QCHECK(g_calls.size() == 1);
  End();

  // 8. both: the copy is decoded and the original call is decoded in place and put back
  Begin();
  sg::InstallAt(&Lookup, true, true);
  EnterCombat();
  update = Slot();
  d = MakeData(kMat, kRing, 1, 0.2f, 0.4f, 0.6f, 0.8f, 0);
  r = update(node, &d);
  QCHECK(g_calls.size() == 2 && Mat(g_calls[0].bytes) == kTube && g_calls[1].data == &d);
  if (g_calls.size() == 2) {
    Rgba(g_calls[0].bytes, seen);
    QCHECK(seen[0] == Dec(0.2f) && seen[3] == 0.8f);
    Rgba(g_calls[1].bytes, seen);
    QCHECK(seen[0] == Dec(0.2f) && seen[1] == Dec(0.4f) && seen[2] == Dec(0.6f) && seen[3] == 0.8f);  // decoded once, not twice
  }
  Rgba(d.b, orig);
  QCHECK(orig[0] == 0.2f && orig[1] == 0.4f && orig[2] == 0.6f);
  QCHECK(r == 0x72);
  End();
}

// 9. both off: nothing installed, one Info line (and nothing enrolled)
void BothSwitchesOffInstallNothing() {
  Begin(/*scopeLive=*/false);
  const nevr_quest::combat::ModuleResult r = sg::InstallAt(&Lookup, false, false);
  QCHECK(!r.installed && r.off && std::strcmp(r.status, "off") == 0);
  QCHECK(!Hooked() && *g_image.Slot(sg::kSlotVaddr) == reinterpret_cast<void*>(g_image.Base() + sg::kFnVaddr));
  QCHECK(Count("combat_shieldgamma_install", "\"result\":\"off\",\"reason\":\"off_config\"", kInfo) == 1 && g_lines.size() == 1U);
  QCHECK(tick::Count() == 0 && !scope::Live());
  End();
}

// 10. a slot that does not hold the pinned function: refused, untouched, Error
void AForeignSlotWordIsRefusedAndLeftAlone() {
  Begin();
  *g_image.Slot(sg::kSlotVaddr) = reinterpret_cast<void*>(0x4242420);
  const nevr_quest::combat::ModuleResult r = sg::InstallAt(&Lookup, true, true);
  QCHECK(!r.installed && std::strcmp(r.status, sentinel::GotStatusName(sentinel::GotStatus::kOriginalMismatch)) == 0);
  QCHECK(*g_image.Slot(sg::kSlotVaddr) == reinterpret_cast<void*>(0x4242420));
  QCHECK(Count("combat_shieldgamma_install", "\"result\":\"refused\"", kError) == 1);
  End();
}

// The arena path: a hook installed on a slot arena's scripts also call. No combat level loaded -> the game's function with the very same
// arguments, once (no mirror call), no read of the data, no write, no counter, no line; with both switches on.
void OutsideACombatLevelTheCallPassesThroughUntouched() {
  Begin();
  QCHECK(sg::InstallAt(&Lookup, true, true).installed);
  g_lines.clear();
  void* node = reinterpret_cast<void*>(0x6000);
  const UpdateFn update = Slot();
  struct Case {
    const char* what;
    const char* first;   // a level the game has loaded, or nullptr for none
    const char* second;  // another, or nullptr
  };
  const Case cases[] = {{"before the first look", nullptr, nullptr},
                        {"arena a", "mpl_arena_a", nullptr},
                        {"arena b", "mpl_arena_b", nullptr},
                        {"tutorial", "mpl_tutorial_a", nullptr},
                        {"the arena lobby", "mpl_lobby_b", nullptr},
                        {"the social lobby", "mpl_lobby_b_social", nullptr},
                        {"combat to arena", "mpl_combat_gauss", "mpl_arena_a"},
                        {"nothing loaded", nullptr, nullptr}};
  for (const Case& c : cases) {
    if (std::strcmp(c.what, "before the first look") != 0) LevelsOf(c.first, c.second);
    QCHECK(!scope::InCombatOrLobby());
    g_calls.clear();
    Data d = MakeData(kMat, kRing, 1, 0.941f, 0.466f, 0.054f, 0.25f, 0);  // the write both parts act on in combat
    const std::uint64_t reads0 = g_safeReads.load(), scans0 = nevr_quest::combat::level::ScanCalls();
    const std::uint64_t r = update(node, &d);
    QCHECK(g_calls.size() == 1 && g_calls[0].node == node && g_calls[0].data == &d && r == 0x71);
    float seen[4];
    Rgba(g_calls[0].bytes, seen);
    QCHECK(seen[0] == 0.941f && seen[1] == 0.466f && seen[2] == 0.054f && seen[3] == 0.25f && Mat(g_calls[0].bytes) == kMat);
    // The data is not read: pointers nobody could read go to the game's function as they came, and nothing is counted.
    g_calls.clear();
    g_reads = false;
    QCHECK(update(node, nullptr) == 0x71 && g_calls.size() == 1 && g_calls[0].data == nullptr);
    g_reads = true;
    // ARENA PAYS NOTHING, BY COUNT: none of these calls read the game's memory or asked which level is loaded (docs/adr/0046).
    QCHECK(g_safeReads.load() == reads0 && nevr_quest::combat::level::ScanCalls() == scans0);
    const sg::Stats stats = sg::CurrentStats();
    QCHECK(stats.decoded == 0 && stats.mirrored == 0 && stats.unreadable == 0);
    QCHECK(g_lines.empty());
  }
  End();
}

void TheModuleFollowsTheLevelBothWays() {
  Begin();
  QCHECK(sg::InstallAt(&Lookup, false, true).installed);
  void* node = reinterpret_cast<void*>(0x6000);
  const UpdateFn update = Slot();
  const auto mirrored = [&] {
    g_calls.clear();
    Data d = MakeData(kMat, kRing, 1, 0.5f, 0.5f, 0.5f, 0.5f, 0);
    update(node, &d);
    return g_calls.size() == 2;
  };
  Levels({"mpl_arena_a"});
  QCHECK(!mirrored());
  EnterCombat();
  QCHECK(mirrored());
  Levels({"mpl_arena_a"});
  QCHECK(!mirrored());
  Levels({"mpl_lobby_b_combat"});
  QCHECK(mirrored());
  Levels({"mpl_lobby_b"});
  QCHECK(!mirrored());
  // The tick stops: a combat answer older than kStaleAfterMs is not believed, and the next look brings it back.
  EnterCombat();
  QCHECK(mirrored());
  g_nowMs += scope::kStaleAfterMs + 1;
  QCHECK(!mirrored());
  const unsigned mirroredBefore = sg::CurrentStats().mirrored;
  QCHECK(!mirrored() && sg::CurrentStats().mirrored == mirroredBefore);  // nothing counted, no second call while the answer is stale
  g_nowMs += scope::kLookEveryMs;
  scope::Refresh(g_model.Address());
  QCHECK(mirrored());
  End();
}

void ALibraryThatIsNotLoadedOrAScopeThatCannotChangeRefusesBeforeAnyWrite() {
  // the library is not loaded (the scope is live: the tick was installed before it went)
  Begin();
  g_image.Drop();
  nevr_quest::combat::ModuleResult r = sg::InstallAt(&Lookup, true, true);
  QCHECK(!r.installed && std::strcmp(r.status, sentinel::GotStatusName(sentinel::GotStatus::kModuleNotLoaded)) == 0);
  QCHECK(Count("combat_shieldgamma_install", "\"result\":\"refused\"", kError) == 1);
  End();
  // the tick was never installed: the scope row refuses and so does the module, before it looks for the library
  Begin(/*scopeLive=*/false);
  QCHECK(!scope::InstallNormal().installed && !scope::Live());
  g_lines.clear();
  r = sg::InstallAt(&Lookup, true, true);
  QCHECK(!r.installed && std::strcmp(r.status, "scope_not_live") == 0 && !Hooked());
  QCHECK(Count("combat_shieldgamma_install", "scope_not_live", kError) == 1);
  End();
  // the tick is installed and its callback table is full: the row cannot register the refresh
  Begin(/*scopeLive=*/false);
  QCHECK(tick::InstallAt(&Lookup));
  for (std::size_t i = 0; i < tick::kMaxCallbacks; ++i) tick::Add([](std::uintptr_t) noexcept {});
  QCHECK(!scope::InstallNormal().installed && !scope::Live());
  g_lines.clear();
  r = sg::InstallAt(&Lookup, true, true);
  QCHECK(!r.installed && std::strcmp(r.status, "scope_not_live") == 0 && !Hooked());
  QCHECK(Count("combat_shieldgamma_install", "scope_not_live", kError) == 1);
  End();
}

// The cost of the hook on the host, measured and printed (the claim "an arena match pays a load, a test and a tail call" is reproducible from the commit): the
// game's function called directly, through the slot in an arena level, and through the slot in a combat level with a call that is not the module's (a material
// that is not the shield's: the data read and the game's function).
void TheArenaPathCostIsMeasured() {
  Begin();
  QCHECK(sg::InstallAt(&Lookup, true, true).installed);
  void* node = reinterpret_cast<void*>(0x6000);
  const UpdateFn update = Slot();
  Data d = MakeData(kMat + 1, kRing, 1, 0.5f, 0.5f, 0.5f, 0.5f, 0);  // another material: guarded, not acted on
  g_record = false;
  constexpr int kCalls = 2000000;
  const auto best = [&](auto&& call) {
    double best = 1e18;
    for (int rep = 0; rep < 7; ++rep) {
      const auto start = std::chrono::steady_clock::now();
      volatile std::uint64_t sink = 0;
      for (int i = 0; i < kCalls; ++i) sink = sink + call();
      const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / kCalls;
      if (ns < best) best = ns;
    }
    return best;
  };
  const UpdateFn direct = &FakeUpdate;
  const double ref = best([&] { return direct(node, &d); });
  Levels({"mpl_arena_a"});
  QCHECK(!scope::SeenCombatOrLobby());
  const std::uint64_t scans0 = nevr_quest::combat::level::ScanCalls();
  const double arena = best([&] { return update(node, &d); });
  QCHECK(nevr_quest::combat::level::ScanCalls() == scans0);  // the cost loop asked no level either
  EnterCombat();
  QCHECK(scope::InCombatOrLobby());
  constexpr int kCombatCalls = 20000;
  double combat = 1e18;
  for (int rep = 0; rep < 7; ++rep) {
    const auto start = std::chrono::steady_clock::now();
    volatile std::uint64_t sink = 0;
    for (int i = 0; i < kCombatCalls; ++i) sink = sink + update(node, &d);
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / kCombatCalls;
    if (ns < combat) combat = ns;
  }
  g_record = true;
  std::printf("shieldgamma_test: cost on the host: the game's function %.1f ns, through the hook in an arena level %.1f ns (+%.1f ns), a combat level call that is not "
              "the shield's (the data read and the game's function) %.1f ns; %d and %d calls, best of 7\n",
              ref, arena, arena - ref, combat, kCalls, kCombatCalls);
  End();
}

}  // namespace

int main() {
  nevr_quest::combat::SetReadCounterForTest(&g_safeReads);
  ThePinsAreTheReadelfAndDisassemblyPins();
  TheModuleActsInACombatLevelAsThePluginDid();
  BothSwitchesOffInstallNothing();
  AForeignSlotWordIsRefusedAndLeftAlone();
  OutsideACombatLevelTheCallPassesThroughUntouched();
  TheModuleFollowsTheLevelBothWays();
  ALibraryThatIsNotLoadedOrAScopeThatCannotChangeRefusesBeforeAnyWrite();
  TheArenaPathCostIsMeasured();
  nevr_quest::combat::SetReadCounterForTest(nullptr);
  if (quest_test::Failures() == 0) std::printf("shieldgamma_test: all checks passed\n");
  return quest_test::Failures() == 0 ? 0 : 1;
}
