// Host test of the static BVH (src/quest/combat/bvh.cpp, #2182): the real thunks, installed by the real GotHook into the six JUMP_SLOTs of a libr15
// image built in memory, and called the way the game calls them: through the slot, against fake stock functions and fake game structures laid out
// as the pinned build's code reads them (docs/quest/combat/bvh.md section 3). Synthetic meshes, no game-derived bytes.
//
// Moved from the q15t plugin's bvh_test: the assertions on the answers are the same (the nearest hit, the segment test, the hit's fields, the
// refusals of Initialize and their lines, the 64-entry table, the callers' gate and its per-thread mark, the 10 s summary, all-or-none install, the
// touch answer, and a Shutdown freeing its CBVHTree's table entry). What changed is the fixture (the plugin's fake slot page is a libr15 image and the manager's install, not a slot swap; the
// plugin's kill files are `combat:` switches) and three deliberate behaviours: every answer is scoped to a combat map (the arena path calls the
// game's function), the arguments of a cast are read with plain loads (the unreadable-argument assertions went with the system call), and a
// thunk with no original answers a miss without calling the handler. New here: the scope, the switches, the legacy switch files.
//
// Built and run by `just test-quest-combat`.
#include "quest/combat/bvh.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "nevr/symbol_hash.h"
#include "quest/combat/combat_level.h"
#include "quest/combat/combat_tick.h"
#include "quest/combat/safe_read.h"
#include "quest/combat/legacy_switch.h"
#include "quest/tests/bvh_build.h"
#include "quest/tests/combat_libr15_image.h"
#include "quest/tests/key_hog.h"
#include "quest/tests/test_check.h"

namespace {

namespace bvh = nevr_quest::combat::bvh;
namespace pinned = sentinel::pinned;
using combat_test::Image;
using EchoVR::CSymbol64Hash;

// ---- the lines ------------------------------------------------------------------------------------------------------------------
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

// ---- fake stock functions (what the pinned build's stubs do) ----
int g_stockInit, g_stockShut, g_stockInt, g_stockOcc, g_stockTouch, g_stockGate;
using IntersectFn = bool (*)(const void*, const float*, std::uint8_t*);
using OccludedFn = bool (*)(const void*, const float*, const float*);
using GateFn = bool (*)(const void*, const void*, void*, void**);
using TouchFn = bool (*)(const void*, const void*, void*, float*);
using InitFn = void (*)(void*, const void*);
using ShutFn = void (*)(void*);

Image g_image;
bvh::Pins g_pins = bvh::kPins;
void* g_cbvhSelf = nullptr;  // the CBVHTree CncaGame::StaticRaycast would pass to Intersect
bool g_nested = false, g_spawn = false;
int g_spawnResult = -1;
bool g_stockGateRanWithArgs = false;
int g_nestedResult = -1;

IntersectFn IntersectSlot() { return reinterpret_cast<IntersectFn>(*g_image.Slot(g_pins.intersect.slot)); }
OccludedFn OccludedSlot() { return reinterpret_cast<OccludedFn>(*g_image.Slot(g_pins.occluded.slot)); }
GateFn GateSlot() { return reinterpret_cast<GateFn>(*g_image.Slot(g_pins.gate.slot)); }
TouchFn TouchSlot() { return reinterpret_cast<TouchFn>(*g_image.Slot(g_pins.touch.slot)); }
InitFn InitSlot() { return reinterpret_cast<InitFn>(*g_image.Slot(g_pins.init.slot)); }
ShutFn ShutSlot() { return reinterpret_cast<ShutFn>(*g_image.Slot(g_pins.shutdown.slot)); }
std::uintptr_t SlotWord(std::uint64_t slot) { return reinterpret_cast<std::uintptr_t>(*g_image.Slot(slot)); }

extern "C" __attribute__((noinline)) void StockInit(void*, const void*) { ++g_stockInit; }
extern "C" __attribute__((noinline)) void StockShut(void*) { ++g_stockShut; }
extern "C" __attribute__((noinline)) bool StockInt(const void*, const float*, std::uint8_t*) {
  ++g_stockInt;
  return false;
}
extern "C" __attribute__((noinline)) bool StockOcc(const void*, const float*, const float*) {
  ++g_stockOcc;
  return false;
}
extern "C" __attribute__((noinline)) bool StockTouch(const void*, const void*, void*, float*) {
  ++g_stockTouch;
  return true;  // distinguishable from the replaced one, which answers false
}

__attribute__((noinline)) bool CallGateAllowedSite(const void* a, const void* b, void* c, void** d) {
  volatile bool r = GateSlot()(a, b, c, d);
  return r;
}
__attribute__((noinline)) bool CallGateOtherSite(const void* a, const void* b, void* c, void** d) {
  volatile bool r = GateSlot()(a, b, c, d);
  return r;
}
bool CallInt(const void* self, const float* ray, std::uint8_t* hit) { return IntersectSlot()(self, ray, hit); }

// CncaGame::StaticRaycast: casts into the gamespace's CBVHTree through the PLT slot
extern "C" __attribute__((noinline)) bool StockGate(const void* self, const void* ray, void* hit, void** gs) {
  ++g_stockGate;
  g_stockGateRanWithArgs = self == reinterpret_cast<const void*>(0x77) && ray != nullptr && hit != nullptr;
  if (g_nested) {  // a second cast from an unlisted call site, inside the allowed one
    g_nested = false;
    void* dummy = nullptr;
    std::uint8_t scratch[0x44];
    g_nestedResult = CallGateOtherSite(self, ray, scratch, &dummy) ? 1 : 0;
  }
  if (g_spawn) {  // another thread casts directly while this thread is inside an allowed cast
    g_spawn = false;
    std::thread th([&] {
      std::uint8_t scratch[0x44];
      g_spawnResult = CallInt(g_cbvhSelf, static_cast<const float*>(ray), scratch);
    });
    th.join();
  }
  const bool r = CallInt(g_cbvhSelf, static_cast<const float*>(ray), static_cast<std::uint8_t*>(hit));
  if (r && gs != nullptr) *gs = reinterpret_cast<void*>(0x1234);
  return r;
}

// ---- the game's levels --------------------------------------------------------------------------------------------------------------
// CR15Game's gamespace table and a gamespace's level, laid out where combat_level.cpp reads them (the offsets are spelled here, not taken from
// the header), read by process_vm_readv as in production.
constexpr std::size_t kTable0 = 0x1f48, kCount0 = 0x1f78, kGamespaceOwner = 0x8, kGamespaceLevel = 0x58;
constexpr std::size_t kFiberRows = 0x1ed8, kFiberCount = 0x1f08, kFiberGamespace = 0x30;
struct Gamespace {
  std::uintptr_t words[0x60 / 8 + 1] = {};
};
struct Game {
  std::uintptr_t words[0x2000 / 8] = {};
  std::uintptr_t table0[8][2] = {};
  Gamespace spaces[8];
  std::uintptr_t fiberTable[4] = {};     // game + 0x1ed8: the init fibers of the gamespaces still loading
  std::uintptr_t fibers[4][0x40 / 8] = {};  // a fiber's gamespace is at fiber + 0x30
  Gamespace loading[4];
};
Game g_game;
int g_loadingCount = 0;
std::uintptr_t GameAddress() { return reinterpret_cast<std::uintptr_t>(g_game.words); }
void LoadLevels(std::initializer_list<const char*> names) {
  g_game = Game{};
  g_loadingCount = 0;
  int row = 0;
  for (const char* name : names) {
    Gamespace& gs = g_game.spaces[row];
    gs.words[kGamespaceOwner / 8] = GameAddress();
    gs.words[kGamespaceLevel / 8] = static_cast<std::uintptr_t>(CSymbol64Hash(name));
    g_game.table0[row][0] = 0x5555;
    g_game.table0[row][1] = reinterpret_cast<std::uintptr_t>(&gs);
    ++row;
  }
  g_game.words[kTable0 / 8] = reinterpret_cast<std::uintptr_t>(g_game.table0);
  g_game.words[kCount0 / 8] = static_cast<std::uintptr_t>(row);
}

// What a path costs, counted: every SafeRead and every level scan goes through a counter behind its test seam (safe_read.h, combat_level.h).
std::atomic<std::uint64_t> g_reads{0}, g_scans{0};
void CountScan() { g_scans.fetch_add(1); }
void StartCounting() {
  g_reads = 0;
  g_scans = 0;
  nevr_quest::combat::SetReadCounterForTest(&g_reads);
  nevr_quest::combat::level::SetScanSeamForTest(&CountScan);
}
void StopCounting() {
  nevr_quest::combat::SetReadCounterForTest(nullptr);
  nevr_quest::combat::level::SetScanSeamForTest(nullptr);
}

// A gamespace still loading `name`: an init fiber in the game's fiber table, in neither of the two loaded tables.
void AddLoading(const char* name) {
  const int i = g_loadingCount++;
  g_game.loading[i].words[kGamespaceLevel / 8] = static_cast<std::uintptr_t>(CSymbol64Hash(name));
  g_game.fibers[i][kFiberGamespace / 8] = reinterpret_cast<std::uintptr_t>(&g_game.loading[i]);
  g_game.fiberTable[i] = reinterpret_cast<std::uintptr_t>(g_game.fibers[i]);
  g_game.words[kFiberRows / 8] = reinterpret_cast<std::uintptr_t>(g_game.fiberTable);
  g_game.words[kFiberCount / 8] = static_cast<std::uintptr_t>(g_loadingCount);
}
std::string LevelHex(const char* name) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "0x%016llx", static_cast<unsigned long long>(CSymbol64Hash(name)));
  return buf;
}

