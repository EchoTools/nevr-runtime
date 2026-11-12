// Host test of the wall-fire check (src/quest/combat/wallfire.cpp, #2182) on top of the real static BVH (src/quest/combat/bvh.cpp): a libr15 image
// built in memory holds the six bvh slots, the QueryCollision slot and the StaticRaycastOccluded slot; the thunk is called through its slot from two
// distinct call sites; the engine's stub is a fake that follows CncaGameSpace -> CncaGame -> vtable +0x278 into a fake
// CncaGame::StaticRaycastOccluded, which calls CBVHTree::Occluded through bvh's own hook on a synthetic room mesh.
//
// From the combat plugin's wallfire_test (#1591): the assertions on the shots are the same (a stock hit passes through, a clear segment is not
// blocked, a segment through each wall is, another call site gets the stock answer, a null gamespace, a non-finite segment and every wrong game
// structure get the stock answer, the log volume, the refusals and what they leave alone). What changed is the fixture (the manager's install on an
// image, not a slot swap; the plugin's kill file is the `combat.wallfire` switch) and three deliberate behaviours: the check acts only on a combat map
// or in the combat lobby, the request and the game chain are read with plain loads (the assertions that gave an unreadable pointer went with the
// system call; a null pointer in the chain still gets the stock answer), and the counters of the dropped cases. New here: the scope, the switch and
// its lines, the legacy file. The scope is bvh's, which is the real combat level answer (combat_scope.h, #2382): the tick is installed on an image of
// its own and the scope row has registered the refresh, and each look runs that refresh before bvh's callback, as the tick does.
//
// Built and run by `just test-quest-combat`.
#include "quest/combat/wallfire.h"

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nevr/symbol_hash.h"
#include "quest/combat/bvh.h"
#include "quest/combat/combat_level.h"
#include "quest/combat/combat_scope.h"
#include "quest/combat/combat_tick.h"
#include "quest/combat/legacy_switch.h"
#include "quest/combat/safe_read.h"
#include "quest/tests/bvh_build.h"
#include "quest/tests/combat_libr15_image.h"
#include "quest/tests/combat_scope_world.h"
#include "quest/tests/test_check.h"

namespace {

namespace bvh = nevr_quest::combat::bvh;
namespace wf = nevr_quest::combat::wallfire;
namespace pinned = sentinel::pinned;
namespace scope = nevr_quest::combat::scope;
namespace world = combat_test::scope_world;
using combat_test::Image;
using EchoVR::CSymbol64Hash;

struct Line {
  sentinel::LogLevel level;
  std::string text;
};
std::vector<Line> g_lines;
void Capture(sentinel::LogLevel level, const char* line) { g_lines.push_back({level, line}); }
bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }
int Count(const std::string& needle, int level = -1) {
  int c = 0;
  for (const Line& l : g_lines) {
    if (Has(l.text, needle) && (level < 0 || static_cast<int>(l.level) == level)) ++c;
  }
  return c;
}
constexpr int kInfo = static_cast<int>(sentinel::LogLevel::kInfo);
constexpr int kWarn = static_cast<int>(sentinel::LogLevel::kWarn);
constexpr int kError = static_cast<int>(sentinel::LogLevel::kError);
std::string Event(const char* name) { return std::string("\"event\":\"") + name + "\""; }

// ---- fakes ----
int g_stockQuery, g_stubCalls, g_gameCalls;
bool g_queryResult = false;
std::uint64_t g_lastId = 0;
std::uintptr_t g_lastGs = 0, g_lastReq = 0;
void* g_cbvh = nullptr;
float g_seenSeg[6];

Image g_image;
bvh::Pins g_bvhPins = bvh::kPins;
wf::Pins g_pins = wf::kPins;

