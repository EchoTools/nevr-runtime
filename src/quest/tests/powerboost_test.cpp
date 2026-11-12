// Host test of POWER BOOST (src/quest/combat/powerboost.cpp + src/runtime/patch/powerboost_rules.h, rows R51..R56, #2187): the real thunks and the real entry,
// installed by the real GotHook into the five words of a libr15 image built in memory (combat_libr15_image.h), and called through the slots;
// the engine functions are fakes that record their arguments. The arm64 entry cannot run on this x86 host (src/quest/combat/check/powerboost.sh
// runs it under qemu); its continuation (ConeAt) is tested directly here.
//
// Moved from the q15t plugin's powerboost_test: the assertions on the marker decode, the three stages, the hold, the cone and the link / unlink
// markers are the same. What changed is the fixture (the plugin's fake slot pages are a libr15 image and the manager's install), the log form
// (structured `combat_powerboost_*` records, matched by event and field, where the plugin's lines were matched by their text), the install's
// statuses (GotStatus names and the module's tokens, `ok` for the plugin's `kOk` and `original_mismatch` for its `kUnexpectedOriginal`), and
// the stage-3 refusal for no net tick (the EC client's registration refuses, where the plugin was told `net_tick == false`). New here: the arena
// scope (nothing acts while an Echo Arena level is loaded, or before a game tick saw a level), the scope's cadence, a full tick table, the
// stage numbers from the config's integer, and the slots held by the module alone.
//
// Built and run by `just test-quest-combat powerboost`.
#include "quest/combat/powerboost.h"

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "nevr/symbol_hash.h"
#include "quest/combat/combat_level.h"
#include "quest/combat/combat_scope.h"
#include "quest/combat/combat_tick.h"
#include "quest/combat/safe_read.h"
#include "quest/sentinel/hook_install.h"
#include "quest/tests/combat_libr15_image.h"
#include "quest/tests/combat_scope_world.h"
#include "quest/tests/key_hog.h"
#include "quest/tests/test_check.h"
#include "runtime/patch/powerboost_rules.h"

// The arena path's read count, through safe_read.h's test seam: every SafeRead after StartReadCount adds one.
namespace {
std::atomic<std::uint64_t> g_arenaReads{0};
[[maybe_unused]] void StartReadCount() {
  g_arenaReads.store(0);
  nevr_quest::combat::SetReadCounterForTest(&g_arenaReads);
}
[[maybe_unused]] std::uint64_t ReadCount() { return g_arenaReads.load(); }
// The arena path's level scans, through combat_level.h's process count: ScanCount is the scans since StartScanCount.
std::uint64_t g_scanBase = 0;
[[maybe_unused]] void StartScanCount() { g_scanBase = nevr_quest::combat::level::ScanCalls(); }
[[maybe_unused]] std::uint64_t ScanCount() { return nevr_quest::combat::level::ScanCalls() - g_scanBase; }
}  // namespace

namespace {

namespace pb = nevr_quest::combat::powerboost;
namespace cb = nevr_quest::combat;
namespace level = nevr_quest::combat::level;
using pb::PhTarget;
using combat_test::Image;
using EchoVR::CSymbol64Hash;

int g_fail = 0;
#define CHECK(cond, ...)                               \
  do {                                                 \
    if (!(cond)) {                                     \
      ++g_fail;                                        \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      std::printf(__VA_ARGS__);                        \
      std::printf("\n");                               \
    }                                                  \
  } while (0)

struct Line {
  sentinel::LogLevel level;
  std::string text;
};
std::vector<Line> g_lines;
void Capture(sentinel::LogLevel level, const char* line) {
  if (std::getenv("PB_LOG")) std::printf("LOG %s\n", line);  // PB_LOG=1: every record as it is emitted
  g_lines.push_back({level, line});
}
// Lines holding every needle (an event name is matched as "event":"<name>").
int Count(std::initializer_list<const char*> needles, int level = -1) {
  int n = 0;
  for (const Line& l : g_lines) {
    bool all = level < 0 || static_cast<int>(l.level) == level;
    for (const char* needle : needles) all = all && l.text.find(needle) != std::string::npos;
    n += all ? 1 : 0;
  }
  return n;
}
std::string Ev(const char* event) { return std::string("\"event\":\"combat_powerboost_") + event + "\""; }
int Events(const char* event, int level = -1) {
  const std::string e = Ev(event);
  return Count({e.c_str()}, level);
}
int EventsWith(const char* event, const char* field, int level = -1) {
  const std::string e = Ev(event);
  return Count({e.c_str(), field}, level);
}
constexpr int kInfo = static_cast<int>(sentinel::LogLevel::kInfo);
constexpr int kError = static_cast<int>(sentinel::LogLevel::kError);

// ---- fakes
struct HealCall {
  void* cs;
  uint64_t handle;
  float amount;
  uint64_t user;
};
struct SendCall {
  void* cs;
  uint64_t handle;
  float amount;
  uint32_t hit;
  uint64_t sym;
};
std::vector<HealCall> g_heals;
std::vector<SendCall> g_sends;
int g_node_calls = 0, g_link_calls = 0;
const void *g_node_arg = nullptr, *g_node_data = nullptr;
const void *g_link_cs = nullptr, *g_link_target = nullptr, *g_link_pos = nullptr;
uint64_t g_link_bubble = 0, g_link_argv = 0;
bool g_node_heals = false;        // the fake node Enter takes its direct path: it calls Heal through the slot
bool g_node_other_thread = false;  // the fake node Enter, while it runs, has another thread relay a marker through the Heal slot

Image g_image;
pb::Pins g_pins = pb::kPins;

using HealFn = void (*)(void*, uint64_t, float, uint64_t);
using NodeFn = void (*)(void*, const uint8_t*);
using LinkFn = void (*)(void*, uint64_t, const void*, const void*, uint64_t);
using ResolveFn = const uint8_t* (*)(const PhTarget*);
NodeFn NodeSlot() { return reinterpret_cast<NodeFn>(*g_image.Slot(g_pins.nodeSlot)); }
HealFn HealSlot() { return reinterpret_cast<HealFn>(*g_image.Slot(g_pins.healSlot)); }
HealFn QueuedSlot() { return reinterpret_cast<HealFn>(*g_image.Slot(g_pins.healGlobDat)); }
LinkFn LinkSlot() { return reinterpret_cast<LinkFn>(*g_image.Slot(g_pins.linkSlot)); }
void* SlotWord(std::uint64_t slot) { return *g_image.Slot(slot); }

extern "C" __attribute__((noinline)) void FakeHeal(void* cs, uint64_t handle, float amount, uint64_t user) {
  g_heals.push_back({cs, handle, amount, user});
}
extern "C" __attribute__((noinline)) void FakeNode(void* node, const uint8_t* data) {
  ++g_node_calls;
  g_node_arg = node;
  g_node_data = data;
  if (g_node_other_thread) {
    g_node_other_thread = false;
    std::thread([] { HealSlot()(reinterpret_cast<void*>(0xc0), 0x10001, 1250030.0f, 5); }).join();
  }
  if (g_node_heals && data != nullptr) {
    float amount;
    uint32_t user;
    std::memcpy(&amount, data + 0x20, 4);
    std::memcpy(&user, data + 0x24, 4);
    HealSlot()(reinterpret_cast<void*>(0xc0), 0x10007, amount, user);
  }
}
extern "C" __attribute__((noinline)) void FakeLink(void* cs, uint64_t bubble, const void* target, const void* pos, uint64_t arg) {
  ++g_link_calls;
  g_link_cs = cs;
  g_link_bubble = bubble;
  g_link_target = target;
  g_link_pos = pos;
  g_link_argv = arg;
}
extern "C" __attribute__((noinline)) void FakeSend(void* cs, uint64_t handle, float amount, uint32_t hit, uint64_t sym) {
  g_sends.push_back({cs, handle, amount, hit, sym});
}
extern "C" __attribute__((noinline)) void ForeignFn() {}  // a function that is not the pinned one: a word somebody else put in a slot

// cone world: bodies by key, the owner's key is 100
alignas(16) uint8_t g_body_a[0xf40], g_body_b[0xf40], g_body_owner[0xf40];
alignas(16) uint8_t g_frame[0x100], g_rec[0x60], g_phys[32];
uint64_t g_primary_physics = 0;
uint32_t g_primary_handle = 0;
int g_resolve_calls = 0;
extern "C" __attribute__((noinline)) const uint8_t* FakeResolve(const PhTarget* t) {
  ++g_resolve_calls;
  switch (t->w[0]) {
    case 1: return g_body_a;
    case 2: return g_body_b;
    case 100: return g_body_owner;
    default: return nullptr;
  }
}
uint64_t g_owner_key = 100;
extern "C" __attribute__((noinline)) PhTarget FakePrimary(const void* physics, uint32_t handle) {
  g_primary_physics = reinterpret_cast<uint64_t>(physics);
  g_primary_handle = handle;
  return PhTarget{{g_owner_key, 0, 0}};
}

uint64_t g_now = 1000;
uint64_t Clock() { return g_now; }
// The combat level answer's own clock (combat_scope.h), in ms: each test frame moves it one look spacing, so each frame's refresh looks at the game.
std::int64_t g_scopeMs = 1000;
// Armed (g_armedReads >= 0): the first clock read is the record's own (LastLook's staleness test), every later read sees the look older than
// kStaleAfterMs. A decision made from a second read of the record, or from a live read, sees the answer stale (the block headed "the one look" in main()).
int g_armedReads = -1;
std::int64_t ScopeClock() noexcept {
  if (g_armedReads >= 0 && g_armedReads++ >= 1) return g_scopeMs + nevr_quest::combat::scope::kStaleAfterMs + 1;
  return g_scopeMs;
}

pb::Fns Fns() {
  pb::Fns f{};
  f.send = &FakeSend;
  f.resolve = &FakeResolve;
  f.primary = &FakePrimary;
  return f;
}

bool g_registrar_ok = true;
int g_registered = 0;
bool Registrar(void (*tick)() noexcept) noexcept {
  if (!g_registrar_ok || tick == nullptr) return false;
  ++g_registered;
  return true;
}

void SetPos(uint8_t* body, float x, float y, float z) {
  const float p[3] = {x, y, z};
  std::memcpy(body + 0x8f8, p, 12);
}
void SetQuat(uint8_t* body, float x, float y, float z, float w) {
  const float q[4] = {x, y, z, w};
  std::memcpy(body + 0xbd0, q, 16);
}

bool Lookup(const char* module, sentinel::ElfImage* out) {
  if (!g_image.Present() || std::strcmp(module, sentinel::pinned::kLibR15) != 0) return false;
  *out = g_image.Elf();
  return true;
}

// The image: every relocation at its pin, every slot holding its (fake) function, the sender's slot and the cone's call word.
void Build() {
  g_pins = pb::kPins;
  g_image.Build({{pb::kNodeSymbol, g_pins.nodeSlot, false, false, g_pins.nodeFn},
                 {pb::kHealSymbol, g_pins.healSlot, false, false, g_pins.healFn},
                 {pb::kHealSymbol, g_pins.healGlobDat, true, false, g_pins.healFn},
                 {pb::kLinkSymbol, g_pins.linkSlot, false, false, g_pins.linkFn},
                 {pb::kResolveSymbol, g_pins.resolveSlot, false, false, g_pins.resolveFn}});
  const std::uintptr_t base = g_image.Base();
  // The real functions are not in the image: the pins hold the fakes' offsets from it.
  g_pins.nodeFn = reinterpret_cast<std::uintptr_t>(&FakeNode) - base;
  g_pins.healFn = reinterpret_cast<std::uintptr_t>(&FakeHeal) - base;
  g_pins.linkFn = reinterpret_cast<std::uintptr_t>(&FakeLink) - base;
  g_pins.sendFn = reinterpret_cast<std::uintptr_t>(&FakeSend) - base;
  g_pins.resolveFn = reinterpret_cast<std::uintptr_t>(&FakeResolve) - base;
  g_pins.primaryFn = reinterpret_cast<std::uintptr_t>(&FakePrimary) - base;
  *g_image.Slot(g_pins.nodeSlot) = reinterpret_cast<void*>(&FakeNode);
  *g_image.Slot(g_pins.healSlot) = reinterpret_cast<void*>(&FakeHeal);
  *g_image.Slot(g_pins.healGlobDat) = reinterpret_cast<void*>(&FakeHeal);
  *g_image.Slot(g_pins.linkSlot) = reinterpret_cast<void*>(&FakeLink);
  *g_image.Slot(g_pins.sendSlot) = reinterpret_cast<void*>(&FakeSend);
  *g_image.Slot(g_pins.resolveSlot) = reinterpret_cast<void*>(&FakeResolve);
  g_image.Put<std::uint32_t>(g_pins.coneCall, pb::kConeCallWord);
}

// ---- the game: CR15Game's gamespace table, one gamespace with a level and the damageable component system ----------------------------
alignas(16) uintptr_t g_game[0x2000 / 8], g_gs[0x500 / 8], g_table[2], g_sys[4], g_dcs[0x140 / 8];
uintptr_t Game() { return reinterpret_cast<uintptr_t>(g_game); }
void BuildWorld(bool with_dcs, const char* level = "mpl_combat_dyson") {
  std::memset(g_game, 0, sizeof g_game);
  std::memset(g_gs, 0, sizeof g_gs);
  std::memset(g_dcs, 0, sizeof g_dcs);
  const uintptr_t gs = reinterpret_cast<uintptr_t>(g_gs), dcs = reinterpret_cast<uintptr_t>(g_dcs);
  g_table[0] = 0x5555;
  g_table[1] = gs;
  g_game[0x1f48 / 8] = reinterpret_cast<uintptr_t>(g_table);
  g_game[0x1f78 / 8] = 1;
  g_gs[0x8 / 8] = Game();
  g_gs[0x58 / 8] = static_cast<uintptr_t>(CSymbol64Hash(level));
  g_gs[0x420 / 8] = reinterpret_cast<uintptr_t>(g_sys);
  g_gs[0x450 / 8] = with_dcs ? 1 : 0;
  g_sys[0] = pb::kDamageableKey;
  g_sys[1] = dcs;
  g_dcs[0] = g_image.Base() + pb::kPins.damageableVptr;
  g_dcs[0x80 / 8] = gs;
}
// One game frame as the tick runs it: the combat level answer's refresh (the scope row registers it before this module's callback), then the module's
// game tick, which asks the answer when the game pointer is new and once every kScopeEveryFrames frames of one game.
void Frame() {
  g_scopeMs += nevr_quest::combat::scope::kLookEveryMs;
  nevr_quest::combat::scope::Refresh(Game());
  pb::GameTick(Game());
}
// A libr15 image with the tick's slot row, on which the tick is installed and the scope row registers its refresh: the combat level answer can
// change, as the shipped table leaves it before this module's row.
Image g_tickImage;
bool TickImageLookup(const char* module, sentinel::ElfImage* out) {
  if (!g_tickImage.Present() || std::strcmp(module, sentinel::pinned::kLibR15) != 0) return false;
  *out = g_tickImage.Elf();
  return true;
}
void StartTheScope() {
  nevr_quest::combat::tick::ResetForTest();
  nevr_quest::combat::scope::ResetForTest();
  nevr_quest::combat::scope::SetClockForTest(&ScopeClock);
  g_scopeMs = 1000;
  g_tickImage.Build({combat_test::scope_world::TickRow()});
  combat_test::scope_world::PutTickSlot(g_tickImage);
  QCHECK(combat_test::scope_world::StartScope(&TickImageLookup));
}
// Makes the module ask the level now (a frame with no game, then one with it).
void Rescope() {
  pb::GameTick(0);
  Frame();
}

// The look seam (combat_scope.h SetLookReadSeamForTest), set only inside the block headed "the one look" in main(): the second LastLook of the armed tick runs it between
// the record's classes and its level, and it writes the arena level's look there, so a caller that reads the record a second time sees another look.
int g_lookReads = 0;
void SecondLookWritesArena() {
  if (++g_lookReads != 2) return;
  BuildWorld(true, "mpl_arena_a");
  nevr_quest::combat::scope::Refresh(Game());
}
// One raw line that holds every needle (the sentinel's JSON text, as the scope line writes it).
bool HasRawLine(std::initializer_list<const char*> needles) {
  for (const Line& l : g_lines) {
    bool all = true;
    for (const char* n : needles) all = all && l.text.find(n) != std::string::npos;
    if (all) return true;
  }
  return false;
}
// The JSON fragment of a level field: `"level":"0x%016llx"` for the level's symbol hash, as the scope's records write it (combat_scope.h).
std::string LevelField(const char* name) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "\"level\":\"0x%016llx\"", static_cast<unsigned long long>(CSymbol64Hash(name)));
  return buf;
}