// The tick's clock: one second per look, so every Tick that asks for a look gets one.
double g_now = 100.0;
int g_clockReads = 0;  // every read of the module's clock: the arena path reads none
double Clock() noexcept {
  ++g_clockReads;
  return g_now;
}
void Look() {
  g_now += 1.0;
  bvh::Tick(GameAddress());
}
void EnterCombat() {
  LoadLevels({"mpl_combat_dyson"});
  Look();
}
void EnterArena() {
  LoadLevels({"mpl_arena_a"});
  Look();
}

// ---- fake game structures ----
bt::Resource RoomResource(bool fixedPtrs, int leaf = 4, std::uint32_t pad = 0) {
  const float lo[3] = {-5, -5, -5}, hi[3] = {5, 5, 5};
  static std::vector<bt::TriIn> in;
  in.clear();
  bt::AddBox(in, lo, hi, 100, true);
  const bt::Built b = bt::Builder(in, leaf).Build(static_cast<int>(pad));
  return bt::MakeResource(b, fixedPtrs);
}

// two parallel squares in one leaf, the far one (z = 5, id0 200) listed before the near one (z = 2, id0 201): the nearest hit is not the first found
bt::Resource TwoQuadResource() {
  std::vector<bt::TriIn> in;
  int id1 = 0;
  const float far_[4][3] = {{-1, -1, 5}, {1, -1, 5}, {1, 1, 5}, {-1, 1, 5}}, near_[4][3] = {{-1, -1, 2}, {1, -1, 2}, {1, 1, 2}, {-1, 1, 2}};
  const float toward[3] = {0, 0, -10};
  bt::AddQuad(in, far_[0], far_[1], far_[2], far_[3], toward, 200, id1);
  bt::AddQuad(in, near_[0], near_[1], near_[2], near_[3], toward, 201, id1);
  return bt::MakeResource(bt::Builder(in, 8).BuildLeafRoot(), true);
}

// ---- the image ----
bool Lookup(const char* module, sentinel::ElfImage* out) {
  if (!g_image.Present() || std::strcmp(module, pinned::kLibR15) != 0) return false;
  *out = g_image.Elf();
  return true;
}

// A libr15 whose six slots hold the stand-ins; the pins' functions are the stand-ins' offsets from the image base.
void Build(const char* buildId = nullptr) {
  g_pins = bvh::kPins;
  g_image.Build({{bvh::kInitSymbol, g_pins.init.slot, false, false, 0},
                 {bvh::kShutSymbol, g_pins.shutdown.slot, false, false, 0},
                 {bvh::kIntersectSymbol, g_pins.intersect.slot, false, false, 0},
                 {bvh::kOccludedSymbol, g_pins.occluded.slot, false, false, 0},
                 {bvh::kTouchSymbol, g_pins.touch.slot, false, false, 0},
                 {bvh::kGateSymbol, g_pins.gate.slot, false, false, 0}},
                buildId);
  const std::uintptr_t base = g_image.Base();
  const void* fns[6] = {reinterpret_cast<const void*>(&StockInit), reinterpret_cast<const void*>(&StockShut),
                        reinterpret_cast<const void*>(&StockInt),  reinterpret_cast<const void*>(&StockOcc),
                        reinterpret_cast<const void*>(&StockTouch), reinterpret_cast<const void*>(&StockGate)};
  bvh::Slot* slots[6] = {&g_pins.init, &g_pins.shutdown, &g_pins.intersect, &g_pins.occluded, &g_pins.touch, &g_pins.gate};
  for (int i = 0; i < 6; ++i) {
    slots[i]->fn = reinterpret_cast<std::uintptr_t>(fns[i]) - base;
    *g_image.Slot(slots[i]->slot) = const_cast<void*>(fns[i]);
  }
  g_pins.allowedCount = 0;
  bvh::SetImageLookupForTest(&Lookup);
  bvh::SetPinsForTest(&g_pins);
}

std::string g_dir;
void Begin() {
  g_lines.clear();
  bvh::ResetForTest();
  nevr_quest::combat::tick::ResetForTest();
  g_stockInit = g_stockShut = g_stockInt = g_stockOcc = g_stockTouch = g_stockGate = 0;
  g_nested = g_spawn = false;
  g_spawnResult = -1;
  g_cbvhSelf = nullptr;
  g_now = 100.0;
  LoadLevels({});
  nevr_quest::combat::legacy::SetDirectoryForTest(nullptr);
  bvh::SetClockForTest(&Clock);
  sentinel::SetLogSink(&Capture);
}
void End() {
  bvh::ResetForTest();
  nevr_quest::combat::tick::ResetForTest();
  nevr_quest::combat::legacy::SetDirectoryForTest(nullptr);
  g_image.Drop();
  sentinel::SetLogSink(nullptr);
}

bvh::Switches Gateless() {
  bvh::Switches sw;
  sw.bvh = true;
  sw.gate = false;
  return sw;
}
bvh::Switches On() {
  bvh::Switches sw;
  sw.bvh = true;
  return sw;
}
// The install, with the captured lines printed when it does not install (a failing check then says why).
bool Installed(const bvh::Switches& sw) {
  const nevr_quest::combat::ModuleResult r = bvh::Install(sw);
  if (!r.installed) {
    for (const Line& l : g_lines) std::fprintf(stderr, "  line: %s\n", l.text.c_str());
  }
  return r.installed;
}

const float kDown[6] = {0, 0, 0, 0, -1, 0};  // from the room's centre, straight down: hits the floor (id0 102) at t = 5

// ---- the tests ------------------------------------------------------------------------------------------------------------------

void ThePinsAreTheReadelfPins() {
  const bvh::Pins& p = bvh::kPins;
  QCHECK(p.init.slot == 0x36ec548 && p.init.fn == 0x1a45a88);
  QCHECK(p.shutdown.slot == 0x36eb648 && p.shutdown.fn == 0x1a45a8c);
  QCHECK(p.intersect.slot == 0x36d38f0 && p.intersect.fn == 0x1a45ae4);
  QCHECK(p.occluded.slot == 0x36e8430 && p.occluded.fn == 0x1a45b38);
  QCHECK(p.gate.slot == 0x36c0288 && p.gate.fn == 0x18066e4);
  QCHECK(p.touch.slot == 0x36f37d8 && p.touch.fn == 0x20cf414);
  QCHECK(p.allowedCount == 3 && p.allowed[0] == 0x12b2b4c && p.allowed[1] == 0x12b2bc4 && p.allowed[2] == 0x12c0a54);
  QCHECK(std::strcmp(bvh::kInitSymbol, "_ZN10NRadEngine8CBVHTree10InitializeERKNS_12SBVHTreeDataE") == 0);
  QCHECK(std::strcmp(bvh::kShutSymbol, "_ZN10NRadEngine8CBVHTree8ShutdownEv") == 0);
  QCHECK(std::strcmp(bvh::kIntersectSymbol, "_ZNK10NRadEngine8CBVHTree9IntersectERKNS_4CRayERNS_7SBVHHitE") == 0);
  QCHECK(std::strcmp(bvh::kOccludedSymbol, "_ZNK10NRadEngine8CBVHTree8OccludedERKNS_8C3VectorES3_") == 0);
  QCHECK(std::strcmp(bvh::kTouchSymbol,
                     "_ZN10NRadEngine8NRadGame22CR15TouchInteractQuery14RayQueryStaticEPKNS_15CncaGameSpaceMTERKNS_4CRayERNS0_8SMeshHitERf") == 0);
  QCHECK(std::strcmp(bvh::kGateSymbol, "_ZNK10NRadEngine13CncaGameSpace13StaticRaycastERKNS_4CRayERNS_7SBVHHitERPNS_15CncaGameSpaceMTE") == 0);
}

// No pthread key is left for the per-thread allowed mark: the install refuses and swaps nothing. It runs first: the key is made once, by the first
// install that reaches it, and never deleted.
void NoThreadKeyRefusesAndChangesNothing() {
  Begin();
  Build();
  std::uintptr_t before[6];
  const bvh::Slot* slots[6] = {&g_pins.init, &g_pins.shutdown, &g_pins.intersect, &g_pins.occluded, &g_pins.touch, &g_pins.gate};
  for (int i = 0; i < 6; ++i) before[i] = SlotWord(slots[i]->slot);
  {
    KeyHog hog;
    const nevr_quest::combat::ModuleResult r = bvh::Install(On());
    QCHECK(!r.installed && std::strcmp(r.status, "no_thread_key") == 0 && !bvh::TreeInstalled());
    for (int i = 0; i < 6; ++i) QCHECK(SlotWord(slots[i]->slot) == before[i]);
    QCHECK(Count(Event("combat_bvh_install"), kError) == 1 && Count("\"reason\":\"no_thread_key\"") == 1);
    QCHECK(Count(std::string("\"error\":") + std::to_string(EAGAIN)) == 1);  // what pthread_key_create answers when the process has no key left
  }
  End();
}