extern "C" __attribute__((noinline)) void StockInit(void*, const void*) {}
extern "C" __attribute__((noinline)) void StockShut(void*) {}
extern "C" __attribute__((noinline)) bool StockInt(const void*, const float*, std::uint8_t*) { return false; }
extern "C" __attribute__((noinline)) bool StockOcc(const void*, const float*, const float*) { return false; }
extern "C" __attribute__((noinline)) bool StockTouch(const void*, const void*, void*, float*) { return true; }
extern "C" __attribute__((noinline)) bool StockGate(const void*, const void*, void*, void**) { return false; }
extern "C" __attribute__((noinline)) bool StockQuery(std::uintptr_t gs, std::uintptr_t req, std::uint64_t id) {
  ++g_stockQuery;
  g_lastGs = gs;
  g_lastReq = req;
  g_lastId = id;
  return g_queryResult;
}
std::uintptr_t SlotWord(std::uint64_t slot) { return reinterpret_cast<std::uintptr_t>(*g_image.Slot(slot)); }
using OccludedFn = bool (*)(const void*, const float*, const float*);
using QueryFn = bool (*)(std::uintptr_t, std::uintptr_t, std::uint64_t);
// CncaGame::StaticRaycastOccluded: the level's CBVHTree answers through bvh's hook
extern "C" __attribute__((noinline)) bool FakeGameOccluded(const void*, const float* a, const float* b) {
  ++g_gameCalls;
  return reinterpret_cast<OccludedFn>(*g_image.Slot(g_bvhPins.occluded.slot))(g_cbvh, a, b);
}
// CncaGameSpace::StaticRaycastOccluded: the stub (ldr x0,[x0,#8]; ldr x8,[x0]; ldr x3,[x8,#0x278]; br x3)
extern "C" __attribute__((noinline)) bool FakeStub(const void* gs, const float* a, const float* b) {
  ++g_stubCalls;
  std::memcpy(g_seenSeg, a, sizeof(g_seenSeg));
  const std::uintptr_t game = reinterpret_cast<const std::uintptr_t*>(gs)[1];
  const std::uintptr_t vptr = *reinterpret_cast<const std::uintptr_t*>(game);
  const std::uintptr_t fn = *reinterpret_cast<const std::uintptr_t*>(vptr + 0x278);
  return reinterpret_cast<OccludedFn>(fn)(reinterpret_cast<const void*>(game), a, b);
}

__attribute__((noinline)) bool CallQuerySite(std::uintptr_t gs, std::uintptr_t req, std::uint64_t id) {
  volatile bool r = reinterpret_cast<QueryFn>(*g_image.Slot(g_pins.querySlot))(gs, req, id);
  return r;
}
__attribute__((noinline)) bool CallQueryOther(std::uintptr_t gs, std::uintptr_t req, std::uint64_t id) {
  volatile bool r = reinterpret_cast<QueryFn>(*g_image.Slot(g_pins.querySlot))(gs, req, id);
  return r;
}

// the game: a gamespace whose +8 is a CncaGame whose vtable +0x278 is FakeGameOccluded
struct FakeGame {
  std::uintptr_t vptr;
  std::uint8_t pad[64];
};
struct FakeGs {
  std::uintptr_t w0, game, w2, w3;
};
std::uintptr_t g_vtable[0x300 / 8];
FakeGame g_game;
FakeGs g_gs;

// ---- the game's levels (see bvh_test.cpp) ----
constexpr std::size_t kTable0 = 0x1f48, kCount0 = 0x1f78, kGamespaceOwner = 0x8, kGamespaceLevel = 0x58;
struct Gamespace {
  std::uintptr_t words[0x60 / 8 + 1] = {};
};
struct Levels {
  std::uintptr_t words[0x2000 / 8] = {};
  std::uintptr_t table0[8][2] = {};
  Gamespace spaces[8];
};
Levels g_levels;
std::uintptr_t LevelsAddress() { return reinterpret_cast<std::uintptr_t>(g_levels.words); }
void LoadLevels(std::initializer_list<const char*> names) {
  g_levels = Levels{};
  int row = 0;
  for (const char* name : names) {
    Gamespace& gs = g_levels.spaces[row];
    gs.words[kGamespaceOwner / 8] = LevelsAddress();
    gs.words[kGamespaceLevel / 8] = static_cast<std::uintptr_t>(CSymbol64Hash(name));
    g_levels.table0[row][0] = 0x5555;
    g_levels.table0[row][1] = reinterpret_cast<std::uintptr_t>(&gs);
    ++row;
  }
  g_levels.words[kTable0 / 8] = reinterpret_cast<std::uintptr_t>(g_levels.table0);
  g_levels.words[kCount0 / 8] = static_cast<std::uintptr_t>(row);
}
std::atomic<std::uint64_t> g_reads{0}, g_scans{0};
void CountScan() { g_scans.fetch_add(1); }
double g_now = 100.0;
int g_clockReads = 0;  // every read of the bvh module's clock or the combat level answer's: the arena path reads none
double Clock() noexcept {
  ++g_clockReads;
  return g_now;
}
std::int64_t ScopeClock() noexcept {
  ++g_clockReads;
  return static_cast<std::int64_t>(std::llround(g_now * 1000.0));
}
// One frame of the game-thread tick a second later: the scope row's refresh (registered first), then bvh's callback.
void Look() {
  g_now += 1.0;
  scope::Refresh(LevelsAddress());
  bvh::Tick(LevelsAddress());
}
void EnterCombat() {
  LoadLevels({"mpl_combat_dyson"});
  Look();
}