void Fresh(const char* level = "mpl_combat_dyson") {
  g_lines.clear();
  g_heals.clear();
  g_sends.clear();
  g_node_calls = g_link_calls = 0;
  g_node_heals = false;
  g_node_other_thread = false;
  g_resolve_calls = 0;
  g_now = 1000;
  g_registrar_ok = true;
  g_registered = 0;
  pb::ResetForTest();
  StartTheScope();
  Build();
  pb::SetImageLookupForTest(&Lookup);
  pb::SetPinsForTest(&g_pins);
  pb::SetClockForTest(&Clock);
  pb::SetNetTickRegistrarForTest(&Registrar);
  sentinel::SetLogSink(&Capture);
  BuildWorld(true, level);
}

nevr_quest::combat::ModuleResult Inst(int stage, const pb::Fns* fns) {
  const nevr_quest::combat::ModuleResult r = pb::InstallAt(&Lookup, g_pins, stage, fns);
  Frame();  // the game tick has seen the game
  return r;
}

struct alignas(16) NodeData {
  uint8_t b[0x40];
};
NodeData Data(float amount, uint32_t user) {
  NodeData d;
  std::memset(d.b, 0, sizeof d.b);
  std::memcpy(d.b + 0x20, &amount, 4);
  std::memcpy(d.b + 0x24, &user, 4);
  return d;
}

bool Untouched(std::initializer_list<std::uint64_t> slots) {
  bool all = true;
  for (std::uint64_t s : slots) {
    const void* want = s == g_pins.nodeSlot      ? reinterpret_cast<void*>(&FakeNode)
                       : s == g_pins.linkSlot    ? reinterpret_cast<void*>(&FakeLink)
                       : s == g_pins.resolveSlot ? reinterpret_cast<void*>(&FakeResolve)
                       : s == g_pins.sendSlot    ? reinterpret_cast<void*>(&FakeSend)
                                                 : reinterpret_cast<void*>(&FakeHeal);
    all = all && SlotWord(s) == want;
  }
  return all;
}

}  // namespace