void TheInstallSwapsTheSlotsAndStatesItOnce() {
  Begin();
  Build();
  const bvh::Slot* slots[6] = {&g_pins.init, &g_pins.shutdown, &g_pins.intersect, &g_pins.occluded, &g_pins.touch, &g_pins.gate};
  std::uintptr_t before[6];
  for (int i = 0; i < 6; ++i) before[i] = SlotWord(slots[i]->slot);
  LoadLevels({"mpl_combat_dyson"});
  bvh::Tick(GameAddress());  // a tick before the install looks at nothing: the scope stays shut
  QCHECK(!bvh::InCombatOrLobby());
  QCHECK(Installed(On()));
  QCHECK(bvh::TreeInstalled());
  for (int i = 0; i < 6; ++i) QCHECK(SlotWord(slots[i]->slot) != before[i]);
  const std::string install = Event("combat_bvh_install");
  QCHECK(Count(install) == 1 && Count(install, kInfo) == 1);
  QCHECK(Count("\"result\":\"installed\",\"reason\":\"ok\",\"hooks\":6,\"gate\":1,\"touch\":1,\"allowed_casts\":0") == 1);
  QCHECK(nevr_quest::combat::tick::Count() == 1);  // the scope's refresh rides the combat tick
  {  // the gate goes in first: until its hook is in no cast is marked, so a tree installed a moment later answers none
    std::size_t gateAt = 0, initAt = 0;
    for (std::size_t i = 0; i < g_lines.size(); ++i) {
      if (Has(g_lines[i].text, Event("got_hook")) && Has(g_lines[i].text, "\"op\":\"install\"")) {
        if (Has(g_lines[i].text, bvh::kGateSymbol)) gateAt = i + 1;
        if (Has(g_lines[i].text, bvh::kInitSymbol)) initAt = i + 1;
      }
    }
    QCHECK(gateAt != 0 && initAt != 0 && gateAt < initAt);
  }
  QCHECK(Count(Event("combat_bvh_legacy_switch")) == 0);  // no kill file in the plugin's data directory: nothing to warn about
  // a second step installs nothing
  const nevr_quest::combat::ModuleResult again = bvh::Install(On());
  QCHECK(!again.installed && std::strcmp(again.status, "already_ran") == 0);
  End();
}

void SwitchesDecideWhatIsInstalled() {
  {  // off: one Info line, no slot touched, no tick registered
    Begin();
    Build();
    const std::uintptr_t before = SlotWord(g_pins.intersect.slot);
    bvh::Switches sw;
    sw.bvh = false;
    const nevr_quest::combat::ModuleResult r = bvh::Install(sw);
    QCHECK(!r.installed && r.off && std::strcmp(r.status, "off_config") == 0 && !bvh::TreeInstalled());
    QCHECK(SlotWord(g_pins.intersect.slot) == before && nevr_quest::combat::tick::Count() == 0);
    QCHECK(Count("\"result\":\"off\",\"reason\":\"off_config\"", kInfo) == 1);
    QCHECK(Count(std::string("\"level\":\"error\"")) == 0);
    End();
  }
  {  // the shipped default is on (echoquest-combat's): Switches{} installs the module, gate and touch included
    Begin();
    Build();
    const nevr_quest::combat::ModuleResult r = bvh::Install(bvh::Switches{});
    QCHECK(r.installed && std::strcmp(r.status, "installed") == 0);
    QCHECK(Count("\"hooks\":6,\"gate\":1,\"touch\":1") == 1);
    End();
  }
  {  // the gate off: five hooks, the gate slot untouched, the line says so
    Begin();
    Build();
    const std::uintptr_t gateBefore = SlotWord(g_pins.gate.slot);
    bvh::Switches sw = Gateless();
    QCHECK(Installed(sw));
    QCHECK(SlotWord(g_pins.gate.slot) == gateBefore);
    QCHECK(Count("\"hooks\":5,\"gate\":0,\"touch\":1") == 1);
    End();
  }
  {  // touch rays kept: the touch slot is the game's
    Begin();
    Build();
    const std::uintptr_t touchBefore = SlotWord(g_pins.touch.slot);
    bvh::Switches sw = On();
    sw.touch = false;
    QCHECK(Installed(sw));
    QCHECK(SlotWord(g_pins.touch.slot) == touchBefore);
    QCHECK(Count("\"hooks\":5,\"gate\":1,\"touch\":0") == 1);
    End();
  }
}

// The touch hook is not part of the all-or-none set: a foreign word in its slot is an Error line, the slot stays the game's and the rest is installed.
void ARefusedTouchHookDoesNotStopTheTree() {
  Begin();
  Build();
  *g_image.Slot(g_pins.touch.slot) = reinterpret_cast<void*>(0x4242420);
  QCHECK(Installed(On()));
  QCHECK(bvh::TreeInstalled() && SlotWord(g_pins.touch.slot) == 0x4242420);
  QCHECK(Count(Event("combat_bvh_touch"), kError) == 1 && Count("\"status\":\"original_mismatch\"") >= 1);
  QCHECK(Count("\"hooks\":5,\"gate\":1,\"touch\":0") == 1 && Count("\"result\":\"installed\"", kInfo) == 1);
  End();
}

void ALegacySwitchFileIsAWarningAndChangesNothing() {
  Begin();
  Build();
  nevr_quest::combat::legacy::SetDirectoryForTest(g_dir.c_str());
  for (const char* f : {"bvh.off", "bvhgate.off", "bvhtouch.keep"}) {
    FILE* file = std::fopen((g_dir + "/" + f).c_str(), "w");
    if (file != nullptr) std::fclose(file);
  }
  QCHECK(Installed(On()));  // no file turns the module off, its gate off or its touch hook off
  QCHECK(Count(Event("combat_bvh_legacy_switch"), kWarn) == 3);
  QCHECK(Count("\"file\":\"bvh.off\",\"replaced_by\":\"combat.bvh: false\"") == 1);
  QCHECK(Count("\"file\":\"bvhgate.off\",\"replaced_by\":\"combat.bvh_gate: false\"") == 1);
  QCHECK(Count("\"file\":\"bvhtouch.keep\",\"replaced_by\":\"combat.bvh_touch: false\"") == 1);
  QCHECK(Count("\"hooks\":6,\"gate\":1,\"touch\":1") == 1);
  for (const char* f : {"bvh.off", "bvhgate.off", "bvhtouch.keep"}) unlink((g_dir + "/" + f).c_str());
  End();
}