bt::Resource g_room;
bt::Resource RoomResource() {
  const float lo[3] = {-5, -5, -5}, hi[3] = {5, 5, 5};
  std::vector<bt::TriIn> in;
  bt::AddBox(in, lo, hi, 100, true);
  return bt::MakeResource(bt::Builder(in, 4).Build(), true);
}

bool Lookup(const char* module, sentinel::ElfImage* out) {
  if (!g_image.Present() || std::strcmp(module, pinned::kLibR15) != 0) return false;
  *out = g_image.Elf();
  return true;
}

// The image: the six bvh slots and the QueryCollision slot hold the stand-ins; the site holds `siteWord` (the test learns the real one, below).
// `site`: the call site's absolute address, or 0 for "learn it".
void Build(std::uint32_t siteWord, std::uintptr_t site) {
  g_bvhPins = bvh::kPins;
  g_pins = wf::kPins;
  g_image.Build({{bvh::kInitSymbol, g_bvhPins.init.slot, false, false, 0},
                 {bvh::kShutSymbol, g_bvhPins.shutdown.slot, false, false, 0},
                 {bvh::kIntersectSymbol, g_bvhPins.intersect.slot, false, false, 0},
                 {bvh::kOccludedSymbol, g_bvhPins.occluded.slot, false, false, 0},
                 {bvh::kTouchSymbol, g_bvhPins.touch.slot, false, false, 0},
                 {bvh::kGateSymbol, g_bvhPins.gate.slot, false, false, 0},
                 {wf::kQuerySymbol, g_pins.querySlot, false, false, 0}});
  const std::uintptr_t base = g_image.Base();
  const auto off = [&](const void* f) { return reinterpret_cast<std::uintptr_t>(f) - base; };
  bvh::Slot* slots[6] = {&g_bvhPins.init, &g_bvhPins.shutdown, &g_bvhPins.intersect, &g_bvhPins.occluded, &g_bvhPins.touch, &g_bvhPins.gate};
  const void* fns[6] = {reinterpret_cast<const void*>(&StockInit), reinterpret_cast<const void*>(&StockShut),
                        reinterpret_cast<const void*>(&StockInt),  reinterpret_cast<const void*>(&StockOcc),
                        reinterpret_cast<const void*>(&StockTouch), reinterpret_cast<const void*>(&StockGate)};
  for (int i = 0; i < 6; ++i) {
    slots[i]->fn = off(fns[i]);
    *g_image.Slot(slots[i]->slot) = const_cast<void*>(fns[i]);
  }
  g_bvhPins.allowedCount = 0;
  g_pins.queryFn = off(reinterpret_cast<const void*>(&StockQuery));
  *g_image.Slot(g_pins.querySlot) = reinterpret_cast<void*>(&StockQuery);
  g_pins.occludedFn = off(reinterpret_cast<const void*>(&FakeStub));
  *g_image.Slot(g_pins.occludedSlot) = reinterpret_cast<void*>(&FakeStub);
  g_pins.gameOccluded = off(reinterpret_cast<const void*>(&FakeGameOccluded));
  g_pins.site = site != 0 ? site - base : 0x60;
  g_pins.siteWord = siteWord;
  if (site == 0) g_image.Put<std::uint32_t>(g_pins.site, siteWord);
  bvh::SetImageLookupForTest(&Lookup);
  bvh::SetPinsForTest(&g_bvhPins);
  wf::SetImageLookupForTest(&Lookup);
  wf::SetPinsForTest(&g_pins);
}

// The game-thread tick on an image of its own, with the scope row's refresh registered: the combat level answer bvh reads can change.
Image g_tickImage;
bool TickImageLookup(const char* module, sentinel::ElfImage* out) {
  if (!g_tickImage.Present() || std::strcmp(module, pinned::kLibR15) != 0) return false;
  *out = g_tickImage.Elf();
  return true;
}