int main() {
  void* node = reinterpret_cast<void*>(0x5000);
  void* cs = reinterpret_cast<void*>(0xc0);
  const pb::Fns fns = Fns();
  using namespace nevr_powerboost;  // the shared rules (#2339); the PC GTest is runtime/tests/test_powerboost_rules.cpp

  // ---- the marker decode (runtime/patch/powerboost_rules.h): v2.0.0's rounding, held inside the band
  {
    struct T {
      float amount;
      int kind, rez;
      uint32_t v, dr, s;
    } t[] = {
        {1250030.0f, 1, 0, 250030, 250, 30}, {2000045.0f, 2, 0, 45, 0, 45}, {3500007.0f, 1, 1, 500007, 500, 7},
        {4000001.0f, 2, 1, 1, 0, 1},         {6000000.0f, 3, 0, 0, 0, 0},   {6000123.0f, 3, 0, 123, 0, 0},
        {999999.0f, 0, 0, 0, 0, 0},          {5000000.0f, 0, 0, 0, 0, 0},   {5999999.0f, 0, 0, 0, 0, 0},
        {7000000.0f, 0, 0, 0, 0, 0},         {1e9f, 0, 0, 0, 0, 0},         {-1250030.0f, 0, 0, 0, 0, 0},
        {std::nanf(""), 0, 0, 0, 0, 0},      {INFINITY, 0, 0, 0, 0, 0},     {1000000.0f, 1, 0, 0, 0, 0},
        {1020009.75f, 1, 0, 20010, 20, 10},  {3020009.75f, 1, 1, 20010, 20, 10}, {1999999.75f, 4, 0, 0, 0, 0},
        {1999999.25f, 1, 0, 999999, 999, 999}, {3999999.75f, 4, 1, 0, 0, 0}, {6999999.5f, 3, 0, 1000000, 0, 0},
    };
    for (const float bad : {std::nanf(""), INFINITY, -INFINITY}) {  // a radius that is not finite carries no angle (#2339)
      volatile float r = bad;
      CHECK(ConeAngle(r) == 0 && BubbleOf(ConeAngle(r)) == Bubble::kNeither, "ConeAngle(%g) = %d", (double)bad, ConeAngle(r));
    }
    for (const T& c : t) {
      const Marker m = Decode(c.amount);
      CHECK(m.kind == c.kind && m.rez == c.rez && m.v == c.v && m.drPermille == c.dr && m.seconds == c.s, "Decode(%g): kind %d rez %d v %u dr %u s %u",
            (double)c.amount, m.kind, m.rez, m.v, m.drPermille, m.seconds);
    }
  }

  // ---- no pthread key left for the per-thread node mark: the install refuses and hooks nothing (first: the key is made once per process)
  Fresh();
  {
    KeyHog hog;
    const auto none = pb::InstallAt(&Lookup, g_pins, pb::kRead, &fns);
    CHECK(!none.installed && std::strcmp(none.status, "no_thread_key") == 0, "no key: %s", none.status);
    CHECK(Untouched({g_pins.nodeSlot, g_pins.healSlot, g_pins.linkSlot}), "no key: a slot was hooked");
    const std::string code = "\"error\":" + std::to_string(EAGAIN);  // what pthread_key_create answers when the process has no key left
    CHECK(EventsWith("install", "\"reason\":\"no_thread_key\"", kError) == 1 && Count({code.c_str()}) == 1, "no key: the refusal is not logged with its error code");
    CHECK(pb::CurrentStats().stage == 0, "no key: stage %d", pb::CurrentStats().stage);
  }

  // ---- off: nothing installed
  Fresh();
  {
    const auto r = pb::InstallAt(&Lookup, g_pins, 0, nullptr);
    CHECK(!r.installed && std::strcmp(r.status, "off") == 0, "off status %s", r.status);
    CHECK(Untouched({g_pins.nodeSlot, g_pins.healSlot, g_pins.healGlobDat, g_pins.linkSlot, g_pins.resolveSlot}), "off changed a slot");
    CHECK(EventsWith("install", "\"result\":\"off\"", kInfo) == 1 && pb::CurrentStats().stage == 0, "off not logged");
    CHECK(nevr_quest::combat::tick::Count() == 1, "off registered a tick (the one callback is the scope row's refresh)");
  }

  // ---- the combat level answer cannot change (the tick is not installed, so the scope row registered no refresh): refused before the image,
  // nothing hooked or registered; off by its key wins
  Fresh();
  {
    nevr_quest::combat::tick::ResetForTest();
    nevr_quest::combat::scope::ResetForTest();
    const auto r = pb::InstallAt(&Lookup, g_pins, pb::kCone, &fns);
    CHECK(!r.installed && std::strcmp(r.status, "scope_not_live") == 0 && nevr_quest::combat::tick::Count() == 0, "a scope that cannot change: %s", r.status);
    CHECK(Untouched({g_pins.nodeSlot, g_pins.healSlot, g_pins.healGlobDat, g_pins.linkSlot, g_pins.resolveSlot}), "a scope that cannot change: a slot was hooked");
    CHECK(EventsWith("install", "\"reason\":\"scope_not_live\"", kError) == 1, "scope_not_live is not an Error line");
    pb::ResetForTest();
    const auto off = pb::InstallAt(&Lookup, g_pins, 0, &fns);
    CHECK(!off.installed && std::strcmp(off.status, "off") == 0, "off with a dead scope: %s", off.status);
    pb::ResetForTest();
    const auto noImage = pb::InstallAt([](const char*, sentinel::ElfImage*) { return false; }, g_pins, pb::kCone, &fns);
    CHECK(std::strcmp(noImage.status, "scope_not_live") == 0, "a dead scope is asked after the image: %s", noImage.status);
  }

  // ---- a full tick table: refused, nothing hooked (the scope comes from the tick)
  Fresh();
  {
    struct Filler {
      static void Tick(std::uintptr_t) noexcept {}
    };
    while (nevr_quest::combat::tick::Add(&Filler::Tick)) {
    }
    const auto r = pb::InstallAt(&Lookup, g_pins, pb::kCone, &fns);
    CHECK(!r.installed && std::strcmp(r.status, "tick_table_full") == 0, "full tick table: %s", r.status);
    CHECK(Untouched({g_pins.nodeSlot, g_pins.healSlot, g_pins.healGlobDat, g_pins.linkSlot, g_pins.resolveSlot}), "full tick table: a slot was hooked");
  }

  // ---- stage 1 (read-only): three words hooked, nothing else
  Fresh();
  auto r = Inst(pb::kRead, &fns);
  CHECK(r.installed && std::strcmp(r.status, "installed") == 0, "stage 1 install: %s", r.status);
  CHECK(!Untouched({g_pins.nodeSlot}) && !Untouched({g_pins.healSlot}) && !Untouched({g_pins.linkSlot}), "a stage-1 slot not hooked");
  CHECK(Untouched({g_pins.healGlobDat, g_pins.resolveSlot, g_pins.sendSlot}), "stage 1 touched GLOB_DAT / ResolveBody / sender slot");
  CHECK(Events("hook", kInfo) == 3 && EventsWith("install", "\"result\":\"installed\"", kInfo) == 1 && pb::CurrentStats().stage == 1, "stage 1 lines: %d",
        Events("hook", kInfo));
  CHECK(nevr_quest::combat::tick::Count() == 2 && g_registered == 0, "stage 1: the game tick is registered beside the scope row's refresh, the net tick is not");

  // node: arguments and result pass through; a marker amount logs one line AFTER the real call, other amounts none
  {
    NodeData d = Data(1250030.0f, 77);
    NodeSlot()(node, d.b);
    CHECK(g_node_calls == 1 && g_node_arg == node && g_node_data == d.b, "node: real not called with the same arguments");
    CHECK(EventsWith("marker", "\"where\":\"node\"") == 1 && Count({"\"kind\":1,", "\"user\":77", "\"seconds\":30"}) == 1, "node marker line");
    g_lines.clear();
    d = Data(50.0f, 1);
    NodeSlot()(node, d.b);
    d = Data(5500000.0f, 1);  // [5e6, 6e6): above the minimum but not a marker
    NodeSlot()(node, d.b);
    CHECK(g_node_calls == 3 && Events("marker") == 1 && EventsWith("marker", "\"kind\":0,") == 1, "non-marker amounts: %zu lines", g_lines.size());
    NodeSlot()(node, nullptr);  // an unreadable data argument: the real function still runs with it, no fault
    CHECK(g_node_calls == 4 && g_node_data == nullptr, "null data not passed on");
  }
  // heal: the float, the handle and the user go through unchanged; marker lines carry the index
  {
    g_lines.clear();
    HealSlot()(cs, 0x2000a, 1500000.0f, 9);
    CHECK(g_heals.size() == 1 && g_heals[0].cs == cs && g_heals[0].handle == 0x2000a && g_heals[0].amount == 1500000.0f && g_heals[0].user == 9, "heal: args changed");
    CHECK(Count({"\"where\":\"heal\"", "\"index\":10,", "\"kind\":1,"}) == 1, "heal marker line: %s", g_lines.empty() ? "" : g_lines[0].text.c_str());
    g_lines.clear();
    HealSlot()(cs, 3, 40.0f, 9);
    CHECK(g_heals.size() == 2 && g_lines.empty(), "an ordinary heal logged or changed");
  }
  // the node's marker is passed on in stage 1 even on the direct path: heal line, then node line
  {
    g_lines.clear();
    g_heals.clear();
    g_node_heals = true;
    NodeData d = Data(2000045.0f, 5);
    NodeSlot()(node, d.b);
    g_node_heals = false;
    CHECK(g_heals.size() == 1 && g_heals[0].amount == 2000045.0f, "stage 1 swallowed a heal");
    CHECK(g_lines.size() == 2 && g_lines[0].text.find("\"where\":\"heal\"") != std::string::npos &&
              g_lines[1].text.find("\"where\":\"node\"") != std::string::npos,
          "heal line then node line");
  }
  // no per-call logging on the high-fan-in path, and no scope check per call
  {
    g_lines.clear();
    const uint32_t checks = pb::CurrentStats().scopeChecks;
    for (int i = 0; i < 5000; ++i) HealSlot()(cs, 3, 40.0f, 9);
    CHECK(g_lines.empty(), "per-call lines in Heal: %zu", g_lines.size());
    CHECK(pb::CurrentStats().scopeChecks == checks, "a call through Heal asked the level (%u -> %u)", checks, pb::CurrentStats().scopeChecks);
  }
  // marker lines are capped (96), with one cap line announcing it
  {
    const int before = static_cast<int>(pb::CurrentStats().markers);  // marker lines so far (each counts against the cap of 96)
    g_lines.clear();
    for (int i = 0; i < 200; ++i) HealSlot()(cs, 3, 1000001.0f, 9);
    CHECK(Events("marker") == 96 - before && EventsWith("capped", "\"kind\":\"marker\"", kInfo) == 1, "marker cap: %d lines, %d cap lines", Events("marker"),
          Events("capped"));
    CHECK(pb::CurrentStats().markers >= 200, "counter");
  }
  // link: the real call first; one line per (bubble, target) change, a repeat only after 1 s
  {
    g_lines.clear();
    alignas(16) uint8_t target[0x60] = {};
    const uint32_t h1 = 0x30011, h2 = 0x30012;
    float pos[3] = {1.5f, 2.5f, 3.5f};
    std::memcpy(target + 0x48, &h1, 4);
    LinkSlot()(cs, 0x50005, target, pos, 3);
    CHECK(g_link_calls == 1 && g_link_cs == cs && g_link_bubble == 0x50005 && g_link_target == target && g_link_pos == pos && g_link_argv == 3, "link args");
    CHECK(Count({Ev("link").c_str(), "\"bubble\":5,", "\"target\":17,", "\"x\":\"1.5\"", "\"z\":\"3.5\""}) == 1, "link line: %s",
          g_lines.empty() ? "" : g_lines[0].text.c_str());
    g_now += 500;
    LinkSlot()(cs, 0x50005, target, pos, 3);
    CHECK(Events("link") == 1, "repeat within 1 s logged");
    g_now += 600;
    LinkSlot()(cs, 0x50005, target, pos, 3);
    CHECK(Events("link") == 2, "repeat after 1 s not logged");
    std::memcpy(target + 0x48, &h2, 4);
    LinkSlot()(cs, 0x50005, target, pos, 3);
    CHECK(Events("link") == 3, "a new target not logged");
    LinkSlot()(cs, 0x50005, target, reinterpret_cast<const void*>(0x1000), 3);  // unreadable position
    g_now += 2000;
    LinkSlot()(cs, 0x50005, target, reinterpret_cast<const void*>(0x1000), 3);
    CHECK(EventsWith("link", "\"at\":\"unreadable\"") == 1 && g_link_calls == 6, "unreadable position: %d lines, %d calls",
          EventsWith("link", "\"at\":\"unreadable\""), g_link_calls);
    // the target pointer itself unreadable: still passed on, no fault
    LinkSlot()(cs, 0x50006, reinterpret_cast<const void*>(0x1000), pos, 3);
    CHECK(g_link_calls == 7, "link with an unreadable target not passed on");
    // line cap
    g_lines.clear();
    for (int i = 0; i < 400; ++i) {
      g_now += 1100;
      LinkSlot()(cs, 0x50005, target, pos, 3);
    }
    CHECK(Events("link", kInfo) <= 160 && EventsWith("capped", "\"kind\":\"link\"") == 1, "link cap: %d", Events("link", kInfo));
    CHECK(pb::CurrentStats().links >= 400, "link counter");
  }

  // ---- all three words or none: a later failure removes the earlier hooks
  for (int bad = 1; bad <= 2; ++bad) {
    Fresh();
    const uint64_t badslot = bad == 1 ? g_pins.healSlot : g_pins.linkSlot;
    *g_image.Slot(badslot) = reinterpret_cast<void*>(&ForeignFn);
    r = Inst(pb::kRead, &fns);
    CHECK(!r.installed && std::strcmp(r.status, "hook_failed") == 0, "bad %d: %s", bad, r.status);
    CHECK(Untouched({g_pins.nodeSlot}), "bad %d: the node hook was not removed", bad);
    CHECK(bad == 2 ? Untouched({g_pins.healSlot}) : true, "bad %d: the heal hook was not removed", bad);
    CHECK(SlotWord(badslot) == reinterpret_cast<void*>(&ForeignFn), "the foreign word was touched");
    CHECK(EventsWith("install", "\"result\":\"refused\"", kError) == 1 && pb::CurrentStats().stage == 0, "no ERROR / stage after a refused install");
    CHECK(EventsWith("hook", "\"status\":\"original_mismatch\"", kError) == 1, "bad %d: the refused hook is not an ERROR line of its own", bad);
    CHECK(EventsWith("install", bad == 1 ? "\"failed_hook\":\"heal\"" : "\"failed_hook\":\"link\"", kError) == 1, "bad %d: the refusal does not name the hook", bad);
  }
  // a first-hook failure leaves everything alone
  Fresh();
  *g_image.Slot(g_pins.nodeSlot) = reinterpret_cast<void*>(&ForeignFn);
  r = Inst(pb::kRead, &fns);
  CHECK(!r.installed && Untouched({g_pins.healSlot, g_pins.linkSlot}), "first-hook failure touched other slots");

  // ---- stage 2 (swallow)
  Fresh();
  r = Inst(pb::kSwallow, &fns);
  CHECK(r.installed && pb::CurrentStats().stage == 2, "stage 2 install: %s", r.status);
  CHECK(!Untouched({g_pins.healGlobDat}) && SlotWord(g_pins.healGlobDat) != SlotWord(g_pins.healSlot) && Untouched({g_pins.sendSlot, g_pins.resolveSlot}),
        "GLOB_DAT word not hooked to its own thunk / other slots touched");
  CHECK(Events("hook", kInfo) == 4 && EventsWith("install", "\"stage\":2", kInfo) == 1, "stage 2 lines");
  // the node's direct path: the marker never reaches Heal, a kind 1 is held for the next queued heal
  {
    g_lines.clear();
    g_heals.clear();
    g_node_heals = true;
    NodeData d = Data(1250030.0f, 5);
    NodeSlot()(node, d.b);
    g_node_heals = false;
    CHECK(g_heals.empty() && g_sends.empty(), "a marker reached Heal / was sent by the direct path");
    CHECK(EventsWith("swallow", "held for the next queued heal") == 1 && EventsWith("swallow", "\"where\":\"node heal\"") == 1, "held line");
    CHECK(pb::CurrentStats().held == 1 && pb::CurrentStats().swallowed == 1, "held / swallowed counters");
    // the next queued heal (any amount) sends the held use: DamageFromClient(cs, handle, -amount, 0, 0), then runs that heal
    g_now += 500;
    QueuedSlot()(cs, 0x10009, 50.0f, 3);
    CHECK(g_sends.size() == 1 && g_sends[0].cs == cs && g_sends[0].handle == 0x10007 && g_sends[0].amount == -1250030.0f && g_sends[0].hit == 0 && g_sends[0].sym == 0,
          "held use not sent exactly: %zu sends", g_sends.size());
    CHECK(g_heals.size() == 1 && g_heals[0].amount == 50.0f && g_heals[0].handle == 0x10009 && g_heals[0].user == 3, "the ordinary queued heal did not run unchanged");
    CHECK(EventsWith("use", "held from the node's direct heal") == 1, "sent line");
    // it is sent once
    QueuedSlot()(cs, 0x10009, 51.0f, 3);
    CHECK(g_sends.size() == 1 && g_heals.size() == 2, "held use sent twice");
  }
  // a held use older than 2 s, or for another damageable system, is dropped, not sent
  {
    for (int variant = 0; variant < 2; ++variant) {
      g_sends.clear();
      g_lines.clear();
      g_node_heals = true;
      NodeData d = Data(1250030.0f, 5);
      NodeSlot()(node, d.b);
      g_node_heals = false;
      g_now += variant == 0 ? 2000 : 100;  // exactly the hold: not "younger than"
      QueuedSlot()(variant == 0 ? cs : reinterpret_cast<void*>(0xd0), 0x10009, 50.0f, 3);
      CHECK(g_sends.empty() && Events("held_dropped") == 1, "variant %d: held use sent / not reported as dropped", variant);
      CHECK(EventsWith("held_dropped", variant == 0 ? "no queued heal within the hold" : "another damageable system") == 1, "variant %d: drop reason", variant);
    }
    // a second direct kind 1 replaces the held one
    g_lines.clear();
    g_node_heals = true;
    NodeData d = Data(1250030.0f, 5);
    NodeSlot()(node, d.b);
    NodeSlot()(node, d.b);
    g_node_heals = false;
    CHECK(EventsWith("swallow", "an older held one dropped") == 1, "replaced held use not reported");
    QueuedSlot()(cs, 1, 1.0f, 1);  // drain it
  }
  // kinds 2 and 3 on the node's direct path, and any server-relayed marker through Heal: swallowed, never sent, never held
  {
    g_sends.clear();
    g_heals.clear();
    g_lines.clear();
    g_node_heals = true;
    NodeData d2 = Data(2000045.0f, 5), d3 = Data(6000000.0f, 5);
    NodeSlot()(node, d2.b);
    NodeSlot()(node, d3.b);
    g_node_heals = false;
    HealSlot()(cs, 0x10001, 1250030.0f, 5);  // relayed by the server (the DamageDealt callback): kind 1, not in the node
    HealSlot()(cs, 0x10001, 3250030.0f, 5);
    CHECK(g_heals.empty() && g_sends.empty(), "a marker reached Heal or was sent: %zu heals %zu sends", g_heals.size(), g_sends.size());
    CHECK(EventsWith("swallow", "relayed by the server: never re-sent") == 2 && EventsWith("swallow", "\"what\":\"nothing sent\"") == 2, "swallow lines");
    QueuedSlot()(cs, 1, 1.0f, 1);
    CHECK(g_sends.empty() && g_heals.size() == 1, "something was held / sent after those");
    // an amount in [5e6, 6e6) or 7e6+ is not a marker: it is an ordinary heal and is passed on
    g_heals.clear();
    HealSlot()(cs, 1, 5500000.0f, 1);
    HealSlot()(cs, 1, 7500000.0f, 1);
    CHECK(g_heals.size() == 2, "non-marker amounts swallowed");
    // an amount that rounds out of its band is declined: not a heal, not held, not sent, and the swallow line says so
    g_heals.clear();
    g_lines.clear();
    g_node_heals = true;
    NodeData dd = Data(1999999.75f, 5);
    NodeSlot()(node, dd.b);
    g_node_heals = false;
    HealSlot()(cs, 0x10001, 2999999.75f, 5);
    QueuedSlot()(cs, 0x10001, 1999999.75f, 5);
    CHECK(g_heals.empty() && g_sends.empty(), "a declined amount reached Heal or was sent: %zu heals %zu sends", g_heals.size(), g_sends.size());
    CHECK(Count({Ev("marker").c_str(), "\"kind\":4,", "\"amount\":", "\"passed_on\":0,"}) == 1 && EventsWith("swallow", "\"kind\":4,") >= 2,
          "declined lines: %zu", g_lines.size());
    CHECK(EventsWith("swallow", "\"where\":\"queued heal\"") == 1 && Count({Ev("swallow").c_str(), "\"where\":\"queued heal\"", "declined: its value rounds out of its band"}) == 1,
          "the queued decline has no reason of its own");
    CHECK(Count({Ev("swallow").c_str(), "\"where\":\"queued heal\"", "the server owns the buffs"}) == 0, "the queued decline took the link reason");
    QueuedSlot()(cs, 1, 1.0f, 1);
    CHECK(g_sends.empty() && g_heals.size() == 1, "a declined amount was held and sent");
    // stage 1 (swallow off) swallows no marker: the game's Heal takes the declined amount as a heal, and the line says so
    pb::SwallowForTest(false);
    g_heals.clear();
    g_lines.clear();
    HealSlot()(cs, 0x10001, 1999999.75f, 5);
    CHECK(g_heals.size() == 1 && Count({Ev("marker").c_str(), "\"kind\":4,", "\"passed_on\":1,"}) == 1 && Count({Ev("marker").c_str(), "\"passed_on\":0,"}) == 0,
          "stage 1: the declined amount's line does not say it reached Heal: %zu heals %zu lines", g_heals.size(), g_lines.size());
    pb::SwallowForTest(true);
  }
  // the node mark is per thread: a marker relayed by the server on another thread while this thread is inside the node's Enter is not the
  // node's own heal. It is swallowed as a relayed one ("heal", nothing held), and the node's own direct heal on this thread is still held.
  {
    g_sends.clear();
    g_heals.clear();
    g_lines.clear();
    const uint32_t held_before = pb::CurrentStats().held;
    g_node_other_thread = true;
    g_node_heals = true;
    NodeData d = Data(1250030.0f, 5);
    NodeSlot()(node, d.b);
    g_node_heals = false;
    CHECK(EventsWith("swallow", "relayed by the server: never re-sent") == 1, "the other thread's marker was not taken for a relayed one (%d lines)",
          EventsWith("swallow", "relayed by the server: never re-sent"));
    CHECK(EventsWith("swallow", "held for the next queued heal") == 1 && pb::CurrentStats().held == held_before + 1,
          "the node's own heal, not the other thread's, was held (held %u -> %u)", held_before, pb::CurrentStats().held);
    CHECK(g_heals.empty() && g_sends.empty(), "a marker reached Heal / was sent");
    QueuedSlot()(cs, 1, 1.0f, 1);  // drain the held one
    // and after the node's Enter returned, a relayed marker on this thread is a relayed one again
    g_lines.clear();
    HealSlot()(cs, 0x10001, 1250030.0f, 5);
    CHECK(EventsWith("swallow", "relayed by the server: never re-sent") == 1, "the node mark leaked past the Enter");
  }
  // the queued path directly: kind 1 is sent at once, kinds 2 / 3 swallowed, ordinary heals run
  {
    g_sends.clear();
    g_heals.clear();
    QueuedSlot()(cs, 0x10003, 3500007.0f, 1);
    CHECK(g_sends.size() == 1 && g_sends[0].handle == 0x10003 && g_sends[0].amount == -3500007.0f && g_heals.empty(), "queued kind 1 (rez) not sent as -amount");
    QueuedSlot()(cs, 0x10003, 4000001.0f, 1);
    QueuedSlot()(cs, 0x10003, 6000000.0f, 1);
    CHECK(g_sends.size() == 1 && g_heals.empty(), "queued kind 2 / 3 sent or run");
    QueuedSlot()(cs, 0x10003, 12.0f, 1);
    CHECK(g_sends.size() == 1 && g_heals.size() == 1 && g_heals[0].amount == 12.0f, "ordinary queued heal not run");
    CHECK(pb::CurrentStats().sent >= 2, "sent counter");
  }
  // the queued-heal word with swallow off (the window between its hook and the flag): it only passes everything through
  {
    g_heals.clear();
    g_sends.clear();
    pb::SwallowForTest(false);
    QueuedSlot()(cs, 0x10003, 1250030.0f, 1);
    QueuedSlot()(cs, 0x10003, 12.0f, 1);
    CHECK(g_heals.size() == 2 && g_heals[0].amount == 1250030.0f && g_sends.empty(), "queued word with swallow off did not pass everything through");
    pb::SwallowForTest(true);
  }

  // ---- stage 2 refusals leave stage 1 running and are loud
  struct Refuse {
    const char* what;
    int variant;
  } refusals[] = {{"sender's slot holds something else", 0}, {"Heal's GLOB_DAT holds something else", 1}, {"no sender function", 2}};
  for (const Refuse& rf : refusals) {
    Fresh();
    pb::Fns f2 = Fns();
    if (rf.variant == 0) *g_image.Slot(g_pins.sendSlot) = reinterpret_cast<void*>(&ForeignFn);
    if (rf.variant == 1) *g_image.Slot(g_pins.healGlobDat) = reinterpret_cast<void*>(&ForeignFn);
    if (rf.variant == 2) f2.send = nullptr;
    r = Inst(pb::kSwallow, &f2);
    CHECK(!r.installed && std::strcmp(r.status, "partial") == 0 && pb::CurrentStats().stage == 1, "%s: %s stage %d", rf.what, r.status, pb::CurrentStats().stage);
    CHECK(Events("swallow_off", kError) == 1 && EventsWith("install", "\"reason\":\"swallow_off\"", kError) == 1, "%s: not an ERROR", rf.what);
    CHECK(!Untouched({g_pins.nodeSlot}), "%s: stage 1 words are not in", rf.what);
    if (rf.variant == 1) CHECK(SlotWord(g_pins.healGlobDat) == reinterpret_cast<void*>(&ForeignFn), "foreign GLOB_DAT word touched");
    // with swallow off, a marker passes through Heal as in stage 1
    HealSlot()(cs, 1, 1250030.0f, 1);
    CHECK(g_heals.size() == 1, "%s: a marker was swallowed although swallow is off", rf.what);
  }
  // engine functions that cannot be resolved (no libr15 on this host): stage 1 only, ERROR
  Fresh();
  r = Inst(pb::kCone, nullptr);
  CHECK(std::strcmp(r.status, "partial") == 0 && pb::CurrentStats().stage == 1 && Events("fns", kError) == 1, "unresolved engine functions: %s stage %d", r.status,
        pb::CurrentStats().stage);
  {
    pb::Fns none{};
    CHECK(!pb::ResolveFns(g_image.Base(), g_pins, &none), "ResolveFns succeeded without a game library");
  }

  // ---- stage 3 (cone): the install
  Fresh();
  r = Inst(pb::kCone, &fns);
  CHECK(r.installed && pb::CurrentStats().stage == 3, "stage 3 install: %s stage %d", r.status, pb::CurrentStats().stage);
  CHECK(!Untouched({g_pins.resolveSlot}) && SlotWord(g_pins.resolveSlot) != nullptr, "ResolveBody's word not hooked");
  CHECK(EventsWith("install", "\"stage\":3", kInfo) == 1 && Events("hook", kInfo) == 5, "stage 3 lines");
  CHECK(EventsWith("install", "\"return_address_vaddr\":\"0x000000000215799c\"") == 1, "the return address is not named in the log");
  CHECK(pb::CurrentStats().coneArmed && g_registered == 1, "stage 3 is on but the cone is not armed / the net tick not registered");
  CHECK(pb::ConeRetForTest() == g_image.Base() + pb::kPins.coneRet, "the entry's return-address word is not base + 0x215799c");
  CHECK(pb::NextForTest() == reinterpret_cast<std::uintptr_t>(&FakeResolve), "the entry's next word is not the slot's ResolveBody");
  {
    // a pass-through call through the slot reaches the real ResolveBody (the host stand-in calls the next word)
    const auto resolve = reinterpret_cast<ResolveFn>(SlotWord(g_pins.resolveSlot));
    PhTarget t{{1, 0, 0}};
    g_resolve_calls = 0;
    CHECK(resolve(&t) == g_body_a && g_resolve_calls == 1, "the hooked word does not reach the real ResolveBody");
  }
  // the entry stays the newest hook on its slot: a handler that asks to join above it is refused (callback_thunk.h rule 7)
  {
    struct LaterTag {};
    using LaterThunk = sentinel::CallbackThunk<LaterTag, const uint8_t*(const PhTarget*)>;
    sentinel::GotHook later;
    CHECK(sentinel::InstallThunk<LaterThunk>(later, pb::ResolveTarget(g_image.Base(), g_pins).Owned("later"), &Lookup) == sentinel::GotStatus::kSlotHeldByEntry,
          "a later handler joined above the entry");
  }
  // refusals leave stage 2 on, the ResolveBody word alone, and are loud
  struct ConeRefuse {
    const char* what;
    int variant;
  } cone_refusals[] = {{"no net tick wired", 0}, {"call site is not the bl", 1}, {"ResolveBody slot holds something else", 2}, {"no PrimaryBody", 3}};
  for (const ConeRefuse& rf : cone_refusals) {
    Fresh();
    pb::Fns f3 = Fns();
    if (rf.variant == 0) g_registrar_ok = false;
    if (rf.variant == 1) g_image.Put<std::uint32_t>(g_pins.coneCall, pb::kConeCallWord ^ 1);
    if (rf.variant == 2) *g_image.Slot(g_pins.resolveSlot) = reinterpret_cast<void*>(&ForeignFn);
    if (rf.variant == 3) f3.primary = nullptr;
    r = Inst(pb::kCone, &f3);
    CHECK(std::strcmp(r.status, "partial") == 0 && pb::CurrentStats().stage == 2 && Events("cone_off", kError) == 1, "%s: status %s stage %d", rf.what, r.status,
          pb::CurrentStats().stage);
    CHECK(rf.variant == 2 ? SlotWord(g_pins.resolveSlot) == reinterpret_cast<void*>(&ForeignFn) : Untouched({g_pins.resolveSlot}), "%s: ResolveBody's word was touched",
          rf.what);
    CHECK(!pb::CurrentStats().coneArmed && pb::ConeRetForTest() == 0, "%s: the cone is armed although it was refused", rf.what);
    CHECK(EventsWith("install", "\"stage\":2", kError) == 1, "%s: stage 2 lost", rf.what);
  }

  // ---- stage 3: ConeAt (what the entry's continuation runs for the one call it acts on)
  Fresh();
  r = Inst(pb::kCone, &fns);
  CHECK(pb::CurrentStats().stage == 3, "stage 3 for the cone tests");
  {
    const uintptr_t dcs = reinterpret_cast<uintptr_t>(g_dcs);
    std::memset(g_body_a, 0, sizeof g_body_a);
    std::memset(g_body_b, 0, sizeof g_body_b);
    std::memset(g_body_owner, 0, sizeof g_body_owner);
    SetPos(g_body_owner, 100, 100, 100);
    SetQuat(g_body_owner, 0, 0, 0, 1);  // identity: the owner's +z is world +z
    // the frame as UpdateActiveHealBubble leaves it at the call: [sp+0xe8] -> radius (record + 0x38), [sp+0xa0] = the target's handle word
    std::memset(g_frame, 0, sizeof g_frame);
    std::memset(g_rec, 0, sizeof g_rec);
    const uint64_t physics = reinterpret_cast<uint64_t>(g_phys);
    std::memcpy(g_rec + 0, &physics, 8);
    const uint32_t owner_handle = 0x77;
    std::memcpy(g_rec + 8, &owner_handle, 4);
    const float radius = 15.0f + 60.0f / 100000.0f;  // 15 m, a 60 degree cone
    std::memcpy(g_rec + 0x38, &radius, 4);
    const uint64_t radius_at = reinterpret_cast<uint64_t>(g_rec + 0x38);
    std::memcpy(g_frame + 0xe8, &radius_at, 8);
    const uint32_t index = 0x40021;
    std::memcpy(g_frame + 0xa0, &index, 4);
    const PhTarget ta{{1, 0, 0}}, tb{{2, 0, 0}};

    // a target 10 m straight ahead of the owner: linked, the real body is returned, a link send is queued
    SetPos(g_body_a, 100, 100, 110);
    g_now = 5000;
    g_resolve_calls = 0;
    const uint8_t* got = pb::ConeAtForTest(&ta, g_frame, dcs);
    CHECK(got == g_body_a, "a target in the cone did not get its own body");
    CHECK(g_primary_physics == physics && g_primary_handle == owner_handle, "the owner was not asked for with the record's physics system and handle");
    CHECK(g_resolve_calls == 2, "ResolveBody calls: %d (target, owner)", g_resolve_calls);
    // a target 10 m off to the side (90 degrees): outside the 60 degree cone: the far stand-in body
    SetPos(g_body_b, 110, 100, 100);
    got = pb::ConeAtForTest(&tb, g_frame, dcs);
    CHECK(got != g_body_b && got != nullptr, "a target outside the cone kept its body");
    float sp[3];
    std::memcpy(sp, got + 0x8f8, 12);
    CHECK(sp[0] == 1e30f && sp[1] == 0.0f && sp[2] == 0.0f, "stand-in position %g %g %g", (double)sp[0], (double)sp[1], (double)sp[2]);
    // the stand-in is the size UpdateActiveHealBubble indexes (body stride 0xf38) and is zero but for the position
    bool zero = true;
    for (size_t i = 0; i < 0xf38; ++i)
      if ((i < 0x8f8 || i >= 0x8f8 + 12) && got[i] != 0) zero = false;
    CHECK(zero, "stand-in has stray non-zero bytes");
    // beyond floor(radius) = 15 m, straight ahead: rejected
    SetPos(g_body_b, 100, 100, 120);
    got = pb::ConeAtForTest(&tb, g_frame, dcs);
    CHECK(got != g_body_b, "a target beyond the range kept its body");
    // the owner itself (closer than 0.3 m): never self-linked
    SetPos(g_body_b, 100, 100, 100.1f);  // dead ahead (0 degrees) and 0.1 m away: only the 0.3 m rule rejects it
    got = pb::ConeAtForTest(&tb, g_frame, dcs);
    CHECK(got != g_body_b, "the owner itself kept its body");
    // a bubble that is not POWER BOOST's (no angle in the radius, or an angle above 180): left alone
    const float plain = 15.0f, big = 15.0f + 200.0f / 100000.0f;
    SetPos(g_body_b, 110, 100, 100);  // 90 degrees off: a 60 degree cone would reject it
    std::memcpy(g_rec + 0x38, &plain, 4);
    CHECK(pb::ConeAtForTest(&tb, g_frame, dcs) == g_body_b, "a bubble without an angle was filtered");
    SetPos(g_body_b, 100, 100, 130);  // 30 m ahead: out of range, but not inside any cone of an angle above 180
    std::memcpy(g_rec + 0x38, &big, 4);
    CHECK(pb::ConeAtForTest(&tb, g_frame, dcs) == g_body_b, "an angle above 180 was filtered");
    // MEDIGUN's band (501..590) is PC's: the Quest leaves that bubble to the game, as it does a radius that is not finite (#2339)
    const float medi = 15.0f + 545.0f / 100000.0f, nanRadius = std::nanf("");
    SetPos(g_body_b, 100, 100, 130);  // 30 m ahead: past floor(radius) = 15 m, so a cone would reject it
    std::memcpy(g_rec + 0x38, &medi, 4);
    CHECK(pb::ConeAtForTest(&tb, g_frame, dcs) == g_body_b, "a MEDIGUN-band bubble was filtered");
    std::memcpy(g_rec + 0x38, &nanRadius, 4);
    CHECK(pb::ConeAtForTest(&tb, g_frame, dcs) == g_body_b, "a NaN radius was filtered");
    std::memcpy(g_rec + 0x38, &radius, 4);
    // unreadable frame / radius pointer / record: the real body, nothing queued
    CHECK(pb::ConeAtForTest(&tb, nullptr, dcs) == g_body_b, "unreadable frame: not the real body");
    uint8_t bad_frame[0x100] = {};
    const uint64_t junk = 0x1000;
    std::memcpy(bad_frame + 0xe8, &junk, 8);
    CHECK(pb::ConeAtForTest(&tb, bad_frame, dcs) == g_body_b, "unreadable radius: not the real body");
    // ResolveBody returned null for the target: null passes (UpdateActiveHealBubble's own path), nothing else is read
    const PhTarget tnull{{0xdead, 0, 0}};
    CHECK(pb::ConeAtForTest(&tnull, g_frame, dcs) == nullptr, "a null body was replaced");
    // the owner has no body: reject with the stand-in
    g_owner_key = 0xdead;
    got = pb::ConeAtForTest(&ta, g_frame, dcs);
    CHECK(got != g_body_a && got != nullptr, "no owner body: not rejected");
    g_owner_key = 100;
    // physics pointer unreadable: the real body
    const uint64_t zero_phys = 0;
    std::memcpy(g_rec + 0, &zero_phys, 8);
    CHECK(pb::ConeAtForTest(&ta, g_frame, dcs) == g_body_a, "unreadable physics system: not the real body");
    std::memcpy(g_rec + 0, &physics, 8);
    CHECK(pb::CurrentStats().coneCalls >= 10 && pb::CurrentStats().coneRejects >= 4, "cone counters %u / %u", pb::CurrentStats().coneCalls,
          pb::CurrentStats().coneRejects);
    // the diagnostic line is rate-limited (2 / s), not per call
    const int lines_before = Events("cone");
    for (int i = 0; i < 300; ++i) pb::ConeAtForTest(&ta, g_frame, dcs);
    CHECK(Events("cone") - lines_before <= 1, "cone diagnostics per call: %d", Events("cone") - lines_before);

    // ---- the link / unlink sends from the net tick
    // no use of ours within 30 s: another client's bubble, nothing is sent
    pb::NetTick();
    CHECK(g_sends.empty(), "a link marker was sent without a use of ours");
    CHECK(EventsWith("link_marker", "no use of ours within 30 s") >= 1, "quiet reason not logged");
    // our own use (a queued kind-1 heal at t = 6000 ms): dr 250, 30 s
    g_now = 6000;
    QueuedSlot()(cs, 0x10003, 1250030.0f, 1);
    CHECK(g_sends.size() == 1, "the use itself not sent");
    g_sends.clear();
    // a fresh bubble: a target in the cone is linked and queued; the net tick sends 2e6 + dr*1000 + the seconds left, to the live system
    const uint32_t idx2 = 0x40022;
    std::memcpy(g_frame + 0xa0, &idx2, 4);
    SetPos(g_body_a, 100, 100, 105);
    g_now = 7000;
    CHECK(pb::ConeAtForTest(&ta, g_frame, dcs) == g_body_a, "in-cone target not linked");
    pb::NetTick();
    // use at 6000, link seen at 7000, tick at 7000: left = (30000 - 1000 + 999) / 1000 = 29
    bool found = false;
    for (const SendCall& s : g_sends)
      if (s.cs == reinterpret_cast<void*>(dcs) && s.handle == idx2 && s.amount == -(2000000.0f + 250.0f * 1000.0f + 29.0f) && s.hit == 0 && s.sym == 0) found = true;
    CHECK(found, "link marker not sent as DamageFromClient(dcs, index, -(2e6 + dr*1000 + left), 0, 0): %zu sends", g_sends.size());
    CHECK(EventsWith("link_marker", "\"sent\":1") >= 1, "no sent line");
    // the target leaves the cone: an unlink marker (6e6) after the minimum interval
    g_sends.clear();
    SetPos(g_body_a, 110, 100, 100);
    g_now = 7400;
    CHECK(pb::ConeAtForTest(&ta, g_frame, dcs) != g_body_a, "target left the cone but kept its body");
    pb::NetTick();
    found = false;
    for (const SendCall& s : g_sends)
      if (s.handle == idx2 && s.amount == -6000000.0f) found = true;
    CHECK(found, "unlink marker not sent");
    // nothing pending: the net tick does nothing
    g_sends.clear();
    pb::NetTick();
    CHECK(g_sends.empty(), "a second net tick re-sent");
    // a change older than 2 s is not sent
    g_sends.clear();
    SetPos(g_body_a, 100, 100, 105);
    g_now = 8000;
    pb::ConeAtForTest(&ta, g_frame, dcs);
    g_now = 10500;
    pb::NetTick();
    CHECK(g_sends.empty() && EventsWith("link_marker", "older than 2 s") >= 1, "a stale change was sent");
    // a damageable system that is no longer live (a level changed): not sent
    g_sends.clear();
    BuildWorld(false);
    SetPos(g_body_a, 100, 100, 105);
    g_now = 11000;  // more than 2 s after the bubble was last seen: a new bubble, the in-cone target is linked afresh
    CHECK(pb::ConeAtForTest(&ta, g_frame, dcs) == g_body_a, "fresh link for the live-system test");
    g_now = 11100;
    pb::NetTick();
    CHECK(g_sends.empty() && EventsWith("link_marker", "its damageable system is gone") >= 1, "sent to a system that is not live");
    // a system with the damageable key whose vptr is not CR15NetDamageableCS's: not a live damageable system, not sent
    g_sends.clear();
    BuildWorld(true);
    g_dcs[0] = g_image.Base() + pb::kPins.damageableVptr + 8;
    g_now = 14000;  // more than 2 s after the bubble was last seen: linked afresh
    CHECK(pb::ConeAtForTest(&ta, g_frame, dcs) == g_body_a, "fresh link for the vptr test");
    g_now = 14100;
    pb::NetTick();
    CHECK(g_sends.empty(), "sent to a system whose vptr is not the damageable system's");
    BuildWorld(true);
    // 30 s after the use: no more link markers
    g_sends.clear();
    SetPos(g_body_a, 100, 100, 105);
    g_now = 40000;
    pb::ConeAtForTest(&ta, g_frame, dcs);
    pb::NetTick();
    CHECK(g_sends.empty(), "a link marker sent more than 30 s after our use");

    // ---- a full pending table (16) drops its oldest; the rest are sent
    {
      BuildWorld(true);
      g_sends.clear();
      g_now = 70000;
      QueuedSlot()(cs, 0x10003, 1250030.0f, 1);  // a use of ours, so the link markers go out
      g_sends.clear();
      static uint8_t rec[3][0x60], fr[3][0x100];
      const unsigned dropped_before = pb::CurrentStats().pendingDropped;
      SetPos(g_body_a, 100, 100, 105);
      uint32_t n = 0;
      for (int b = 0; b < 3; ++b) {  // three bubbles (records): 8 + 8 + 1 distinct targets = 17 pending changes
        std::memcpy(rec[b], g_rec, sizeof rec[b]);
        std::memcpy(fr[b], g_frame, sizeof fr[b]);
        const uint64_t ra = reinterpret_cast<uint64_t>(rec[b] + 0x38);
        std::memcpy(fr[b] + 0xe8, &ra, 8);
        for (int k = 0; k < (b == 2 ? 1 : 8); ++k) {
          const uint32_t ix = 0x50001 + n++;
          std::memcpy(fr[b] + 0xa0, &ix, 4);
          CHECK(pb::ConeAtForTest(&ta, fr[b], dcs) == g_body_a, "full-table test: target %u not linked", ix);
        }
      }
      CHECK(pb::CurrentStats().pendingDropped == dropped_before + 1, "17th pending change: dropped %u", pb::CurrentStats().pendingDropped - dropped_before);
      CHECK(EventsWith("cone", "pending full: oldest dropped") >= 1, "the drop is not reported");
      g_now = 70100;
      pb::NetTick();
      bool first = false, last = false;
      for (const SendCall& sc : g_sends) {
        first = first || sc.handle == 0x50001;
        last = last || sc.handle == 0x50011;
      }
      CHECK(g_sends.size() == 16 && !first && last, "full table: %zu sends, oldest sent %d, newest sent %d", g_sends.size(), first, last);
    }

    // a pending change for a damageable system that is not among the live ones (another one is live): dropped, not sent
    {
      g_sends.clear();
      g_lines.clear();
      g_now = 80000;
      QueuedSlot()(cs, 0x10003, 1250030.0f, 1);  // a use of ours
      g_sends.clear();
      uint32_t ix = 0x70001;
      std::memcpy(g_frame + 0xa0, &ix, 4);
      CHECK(pb::ConeAtForTest(&ta, g_frame, dcs + 0x10) == g_body_a, "stale-system test: not linked");
      g_now = 80050;
      pb::NetTick();
      CHECK(g_sends.empty() && EventsWith("link_marker", "its damageable system is gone") >= 1, "a change for a system that is not live was sent: %zu", g_sends.size());
    }

    // a use with the rez flag (3.5e6 band): the link marker carries it (+ 2e6)
    {
      g_sends.clear();
      g_now = 90000;
      QueuedSlot()(cs, 0x10003, 3500007.0f, 1);  // dr 500, 7 s, rez
      g_sends.clear();
      uint32_t ix = 0x60001;
      std::memcpy(g_frame + 0xa0, &ix, 4);
      CHECK(pb::ConeAtForTest(&ta, g_frame, dcs) == g_body_a, "rez test: not linked");
      g_now = 90010;
      pb::NetTick();
      bool found_rez = false;
      for (const SendCall& sc : g_sends)
        if (sc.handle == 0x60001 && sc.amount == -(2000000.0f + 500.0f * 1000.0f + 7.0f + 2000000.0f)) found_rez = true;
      CHECK(found_rez, "rez link marker not 2e6 + dr*1000 + left + 2e6: %zu sends", g_sends.size());
    }
  }

  // ---- the cone's hysteresis and rate limit: linked, the cone is 5 degrees wider; a change is sent at most every 250 ms
  {
    ConeTable t;
    ConeVerdict v = ConeDecide(t, 1, 2, 3, 15.0f, 60, 10.0f, 62.0f, 1000);
    CHECK(!v.ok && !v.send, "fresh target 62 deg off in a 60 deg cone: ok %d send %d", v.ok, v.send);
    v = ConeDecide(t, 1, 2, 3, 15.0f, 60, 10.0f, 58.0f, 1100);
    CHECK(v.ok && v.send, "58 deg: not linked / not sent");
    v = ConeDecide(t, 1, 2, 3, 15.0f, 60, 10.0f, 62.0f, 1200);
    CHECK(v.ok && !v.send, "linked target at 62 deg (5 deg slack): ok %d send %d", v.ok, v.send);
    v = ConeDecide(t, 1, 2, 3, 15.0f, 60, 10.0f, 66.0f, 1300);
    CHECK(!v.ok && !v.send, "66 deg is out, but the change is 200 ms after the last send: ok %d send %d", v.ok, v.send);
    v = ConeDecide(t, 1, 2, 3, 15.0f, 60, 10.0f, 66.0f, 1400);
    CHECK(!v.ok && v.send, "300 ms after the last send the unlink goes out: ok %d send %d", v.ok, v.send);
    v = ConeDecide(t, 1, 2, 3, 15.0f, 60, 16.0f, 0.0f, 1500);
    CHECK(!v.ok, "beyond floor(radius) = 15 m");
  }
  // without stage 3 the net tick is inert
  Fresh();
  Inst(pb::kSwallow, &fns);
  pb::NetTick();
  CHECK(g_sends.empty(), "net tick active below stage 3");

  // ---- arena (same APK): with an Echo Arena level loaded, stage 3 installed does NOTHING: every call goes on unchanged, nothing is
  // logged, held, swallowed, queued or sent. The same with no game seen yet, and in an arena lobby. A combat map then makes it act.
  for (const char* level : {"mpl_arena_a", "mpl_lobby_b2", static_cast<const char*>(nullptr)}) {
    Fresh(level != nullptr ? level : "mpl_arena_a");
    r = pb::InstallAt(&Lookup, g_pins, pb::kCone, &fns);
    CHECK(r.installed, "arena: stage 3 not installed: %s", r.status);
    if (level != nullptr) Frame();  // nullptr: no game tick has run
    CHECK(!pb::InScope(), "arena (%s): in scope", level != nullptr ? level : "no game");
    g_lines.clear();
    const pb::Stats before = pb::CurrentStats();
    StartReadCount();  // what follows is the arena path: no read of game memory, no scan of the levels (count, not only the game's view)
    StartScanCount();
    g_node_heals = true;
    NodeData d = Data(1250030.0f, 5);
    NodeSlot()(node, d.b);  // the node's direct heal goes on to Heal with the marker
    g_node_heals = false;
    HealSlot()(cs, 0x10001, 1250030.0f, 5);
    QueuedSlot()(cs, 0x10003, 1250030.0f, 1);
    alignas(16) uint8_t target[0x60] = {};
    float pos[3] = {1, 2, 3};
    LinkSlot()(cs, 0x50005, target, pos, 3);
    // g_frame still holds the POWER BOOST bubble the cone tests built: in combat it would reject this target (90 degrees off)
    SetPos(g_body_a, 110, 100, 100);
    const PhTarget ta{{1, 0, 0}};
    const uint8_t* body = pb::ConeAtForTest(&ta, g_frame, reinterpret_cast<uintptr_t>(g_dcs));
    pb::NetTick();
    CHECK(ReadCount() == 0 && ScanCount() == 0, "arena (%s): %llu reads and %llu scans on the arena path",
          level != nullptr ? level : "no game", static_cast<unsigned long long>(ReadCount()),
          static_cast<unsigned long long>(ScanCount()));
    CHECK(g_heals.size() == 3 && g_heals[0].amount == 1250030.0f && g_heals[1].amount == 1250030.0f && g_heals[2].amount == 1250030.0f,
          "arena (%s): a heal was swallowed or changed (%zu heals)", level != nullptr ? level : "no game", g_heals.size());
    CHECK(g_node_calls == 1 && g_link_calls == 1 && body == g_body_a && g_sends.empty(), "arena (%s): a call was changed or something was sent",
          level != nullptr ? level : "no game");
    const pb::Stats after = pb::CurrentStats();
    CHECK(after.markers == before.markers && after.swallowed == before.swallowed && after.held == before.held && after.links == before.links &&
              after.coneCalls == before.coneCalls && after.sent == before.sent && after.pendingSent == before.pendingSent,
          "arena (%s): the module counted something", level != nullptr ? level : "no game");
    CHECK(Events("marker") + Events("swallow") + Events("link") + Events("cone") + Events("use") + Events("link_marker") == 0, "arena (%s): %zu lines logged",
          level != nullptr ? level : "no game", g_lines.size());
    // a combat map loads: the next frames ask the level and the module acts
    BuildWorld(true, "mpl_combat_fission");
    Rescope();
    CHECK(pb::InScope() && Events("scope") >= 1 && EventsWith("scope", "\"in_combat\":1") == 1, "combat after arena: not in scope");
    g_heals.clear();
    HealSlot()(cs, 0x10001, 1250030.0f, 5);
    CHECK(g_heals.empty(), "combat after arena: the relayed marker reached Heal");
  }
  // the combat lobby is in scope; an arena level loaded beside a combat map wins (combat_level.h)
  {
    Fresh("mpl_lobby_b_combat");
    Inst(pb::kRead, &fns);
    CHECK(pb::InScope(), "the combat lobby is not in scope");
    BuildWorld(true, "mpl_arena_b");
    Rescope();
    CHECK(!pb::InScope() && EventsWith("scope", "\"kind\":\"echo_arena\"") == 1, "an arena level did not take the scope away");
  }
  // the module's ask reads a look older than kStaleAfterMs (the game tick still runs; the stopped-tick case is the held block below): the answer
  // is not combat, and its line names the class `stale`
  {
    Fresh();
    Inst(pb::kRead, &fns);
    CHECK(pb::InScope(), "a combat map is not in scope");
    g_scopeMs += nevr_quest::combat::scope::kStaleAfterMs + 1;
    for (uint32_t i = 0; i < pb::kScopeEveryFrames; ++i) pb::GameTick(Game());  // the module's game tick alone
    CHECK(!pb::InScope() && EventsWith("scope", "\"kind\":\"stale\"") == 1, "a stale combat answer kept the scope");
    // the stale line names the class and the level the answer was held for (the record's level, not zero)
    CHECK(HasRawLine({"\"in_combat\":0,\"kind\":\"stale\"", LevelField("mpl_combat_dyson").c_str()}), "the stale line does not name the held level");
  }
  // the held answer: the game tick is stopped (no Refresh and no GameTick) and the scope's clock moves on 60 s. The scope's own look is then not
  // fresh, but POWER BOOST reads only the atomic its last ask stored, so it stays in scope and writes no line (powerboost.md, "Arena").
  {
    Fresh();
    Inst(pb::kRead, &fns);
    CHECK(pb::InScope(), "held: a combat map is not in scope");
    const int scopeLines = Events("scope");
    g_scopeMs += 60000;
    CHECK(!nevr_quest::combat::scope::LookIsFresh(), "held: the scope still believes its look 60 s on");
    CHECK(pb::InScope(), "held: POWER BOOST dropped its answer while the game tick was stopped");
    CHECK(Events("scope") == scopeLines, "held: a scope line was written with the game tick stopped");
  }
  // the scope's cadence, as literals: 64 frames of one game ask the level twice (the new game at the first frame, then the frame 32 later), so
  // the cadence is 32 frames. The literal, not kScopeEveryFrames, so a changed constant fails here.
  {
    Fresh();
    Inst(pb::kRead, &fns);
    const uint32_t first = pb::CurrentStats().scopeChecks;
    for (uint32_t i = 0; i < 64; ++i) Frame();
    const uint32_t checks = pb::CurrentStats().scopeChecks - first;
    CHECK(checks == 2, "%u level reads in 64 frames, want 2 (one every 32 frames of one game)", checks);
    BuildWorld(true, "mpl_arena_a");  // the level changes under the same game: seen within kScopeEveryFrames frames
    for (uint32_t i = 0; i < pb::kScopeEveryFrames; ++i) Frame();
    CHECK(!pb::InScope(), "an arena level was not seen within %u frames", pb::kScopeEveryFrames);
  }

  // ---- the one look (the case dronekill_test.cpp's TestTheRecordDecidesNotTheLiveRead states): the decision, the logged class and the logged level come from the one
  // record the tick reads. The clock is armed, so the record's own read is fresh and every later read sees it older than kStaleAfterMs; the second
  // read of the record writes the arena level's look, so a second read names another look. The one record must decide all three.
  {
    Fresh("mpl_arena_a");
    Inst(pb::kRead, &fns);  // the first line: echo_arena, out of scope
    CHECK(!pb::InScope() && HasRawLine({"\"in_combat\":0,\"kind\":\"echo_arena\"", LevelField("mpl_arena_a").c_str()}), "the arena line");
    BuildWorld(true, "mpl_combat_fission");
    g_scopeMs += nevr_quest::combat::scope::kLookEveryMs;
    nevr_quest::combat::scope::Refresh(Game());  // the look the tick decides from: a combat map
    g_lookReads = 0;
    g_armedReads = 0;
    nevr_quest::combat::scope::SetLookReadSeamForTest(&SecondLookWritesArena);
    const uint32_t checks0 = pb::CurrentStats().scopeChecks;
    for (uint32_t i = 0; i <= pb::kScopeEveryFrames && pb::CurrentStats().scopeChecks == checks0; ++i) pb::GameTick(Game());
    nevr_quest::combat::scope::SetLookReadSeamForTest(nullptr);
    g_armedReads = -1;
    CHECK(pb::CurrentStats().scopeChecks == checks0 + 1, "the tick did not ask the answer once in %u frames", pb::kScopeEveryFrames);
    CHECK(pb::InScope(), "the record says a combat map, but the scope did not start: a live read decided it");
    CHECK(HasRawLine({"\"in_combat\":1,\"kind\":\"combat_or_lobby\"", LevelField("mpl_combat_fission").c_str()}),
          "the change line does not name the record's class and level");
  }

  // ---- the scope line's fields, per class (combat_scope.h LookClassName): a combat map, the combat lobby, another lobby, no look yet
  {
    Fresh("mpl_combat_dyson");
    Inst(pb::kRead, &fns);
    CHECK(HasRawLine({"\"in_combat\":1,\"kind\":\"combat_or_lobby\"", LevelField("mpl_combat_dyson").c_str(),
                      "\"msg\":\"a combat map or the combat lobby is loaded: POWER BOOST acts\""}),
          "combat map: the scope line");
  }
  {
    Fresh("mpl_lobby_b_combat");
    Inst(pb::kRead, &fns);
    CHECK(HasRawLine({"\"in_combat\":1,\"kind\":\"combat_or_lobby\"", LevelField("mpl_lobby_b_combat").c_str()}), "combat lobby: the scope line");
  }
  {
    Fresh("mpl_lobby_b");
    Inst(pb::kRead, &fns);
    CHECK(HasRawLine({"\"in_combat\":0,\"kind\":\"other\"", "\"msg\":\"no combat map or combat lobby is loaded: POWER BOOST only passes every call on\""}),
          "another lobby: the scope line");
  }
  {
    Fresh();  // no look yet: the first ask reads an answer that was never made
    pb::GameTick(Game());
    CHECK(HasRawLine({"\"in_combat\":0,\"kind\":\"no_look\"", "\"level\":\"0x0000000000000000\""}), "no look yet: the scope line");
  }
  // a class that changes while the scope stays out keeps its line: other (a lobby) to echo_arena, both out of scope
  {
    Fresh("mpl_lobby_b");
    Inst(pb::kRead, &fns);
    const int before = Events("scope");
    BuildWorld(true, "mpl_arena_a");
    Rescope();
    CHECK(EventsWith("scope", "\"kind\":\"echo_arena\"") == 1 && Events("scope") == before + 1, "other -> echo arena, both out of scope: no change line");
  }

  // ---- the stage numbers: 1..3 install that stage, 0 and anything else nothing
  for (int stage : {-1, 0, 4, 99}) {
    Fresh();
    r = pb::InstallAt(&Lookup, g_pins, stage, &fns);
    CHECK(!r.installed && std::strcmp(r.status, "off") == 0 && Untouched({g_pins.nodeSlot}), "stage %d: %s", stage, r.status);
  }
  // once per process: a second step installs nothing
  {
    Fresh();
    Inst(pb::kRead, &fns);
    const auto again = pb::InstallAt(&Lookup, g_pins, pb::kRead, &fns);
    CHECK(!again.installed && std::strcmp(again.status, "already_ran") == 0, "second install: %s", again.status);
  }
  // the build ID must be the patched libr15's
  {
    Fresh();
    g_image.Build({{pb::kNodeSymbol, g_pins.nodeSlot, false, false, g_pins.nodeFn}}, "0000000000000000000000000000000000000000");
    const auto wrong = pb::InstallAt(&Lookup, g_pins, pb::kRead, &fns);
    CHECK(!wrong.installed && std::strcmp(wrong.status, "build_id_mismatch") == 0, "wrong build id: %s", wrong.status);
  }

  pb::ResetForTest();
  nevr_quest::combat::tick::ResetForTest();
  g_image.Drop();
  sentinel::SetLogSink(nullptr);
  if (g_fail == 0 && quest_test::Failures() == 0) std::printf("powerboost_test: all checks passed\n");
  return g_fail == 0 && quest_test::Failures() == 0 ? 0 : 1;
}