// Initialize / Intersect / Occluded / Shutdown through the slots, on a combat map.
void ACombatMapGetsTheTreesAnswers() {
  Begin();
  Build();
  QCHECK(Installed(Gateless()));
  EnterCombat();
  QCHECK(bvh::InCombatOrLobby());
  QCHECK(Count(Event("combat_bvh_scope"), kInfo) == 1 && Count("\"from\":0,\"to\":1") == 1);
  Look();  // the same answer again: no second line
  QCHECK(Count(Event("combat_bvh_scope")) == 1);

  bt::Resource res = RoomResource(true);
  std::uint64_t cbvh = 0xdeadbeef;  // the stock constructor zeroes it; a garbage value must not matter
  g_cbvhSelf = &cbvh;
  InitSlot()(&cbvh, res.bytes.data());
  QCHECK(g_stockInit == 1);
  QCHECK(cbvh != 0 && cbvh != 0xdeadbeef);
  QCHECK(Count(Event("combat_bvh_tree")) == 1 && Count("\"result\":\"built\"", kInfo) == 1 && Count("\"triangle4\":") == 1);
  QCHECK(bvh::TreeCount() == 1);

  std::uint8_t hit[0x44];
  std::memset(hit, 0xab, sizeof(hit));
  const std::int32_t m1 = -1;
  std::memcpy(hit + 0, &m1, 4);
  std::memcpy(hit + 4, &m1, 4);
  QCHECK(CallInt(&cbvh, kDown, hit));
  std::int32_t id0, id1;
  float u, v, t, n[3];
  std::memcpy(&id0, hit + 0, 4);
  std::memcpy(&id1, hit + 4, 4);
  std::memcpy(&u, hit + 8, 4);
  std::memcpy(&v, hit + 12, 4);
  std::memcpy(&t, hit + 0x10, 4);
  std::memcpy(n, hit + 0x38, 12);
  QCHECK(id0 == 102 && id1 >= 0 && t > 4.99f && t < 5.01f);
  {  // every field is what the core answers for the same tree and ray
    bvh::Tree tree;
    bvh::Blocks blocks;
    const char* why = "";
    QCHECK(bvh::Parse(res.bytes.data(), reinterpret_cast<std::uintptr_t>(res.bytes.data()), &tree, &blocks, &why));
    bvh::Hit want;
    QCHECK(bvh::Trace(tree, bvh::Ray(kDown, kDown + 3, 0.0f, INFINITY), false, &want));
    QCHECK(id0 == want.id0 && id1 == want.id1 && u == want.u && v == want.v && t == want.t);
    QCHECK(n[0] == want.n[0] && n[1] == want.n[1] && n[2] == want.n[2]);
    // and for a ray off the diagonal, where u and v differ, so a swapped pair shows
    const float off[6] = {1.7f, 0, 0.4f, 0, -1, 0};
    bvh::Hit want2;
    QCHECK(bvh::Trace(tree, bvh::Ray(off, off + 3, 0.0f, INFINITY), false, &want2));
    QCHECK(want2.u != want2.v);
    std::uint8_t hit2[0x44] = {};
    QCHECK(CallInt(&cbvh, off, hit2));
    float u2, v2;
    std::int32_t a0, a1;
    std::memcpy(&a0, hit2 + 0, 4);
    std::memcpy(&a1, hit2 + 4, 4);
    std::memcpy(&u2, hit2 + 8, 4);
    std::memcpy(&v2, hit2 + 12, 4);
    QCHECK(a0 == want2.id0 && a1 == want2.id1 && u2 == want2.u && v2 == want2.v);
  }
  QCHECK(u >= 0 && v >= 0 && u + v <= 1.0001f);
  QCHECK(n[1] > 0.99f && n[0] * n[0] + n[1] * n[1] + n[2] * n[2] > 0.99f);
  bool untouched = true;
  for (int i = 0x14; i < 0x38; ++i) untouched &= hit[i] == 0xab;
  QCHECK(untouched);  // bytes 0x14..0x37 of the hit are the caller's
  QCHECK(Count(Event("combat_bvh_first_hit"), kInfo) == 1);
  CallInt(&cbvh, kDown, hit);
  QCHECK(Count(Event("combat_bvh_first_hit")) == 1);  // once
  // a miss writes nothing
  std::uint8_t clean[0x44];
  std::memset(clean, 0xcd, sizeof(clean));
  const float upOut[6] = {0, 20, 0, 0, 1, 0};  // from outside, away from the room
  QCHECK(!CallInt(&cbvh, upOut, clean));
  bool allCd = true;
  for (std::uint8_t b : clean) allCd &= b == 0xcd;
  QCHECK(allCd && g_stockInt == 0);

  // Occluded: the segment a..b is t in (0, 1): clear inside, blocked through a wall
  const float a[3] = {0, 0, 0}, in_[3] = {0, 0, 4.9f}, out[3] = {0, 0, 5.2f};
  QCHECK(!OccludedSlot()(&cbvh, a, in_));
  QCHECK(OccludedSlot()(&cbvh, a, out));
  QCHECK(Count(Event("combat_bvh_first_block"), kInfo) == 1);
  OccludedSlot()(&cbvh, a, out);
  QCHECK(Count(Event("combat_bvh_first_block")) == 1);
  const float a3[3] = {0, 0, 3}, out2[3] = {0, 0, 5.5f};  // blocked going +z, but not if the direction were reversed
  QCHECK(OccludedSlot()(&cbvh, a3, out2));
  bvh::Counters c = bvh::GetCounters();
  QCHECK(c.intersect == 4 && c.intersectHit == 3 && c.occluded == 4 && c.occludedBlocked == 3 && c.treesBuilt == 1 && c.stock == 0);

  // the file-form resource (both header pointers 0: the blocks follow the header) and a root that is not node 0
  bt::Resource fileForm = RoomResource(false, 2, 5);
  std::uint64_t cbvh2 = 0;
  InitSlot()(&cbvh2, fileForm.bytes.data());
  std::uint8_t hit2[0x44];
  std::memset(hit2, 0, sizeof(hit2));
  QCHECK(cbvh2 != 0 && CallInt(&cbvh2, kDown, hit2));
  QCHECK(Count("\"root_ref\":\"0x0000000000000028\"") == 1);

  // the nearest hit, not the first one found: a leaf that lists the far square first
  bt::Resource two = TwoQuadResource();
  std::uint64_t cbvh3 = 0;
  InitSlot()(&cbvh3, two.bytes.data());
  const float ray3[6] = {0, 0, -10, 0, 0, 1};
  std::uint8_t hit3[0x44];
  std::memset(hit3, 0, sizeof(hit3));
  std::int32_t id3 = -1;
  float t3 = 0;
  QCHECK(cbvh3 != 0 && CallInt(&cbvh3, ray3, hit3));
  std::memcpy(&id3, hit3, 4);
  std::memcpy(&t3, hit3 + 0x10, 4);
  QCHECK(id3 == 201 && t3 > 11.99f && t3 < 12.01f);

  // Shutdown clears; the next Intersect goes to the stock function
  ShutSlot()(&cbvh);
  QCHECK(g_stockShut == 1 && cbvh == 0);
  g_cbvhSelf = &cbvh;
  QCHECK(!CallInt(&cbvh, kDown, hit) && g_stockInt == 1);
  QCHECK(!OccludedSlot()(&cbvh, a, out) && g_stockOcc == 1);

  // a CBVHTree word that is not one of our table entries is a doubt: the stock function answers, nothing is dereferenced
  std::uint64_t garbage = 0x1234567;
  QCHECK(!CallInt(&garbage, kDown, hit) && g_stockInt == 2);
  std::uint64_t misaligned = cbvh2 + 1;
  QCHECK(!CallInt(&misaligned, kDown, hit) && g_stockInt == 3);
  std::uint64_t kept = cbvh2;
  QCHECK(CallInt(&kept, kDown, hit));  // a copy of a valid CBVHTree (same word) answers
  QCHECK(bvh::GetCounters().stock == 4);  // the two Shutdown'd calls and the two foreign words

  // touch rays: a miss, as the stock Quest
  QCHECK(SlotWord(g_pins.touch.slot) != reinterpret_cast<std::uintptr_t>(&StockTouch));
  QCHECK(!TouchSlot()(nullptr, nullptr, nullptr, nullptr) && g_stockTouch == 0);
  QCHECK(bvh::GetCounters().touch == 1);
  End();
}

// Arena shares the APK: with no combat map loaded, or an Echo Arena level loaded beside one, every hook answers with the game's function.
void WithoutACombatMapEveryHookCallsTheGamesFunction() {
  Begin();
  Build();
  QCHECK(Installed(On()));
  bt::Resource res = RoomResource(true);
  std::uint64_t cbvh = 0;
  g_cbvhSelf = &cbvh;
  // No game has been seen by the tick yet, so nothing is known about the level being loaded: an Initialize before the first look still gets its tree.
  InitSlot()(&cbvh, res.bytes.data());
  QCHECK(cbvh != 0 && bvh::TreeCount() == 1 && g_stockInit == 1);
  std::uint8_t hit[0x44] = {};
  const float a[3] = {0, 0, 0}, out[3] = {0, 0, 5.2f};
  const auto allStock = [&]() {
    const int int0 = g_stockInt, occ0 = g_stockOcc, touch0 = g_stockTouch, gate0 = g_stockGate;
    void* gs = nullptr;
    StartCounting();
    const int clock0 = g_clockReads;
    QCHECK(!CallInt(&cbvh, kDown, hit));
    QCHECK(!OccludedSlot()(&cbvh, a, out));
    QCHECK(TouchSlot()(nullptr, nullptr, nullptr, nullptr));  // StockTouch answers true: the replaced one would answer false
    QCHECK(!CallGateAllowedSite(reinterpret_cast<const void*>(0x77), kDown, hit, &gs));
    StopCounting();
    QCHECK(g_reads == 0 && g_scans == 0);  // the arena path of all four hooks reads no game memory and scans no level
    QCHECK(g_clockReads == clock0);  // and reads no clock: the scope's age is asked only when the cached answer is true
    QCHECK(g_stockInt == int0 + 2 && g_stockOcc == occ0 + 1 && g_stockTouch == touch0 + 1 && g_stockGate == gate0 + 1);  // the gate's own stock cast reaches Intersect too
    const bvh::Counters c = bvh::GetCounters();
    QCHECK(c.intersect == 0 && c.occluded == 0 && c.touch == 0 && c.gated == 0 && c.stock == 0);
    QCHECK(bvh::LastGateReturnForTest() == 0);  // the gate did nothing at all
  };
  QCHECK(!bvh::InCombatOrLobby());  // no look yet: nothing is combat
  allStock();
  LoadLevels({});
  Look();
  allStock();
  EnterArena();
  QCHECK(!bvh::InCombatOrLobby());
  allStock();
  LoadLevels({"mpl_combat_dyson", "mpl_arena_a"});  // an arena level that is loaded wins over any other still loaded
  Look();
  QCHECK(!bvh::InCombatOrLobby());
  allStock();
  LoadLevels({"mpl_lobby_b"});  // a lobby arena players use is no combat class
  Look();
  QCHECK(!bvh::InCombatOrLobby());
  allStock();
  // the combat map comes back: the answers come back with it, and the change is one line each way
  g_lines.clear();
  EnterCombat();
  QCHECK(bvh::InCombatOrLobby());
  QCHECK(!CallInt(&cbvh, kDown, hit) && bvh::GetCounters().gated == 1);  // the hook acts again: this direct cast is kept off the tree by the gate
  EnterArena();
  QCHECK(Count(Event("combat_bvh_scope")) == 2 && Count("\"from\":0,\"to\":1") == 1 && Count("\"from\":1,\"to\":0") == 1);
  End();
}