void Begin() {
  g_lines.clear();
  bvh::ResetForTest();
  wf::ResetForTest();
  nevr_quest::combat::tick::ResetForTest();
  scope::ResetForTest();
  scope::SetClockForTest(&ScopeClock);
  g_tickImage.Build({world::TickRow()});
  world::PutTickSlot(g_tickImage);
  QCHECK(world::StartScope(&TickImageLookup));
  nevr_quest::combat::legacy::SetDirectoryForTest(nullptr);
  g_stockQuery = g_stubCalls = g_gameCalls = 0;
  g_queryResult = false;
  g_cbvh = nullptr;
  g_now = 100.0;
  LoadLevels({});
  bvh::SetClockForTest(&Clock);
  sentinel::SetLogSink(&Capture);
}
void End() {
  bvh::ResetForTest();
  wf::ResetForTest();
  nevr_quest::combat::tick::ResetForTest();
  scope::ResetForTest();
  nevr_quest::combat::legacy::SetDirectoryForTest(nullptr);
  g_image.Drop();
  g_tickImage.Drop();
  sentinel::SetLogSink(nullptr);
}

// The bvh module on the image (gate off: the tree answers every cast, as Occluded is not gated anyway), a combat map, and its tree.
std::uint64_t g_cbvhWord = 0;
void InstallBvhAndTree() {
  bvh::Switches sw;
  sw.bvh = true;
  sw.gate = false;
  QCHECK(bvh::Install(sw).installed);
  EnterCombat();
  g_cbvhWord = 0;
  g_room = RoomResource();
  reinterpret_cast<void (*)(void*, const void*)>(*g_image.Slot(g_bvhPins.init.slot))(&g_cbvhWord, g_room.bytes.data());
  g_cbvh = &g_cbvhWord;
}

void SetGame() {
  std::memset(g_vtable, 0, sizeof(g_vtable));
  g_vtable[0x278 / 8] = g_image.Base() + g_pins.gameOccluded;
  g_game.vptr = reinterpret_cast<std::uintptr_t>(g_vtable);
  g_gs = {0, reinterpret_cast<std::uintptr_t>(&g_game), 0, 0};
}

// SPhQueryCollisionRequest as FireGunLocal builds it: the first 24 bytes are the segment
float g_req[16];
std::uintptr_t Req(float a0, float a1, float a2, float b0, float b1, float b2) {
  std::memset(g_req, 0, sizeof(g_req));
  const float s[6] = {a0, a1, a2, b0, b1, b2};
  std::memcpy(g_req, s, sizeof(s));
  return reinterpret_cast<std::uintptr_t>(g_req);
}

wf::Counters C() { return wf::GetCounters(); }
bool InstalledWallfire() {
  const nevr_quest::combat::ModuleResult r = wf::Install(true);
  if (!r.installed) {
    for (const Line& l : g_lines) std::fprintf(stderr, "  line: %s\n", l.text.c_str());
  }
  return r.installed;
}

// FireGunLocal's site, learnt once: the return address of CallQuerySite and the instruction word there.
std::uintptr_t g_siteAbs = 0;
std::uint32_t g_siteWord = 0;
void LearnTheSite() {
  Begin();
  Build(0xdeadbeef, 0);
  InstallBvhAndTree();
  SetGame();
  QCHECK(InstalledWallfire());
  CallQuerySite(reinterpret_cast<std::uintptr_t>(&g_gs), Req(0, 0, 0, 0, 0, 1), 0xffffffff);
  g_siteAbs = wf::LastReturnForTest() + g_image.Base();
  QCHECK(g_siteAbs != g_image.Base());
  QCHECK(nevr_quest::combat::SafeRead(g_siteAbs, &g_siteWord, sizeof(g_siteWord)));
  End();
}

// ---- the tests ----

