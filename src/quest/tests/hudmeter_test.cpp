// Host test of the wrist HUD module (src/quest/combat/hudmeter.cpp, #2186, row R57): the real handler through the thunk, installed by the real
// GotHook into the JUMP_SLOT of a libr15 image built in memory (combat_libr15_image.h) whose slot holds base + the pinned function, and called
// the way the game calls it: through the slot. The fake CSetCanvasElementAttributesNode::Enter records what it was called with.
//
// The checks 2 to 11 are the q15t plugin's hudmeter_test, moved: the assertions are the same and the fixture is the manager's (the image, the
// slot and the log lines are the combat area's, and the module acts in a combat level, which the test puts the game in through the real combat
// level answer, combat_scope.h). The rest is new: the arena path (no combat level loaded: the game's function with the same arguments, no read of
// the data, no write, no line), the combat lobby and the lobbies arena players use, the switch, and each refusal.
//
// Built and run by `just test-quest-combat`.

#include "quest/combat/hudmeter.h"

#include <sys/mman.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "quest/combat/combat_scope.h"
#include "quest/combat/combat_tick.h"
#include "quest/combat/combat_tick_targets.h"
#include "quest/combat/hudmeter_targets.h"
#include "quest/tests/combat_game_model.h"
#include "quest/tests/combat_libr15_image.h"
#include "quest/tests/test_check.h"

namespace {

namespace hud = nevr_quest::combat::hudmeter;
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
// The lines of one event that also hold `needle` (and are at `level`, when one is given).
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

constexpr std::uint64_t kComp = hud::kLobbyComponent, kItem = hud::kItem;

// the fake CSetCanvasElementAttributesNode::Enter
int g_realCalls = 0;
const void *g_realNode = nullptr, *g_realData = nullptr;
float g_realSize[2];
bool g_fakeReads = true;  // false: the data argument is not dereferenced (unreadable-data cases)
std::uint64_t FakeEnter(void* node, const void* data) {
  ++g_realCalls;
  g_realNode = node;
  g_realData = data;
  if (g_fakeReads) std::memcpy(g_realSize, static_cast<const std::uint8_t*>(data) + 0x38, 8);
  return 0x1234;
}
using EnterFn = std::uint64_t (*)(void*, const void*);

struct alignas(16) Data {
  std::uint8_t b[0x80];
};
Data MakeData(std::uint64_t comp, std::uint64_t item, std::int32_t sizeOn, float x, float y) {
  Data d;
  std::memset(d.b, 0xab, sizeof d.b);
  std::memcpy(d.b + 0x18, &comp, 8);
  std::memcpy(d.b + 0x20, &item, 8);
  std::memcpy(d.b + 0x34, &sizeOn, 4);
  std::memcpy(d.b + 0x38, &x, 4);
  std::memcpy(d.b + 0x3c, &y, 4);
  return d;
}
float SizeX(const Data& d) {
  float v;
  std::memcpy(&v, d.b + 0x38, 4);
  return v;
}
float SizeY(const Data& d) {
  float v;
  std::memcpy(&v, d.b + 0x3c, 4);
  return v;
}

Image g_image;
combat_test::GameModel g_model;
std::int64_t g_nowMs = 1000;
std::int64_t Clock() noexcept { return g_nowMs; }
bool Lookup(const char* module, sentinel::ElfImage* out) {
  if (!g_image.Present() || std::strcmp(module, pinned::kLibR15) != 0) return false;
  *out = g_image.Elf();
  return true;
}
EnterFn Slot() { return reinterpret_cast<EnterFn>(*g_image.Slot(hud::kSlotVaddr)); }
bool Hooked() { return *g_image.Slot(hud::kSlotVaddr) == hud::Thunk::EntryAddress(); }

// A libr15 whose slot holds the game's function (the stand-in), and a game in which no level is loaded.
void FakeTickFrame(void*) {}
void Build() {
  namespace tt = nevr_quest::combat::tick::targets;
  g_image.Build({{hud::kSymbol, hud::kSlotVaddr, false, false, hud::kFnVaddr}, {tt::kSymbol, tt::kSlotVaddr, false, false, tt::kStockVaddr}});
  *g_image.Slot(tt::kSlotVaddr) = reinterpret_cast<void*>(&FakeTickFrame);  // the game's function: any address in an executable mapping
  g_image.Real(hud::kFnVaddr, reinterpret_cast<void*>(&FakeEnter));
  *g_image.Slot(hud::kSlotVaddr) = reinterpret_cast<void*>(g_image.Base() + hud::kFnVaddr);
}
// `scopeLive`: the combat tick is installed on the image and the `scope` row has registered the refresh (what the shipped table does before this module).
void Begin(bool scopeLive = true) {
  g_lines.clear();
  g_realCalls = 0;
  g_realNode = g_realData = nullptr;
  g_fakeReads = true;
  g_nowMs = 1000;
  g_model.Clear();
  tick::ResetForTest();
  scope::ResetForTest();
  scope::SetClockForTest(&Clock);
  hud::ResetForTest();
  sentinel::SetLogSink(&Capture);
  Build();
  if (scopeLive) {
    QCHECK(tick::InstallAt(&Lookup));
    QCHECK(scope::InstallNormal().installed && scope::Live());
    g_lines.clear();
  }
}
void End() {
  hud::ResetForTest();
  scope::ResetForTest();
  tick::ResetForTest();
  g_image.Drop();
  sentinel::SetLogSink(nullptr);
}
// The game goes to these levels; the combat tick's callback runs once (the clock moves past the look spacing).
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
  QCHECK(hud::kSlotVaddr == 0x36c6ce8 && hud::kFnVaddr == 0x2726eac);
  QCHECK(std::strcmp(hud::kSymbol,
                     "_ZN10NRadEngine31CSetCanvasElementAttributesNode5EnterERKNS_35SSetCanvasElementAttributesNodeDataE") == 0);
  QCHECK(hud::kLobbyComponent == 0xe55af932c88ec864ULL && hud::kItem == 0x9fd2e8cc0e46df3cULL);
  QCHECK(hud::kFullX == 86.0f && hud::kFullY == 88.0f);
  QCHECK(hud::kDataComponent == 0x18 && hud::kDataItem == 0x20 && hud::kDataSizeOn == 0x34 && hud::kDataSize == 0x38 && hud::kDataPrefix == 0x40);
}