void TheScopeIsLookedAtFourTimesASecond() {
  Begin();
  Build();
  QCHECK(Installed(On()));
  LoadLevels({"mpl_combat_dyson"});
  g_now = 100.0;
  bvh::Tick(GameAddress());  // the first look
  QCHECK(bvh::InCombatOrLobby());
  LoadLevels({"mpl_arena_a"});
  g_now = 100.1;
  bvh::Tick(GameAddress());  // inside the quarter second: no look
  QCHECK(bvh::InCombatOrLobby());
  g_now = 100.3;
  bvh::Tick(GameAddress());
  QCHECK(!bvh::InCombatOrLobby());
  End();
}

// The combat lobby is in scope (the plugin answered there too), a lobby arena players use is not, and an arena level that is loaded wins over both.
void TheCombatLobbyIsInScope() {
  Begin();
  Build();
  QCHECK(Installed(Gateless()));
  LoadLevels({"mpl_lobby_b_combat"});
  Look();
  QCHECK(bvh::InCombatOrLobby());
  bt::Resource res = RoomResource(true);
  std::uint64_t cbvh = 0;
  g_cbvhSelf = &cbvh;
  InitSlot()(&cbvh, res.bytes.data());
  std::uint8_t hit[0x44] = {};
  QCHECK(cbvh != 0 && CallInt(&cbvh, kDown, hit));
  LoadLevels({"mpl_lobby_b_combat", "mpl_arena_a"});
  Look();
  QCHECK(!bvh::InCombatOrLobby() && !CallInt(&cbvh, kDown, hit));
  End();
}

// The scope fails closed: a look older than a second is not believed, so a tick that stopped leaves every hook calling the game's function.
void AStalledTickFailsClosed() {
  Begin();
  Build();
  QCHECK(Installed(Gateless()));
  EnterCombat();
  bt::Resource res = RoomResource(true);
  std::uint64_t cbvh = 0;
  g_cbvhSelf = &cbvh;
  InitSlot()(&cbvh, res.bytes.data());
  std::uint8_t hit[0x44] = {};
  const float a[3] = {0, 0, 0}, out[3] = {0, 0, 5.2f};
  QCHECK(bvh::InCombatOrLobby() && CallInt(&cbvh, kDown, hit) && OccludedSlot()(&cbvh, a, out));
  g_now += 1.0;  // exactly a second after the look: still believed
  QCHECK(bvh::InCombatOrLobby());
  g_now += 0.01;  // no Tick since: the look is stale
  QCHECK(!bvh::InCombatOrLobby());
  const int int0 = g_stockInt, occ0 = g_stockOcc, touch0 = g_stockTouch;
  QCHECK(!CallInt(&cbvh, kDown, hit) && !OccludedSlot()(&cbvh, a, out) && TouchSlot()(nullptr, nullptr, nullptr, nullptr));
  QCHECK(g_stockInt == int0 + 1 && g_stockOcc == occ0 + 1 && g_stockTouch == touch0 + 1);  // the game's functions answered
  g_now += 5.0;
  QCHECK(!bvh::InCombatOrLobby());  // and it stays shut for as long as the tick is gone
  QCHECK(Count(Event("combat_bvh_scope_stale")) == 0);  // nothing can say so while the tick is gone
  Look();  // the tick comes back, 7.01 s after its last look, and says the scope was shut for that gap
  QCHECK(bvh::InCombatOrLobby() && CallInt(&cbvh, kDown, hit));
  QCHECK(Count(Event("combat_bvh_scope_stale"), kWarn) == 1);
  QCHECK(Count(std::string("\"gap_s\":\"7.01\",\"stale_after_s\":\"1.00\",\"level\":\"") + LevelHex("mpl_combat_dyson") + "\"") == 1);
  Look();  // a look on time (one second): no line
  QCHECK(Count(Event("combat_bvh_scope_stale")) == 1);
  EnterArena();  // the scope is shut on purpose: a long gap after that closed nothing, so it is not a stall worth a line
  g_now += 10.0;
  Look();
  QCHECK(Count(Event("combat_bvh_scope_stale")) == 1);
  End();
}

// The tree is built only for a combat map or the combat lobby (loaded or loading), or when the game has not been seen; the log names the level.
void ALevelThatIsNotCombatGetsNoTree() {
  Begin();
  Build();
  QCHECK(Installed(Gateless()));
  bt::Resource res = RoomResource(true);
  const auto init = [&](std::uint64_t* word) {
    *word = 0x55;
    g_lines.clear();
    InitSlot()(word, res.bytes.data());
  };
  std::uint64_t cbvh = 0;
  QCHECK(bvh::GetCounters().treesSkipped == 0);
  // an Echo Arena level loaded, a lobby arena players use loaded, the same with a combat map beside an arena level: no tree, no table entry
  for (const char* name : {"mpl_arena_a", "mpl_lobby_b"}) {
    LoadLevels({name});
    Look();
    init(&cbvh);
    QCHECK(cbvh == 0 && bvh::TreeCount() == 0);
    const bool arena = std::strcmp(name, "mpl_arena_a") == 0;
    QCHECK(Count("\"result\":\"skipped\"", kInfo) == 1 && Count(std::string("\"scope\":\"") + (arena ? "arena" : "other") + "\"") == 1);
    // the level is named when the scan classifies it (an Echo Arena level); a lobby arena players use is no class, so it is level 0 with scope "other"
    QCHECK(Count(std::string("\"level\":\"") + (arena ? LevelHex(name) : std::string("0x0000000000000000")) + "\"") == 1);
  }
  LoadLevels({"mpl_combat_dyson", "mpl_arena_a"});
  Look();
  init(&cbvh);
  QCHECK(cbvh == 0 && Count("\"scope\":\"arena\"") == 1);
  QCHECK(bvh::GetCounters().treesSkipped == 3 && bvh::GetCounters().treesBuilt == 0 && g_stockInit == 3);  // the game's Initialize ran each time
  // a combat map loaded, the combat lobby loaded: built, and the level is named
  for (const char* name : {"mpl_combat_dyson", "mpl_lobby_b_combat"}) {
    LoadLevels({name});
    Look();
    init(&cbvh);
    QCHECK(cbvh != 0);
    QCHECK(Count("\"result\":\"built\"", kInfo) == 1 && Count("\"scope\":\"combat\"") == 1 && Count(std::string("\"level\":\"") + LevelHex(name) + "\"") == 1);
  }
  // a combat map still loading beside a lobby arena players use: the resources being built are the combat map's (the scan reads the init fibers)
  LoadLevels({"mpl_lobby_b"});
  AddLoading("mpl_combat_dyson");
  Look();
  init(&cbvh);
  QCHECK(cbvh != 0 && Count("\"scope\":\"combat\"") == 1);
  // an arena level still loading beside a combat map: arena wins
  LoadLevels({"mpl_combat_dyson"});
  AddLoading("mpl_arena_a");
  Look();
  init(&cbvh);
  QCHECK(cbvh == 0 && Count("\"scope\":\"arena\"") == 1);
  End();
}