void ThePinsAreTheReadelfAndDisassemblyPins() {
  const wf::Pins& p = wf::kPins;
  QCHECK(p.querySlot == 0x36e0c38 && p.queryFn == 0x1806784);
  QCHECK(p.site == 0x215566c && p.siteWord == 0x3941b3e8);
  QCHECK(p.occludedSlot == 0x36ce380 && p.occludedFn == 0x18066f4);
  QCHECK(p.gameOccluded == 0x17f2614);
  QCHECK(wf::kGsGame == 0x8 && wf::kGameVtblSlot == 0x278 && wf::kReqSegFloats == 6);
  QCHECK(std::strcmp(wf::kQuerySymbol, "_ZNK10NRadEngine13CncaGameSpace14QueryCollisionERNS_24SPhQueryCollisionRequestENS_5CPhIdE") == 0);
}

void WithoutBvhNothingIsInstalled() {
  Begin();
  Build(g_siteWord, g_siteAbs);
  const std::uintptr_t before = SlotWord(g_pins.querySlot);
  const nevr_quest::combat::ModuleResult r = wf::Install(true);
  QCHECK(!r.installed && std::strcmp(r.status, "bvh_not_installed") == 0);
  QCHECK(SlotWord(g_pins.querySlot) == before && Count(Event("combat_wallfire_install"), kWarn) == 1 && Count("\"reason\":\"bvh_not_installed\"") == 1);
  End();
}

void TheSwitchAndItsLines() {
  {  // off: one Info line, nothing touched
    Begin();
    Build(g_siteWord, g_siteAbs);
    InstallBvhAndTree();
    const std::uintptr_t before = SlotWord(g_pins.querySlot);
    const nevr_quest::combat::ModuleResult r = wf::Install(false);
    QCHECK(!r.installed && r.off && std::strcmp(r.status, "off_config") == 0 && SlotWord(g_pins.querySlot) == before);
    QCHECK(Count("\"result\":\"off\",\"reason\":\"off_config\"", kInfo) == 1);
    End();
  }
  {  // on, from the default
    Begin();
    Build(g_siteWord, g_siteAbs);
    InstallBvhAndTree();
    g_lines.clear();
    const nevr_quest::combat::ModuleResult first = wf::Install(true);
    QCHECK(first.installed && std::strcmp(first.status, "installed") == 0);
    QCHECK(Count("\"result\":\"installed\",\"reason\":\"ok\"", kInfo) == 1);
    const nevr_quest::combat::ModuleResult again = wf::Install(true);  // a second step installs nothing
    QCHECK(!again.installed && std::strcmp(again.status, "already_ran") == 0);
    QCHECK(std::strcmp(wf::Install(true).status, "already_ran") == 0);
    End();
  }
  {  // the plugin's kill file is a warning and changes nothing
    Begin();
    Build(g_siteWord, g_siteAbs);
    InstallBvhAndTree();
    nevr_quest::combat::legacy::SetDirectoryForTest(nullptr);
    // a fresh directory of its own in the system temp directory, so no other run's file can be found and nothing of another run's is removed
    const char* const tmp = std::getenv("TMPDIR");
    std::string dir = std::string(tmp != nullptr && tmp[0] != '\0' ? tmp : "/tmp") + "/combat-wallfire-legacy-XXXXXX";
    const bool made = mkdtemp(dir.data()) != nullptr;
    QCHECK(made);
    if (made) {
      nevr_quest::combat::legacy::SetDirectoryForTest(dir.c_str());
      const std::string file = dir + "/wallfire.off";
      if (FILE* f = std::fopen(file.c_str(), "w")) std::fclose(f);
      QCHECK(wf::Install(true).installed);
      QCHECK(Count(Event("combat_wallfire_legacy_switch"), kWarn) == 1 && Count("\"replaced_by\":\"combat.wallfire: false\"") == 1);
      nevr_quest::combat::legacy::SetDirectoryForTest(nullptr);
      unlink(file.c_str());  // the one file this test made, then the directory mkdtemp made
      rmdir(dir.c_str());
    }
    End();
  }
}