void TheModuleActsInACombatLevelAsThePluginDid() {
  Begin();
  void* node = reinterpret_cast<void*>(0x5000);
  // 1. install: the slot holds the thunk, one Info line, the combat level answer is refreshed from the tick
  const nevr_quest::combat::ModuleResult ir = hud::InstallAt(&Lookup, true);
  QCHECK(ir.installed && std::strcmp(ir.status, "installed") == 0);
  QCHECK(Hooked());
  QCHECK(Count("combat_hudmeter_install", "\"result\":\"installed\"", kInfo) == 1);
  QCHECK(tick::Count() == 1);  // the scope row registered the refresh, the module did not add one
  EnterCombat();
  QCHECK(scope::InCombatOrLobby());
  const EnterFn enter = Slot();

  // 2. a 0 x 0 write to weapon_meter on the lobby helmet's canvas: the real Enter sees 86 x 88, the node's value is put back
  Data d = MakeData(kComp, kItem, 1, 0.0f, 0.0f);
  std::uint64_t r = enter(node, &d);
  QCHECK(g_realCalls == 1 && r == 0x1234);
  QCHECK(g_realSize[0] == 86.0f && g_realSize[1] == 88.0f);
  QCHECK(g_realNode == node && g_realData == &d);
  QCHECK(SizeX(d) == 0.0f && SizeY(d) == 0.0f);
  QCHECK(d.b[0x34] == 1 && d.b[0x40] == 0xab && d.b[0x20] == (kItem & 0xff));
  QCHECK(Count("combat_hudmeter_full") == 1 && Count("combat_hudmeter_full", "\"n\":1,") == 1);
  QCHECK(Count("combat_hudmeter_component") == 0);
  QCHECK(hud::CurrentStats().fullWrites == 1);

  // 3. the next size to the same item (the fill) is passed on untouched; the first one logs a pass line once
  d = MakeData(kComp, kItem, 1, 86.0f, 0.0f);
  r = enter(node, &d);
  QCHECK(g_realSize[0] == 86.0f && g_realSize[1] == 0.0f && g_realData == &d);
  QCHECK(Count("combat_hudmeter_pass") == 1);
  for (float y : {10.0f, 10.0f, 20.0f, 30.0f}) {
    d = MakeData(kComp, kItem, 1, 86.0f, y);
    enter(node, &d);
    QCHECK(g_realSize[1] == y);
  }
  QCHECK(Count("combat_hudmeter_pass") == 1);
  QCHECK(Count("combat_hudmeter_fill") == 3);  // y 0 -> 10 -> 20 -> 30 (10 twice = one line)

  // the fill lines are capped at 8 for good (3 are out; heights that keep changing add 5 more and no more)
  g_lines.clear();
  for (int i = 0; i < 40; ++i) {
    d = MakeData(kComp, kItem, 1, 86.0f, 100.0f + static_cast<float>(i));
    enter(node, &d);
  }
  QCHECK(Count("combat_hudmeter_fill") == 5);

  // 4. a later helmet start (0 x 0 again) is guarded again and a new pass line follows
  d = MakeData(kComp, kItem, 1, 0.0f, 0.0f);
  enter(node, &d);
  QCHECK(g_realSize[0] == 86.0f && g_realSize[1] == 88.0f);
  d = MakeData(kComp, kItem, 1, 86.0f, 44.0f);
  enter(node, &d);
  QCHECK(Count("combat_hudmeter_pass") == 1);

  // 5. full-write line thinning: 1st, 2nd, 4th ... (the 3rd above already counted: n so far 2)
  g_lines.clear();
  for (int i = 0; i < 6; ++i) {
    d = MakeData(kComp, kItem, 1, 0.0f, 0.0f);
    enter(node, &d);
  }
  QCHECK(Count("combat_hudmeter_full") == 2);  // n=4 and n=8

  // 6. any canvas component is guarded; the first other one is named once
  g_lines.clear();
  d = MakeData(0x1111, kItem, 1, 0.0f, 0.0f);
  enter(node, &d);
  QCHECK(g_realSize[0] == 86.0f && g_realSize[1] == 88.0f);
  QCHECK(Count("combat_hudmeter_component") == 1 && Count("combat_hudmeter_component", "\"comp\":\"0x0000000000001111\"") == 1);
  d = MakeData(0x2222, kItem, 1, 0.0f, 0.0f);
  enter(node, &d);
  QCHECK(Count("combat_hudmeter_component") == 1);

  // 7. everything else passes untouched and is not logged: another item, size flag off, only one coordinate 0
  g_lines.clear();
  const unsigned guarded = hud::CurrentStats().callsGuarded;
  d = MakeData(kComp, kItem + 1, 1, 0.0f, 0.0f);
  enter(node, &d);
  QCHECK(g_realSize[0] == 0.0f && g_realSize[1] == 0.0f && g_realData == &d);
  d = MakeData(kComp, kItem, 0, 0.0f, 0.0f);
  enter(node, &d);
  QCHECK(g_realSize[0] == 0.0f && g_realSize[1] == 0.0f);
  d = MakeData(kComp, kItem, 1, 0.0f, 5.0f);
  enter(node, &d);
  QCHECK(g_realSize[0] == 0.0f && g_realSize[1] == 5.0f);
  d = MakeData(kComp, kItem, 1, 7.0f, 0.0f);
  enter(node, &d);
  QCHECK(g_realSize[0] == 7.0f && g_realSize[1] == 0.0f);
  QCHECK(g_lines.size() <= 2 && hud::CurrentStats().callsGuarded == guarded + 2);

  // 8. no per-call logging on the hot path
  g_lines.clear();
  d = MakeData(kComp, kItem + 7, 1, 1.0f, 2.0f);
  for (int i = 0; i < 2000; ++i) enter(node, &d);
  QCHECK(g_lines.empty());

  // 9. a data argument that cannot be read: the real function runs with the very same arguments, nothing is written or faults
  g_fakeReads = false;
  g_realCalls = 0;
  unsigned unread = hud::CurrentStats().unreadable;
  r = enter(node, nullptr);
  QCHECK(g_realCalls == 1 && r == 0x1234 && g_realData == nullptr && g_realNode == node);
  QCHECK(hud::CurrentStats().unreadable == unread + 1);
  r = enter(node, reinterpret_cast<const void*>(0x1000));
  QCHECK(g_realCalls == 2 && g_realData == reinterpret_cast<const void*>(0x1000));

  // 10. the data right at the end of a mapping: exactly 0x40 readable bytes is enough; one word less is passed on untouched
  std::uint8_t* two = static_cast<std::uint8_t*>(mmap(nullptr, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  mprotect(two + 4096, 4096, PROT_NONE);
  const Data src = MakeData(kComp, kItem, 1, 0.0f, 0.0f);
  std::uint8_t* edge = two + 4096 - 0x40;
  std::memcpy(edge, src.b, 0x40);
  g_realCalls = 0;
  unread = hud::CurrentStats().unreadable;
  g_fakeReads = true;
  enter(node, edge);
  QCHECK(g_realCalls == 1 && g_realSize[0] == 86.0f && g_realSize[1] == 88.0f);
  QCHECK(hud::CurrentStats().unreadable == unread);
  std::uint8_t* shortEdge = two + 4096 - 0x3c;
  std::memcpy(shortEdge, src.b, 0x3c);
  g_fakeReads = false;
  enter(node, shortEdge);
  QCHECK(hud::CurrentStats().unreadable == unread + 1 && std::memcmp(shortEdge, src.b, 0x3c) == 0);
  g_fakeReads = true;
  munmap(two, 8192);
  End();
}

// 11. a slot that does not hold the pinned function is refused, left alone, and logged as an Error
void AForeignSlotWordIsRefusedAndLeftAlone() {
  Begin();
  *g_image.Slot(hud::kSlotVaddr) = reinterpret_cast<void*>(0x4242420);
  const nevr_quest::combat::ModuleResult bad = hud::InstallAt(&Lookup, true);
  QCHECK(!bad.installed && std::strcmp(bad.status, sentinel::GotStatusName(sentinel::GotStatus::kOriginalMismatch)) == 0);
  QCHECK(*g_image.Slot(hud::kSlotVaddr) == reinterpret_cast<void*>(0x4242420));
  QCHECK(Count("combat_hudmeter_install", "\"result\":\"refused\"", kError) == 1);
  QCHECK(Count("combat_hudmeter_install", "the wrist HUD's heat fill is the game's own") == 1);
  End();
}

// The arena path: a hook installed on a slot arena's scripts also call. No combat level loaded -> the game's function with the very same
// arguments, no read of the data (a null or unmapped data pointer is not counted as unreadable), no write (the 0 x 0 stays 0 x 0), no counter, no line.
void OutsideACombatLevelTheCallPassesThroughUntouched() {
  Begin();
  QCHECK(hud::InstallAt(&Lookup, true).installed);
  g_lines.clear();
  void* node = reinterpret_cast<void*>(0x5000);
  const EnterFn enter = Slot();
  struct Case {
    const char* what;
    const char* first;   // a level the game has loaded, or nullptr for none
    const char* second;  // another, or nullptr
  };
  const Case cases[] = {{"before the first look", nullptr, nullptr},
                        {"arena a", "mpl_arena_a", nullptr},
                        {"arena b", "mpl_arena_b", nullptr},
                        {"arena lowspec", "mpl_arena_a_lowspec", nullptr},
                        {"tutorial", "mpl_tutorial_a", nullptr},
                        {"the arena lobby", "mpl_lobby_b", nullptr},
                        {"the social lobby", "mpl_lobby_b_social", nullptr},
                        {"combat to arena", "mpl_combat_gauss", "mpl_arena_a"},
                        {"nothing loaded", nullptr, nullptr}};
  for (const Case& c : cases) {
    if (std::strcmp(c.what, "before the first look") != 0) LevelsOf(c.first, c.second);
    QCHECK(!scope::InCombatOrLobby());
    Data d = MakeData(kComp, kItem, 1, 0.0f, 0.0f);  // the write the module fixes in combat
    g_realCalls = 0;
    const std::uint64_t r = enter(node, &d);
    QCHECK(g_realCalls == 1 && r == 0x1234 && g_realNode == node && g_realData == &d);
    QCHECK(g_realSize[0] == 0.0f && g_realSize[1] == 0.0f);  // not made 86 x 88
    QCHECK(SizeX(d) == 0.0f && SizeY(d) == 0.0f);
    // The data is not read: pointers nobody could read go to the game's function as they came, and nothing is counted.
    g_fakeReads = false;
    QCHECK(enter(node, nullptr) == 0x1234 && g_realData == nullptr);
    QCHECK(enter(node, reinterpret_cast<const void*>(0x1000)) == 0x1234 && g_realData == reinterpret_cast<const void*>(0x1000));
    g_fakeReads = true;
    const hud::Stats stats = hud::CurrentStats();
    QCHECK(stats.callsGuarded == 0 && stats.fullWrites == 0 && stats.unreadable == 0);
    QCHECK(g_lines.empty());
  }
  End();
}

// The scope follows the game: arena -> combat -> arena -> the combat lobby -> an arena lobby.
void TheModuleFollowsTheLevelBothWays() {
  Begin();
  QCHECK(hud::InstallAt(&Lookup, true).installed);
  void* node = reinterpret_cast<void*>(0x5000);
  const EnterFn enter = Slot();
  const auto fixed = [&] {
    Data d = MakeData(kComp, kItem, 1, 0.0f, 0.0f);
    enter(node, &d);
    return g_realSize[0] == 86.0f && g_realSize[1] == 88.0f && SizeX(d) == 0.0f;
  };
  Levels({"mpl_arena_a"});
  QCHECK(!fixed());
  EnterCombat();
  QCHECK(fixed());
  Levels({"mpl_arena_a"});
  QCHECK(!fixed());
  Levels({"mpl_lobby_b_combat"});  // the combat lobby: the helmet's HUD is combat's there
  QCHECK(fixed());
  Levels({"mpl_lobby_b"});         // the lobby arena players use: not
  QCHECK(!fixed());
  Levels({"mpl_combat_dyson", "mpl_arena_a"});  // a transition holding both: arena wins
  QCHECK(!fixed());
  // The tick stops: a combat answer older than kStaleAfterMs is not believed, and the next look brings it back.
  EnterCombat();
  QCHECK(fixed());
  g_nowMs += scope::kStaleAfterMs + 1;
  QCHECK(!fixed());
  const unsigned writes = hud::CurrentStats().fullWrites;
  QCHECK(!fixed() && hud::CurrentStats().fullWrites == writes);  // nothing counted, nothing written while the answer is stale
  g_nowMs += scope::kLookEveryMs;
  scope::Refresh(g_model.Address());
  QCHECK(fixed());
  End();
}

void TheSwitchOffInstallsNothing() {
  Begin(/*scopeLive=*/false);
  const nevr_quest::combat::ModuleResult r = hud::InstallAt(&Lookup, false);
  QCHECK(!r.installed && std::strcmp(r.status, "off") == 0);
  QCHECK(!Hooked() && *g_image.Slot(hud::kSlotVaddr) == reinterpret_cast<void*>(g_image.Base() + hud::kFnVaddr));
  QCHECK(tick::Count() == 0 && !scope::Live());  // a module that is off asks nothing of the scope
  QCHECK(Count("combat_hudmeter_install", "\"result\":\"off\",\"reason\":\"off_config\"", kInfo) == 1);
  QCHECK(g_lines.size() == 1U);
  End();
}

void ALibraryThatIsNotLoadedOrAScopeThatCannotChangeRefusesBeforeAnyWrite() {
  // the library is not loaded (the scope is live: the tick was installed before it went)
  Begin();
  g_image.Drop();
  nevr_quest::combat::ModuleResult r = hud::InstallAt(&Lookup, true);
  QCHECK(!r.installed && std::strcmp(r.status, sentinel::GotStatusName(sentinel::GotStatus::kModuleNotLoaded)) == 0);
  QCHECK(Count("combat_hudmeter_install", "\"result\":\"refused\"", kError) == 1);
  End();
  // the tick was never installed: the scope row refuses and so does the module, before it looks for the library
  Begin(/*scopeLive=*/false);
  QCHECK(!scope::InstallNormal().installed && !scope::Live());
  g_lines.clear();
  r = hud::InstallAt(&Lookup, true);
  QCHECK(!r.installed && std::strcmp(r.status, "scope_not_live") == 0 && !Hooked());
  QCHECK(Count("combat_hudmeter_install", "scope_not_live", kError) == 1);
  End();
  // the tick is installed and its callback table is full: the row cannot register the refresh
  Begin(/*scopeLive=*/false);
  QCHECK(tick::InstallAt(&Lookup));
  for (std::size_t i = 0; i < tick::kMaxCallbacks; ++i) tick::Add([](std::uintptr_t) noexcept {});
  QCHECK(!scope::InstallNormal().installed && !scope::Live());
  g_lines.clear();
  r = hud::InstallAt(&Lookup, true);
  QCHECK(!r.installed && std::strcmp(r.status, "scope_not_live") == 0 && !Hooked());
  QCHECK(Count("combat_hudmeter_install", "scope_not_live", kError) == 1);
  End();
}

}  // namespace

int main() {
  ThePinsAreTheReadelfAndDisassemblyPins();
  TheModuleActsInACombatLevelAsThePluginDid();
  AForeignSlotWordIsRefusedAndLeftAlone();
  OutsideACombatLevelTheCallPassesThroughUntouched();
  TheModuleFollowsTheLevelBothWays();
  TheSwitchOffInstallsNothing();
  ALibraryThatIsNotLoadedOrAScopeThatCannotChangeRefusesBeforeAnyWrite();
  if (quest_test::Failures() == 0) std::printf("hudmeter_test: all checks passed\n");
  return quest_test::Failures() == 0 ? 0 : 1;
}