void AnInitializeThatIsRefusedLeavesNoTreeAndSaysWhy() {
  Begin();
  Build();
  QCHECK(Installed(Gateless()));
  EnterCombat();
  std::uint64_t cbvh = 0x55;
  const std::uint8_t zerosForNullSelf[0x50] = {};
  const auto init = [&](const void* data) { InitSlot()(&cbvh, data); };
  init(nullptr);
  QCHECK(cbvh == 0 && Count("\"result\":\"none\",\"data\":\"0x0000000000000000\",\"why\":\"no data\"", kInfo) == 1);
  InitSlot()(nullptr, zerosForNullSelf);  // a null CBVHTree: nothing to store into, no fault
  QCHECK(bvh::TreeCount() == 0);
  cbvh = 0x55;
  init(reinterpret_cast<const void*>(0x1000));
  QCHECK(cbvh == 0 && Count("\"why\":\"header unreadable\"", kWarn) == 1);
  std::uint8_t zeros[0x50] = {};
  cbvh = 0x55;
  init(zeros);
  QCHECK(cbvh == 0 && Count("\"why\":\"empty\"", kInfo) == 1);
  std::uint8_t bad[0x50] = {};
  const std::uint64_t s0 = 0xe1, s1 = 0x40;
  std::memcpy(bad + 8, &s0, 8);
  std::memcpy(bad + 0x28, &s1, 8);
  cbvh = 0x55;
  init(bad);
  QCHECK(cbvh == 0 && Count("\"why\":\"block sizes\"", kWarn) == 1);
  std::uint8_t farBlocks[0x50] = {};  // a header whose blocks are in unmapped memory
  const std::uint64_t p0 = 0x10000, p1 = 0x20000, e0 = 0xe0;
  std::memcpy(farBlocks + 0, &p0, 8);
  std::memcpy(farBlocks + 8, &e0, 8);
  std::memcpy(farBlocks + 0x20, &p1, 8);
  std::memcpy(farBlocks + 0x28, &s1, 8);
  cbvh = 0x55;
  init(farBlocks);
  QCHECK(cbvh == 0 && Count("\"why\":\"block unreadable\"", kWarn) == 1);
  // a block whose FIRST page reads but whose later pages do not: the whole block is probed
  void* two = mmap(nullptr, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  munmap(static_cast<std::uint8_t*>(two) + 4096, 4096);
  std::uint8_t half[0x50] = {};
  const std::uint64_t pp = reinterpret_cast<std::uintptr_t>(two) + 4096 - 0xe0, big = 4 * 0xe0;  // 2 records straddle into the unmapped page
  std::memcpy(half + 0, &pp, 8);
  std::memcpy(half + 8, &big, 8);
  const std::uint64_t pn = reinterpret_cast<std::uintptr_t>(two);
  std::memcpy(half + 0x20, &pn, 8);
  std::memcpy(half + 0x28, &s1, 8);
  cbvh = 0x55;
  g_lines.clear();
  init(half);
  QCHECK(cbvh == 0 && Count("\"why\":\"block unreadable\"", kWarn) == 1);
  // the same for the node block
  std::uint8_t halfNodes[0x50] = {};
  const std::uint64_t oneTri = 0xe0, twoNodes = 2 * 0x40;
  const std::uint64_t pnn = reinterpret_cast<std::uintptr_t>(two) + 4096 - 0x40;  // the second node is in the unmapped page
  std::memcpy(halfNodes + 0, &pn, 8);
  std::memcpy(halfNodes + 8, &oneTri, 8);
  std::memcpy(halfNodes + 0x20, &pnn, 8);
  std::memcpy(halfNodes + 0x28, &twoNodes, 8);
  cbvh = 0x55;
  g_lines.clear();
  init(halfNodes);
  QCHECK(cbvh == 0 && Count("\"why\":\"block unreadable\"", kWarn) == 1);
  munmap(two, 4096);
  QCHECK(bvh::GetCounters().treesRefused == 7 && bvh::GetCounters().treesBuilt == 0);
  QCHECK(bvh::TreeCount() == 0);  // a refused tree took no table entry
  End();
}

// A Shutdown frees the entry its CBVHTree owns: more than 64 trees can be built and shut down in one process, one at a time (#2307).
void AShutDownTreeFreesItsEntry() {
  Begin();
  Build();
  QCHECK(Installed(Gateless()));
  EnterCombat();
  bt::Resource res = RoomResource(true, 8);
  std::uint64_t cb = 0;
  bool built = true, freed = true;
  for (int i = 0; i < 200; ++i) {  // 200 level loads: three times the table
    cb = 0;
    InitSlot()(&cb, res.bytes.data());
    built &= cb != 0 && bvh::TreeCount() == 1;
    ShutSlot()(&cb);
    freed &= cb == 0 && bvh::TreeCount() == 0;
  }
  QCHECK(built && freed);
  QCHECK(Count("\"why\":\"tree table full\"") == 0);
  QCHECK(Count("\"result\":\"built\"", kInfo) == 200);
  End();
}

// The table is full only while 64 CBVHTrees hold an entry; a shut-down tree's entry is the next one built.
void TheTableHolds64LiveTreesAndAShutdownMakesRoom() {
  Begin();
  Build();
  QCHECK(Installed(Gateless()));
  EnterCombat();
  std::vector<bt::Resource> res;
  for (int i = 0; i < 66; ++i) res.push_back(RoomResource(true, 8));
  std::vector<std::uint64_t> cb(66, 0);
  for (int i = 0; i < 64; ++i) InitSlot()(&cb[i], res[i].bytes.data());
  QCHECK(bvh::TreeCount() == 64);
  bool all = true;
  for (int i = 0; i < 64; ++i) all &= cb[i] != 0;
  QCHECK(all);
  InitSlot()(&cb[64], res[64].bytes.data());
  QCHECK(cb[64] == 0 && Count("\"why\":\"tree table full\"", kError) == 1 && bvh::TreeCount() == 64);
  std::uint64_t again = 0;
  InitSlot()(&again, res[3].bytes.data());  // the same data under a CBVHTree that is not cb[3]: no room, whatever the data
  QCHECK(again == 0 && Count("\"why\":\"tree table full\"", kError) == 2);
  // a CBVHTree initialized again without a Shutdown gives back its own entry first, so it is built even with the table full
  InitSlot()(&cb[3], res[3].bytes.data());
  QCHECK(cb[3] != 0 && bvh::TreeCount() == 64 && Count("\"cause\":\"reinitialize\"", kInfo) == 1);
  // a Shutdown makes room for exactly one more
  ShutSlot()(&cb[10]);
  QCHECK(cb[10] == 0 && bvh::TreeCount() == 63);
  InitSlot()(&cb[64], res[64].bytes.data());
  QCHECK(cb[64] != 0 && bvh::TreeCount() == 64);
  InitSlot()(&cb[65], res[65].bytes.data());
  QCHECK(cb[65] == 0 && bvh::TreeCount() == 64 && Count("\"why\":\"tree table full\"", kError) == 3);
  // every live tree still answers
  std::uint8_t hit[0x44] = {};
  bool answers = true;
  for (int i : {0, 3, 9, 11, 63, 64}) answers &= CallInt(&cb[i], kDown, hit);
  QCHECK(answers);
  // a CBVHTree word that is the right distance from the table but past its last entry is not a tree
  const std::uint64_t stride = cb[63] - cb[62];
  std::uint64_t past = cb[63] + stride;
  const int stockBefore = g_stockInt;
  std::uint8_t hitP[0x44] = {};
  QCHECK(!CallInt(&past, kDown, hitP) && g_stockInt == stockBefore + 1);
  std::uint64_t below = cb[0] - stride;
  QCHECK(!CallInt(&below, kDown, hitP) && g_stockInt == stockBefore + 2);
  std::uint64_t last = cb[63];
  QCHECK(CallInt(&last, kDown, hitP) && g_stockInt == stockBefore + 2);
  End();
}

// The one free entry may be the last the search reaches: with the table full, shutting down the tree built last leaves its entry just behind the cursor.
void TheLastEntryOfTheSearchIsUsed() {
  Begin();
  Build();
  QCHECK(Installed(Gateless()));
  EnterCombat();
  bt::Resource res = RoomResource(true, 8);
  std::vector<std::uint64_t> cb(65, 0);
  for (int i = 0; i < 64; ++i) InitSlot()(&cb[i], res.bytes.data());
  QCHECK(bvh::TreeCount() == 64);
  ShutSlot()(&cb[63]);
  InitSlot()(&cb[64], res.bytes.data());
  QCHECK(cb[64] != 0 && bvh::TreeCount() == 64 && Count("\"why\":\"tree table full\"") == 0);
  End();
}

// Two CBVHTrees built from the same data own an entry each; a Shutdown frees its own and leaves the other answering.
void ASharedResourceKeepsTheOtherTreesEntry() {
  Begin();
  Build();
  QCHECK(Installed(Gateless()));
  EnterCombat();
  bt::Resource res = RoomResource(true);
  std::uint64_t a = 0, b = 0;
  InitSlot()(&a, res.bytes.data());
  InitSlot()(&b, res.bytes.data());
  QCHECK(a != 0 && b != 0 && a != b && bvh::TreeCount() == 2);
  char named[160];  // the line names the CBVHTree (its address) and the data it was built from
  std::snprintf(named, sizeof(named), "\"event\":\"combat_bvh_tree_freed\",\"tree\":\"0x%016llx\",\"data\":\"0x%016llx\"",
                static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(&a)),
                static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(res.bytes.data())));
  ShutSlot()(&a);
  QCHECK(a == 0 && bvh::TreeCount() == 1);
  std::uint8_t hit[0x44] = {};
  QCHECK(CallInt(&b, kDown, hit));
  QCHECK(Count(Event("combat_bvh_tree_freed"), kInfo) == 1 && Count("\"cause\":\"shutdown\",\"in_use\":1") == 1);
  QCHECK(Count(named) == 1 && bvh::GetCounters().treesFreed == 1);
  End();
}

// A Shutdown of a CBVHTree that owns no entry (an arena level's, a refused one, one never initialized) frees nothing and writes nothing.
void AShutdownOfATreeWithoutAnEntryFreesNothing() {
  Begin();
  Build();
  QCHECK(Installed(Gateless()));
  EnterCombat();
  bt::Resource res = RoomResource(true);
  std::uint64_t live = 0;
  InitSlot()(&live, res.bytes.data());
  QCHECK(bvh::TreeCount() == 1);
  std::uint64_t garbage = live;  // a word that names the live tree's entry, in an object that never built one
  ShutSlot()(&garbage);
  QCHECK(garbage == 0 && bvh::TreeCount() == 1);
  std::uint64_t never = 0x1234567;
  ShutSlot()(&never);
  QCHECK(never == 0 && bvh::TreeCount() == 1 && g_stockShut == 2);
  QCHECK(Count(Event("combat_bvh_tree_freed")) == 0 && bvh::GetCounters().treesFreed == 0);
  std::uint8_t hit[0x44] = {};
  QCHECK(CallInt(&live, kDown, hit));  // the live tree still answers
  ShutSlot()(nullptr);                 // a null object: the game's function ran, nothing else
  QCHECK(g_stockShut == 3 && bvh::TreeCount() == 1);
  End();
}