void ShotsFromTheSiteAreCheckedOnACombatMap() {
  Begin();
  Build(g_siteWord, g_siteAbs);
  InstallBvhAndTree();
  SetGame();
  const std::uintptr_t before = SlotWord(g_pins.querySlot);
  QCHECK(InstalledWallfire());
  QCHECK(SlotWord(g_pins.querySlot) != before);
  QCHECK(SlotWord(g_pins.occludedSlot) == reinterpret_cast<std::uintptr_t>(&FakeStub));  // the StaticRaycastOccluded slot is only read

  const std::uintptr_t gs = reinterpret_cast<std::uintptr_t>(&g_gs);
  // the stock query's own answer passes through, the static level is not asked
  g_queryResult = true;
  QCHECK(CallQuerySite(gs, Req(0, 0, 0, 0, 0, 5.2f), 0xffffffff) && g_stubCalls == 0);
  g_queryResult = false;
  QCHECK(g_stockQuery == 1 && g_lastGs == gs && g_lastId == 0xffffffffULL);
  QCHECK(g_lastReq == reinterpret_cast<std::uintptr_t>(g_req));
  // a segment inside the room: not blocked; the stock answer (false) stands
  QCHECK(!CallQuerySite(gs, Req(0, 0, 0, 0, 0, 4.9f), 0xffffffff));
  QCHECK(g_stubCalls == 1 && g_gameCalls == 1);
  // the segment arguments are start xyz and end xyz
  const float want[6] = {1, 2, 3, 1, 2, 4};
  CallQuerySite(gs, Req(1, 2, 3, 1, 2, 4), 0xffffffff);
  QCHECK(std::memcmp(g_seenSeg, want, sizeof(want)) == 0);
  // through a wall: blocked, so the shot is refused (QueryCollision answers 1)
  QCHECK(CallQuerySite(gs, Req(0, 0, 0, 0, 0, 5.2f), 0xffffffff));
  QCHECK(CallQuerySite(gs, Req(0, 0, 0, 6, 0, 0), 0xffffffff));
  QCHECK(CallQuerySite(gs, Req(3, 1, 0, 3, 7, 0), 0xffffffff));
  QCHECK(Count(Event("combat_wallfire_blocked"), kInfo) == 3);
  {  // the first line counts from 1 and the segment is written start then end
    std::vector<std::string> blocked;
    for (const Line& l : g_lines) {
      if (Has(l.text, Event("combat_wallfire_blocked"))) blocked.push_back(l.text);
    }
    QCHECK(blocked.size() == 3U);
    if (blocked.size() == 3U) {
      QCHECK(Has(blocked[0], "\"blocked\":1,") && Has(blocked[2], "\"blocked\":3,"));
      QCHECK(Has(blocked[2], "\"segment\":\"3.00 1.00 0.00 -> 3.00 7.00 0.00\""));
    }
  }
  // other callers of QueryCollision get the stock answer, whatever the segment
  QCHECK(!CallQueryOther(gs, Req(0, 0, 0, 0, 0, 5.2f), 0xffffffff));
  // a null gamespace and a non-finite segment: the stock answer
  QCHECK(!CallQuerySite(0, Req(0, 0, 0, 0, 0, 5.2f), 0xffffffff));
  QCHECK(!CallQuerySite(gs, Req(0, 0, 0, NAN, 0, 5.2f), 0xffffffff));
  QCHECK(!CallQuerySite(gs, Req(0, 0, 0, 0, INFINITY, 5.2f), 0xffffffff));
  wf::Counters c = C();
  QCHECK(c.atSite == 7 && c.checked == 5 && c.blocked == 3 && c.skippedNan == 2 && c.skippedGame == 0);

  // the game's structures: each wrong one gets the stock answer
  FakeGs badGs = {0, 0, 0, 0};  // no CncaGame
  QCHECK(!CallQuerySite(reinterpret_cast<std::uintptr_t>(&badGs), Req(0, 0, 0, 0, 0, 5.2f), 0xffffffff));
  FakeGs wild = {0, 0x1000, 0, 0};  // a CncaGame that cannot be read
  QCHECK(!CallQuerySite(reinterpret_cast<std::uintptr_t>(&wild), Req(0, 0, 0, 0, 0, 5.2f), 0xffffffff));
  FakeGame wildVtable = {0x1000, {}};  // a vtable pointer that cannot be read
  FakeGs gsWildVtable = {0, reinterpret_cast<std::uintptr_t>(&wildVtable), 0, 0};
  QCHECK(!CallQuerySite(reinterpret_cast<std::uintptr_t>(&gsWildVtable), Req(0, 0, 0, 0, 0, 5.2f), 0xffffffff));
  QCHECK(!CallQuerySite(0x1000, Req(0, 0, 0, 0, 0, 5.2f), 0xffffffff));  // a gamespace that cannot be read
  FakeGame nov = {0, {}};
  FakeGs gsNov = {0, reinterpret_cast<std::uintptr_t>(&nov), 0, 0};
  QCHECK(!CallQuerySite(reinterpret_cast<std::uintptr_t>(&gsNov), Req(0, 0, 0, 0, 0, 5.2f), 0xffffffff));  // a game without a vtable
  std::uintptr_t otherVt[0x300 / 8] = {};
  otherVt[0x278 / 8] = g_image.Base() + 0x1234;  // another class's virtual
  FakeGame other = {reinterpret_cast<std::uintptr_t>(otherVt), {}};
  FakeGs gsOther = {0, reinterpret_cast<std::uintptr_t>(&other), 0, 0};
  QCHECK(!CallQuerySite(reinterpret_cast<std::uintptr_t>(&gsOther), Req(0, 0, 0, 0, 0, 5.2f), 0xffffffff));
  QCHECK(Count(Event("combat_wallfire_skipped"), kWarn) == 1);  // once
  c = C();
  QCHECK(c.skippedGame == 6 && c.checked == 5 && g_stubCalls == 5);
  // the pinned game still works after the others, and a vtable whose slot changes later is checked again
  QCHECK(CallQuerySite(gs, Req(0, 0, 0, 0, 0, 5.2f), 0xffffffff));
  QCHECK(!CallQuerySite(reinterpret_cast<std::uintptr_t>(&gsOther), Req(0, 0, 0, 0, 0, 5.2f), 0xffffffff));
  const std::uintptr_t saved = g_vtable[0x278 / 8];
  g_vtable[0x278 / 8] = g_image.Base() + 0x1234;
  QCHECK(!CallQuerySite(gs, Req(0, 0, 0, 0, 0, 5.2f), 0xffffffff));
  g_vtable[0x278 / 8] = saved;

  // log volume: the first 16 blocked shots, then powers of two
  g_lines.clear();
  for (int i = 0; i < 60; ++i) CallQuerySite(gs, Req(0, 0, 0, 0, 0, 5.2f), 0xffffffff);
  // blocked so far: 3 + 1 + 60 = 64 total; lines for n = 5..16 (12) + 32 + 64 = 14
  QCHECK(Count(Event("combat_wallfire_blocked"), kInfo) == 14);
  QCHECK(C().blocked == 64);
  End();
}