// A cast on a CBVHTree after its Shutdown: the word is cleared, so the game's function answers; a copy of the old word names a freed entry, so it does too;
// and with free entries ahead of the cursor the next build takes one of those, not the freed entry, so the copy does not name the next tree's entry here either
// (when the freed entry is the only free one it is reused at once: TheLastEntryOfTheSearchIsUsed).
void ACastOnAShutDownTreeIsTheGamesOwn() {
  Begin();
  Build();
  QCHECK(Installed(Gateless()));
  EnterCombat();
  bt::Resource res = RoomResource(true);
  std::uint64_t a = 0;
  InitSlot()(&a, res.bytes.data());
  std::uint64_t stale = a;  // a copy of the word, as a caller that cached it would hold
  std::uint8_t hit[0x44] = {};
  QCHECK(CallInt(&stale, kDown, hit) && g_stockInt == 0);
  ShutSlot()(&a);
  QCHECK(a == 0);
  QCHECK(!CallInt(&a, kDown, hit) && g_stockInt == 1);          // the cleared word
  QCHECK(!CallInt(&stale, kDown, hit) && g_stockInt == 2);      // the freed entry
  QCHECK(!OccludedSlot()(&stale, kDown, kDown + 3) && g_stockOcc == 1);
  QCHECK(bvh::GetCounters().intersect == 1 && bvh::GetCounters().stock == 3);
  std::uint64_t b = 0;
  InitSlot()(&b, res.bytes.data());  // the next tree takes the next entry, not the freed one
  QCHECK(b != 0 && b != stale);
  QCHECK(!CallInt(&stale, kDown, hit) && g_stockInt == 3);
  QCHECK(CallInt(&b, kDown, hit));
  End();
}

// Casts on a live tree go on while other trees are built and shut down, entries being reused around it (the table's reads take no lock).
void ALiveTreeAnswersWhileOthersAreBuiltAndShutDown() {
  Begin();
  Build();
  QCHECK(Installed(Gateless()));
  EnterCombat();
  bt::Resource live = RoomResource(true);
  bt::Resource churn = RoomResource(true, 8);
  std::uint64_t a = 0;
  InitSlot()(&a, live.bytes.data());
  std::atomic<bool> stop{false};
  std::atomic<int> wrong{0}, casts{0};
  std::thread reader([&] {
    std::uint8_t h[0x44];
    while (!stop.load()) {
      std::memset(h, 0, sizeof(h));
      std::int32_t id0 = -1;
      const bool got = CallInt(&a, kDown, h);
      std::memcpy(&id0, h, 4);
      if (!got || id0 != 102) wrong.fetch_add(1);
      casts.fetch_add(1);
    }
  });
  for (int i = 0; i < 300; ++i) {
    std::uint64_t b = 0;
    InitSlot()(&b, churn.bytes.data());
    ShutSlot()(&b);
  }
  stop.store(true);
  reader.join();
  QCHECK(wrong.load() == 0 && casts.load() > 0);
  QCHECK(bvh::TreeCount() == 1 && Count("\"why\":\"tree table full\"") == 0);
  End();
}

void TheCallersGateAnswersTheListedCastsOnly() {
  std::uint64_t allowedAbs = 0, otherRet = 0;  // absolute: the image base differs from one world to the next
  {  // pass 1: no allowed casts; learn the return address of CallGateAllowedSite
    Begin();
    Build();
    QCHECK(Installed(On()));
    EnterCombat();
    std::uint64_t cbvh = 0;
    bt::Resource res = RoomResource(true);
    InitSlot()(&cbvh, res.bytes.data());
    g_cbvhSelf = &cbvh;
    std::uint8_t hit[0x44] = {};
    void* gs = nullptr;
    const bool r = CallGateAllowedSite(reinterpret_cast<const void*>(0x77), kDown, hit, &gs);
    QCHECK(!r && gs == nullptr);  // a cast from an unlisted call site did not hit the tree
    QCHECK(g_stockGate == 1 && g_stockGateRanWithArgs);
    QCHECK(g_stockInt == 1 && bvh::GetCounters().gated == 1 && bvh::GetCounters().intersect == 0);
    allowedAbs = bvh::LastGateReturnForTest() + g_image.Base();
    QCHECK(allowedAbs != g_image.Base());
    CallGateOtherSite(reinterpret_cast<const void*>(0x77), kDown, hit, &gs);
    otherRet = bvh::LastGateReturnForTest() + g_image.Base();
    QCHECK(otherRet != allowedAbs);
    QCHECK(!CallInt(&cbvh, kDown, hit));  // a direct cast (not through the gate: LocalStaticRaycast) is also kept off the tree
    End();
  }
  {
    Begin();
    Build();
    g_pins.allowed[0] = allowedAbs - g_image.Base();
    g_pins.allowedCount = 1;
    QCHECK(Installed(On()));
    EnterCombat();
    std::uint64_t cbvh = 0;
    bt::Resource res = RoomResource(true);
    InitSlot()(&cbvh, res.bytes.data());
    g_cbvhSelf = &cbvh;
    std::uint8_t hit[0x44] = {};
    void* gs = nullptr;
    // the listed call site: the tree answers; the other call site (same slot, same args): the stock miss
    QCHECK(CallGateAllowedSite(reinterpret_cast<const void*>(0x77), kDown, hit, &gs) && gs == reinterpret_cast<void*>(0x1234));
    float t;
    std::memcpy(&t, hit + 0x10, 4);
    QCHECK(t > 4.99f && t < 5.01f);
    std::memset(hit, 0, sizeof(hit));
    gs = nullptr;
    QCHECK(!CallGateOtherSite(reinterpret_cast<const void*>(0x77), kDown, hit, &gs));
    QCHECK(!CallInt(&cbvh, kDown, hit));  // the mark is restored when the allowed cast returns
    // nested: an unlisted cast inside an allowed one is gated, and the allowed one still is allowed afterwards
    g_nested = true;
    g_nestedResult = -1;
    std::memset(hit, 0, sizeof(hit));
    QCHECK(CallGateAllowedSite(reinterpret_cast<const void*>(0x77), kDown, hit, &gs));
    QCHECK(g_nestedResult == 0);  // the unlisted cast inside the allowed one was gated
    QCHECK(!CallInt(&cbvh, kDown, hit));  // and the mark did not leak after the nested casts
    // another thread casting directly during an allowed cast is gated: the mark is per thread
    g_spawn = true;
    QCHECK(CallGateAllowedSite(reinterpret_cast<const void*>(0x77), kDown, hit, &gs));
    QCHECK(g_spawnResult == 0);
    // Occluded is not gated
    const float a[3] = {0, 0, 0}, out[3] = {0, 0, 5.2f};
    QCHECK(OccludedSlot()(&cbvh, a, out));
    QCHECK(Count("\"allowed_casts\":1") == 1);
    End();
  }
  {  // two allowed call sites: the second entry of the list counts as much as the first
    Begin();
    Build();
    g_pins.allowed[0] = allowedAbs - g_image.Base() + 0x1000;
    g_pins.allowed[1] = allowedAbs - g_image.Base();
    g_pins.allowedCount = 2;
    QCHECK(Installed(On()));
    EnterCombat();
    std::uint64_t cbvh = 0;
    bt::Resource res = RoomResource(true);
    InitSlot()(&cbvh, res.bytes.data());
    g_cbvhSelf = &cbvh;
    std::uint8_t hit[0x44] = {};
    void* gs = nullptr;
    QCHECK(CallGateAllowedSite(reinterpret_cast<const void*>(0x77), kDown, hit, &gs));
    QCHECK(!CallGateOtherSite(reinterpret_cast<const void*>(0x77), kDown, hit, &gs));
    // after the module's state is reset (the slots stay swapped) the thunks have no original: they answer a miss and do not fault
    bvh::ResetForTest();
    QCHECK(!CallGateAllowedSite(reinterpret_cast<const void*>(0x77), kDown, hit, &gs));
    QCHECK(!CallInt(&cbvh, kDown, hit));
    const float a[3] = {0, 0, 0}, out[3] = {0, 0, 5.2f};
    QCHECK(!OccludedSlot()(&cbvh, a, out));
    End();
  }
}