// Arena shares the APK: with no combat map loaded the check is the game's function and a return, and it comes back with the map.
void WithoutACombatMapTheGamesAnswerStands() {
  Begin();
  Build(g_siteWord, g_siteAbs);
  InstallBvhAndTree();
  SetGame();
  QCHECK(InstalledWallfire());
  const std::uintptr_t gs = reinterpret_cast<std::uintptr_t>(&g_gs);
  LoadLevels({"mpl_arena_a"});
  Look();
  QCHECK(!bvh::InCombatOrLobby());
  g_reads = 0;
  g_scans = 0;
  nevr_quest::combat::SetReadCounterForTest(&g_reads);
  nevr_quest::combat::level::SetScanSeamForTest(&CountScan);
  const int clock0 = g_clockReads;
  QCHECK(!CallQuerySite(gs, Req(0, 0, 0, 0, 0, 5.2f), 0xffffffff));  // a segment through the wall is not refused
  QCHECK(!CallQueryOther(gs, Req(0, 0, 0, 0, 0, 5.2f), 0xffffffff));
  nevr_quest::combat::SetReadCounterForTest(nullptr);
  nevr_quest::combat::level::SetScanSeamForTest(nullptr);
  QCHECK(g_reads == 0 && g_scans == 0);  // the arena path reads no game memory and scans no level
  QCHECK(g_clockReads == clock0);  // and no clock: the scope's age is asked only when the cached answer is true
  QCHECK(g_stockQuery == 2 && g_stubCalls == 0 && g_gameCalls == 0);
  QCHECK(C().atSite == 0 && C().checked == 0 && C().blocked == 0);
  LoadLevels({"mpl_lobby_b"});  // a lobby arena players use is no combat class
  Look();
  QCHECK(!bvh::InCombatOrLobby());
  QCHECK(!CallQuerySite(gs, Req(0, 0, 0, 0, 0, 5.2f), 0xffffffff) && g_stubCalls == 0);
  LoadLevels({"mpl_combat_dyson", "mpl_arena_a"});  // an arena level that is loaded wins
  Look();
  QCHECK(!CallQuerySite(gs, Req(0, 0, 0, 0, 0, 5.2f), 0xffffffff) && g_stubCalls == 0);
  EnterCombat();
  QCHECK(CallQuerySite(gs, Req(0, 0, 0, 0, 0, 5.2f), 0xffffffff) && g_stubCalls == 1);
  End();
}

void ANullVtablePointerOnTheVeryFirstShotIsNotTrusted() {
  Begin();
  Build(g_siteWord, g_siteAbs);
  InstallBvhAndTree();
  SetGame();
  QCHECK(InstalledWallfire());
  FakeGame nov = {0, {}};
  FakeGs gsNov = {0, reinterpret_cast<std::uintptr_t>(&nov), 0, 0};
  QCHECK(!CallQuerySite(reinterpret_cast<std::uintptr_t>(&gsNov), Req(0, 0, 0, 0, 0, 5.2f), 0xffffffff) && g_stubCalls == 0);
  QCHECK(C().skippedGame == 1);
  End();
}

void ARefusedInstallChangesNothingAndSaysWhy() {
  {  // the site holds another word
    Begin();
    Build(g_siteWord ^ 1U, g_siteAbs);
    InstallBvhAndTree();
    const std::uintptr_t before = SlotWord(g_pins.querySlot);
    const nevr_quest::combat::ModuleResult r = wf::Install(true);
    QCHECK(!r.installed && std::strcmp(r.status, "site_word") == 0 && SlotWord(g_pins.querySlot) == before);
    QCHECK(Count(Event("combat_wallfire_install"), kError) == 1 && Count("\"reason\":\"site_word\"") == 1);
    End();
  }
  {  // the StaticRaycastOccluded slot holds another function
    Begin();
    Build(g_siteWord, g_siteAbs);
    InstallBvhAndTree();
    const std::uintptr_t before = SlotWord(g_pins.querySlot);
    *g_image.Slot(g_pins.occludedSlot) = reinterpret_cast<void*>(0x4242420);
    const nevr_quest::combat::ModuleResult r = wf::Install(true);
    QCHECK(!r.installed && std::strcmp(r.status, "occluded_slot") == 0 && SlotWord(g_pins.querySlot) == before);
    End();
  }
  {  // the QueryCollision slot holds a foreign word
    Begin();
    Build(g_siteWord, g_siteAbs);
    InstallBvhAndTree();
    *g_image.Slot(g_pins.querySlot) = reinterpret_cast<void*>(0x4242420);
    const nevr_quest::combat::ModuleResult r = wf::Install(true);
    QCHECK(!r.installed && std::strcmp(r.status, "original_mismatch") == 0 && SlotWord(g_pins.querySlot) == 0x4242420);
    QCHECK(Count("\"reason\":\"hook_refused\"", kError) == 1);
    End();
  }
  {  // the slot holds some other mapped function (not the pinned one): refused, not chained
    Begin();
    Build(g_siteWord, g_siteAbs);
    InstallBvhAndTree();
    *g_image.Slot(g_pins.querySlot) = reinterpret_cast<void*>(&StockInit);
    const nevr_quest::combat::ModuleResult r = wf::Install(true);
    QCHECK(!r.installed && std::strcmp(r.status, "original_mismatch") == 0 && SlotWord(g_pins.querySlot) == reinterpret_cast<std::uintptr_t>(&StockInit));
    End();
  }
  {  // libr15 not loaded
    Begin();
    Build(g_siteWord, g_siteAbs);
    InstallBvhAndTree();
    g_image.Drop();
    const nevr_quest::combat::ModuleResult r = wf::Install(true);
    QCHECK(!r.installed && std::strcmp(r.status, "module_not_loaded") == 0);
    End();
  }
}

}  // namespace

int main() {
  LearnTheSite();
  ThePinsAreTheReadelfAndDisassemblyPins();
  WithoutBvhNothingIsInstalled();
  TheSwitchAndItsLines();
  ShotsFromTheSiteAreCheckedOnACombatMap();
  WithoutACombatMapTheGamesAnswerStands();
  ANullVtablePointerOnTheVeryFirstShotIsNotTrusted();
  ARefusedInstallChangesNothingAndSaysWhy();
  if (quest_test::Failures() == 0) std::printf("wallfire_test: all checks pass\n");
  return quest_test::Failures() == 0 ? 0 : 1;
}