// All or none: a slot that does not hold the pinned function refuses the module, and what was installed before it is put back.
void ASlotThatDoesNotHoldItsPinnedFunctionRefusesAllOfIt() {
  struct Case {
    const char* name;
    bvh::Slot bvh::Pins::*slot;
  };
  const Case cases[] = {{"CBVHTree::Intersect", &bvh::Pins::intersect},
                        {"CBVHTree::Occluded", &bvh::Pins::occluded},
                        {"CBVHTree::Initialize", &bvh::Pins::init},
                        {"CBVHTree::Shutdown", &bvh::Pins::shutdown},
                        {"CncaGameSpace::StaticRaycast", &bvh::Pins::gate}};
  for (const Case& c : cases) {
    Begin();
    Build();
    const bvh::Slot* slots[6] = {&g_pins.init, &g_pins.shutdown, &g_pins.intersect, &g_pins.occluded, &g_pins.touch, &g_pins.gate};
    *g_image.Slot((g_pins.*c.slot).slot) = reinterpret_cast<void*>(0x4242420);
    std::uintptr_t before[6];
    for (int i = 0; i < 6; ++i) before[i] = SlotWord(slots[i]->slot);
    const nevr_quest::combat::ModuleResult r = bvh::Install(On());
    QCHECK(!r.installed && std::strcmp(r.status, "original_mismatch") == 0 && !bvh::TreeInstalled());
    for (int i = 0; i < 6; ++i) QCHECK(SlotWord(slots[i]->slot) == before[i]);  // nothing stays swapped
    QCHECK(Count(Event("combat_bvh_install"), kError) == 1 && Count(std::string("\"hook\":\"") + c.name + "\"") == 1);
    QCHECK(Count("\"status\":\"original_mismatch\"") >= 1);
    End();
  }
  {  // the gate's slot does not matter when the gate is off
    Begin();
    Build();
    *g_image.Slot(g_pins.gate.slot) = reinterpret_cast<void*>(0x4242420);
    QCHECK(Installed(Gateless()));
    End();
  }
  {  // a slot outside the writable segments cannot be hooked: what was hooked before it is put back
    Begin();
    Build();
    g_image.Drop();
    g_pins.occluded.slot = 0x2000;  // in the executable segment
    g_image.Build({{bvh::kInitSymbol, g_pins.init.slot, false, false, 0},
                   {bvh::kShutSymbol, g_pins.shutdown.slot, false, false, 0},
                   {bvh::kIntersectSymbol, g_pins.intersect.slot, false, false, 0},
                   {bvh::kOccludedSymbol, g_pins.occluded.slot, false, false, 0},
                   {bvh::kTouchSymbol, g_pins.touch.slot, false, false, 0},
                   {bvh::kGateSymbol, g_pins.gate.slot, false, false, 0}});
    const std::uintptr_t base = g_image.Base();
    *g_image.Slot(g_pins.init.slot) = reinterpret_cast<void*>(&StockInit);
    *g_image.Slot(g_pins.shutdown.slot) = reinterpret_cast<void*>(&StockShut);
    *g_image.Slot(g_pins.intersect.slot) = reinterpret_cast<void*>(&StockInt);
    *g_image.Slot(g_pins.touch.slot) = reinterpret_cast<void*>(&StockTouch);
    *g_image.Slot(g_pins.gate.slot) = reinterpret_cast<void*>(&StockGate);
    g_pins.init.fn = reinterpret_cast<std::uintptr_t>(&StockInit) - base;
    g_pins.shutdown.fn = reinterpret_cast<std::uintptr_t>(&StockShut) - base;
    g_pins.intersect.fn = reinterpret_cast<std::uintptr_t>(&StockInt) - base;
    g_pins.occluded.fn = reinterpret_cast<std::uintptr_t>(&StockOcc) - base;
    g_pins.touch.fn = reinterpret_cast<std::uintptr_t>(&StockTouch) - base;
    g_pins.gate.fn = reinterpret_cast<std::uintptr_t>(&StockGate) - base;
    bvh::SetImageLookupForTest(&Lookup);
    bvh::SetPinsForTest(&g_pins);
    const nevr_quest::combat::ModuleResult r = bvh::Install(On());
    QCHECK(!r.installed && !bvh::TreeInstalled());
    QCHECK(SlotWord(g_pins.init.slot) == reinterpret_cast<std::uintptr_t>(&StockInit));
    QCHECK(SlotWord(g_pins.shutdown.slot) == reinterpret_cast<std::uintptr_t>(&StockShut));
    QCHECK(SlotWord(g_pins.intersect.slot) == reinterpret_cast<std::uintptr_t>(&StockInt));
    QCHECK(SlotWord(g_pins.gate.slot) == reinterpret_cast<std::uintptr_t>(&StockGate));
    QCHECK(bvh::TreeCount() == 0);
    End();
  }
}

void ARefusedStepTouchesNothingAndSaysWhy() {
  {  // libr15 not loaded
    Begin();
    Build();
    g_image.Drop();
    const nevr_quest::combat::ModuleResult r = bvh::Install(On());
    QCHECK(!r.installed && std::strcmp(r.status, "module_not_loaded") == 0);
    QCHECK(Count("\"reason\":\"module_not_loaded\"", kError) == 1);
    End();
  }
  {  // a libr15 that is not the pinned build
    Begin();
    Build("0000000000000000000000000000000000000000");
    const nevr_quest::combat::ModuleResult r = bvh::Install(On());
    QCHECK(!r.installed && std::strcmp(r.status, "build_id_mismatch") == 0);
    QCHECK(Count(Event("combat_bvh_install"), kError) == 1);
    End();
  }
  {  // no room in the tick table: the scope would never turn on, so nothing is installed
    Begin();
    Build();
    for (std::size_t i = 0; i < nevr_quest::combat::tick::kMaxCallbacks; ++i) {
      QCHECK(nevr_quest::combat::tick::Add(+[](std::uintptr_t) noexcept {}));
    }
    const std::uintptr_t before = SlotWord(g_pins.intersect.slot);
    const nevr_quest::combat::ModuleResult r = bvh::Install(On());
    QCHECK(!r.installed && std::strcmp(r.status, "tick_table_full") == 0 && SlotWord(g_pins.intersect.slot) == before);
    End();
  }
}

void TheSummaryIsOneLineEveryTenSecondsWhenACounterMoved() {
  Begin();
  Build();
  QCHECK(Installed(Gateless()));
  EnterCombat();  // g_now is 101 and the next summary is due at 111
  bt::Resource res = RoomResource(true);
  std::uint64_t cbvh = 0;
  InitSlot()(&cbvh, res.bytes.data());
  std::uint8_t hit[0x44] = {};
  CallInt(&cbvh, kDown, hit);
  const float upOut[6] = {0, 20, 0, 0, 1, 0};
  CallInt(&cbvh, upOut, hit);  // a miss: two casts, one hit
  g_now = 105.0;
  bvh::Tick(GameAddress());
  QCHECK(Count(Event("combat_bvh_summary")) == 0);  // inside the window
  g_now = 112.0;
  bvh::Tick(GameAddress());
  QCHECK(Count(Event("combat_bvh_summary"), kInfo) == 1 && Count("\"intersect\":2,\"intersect_hit\":1") == 1);
  QCHECK(Count("\"in_use\":1,") == 1);  // the one live tree holds one entry
  g_now = 125.0;
  bvh::Tick(GameAddress());
  QCHECK(Count(Event("combat_bvh_summary")) == 1);  // nothing moved: no line
  CallInt(&cbvh, kDown, hit);
  g_now = 130.0;
  bvh::Tick(GameAddress());
  QCHECK(Count(Event("combat_bvh_summary")) == 1);  // a second summary inside the 10 s window
  g_now = 140.0;
  bvh::Tick(GameAddress());
  QCHECK(Count(Event("combat_bvh_summary")) == 2);
  End();
}

}  // namespace

int main() {
  QCHECK(!bvh::InCombatOrLobby());  // before anything: no combat map is known, so the hooks would call the game's functions
  // the cached answer itself starts false (not only the guarded accessor, which the stale look also shuts): the first look in a level that is not
  // combat then changes nothing and writes no `combat_bvh_scope` line
  QCHECK(!bvh::detail::g_inCombatOrLobby.load());
  char tmpl[] = "/tmp/combat-bvh-XXXXXX";
  if (mkdtemp(tmpl) == nullptr) return 2;
  g_dir = tmpl;
  ThePinsAreTheReadelfPins();
  NoThreadKeyRefusesAndChangesNothing();
  TheInstallSwapsTheSlotsAndStatesItOnce();
  SwitchesDecideWhatIsInstalled();
  ARefusedTouchHookDoesNotStopTheTree();
  ALegacySwitchFileIsAWarningAndChangesNothing();
  ACombatMapGetsTheTreesAnswers();
  WithoutACombatMapEveryHookCallsTheGamesFunction();
  TheScopeIsLookedAtFourTimesASecond();
  TheCombatLobbyIsInScope();
  AStalledTickFailsClosed();
  ALevelThatIsNotCombatGetsNoTree();
  AnInitializeThatIsRefusedLeavesNoTreeAndSaysWhy();
  AShutDownTreeFreesItsEntry();
  TheTableHolds64LiveTreesAndAShutdownMakesRoom();
  TheLastEntryOfTheSearchIsUsed();
  ASharedResourceKeepsTheOtherTreesEntry();
  AShutdownOfATreeWithoutAnEntryFreesNothing();
  ACastOnAShutDownTreeIsTheGamesOwn();
  ALiveTreeAnswersWhileOthersAreBuiltAndShutDown();
  TheCallersGateAnswersTheListedCastsOnly();
  ASlotThatDoesNotHoldItsPinnedFunctionRefusesAllOfIt();
  ARefusedStepTouchesNothingAndSaysWhy();
  TheSummaryIsOneLineEveryTenSecondsWhenACounterMoved();
  rmdir(g_dir.c_str());
  if (quest_test::Failures() == 0) std::printf("bvh_test: all checks pass\n");
  return quest_test::Failures() == 0 ? 0 : 1;
}
