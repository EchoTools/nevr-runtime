// Host test of gun connect (src/quest/combat/gunconnect.cpp, #2185; moved from the q15t plugin's gunconnect). A fake CR15Game: the two gamespace
// tables, one or two gun component systems with the layout the module reads (cs+0xf2 count, +0xf8 instances of 0x168, +0xb8 -> props of 0xa0, +0x1c0
// ring), a fake CR15NetGame member table. Real SafeRead, a fake clock; Connect, UserActor, HasTag and the combat tag are fakes.
//
// Moved: sections 1 to 14 are the plugin test's sections with the same assertions on the same behaviour (the hold, the retry spacing, the ring, the
// tag, which guns count, the unreadable member, a vanished gun CS, two systems, the scan rates, the key and the vptr, a missing library, the address
// resolution). What changed is the fixture: the plugin's fake slot page is the manager's test access (InstallResolved with the fakes; InstallAt
// with a libr15 image built in memory), the game now has a level (the module acts in a combat level only), and the lines are the module's
// `combat_gunconnect_*` records. New with the move: 15 the switch (off by default, nothing registered), 16 the kill file is not read and warns, 17 the
// scope (arena runs on the same APK: no Echo Arena level, lobby or transition ever reaches the game's functions), 18 the install on an image (build
// id, pinned words, a name that resolves elsewhere), 19 the cost a pass adds in an arena level. The scope is the real combat level answer
// (combat_scope.h, #2382): each test frame runs its refresh against the fake game before the module's callback, as the tick does, and a scope that
// cannot change refuses the install (`scope_not_live`).
//
// Built and run by `just test-quest-combat`.
#include "quest/combat/gunconnect.h"

#include <dirent.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "nevr/symbol_hash.h"
#include "quest/combat/combat_level.h"
#include "quest/combat/combat_scope.h"
#include "quest/combat/combat_tick.h"
#include "quest/combat/gamespace_targets.h"
#include "quest/combat/gunconnect_targets.h"
#include "quest/combat/legacy_switch.h"
#include "quest/combat/netgame_targets.h"
#include "quest/combat/safe_read.h"
#include "quest/sentinel/hook_log.h"
#include "quest/tests/combat_libr15_image.h"
#include "quest/tests/combat_pins.h"
#include "quest/tests/combat_scope_world.h"
#include "quest/tests/test_check.h"

namespace {
// Reads made through SafeRead: safe_read.h's test seam adds one to this counter for each (set in main).
std::atomic<std::uint64_t> g_safeReads{0};
std::uint64_t SafeReadCalls() { return g_safeReads.load(std::memory_order_relaxed); }

namespace gc = nevr_quest::combat::gunconnect;
namespace scope = nevr_quest::combat::scope;
namespace world = combat_test::scope_world;
namespace gt = nevr_quest::combat::gunconnect::targets;
using EchoVR::CSymbol64Hash;

#define CHECK(cond, ...)                                                           \
  do {                                                                             \
    if (!(cond)) {                                                                 \
      std::fprintf(stderr, "%s:%d CHECK failed: %s  ", __FILE__, __LINE__, #cond); \
      std::fprintf(stderr, __VA_ARGS__);                                           \
      std::fprintf(stderr, "\n");                                                  \
      ++quest_test::Failures();                                                    \
    }                                                                              \
  } while (0)

struct Line {
  sentinel::LogLevel level;
  std::string text;
};
std::vector<Line> g_lines;
void Capture(sentinel::LogLevel level, const char* line) { g_lines.push_back({level, line}); }
bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }
std::vector<Line> Lines(const char* event) {
  std::vector<Line> out;
  const std::string key = std::string("\"event\":\"") + event + "\"";
  for (const Line& l : g_lines) {
    if (Has(l.text, key)) out.push_back(l);
  }
  return out;
}
int Count(const char* event, const char* needle = "") {
  int n = 0;
  for (const Line& l : Lines(event)) n += Has(l.text, needle) ? 1 : 0;
  return n;
}
bool HasLine(const char* event, const char* needle = "") { return Count(event, needle) > 0; }

const std::uintptr_t kBase = 0x7000000000ull;
const std::uint32_t kUser = 3u | (7u << 16);  // slot 3, generation 7

template <typename T>
void Put(void* base, std::size_t off, T v) {
  std::memcpy(static_cast<char*>(base) + off, &v, sizeof v);
}
template <typename T>
T Get(const void* base, std::size_t off) {
  T v;
  std::memcpy(&v, static_cast<const char*>(base) + off, sizeof v);
  return v;
}

// ---- the fake world: the offsets are spelled here, not taken from the headers ----
struct GunCs {
  alignas(16) std::uintptr_t cs[0x200 / 8];
  std::uintptr_t propsPtr;
  alignas(16) char insts[4][0x168];
  alignas(16) char props[4][0xa0];
};
alignas(16) std::uintptr_t g_game[0x9000 / 8];
alignas(16) std::uintptr_t g_gs[0x500 / 8];
alignas(16) std::uintptr_t g_table[2];
alignas(16) std::uintptr_t g_sys[4];
alignas(16) std::uintptr_t g_net[0x400 / 8];
GunCs g_gc[2];
std::uint64_t g_tagSym = 0x1122334455667788ull;

bool g_tagVal = true, g_connectWorks = true;
int g_hasTagCalls = 0, g_userActorCalls = 0;
void* g_uaNet = nullptr;
std::uint16_t g_uaMidx = 0xffff;
void* g_tagGame = nullptr;
gc::Guid g_tagGuid{0, 0};
std::uint64_t g_tagSeen = 0;
struct ConnectCall {
  int cs;
  std::uint32_t user;
  double at;
};
std::vector<ConnectCall> g_connects;
double g_now = 100.0;
double Clock() { return g_now; }
// The combat level answer's clock (combat_scope.h) is the same fake clock, in ms.
std::int64_t ScopeClock() noexcept { return static_cast<std::int64_t>(g_now * 1000.0); }

void FConnect(void* cs, std::uint32_t user) {
  const int k = cs == static_cast<void*>(g_gc[0].cs) ? 0 : 1;
  g_connects.push_back({k, user, g_now});
  if (!g_connectWorks) return;
  for (int i = 0; i < 4 && i < Get<std::uint16_t>(g_gc[k].cs, 0xf2); ++i) {
    if (Get<std::uint64_t>(g_gc[k].props[i], 0x20) == (user & 0xffff) && Get<std::uint32_t>(g_gc[k].props[i], 0x9c) != 0) Put<std::uint32_t>(g_gc[k].insts[i], 0xd8, 0);
  }
}
gc::Guid FUserActor(void* net, std::uint16_t midx) {
  ++g_userActorCalls;
  g_uaNet = net;
  g_uaMidx = midx;
  return gc::Guid{0x1111u + midx, 0x2222u};
}
bool FHasTag(void* game, const gc::Guid* guid, std::uint64_t tag) {
  ++g_hasTagCalls;
  g_tagGame = game;
  g_tagGuid = *guid;
  g_tagSeen = tag;
  return g_tagVal;
}

void Gun(int k, int i, std::uint64_t owner, std::uint32_t flag, std::uint32_t state) {
  Put<std::uint16_t>(g_gc[k].insts[i], 0xa0, static_cast<std::uint16_t>(i));
  Put<std::uint32_t>(g_gc[k].insts[i], 0xd8, state);
  Put<std::uint64_t>(g_gc[k].props[i], 0x20, owner);
  Put<std::uint32_t>(g_gc[k].props[i], 0x9c, flag);
}

// The one gamespace of the game holds `level` (gs+0x58) and the gun component systems.
void BuildWorld(int ncs, const char* level = "mpl_combat_dyson") {
  std::memset(g_game, 0, sizeof g_game);
  std::memset(g_gs, 0, sizeof g_gs);
  std::memset(g_sys, 0, sizeof g_sys);
  std::memset(g_net, 0, sizeof g_net);
  std::memset(g_gc, 0, sizeof g_gc);
  const std::uintptr_t game = reinterpret_cast<std::uintptr_t>(g_game), gs = reinterpret_cast<std::uintptr_t>(g_gs);
  g_table[0] = 0x5555;
  g_table[1] = gs;
  g_game[0x1f48 / 8] = reinterpret_cast<std::uintptr_t>(g_table);
  g_game[0x1f78 / 8] = 1;
  g_gs[0x8 / 8] = game;
  g_gs[0x58 / 8] = static_cast<std::uintptr_t>(CSymbol64Hash(level));
  g_gs[0x420 / 8] = reinterpret_cast<std::uintptr_t>(g_sys);
  g_gs[0x450 / 8] = static_cast<std::uintptr_t>(ncs);
  for (int k = 0; k < ncs; ++k) {
    g_sys[2 * k] = 0x7a804a0a9b2f5386ull;
    g_sys[2 * k + 1] = reinterpret_cast<std::uintptr_t>(g_gc[k].cs);
    g_gc[k].cs[0] = kBase + 0x3696050;
    g_gc[k].cs[0x80 / 8] = gs;
    Put<std::uint16_t>(g_gc[k].cs, 0xf2, 2);
    Put<std::uint32_t>(g_gc[k].cs, 0x1c0, 0);
    g_gc[k].cs[0xf8 / 8] = reinterpret_cast<std::uintptr_t>(g_gc[k].insts);
    g_gc[k].propsPtr = reinterpret_cast<std::uintptr_t>(g_gc[k].props);
    g_gc[k].cs[0xb8 / 8] = reinterpret_cast<std::uintptr_t>(&g_gc[k].propsPtr);
    Gun(k, 0, 3, 1, 3);  // two local guns, never connected (state 3)
    Gun(k, 1, 3, 1, 1);  // and one disconnected (state 1)
  }
  g_game[0x8510 / 8] = reinterpret_cast<std::uintptr_t>(g_net);
  Put<std::uint16_t>(g_net, 0xde, 1);
  Put<std::uint16_t>(g_net, 0xdc, 1);
  Put<std::uint16_t>(g_net, 0x178, 0x00e3);  // & 0x1f = 3
  Put<std::uint16_t>(g_net, 0x13a + 3 * 4, 7);
  Put<std::uint16_t>(g_net, 0x138 + 3 * 4, 0);
  Put<std::uint16_t>(g_net, 0xe2, 2);
}

std::string g_dir;
void ClearDir() {
  DIR* d = opendir(g_dir.c_str());
  if (d == nullptr) return;
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] != '.') unlink((g_dir + "/" + e->d_name).c_str());
  }
  closedir(d);
}

gc::Fns Fns() {
  gc::Fns f{};
  f.connect = &FConnect;
  f.userActor = &FUserActor;
  f.hasTag = &FHasTag;
  f.tagCombatAddr = reinterpret_cast<std::uintptr_t>(&g_tagSym);
  return f;
}
// The module as the install step leaves it, with the fakes standing in for the game's functions.
void Reset(int ncs, const char* level = "mpl_combat_dyson") {
  g_lines.clear();
  ClearDir();
  BuildWorld(ncs, level);
  g_tagVal = true;
  g_connectWorks = true;
  g_tagSym = 0x1122334455667788ull;
  g_hasTagCalls = g_userActorCalls = 0;
  g_connects.clear();
  g_now = 100.0;
  nevr_quest::combat::tick::ResetForTest();
  scope::ResetForTest();
  scope::SetClockForTest(&ScopeClock);
  gc::ResetForTest();
  gc::SetClockForTest(&Clock);
  gc::SetLegacyDirForTest(g_dir.c_str());
  QCHECK(gc::InstallResolved(kBase, Fns()).installed);
}
// One frame of the game-thread tick: the combat level answer's refresh (the scope row registers it before this module's callback), then the module's.
void Tick() {
  scope::Refresh(reinterpret_cast<std::uintptr_t>(g_game));
  gc::Tick(reinterpret_cast<std::uintptr_t>(g_game));
  g_now += 0.0625;  // exact in binary: a pass every 0.25 s lands on a multiple of 0.25
}
void Run(double seconds) {
  const double end = g_now + seconds;
  while (g_now < end) Tick();
}

// the world's guns of cs k, made unconnected again
void Unconnect(int k) {
  for (int i = 0; i < 2; ++i) Put<std::uint32_t>(g_gc[k].insts[i], 0xd8, 3);
}
bool ConnectedAll(int k) { return Get<std::uint32_t>(g_gc[k].insts[0], 0xd8) == 0 && Get<std::uint32_t>(g_gc[k].insts[1], 0xd8) == 0; }

const char* g_wrongSym = nullptr;
void* g_wrongAt = nullptr;
std::uintptr_t g_lookupBase = 0;
void* LookupRight(const char* s) {
  static const struct {
    const char* sym;
    std::uintptr_t vaddr;
  } t[] = {{"_ZN10NRadEngine8NRadGame12CR15NetGunCS7ConnectENS0_13SR15NetUserIdE", 0x2153438},
           {"_ZNK10NRadEngine8NRadGame11CR15NetGame9UserActorEt", 0x1251654},
           {"_ZN10NRadEngine6CTagCS6HasTagEPNS_10CncaGameMTERKNS_10SActorGUIDENS_9CSymbol64E", 0x1723760},
           {"_ZN10NRadEngine8NRadGame14CR15ActorUtils15kActorTagCombatE", 0x37af288}};
  for (const auto& e : t) {
    if (std::strcmp(s, e.sym) == 0) return g_wrongSym != nullptr && std::strcmp(s, g_wrongSym) == 0 ? g_wrongAt : reinterpret_cast<void*>(g_lookupBase + e.vaddr);
  }
  return nullptr;
}
void* LookupNothing(const char*) { return nullptr; }

// ---- 1-14: the plugin test's sections ----
void TheFix() {  // 1. nothing before the 2 s hold, then one Connect(cs, slot | gen << 16), then "connected" within a quarter second
  Reset(1);
  Run(1.9);
  CHECK(g_connects.empty(), "Connect before the 2 s hold: %zu", g_connects.size());
  CHECK(HasLine("combat_gunconnect_install", "\"result\":\"installed\"") && HasLine("combat_gunconnect_gun_cs"), "no start / gun cs records");
  Run(0.3);
  CHECK(g_connects.size() == 1, "calls %zu", g_connects.size());
  CHECK(g_connects.size() == 1 && g_connects[0].user == kUser && g_connects[0].cs == 0, "user %x", g_connects.empty() ? 0 : g_connects[0].user);
  CHECK(g_connects.size() == 1 && g_connects[0].at == 102.0, "first Connect at %.4f, wanted 102.0", g_connects.empty() ? 0.0 : g_connects[0].at);
  CHECK(Count("combat_gunconnect_connect", "\"tries\":1,") == 1 && HasLine("combat_gunconnect_connect", "\"slot\":3,\"gen\":7,\"midx\":0") &&
            HasLine("combat_gunconnect_connect", "\"actor_hi\":\"0x0000000000002222\""),
        "no Connect record");
  CHECK(g_uaNet == static_cast<void*>(g_net) && g_uaMidx == 0, "UserActor args");
  CHECK(g_tagGame == static_cast<void*>(g_game) && g_tagSeen == g_tagSym && g_tagGuid.lo == 0x1111 && g_tagGuid.hi == 0x2222, "HasTag args");
  Run(0.5);
  CHECK(ConnectedAll(0) && Count("combat_gunconnect_connected", "\"tries\":1,") == 1, "no 'connected' record within 0.5 s of the Connect");
  Run(5);
  CHECK(g_connects.size() == 1, "Connect repeated after the guns connected: %zu", g_connects.size());
}

void RetryAndGiveUp() {  // 2. Connect has no effect: retried every second, 10 times, then one WARNING and no more
  Reset(1);
  g_connectWorks = false;
  Run(30);
  CHECK(g_connects.size() == 10, "tries: %zu, wanted 10", g_connects.size());
  for (std::size_t i = 0; i < g_connects.size(); ++i) CHECK(g_connects[i].at == 102.0 + static_cast<double>(i), "try %zu at %.4f", i, g_connects[i].at);
  CHECK(Lines("combat_gunconnect_gave_up").size() == 1 && Lines("combat_gunconnect_gave_up")[0].level == sentinel::LogLevel::kWarn,
        "no WARNING after the 10th try (%zu)", Lines("combat_gunconnect_gave_up").size());
}

void TheRing() {  // 3. the connect ring is not empty: nothing; once it is empty the 2 s hold starts then
  Reset(1);
  Put<std::uint32_t>(g_gc[0].cs, 0x1c0, 1);
  Run(5);
  CHECK(g_connects.empty(), "Connect while the ring is not empty");
  const double tRing = g_now;
  Put<std::uint32_t>(g_gc[0].cs, 0x1c0, 0);
  Run(3);
  CHECK(g_connects.size() == 1 && g_connects[0].at >= tRing + 2.0 && g_connects[0].at <= tRing + 2.3125, "ring cleared at %.4f, Connect at %.4f", tRing,
        g_connects.empty() ? 0.0 : g_connects[0].at);
  // The game loads the ring as a u64 (ExclusiveUpdateConnect): a count held in the upper half is not an empty ring.
  Reset(1);
  Put<std::uint64_t>(g_gc[0].cs, 0x1c0, 1ull << 32);
  Run(5);
  CHECK(g_connects.empty(), "Connect while the upper half of the ring count is set");
}

void TheTag() {  // 4. the local actor has no combat tag: nothing; the hold restarts when the tag comes back
  Reset(1);
  g_tagVal = false;
  Run(5);
  CHECK(g_connects.empty() && g_hasTagCalls > 0, "Connect without the tag (calls %zu, tag checks %d)", g_connects.size(), g_hasTagCalls);
  g_tagVal = true;
  Run(1.5);
  g_tagVal = false;
  Run(0.5);
  g_tagVal = true;
  const double tTag = g_now;
  Run(3);
  CHECK(g_connects.size() == 1 && g_connects[0].at >= tTag + 2.0, "hold did not restart after the tag dropped (Connect at %.4f, tag back at %.4f)",
        g_connects.empty() ? 0.0 : g_connects[0].at, tTag);
  Reset(1);
  g_tagSym = 0;  // the tag symbol reads 0: no check is made
  Run(5);
  CHECK(g_connects.empty() && g_hasTagCalls == 0, "zero tag symbol used: calls %zu, checks %d", g_connects.size(), g_hasTagCalls);
}

void WhichGunsCount() {  // 5. only local (owner slot == local slot), flagged (props+0x9c), not connected (state != 0)
  Reset(1);
  Gun(0, 0, 9, 1, 3);  // someone else's
  Gun(0, 1, 3, 0, 3);  // local but flag 0
  Run(5);
  CHECK(g_connects.empty() && g_hasTagCalls == 0, "Connect for guns that are not local+flagged: %zu", g_connects.size());
  Gun(0, 1, 3, 1, 0);  // local, flagged, connected
  Run(5);
  CHECK(g_connects.empty() && g_hasTagCalls == 0, "Connect for a connected gun: %zu", g_connects.size());
  Gun(0, 1, 3, 1, 1);  // disconnected (1)
  Run(3);
  CHECK(g_connects.size() == 1, "state 1 (disconnected) not fixed: %zu", g_connects.size());
  Reset(1);
  Gun(0, 0, 3, 1, 0);
  Gun(0, 1, 3, 1, 3);  // never processed (3)
  Run(3);
  CHECK(g_connects.size() == 1, "state 3 (never processed) not fixed: %zu", g_connects.size());
  Reset(1);
  Put<std::uint16_t>(g_gc[0].cs, 0xf2, 4097);  // more guns than the bound: unreadable, nothing
  Run(5);
  CHECK(g_connects.empty(), "Connect for an out-of-bound gun count");
}

void TheLocalMember() {  // 6. the local member cannot be read: nothing, and the hold restarts when it can
  Reset(1);
  Run(1.5);
  Put<std::uint16_t>(g_net, 0xde, 0);
  Run(1);
  CHECK(g_connects.empty() && HasLine("combat_gunconnect_waiting", "netgame +0xde/+0xdc zero"), "no waiting record for a zero +0xde");
  Put<std::uint16_t>(g_net, 0xde, 1);
  const double tOk = g_now;
  Run(3);
  CHECK(g_connects.size() == 1 && g_connects[0].at >= tOk + 2.0, "hold survived an unreadable local member (Connect at %.4f, readable at %.4f)",
        g_connects.empty() ? 0.0 : g_connects[0].at, tOk);
  struct Bad {
    const char* what;
    const char* reason;
  };
  const Bad bads[] = {{"netgame", "no netgame"},     {"dc", "netgame +0xde/+0xdc zero"}, {"midx", "member slot unused"},
                      {"gen", "member slot unused"}, {"range", "member index out of range"}};
  for (const Bad& b : bads) {
    Reset(1);
    if (!std::strcmp(b.what, "netgame")) g_game[0x8510 / 8] = 0;
    if (!std::strcmp(b.what, "dc")) Put<std::uint16_t>(g_net, 0xdc, 0);
    if (!std::strcmp(b.what, "midx")) Put<std::uint16_t>(g_net, 0x138 + 3 * 4, 0xffff);
    if (!std::strcmp(b.what, "gen")) Put<std::uint16_t>(g_net, 0x13a + 3 * 4, 0xffff);
    if (!std::strcmp(b.what, "range")) Put<std::uint16_t>(g_net, 0x138 + 3 * 4, 2);  // members == 2: index 2 is out
    Run(5);
    CHECK(g_connects.empty() && HasLine("combat_gunconnect_waiting", b.reason) && g_userActorCalls == 0, "%s: calls %zu, UserActor %d", b.what, g_connects.size(),
          g_userActorCalls);
  }
}

void AGunCsThatGoesAway() {  // 7. a gun CS that goes away (level change) is forgotten; a new one starts from scratch
  Reset(1);
  g_connectWorks = false;
  Run(30);
  CHECK(g_connects.size() == 10, "setup: %zu", g_connects.size());
  g_gs[0x450 / 8] = 0;  // no component systems
  Run(2);
  g_gs[0x450 / 8] = 1;
  g_connectWorks = true;
  Run(4);
  CHECK(g_connects.size() == 11 && ConnectedAll(0), "gun CS back after a level change not served: %zu", g_connects.size());
  CHECK(Count("combat_gunconnect_connect", "\"tries\":1,") == 2, "tries not restarted for the new gun CS");
}

void TwoGunSystems() {  // 8. two gun CSs keep their own state: both are served in the same pass
  Reset(2);
  Run(3);
  CHECK(g_connects.size() == 2 && g_connects[0].cs != g_connects[1].cs && g_connects[0].at == g_connects[1].at, "two gun CSs: %zu calls", g_connects.size());
  CHECK(ConnectedAll(0) && ConnectedAll(1), "not both connected");
}

void TriesRestart() {  // 9. tries restart after the guns connected: a later disconnect gets try 1 again
  Reset(1);
  Run(3);
  CHECK(ConnectedAll(0), "setup");
  Run(2);
  Unconnect(0);
  Run(3);
  CHECK(g_connects.size() == 2 && Count("combat_gunconnect_connect", "\"tries\":1,") == 2, "second disconnect: %zu calls, %d try-1 records", g_connects.size(),
        Count("combat_gunconnect_connect", "\"tries\":1,"));
}

void ScanRates() {  // 10. scan rate: 1 Hz while nothing is unconnected, 4 Hz while something is (or cannot be read)
  Reset(1);
  Put<std::uint32_t>(g_gc[0].insts[0], 0xd8, 0);
  Put<std::uint32_t>(g_gc[0].insts[1], 0xd8, 0);
  Run(10);
  CHECK(g_userActorCalls >= 9 && g_userActorCalls <= 11 && g_hasTagCalls == 0, "idle passes in 10 s: %d (tag checks %d)", g_userActorCalls, g_hasTagCalls);
  Reset(1);
  Put<std::uint32_t>(g_gc[0].cs, 0x1c0, 1);  // unconnected, ring busy: 4 Hz, nothing sent
  Run(4);
  CHECK(g_userActorCalls >= 15, "active passes in 4 s: %d", g_userActorCalls);
  Reset(1);
  g_gc[0].cs[0xb8 / 8] = 0;  // the property table cannot be read
  Run(4);
  CHECK(g_connects.empty() && g_userActorCalls >= 15, "unreadable gun CS: %zu calls, %d passes in 4 s", g_connects.size(), g_userActorCalls);
}

void KeyAndVptr() {  // 11. the gun CS is found by key, vptr and gamespace link only
  Reset(1);
  g_sys[0] = 0x1234;
  Run(5);
  CHECK(g_connects.empty(), "wrong key");
  Reset(1);
  g_gc[0].cs[0] = kBase + 0x1111;
  Run(5);
  CHECK(g_connects.empty(), "wrong vptr");
}

// A libr15 image with the tick's slot row, on which the tick is installed and the scope row registers its refresh: the combat level answer can
// change, as the shipped table leaves it before this module's row.
combat_test::Image g_tickImage;
bool TickImageLookup(const char* module, sentinel::ElfImage* out) {
  if (!g_tickImage.Present() || std::strcmp(module, sentinel::pinned::kLibR15) != 0) return false;
  *out = g_tickImage.Elf();
  return true;
}
void StartTheScope() {
  nevr_quest::combat::tick::ResetForTest();
  scope::ResetForTest();
  scope::SetClockForTest(&ScopeClock);
  g_tickImage.Build({world::TickRow()});
  world::PutTickSlot(g_tickImage);
  QCHECK(world::StartScope(&TickImageLookup));
}

void NoLibrary() {  // 13. no libr15 to resolve from (the host): one ERROR, nothing runs
  g_lines.clear();
  StartTheScope();
  gc::ResetForTest();
  gc::SetClockForTest(&Clock);
  const nevr_quest::combat::ModuleResult r = gc::InstallAt([](const char*, sentinel::ElfImage*) { return false; }, &LookupNothing, true);
  BuildWorld(1);
  Run(5);
  CHECK(!r.installed && gc::Phase() == 2 && g_connects.empty() && nevr_quest::combat::tick::Count() == 1, "missing library registered a tick");
  CHECK(Lines("combat_gunconnect_install").size() == 1 && Lines("combat_gunconnect_install")[0].level == sentinel::LogLevel::kError &&
            HasLine("combat_gunconnect_install", "\"reason\":\"module_not_loaded\""),
        "missing library not loud");
  g_tickImage.Drop();
}

// The combat level answer cannot change (the tick is not installed, or the scope row did not register its refresh): the pass would never act, so the
// module is not installed, says why at Error and registers nothing. Off by the key wins: nothing is asked of the scope.
void AScopeThatCannotChangeRefusesTheInstall() {
  g_lines.clear();
  nevr_quest::combat::tick::ResetForTest();
  scope::ResetForTest();
  gc::ResetForTest();
  nevr_quest::combat::ModuleResult r = gc::InstallAt([](const char*, sentinel::ElfImage*) { return true; }, &LookupNothing, true);
  CHECK(!r.installed && !r.off && std::strcmp(r.status, "scope_not_live") == 0 && gc::Phase() == 2 && nevr_quest::combat::tick::Count() == 0,
        "a scope that cannot change: %s", r.status);
  CHECK(Lines("combat_gunconnect_install").size() == 1 && Lines("combat_gunconnect_install")[0].level == sentinel::LogLevel::kError &&
            HasLine("combat_gunconnect_install", "\"reason\":\"scope_not_live\""),
        "scope_not_live not loud");
  // the tick is installed but no row registered the refresh
  g_lines.clear();
  g_tickImage.Build({world::TickRow()});
  world::PutTickSlot(g_tickImage);
  QCHECK(nevr_quest::combat::tick::InstallAt(&TickImageLookup));
  gc::ResetForTest();
  r = gc::InstallAt([](const char*, sentinel::ElfImage*) { return true; }, &LookupNothing, true);
  CHECK(!r.installed && std::strcmp(r.status, "scope_not_live") == 0 && !scope::Live(), "an unregistered refresh: %s", r.status);
  // off by the key: off_config, whatever the scope
  gc::ResetForTest();
  r = gc::InstallAt([](const char*, sentinel::ElfImage*) { return true; }, &LookupNothing, false);
  CHECK(!r.installed && r.off && std::strcmp(r.status, "off_config") == 0, "off with a dead scope: %s", r.status);
  nevr_quest::combat::tick::ResetForTest();
  g_tickImage.Drop();
}

void AddressResolution() {  // 14. fixed addresses (the four constants), exported names must agree with them
  gc::Fns f{};
  const char* why = "";
  CHECK(gc::Resolve(kBase, nullptr, &f, &why) && f.named == 0, "fixed-address resolve");
  CHECK(reinterpret_cast<std::uintptr_t>(f.connect) == kBase + 0x2153438, "Connect %llx", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(f.connect)));
  CHECK(reinterpret_cast<std::uintptr_t>(f.userActor) == kBase + 0x1251654, "UserActor");
  CHECK(reinterpret_cast<std::uintptr_t>(f.hasTag) == kBase + 0x1723760, "HasTag");
  CHECK(f.tagCombatAddr == kBase + 0x37af288, "kActorTagCombat");
  g_lookupBase = kBase;
  g_wrongSym = nullptr;
  CHECK(gc::Resolve(kBase, &LookupRight, &f, &why) && f.named == 4, "named resolve: %d", f.named);
  static const char* const syms[] = {"_ZN10NRadEngine8NRadGame12CR15NetGunCS7ConnectENS0_13SR15NetUserIdE", "_ZNK10NRadEngine8NRadGame11CR15NetGame9UserActorEt",
                                     "_ZN10NRadEngine6CTagCS6HasTagEPNS_10CncaGameMTERKNS_10SActorGUIDENS_9CSymbol64E",
                                     "_ZN10NRadEngine8NRadGame14CR15ActorUtils15kActorTagCombatE"};
  for (const char* s : syms) {
    g_wrongSym = s;
    g_wrongAt = reinterpret_cast<void*>(kBase + 0x1000);
    gc::Fns g{};
    const char* w = "";
    CHECK(!gc::Resolve(kBase, &LookupRight, &g, &w) && std::strcmp(w, s) == 0 && g.connect == nullptr, "a name at the wrong address accepted: %s", s);
  }
  g_wrongSym = nullptr;
}

// ---- 15-19: new with the move ----
void TheSwitchIsOffByDefault() {  // 15
  g_lines.clear();
  nevr_quest::combat::tick::ResetForTest();
  gc::ResetForTest();
  gc::SetLegacyDirForTest(g_dir.c_str());
  const nevr_quest::combat::ModuleResult r = gc::InstallAt([](const char*, sentinel::ElfImage*) { return true; }, &LookupNothing, false);
  CHECK(!r.installed && r.off && std::strcmp(r.status, "off_config") == 0, "off result");
  CHECK(gc::Phase() == 2 && nevr_quest::combat::tick::Count() == 0, "an off module registered a tick");
  const std::vector<Line> install = Lines("combat_gunconnect_install");
  CHECK(install.size() == 1 && install[0].level == sentinel::LogLevel::kInfo && Has(install[0].text, "\"result\":\"off\"") &&
            Has(install[0].text, "\"reason\":\"off_config\"") && Has(install[0].text, "\"switch\":\"combat.gunconnect\""),
        "off line");
  CHECK(g_lines.size() == 1, "an off module wrote %zu lines", g_lines.size());
}

void TheKillFileIsNotReadAndWarns() {  // 16
  Reset(1);
  const std::string path = g_dir + "/gunconnect.off";
  std::FILE* m = std::fopen(path.c_str(), "w");
  std::fclose(m);
  g_lines.clear();
  nevr_quest::combat::tick::ResetForTest();
  gc::ResetForTest();
  gc::SetClockForTest(&Clock);
  gc::SetLegacyDirForTest(g_dir.c_str());
  gc::InstallAt([](const char*, sentinel::ElfImage*) { return false; }, &LookupNothing, true);  // refused here (no image): the warning comes first
  const std::vector<Line> legacy = Lines("combat_gunconnect_legacy_switch");
  CHECK(legacy.size() == 1 && legacy[0].level == sentinel::LogLevel::kWarn && Has(legacy[0].text, "\"file\":\"gunconnect.off\"") &&
            Has(legacy[0].text, "\"replaced_by\":\"combat.gunconnect\""),
        "no legacy warning");
  // the file does not turn the module off: with the module installed it runs
  Reset(1);
  m = std::fopen(path.c_str(), "w");
  std::fclose(m);
  Run(5);
  CHECK(g_connects.size() == 1 && gc::Phase() == 1, "the plugin's kill file still turned the module off");
}


// The reads a level scan makes on this fake game (measured, not assumed), so a path that must read nothing but its scope test can be counted exactly.
std::uint32_t ReadsPerScan(std::uintptr_t game) {
  const std::uint64_t before = SafeReadCalls();
  nevr_quest::combat::level::Scan(game);
  return SafeReadCalls() - before;
}

void ArenaAndOtherLevelsAreLeftAlone() {  // 17
  const char* quiet[] = {"mpl_arena_a", "mpl_arena_a_lowspec", "mpl_arena_b", "mpl_tutorial_a", "mpl_lobby_b", "mpl_lobby_b2", "mpl_lobby_b_social",
                         "mpl_lobby_b_arena"};
  for (const char* name : quiet) {
    Reset(1, name);
    const std::uint32_t perScan = ReadsPerScan(reinterpret_cast<std::uintptr_t>(g_game));
    const std::uint64_t reads0 = SafeReadCalls(), scans0 = nevr_quest::combat::level::ScanCalls();
    Run(10);
    CHECK(g_connects.empty() && g_hasTagCalls == 0 && g_userActorCalls == 0, "%s: Connect %zu, HasTag %d, UserActor %d", name, g_connects.size(), g_hasTagCalls,
          g_userActorCalls);
    // By count: the only scans are the combat level answer's own looks (combat_scope.h), and nothing else reads: a pass reads no level, no member,
    // no gun system and no table of its own.
    const std::uint64_t scans = nevr_quest::combat::level::ScanCalls() - scans0, reads = SafeReadCalls() - reads0;
    CHECK(scans == scope::Looks() && scans >= 9, "%s: %llu scans for %u looks", name, static_cast<unsigned long long>(scans), scope::Looks());
    CHECK(reads == scans * perScan, "%s: %llu reads for %llu scans of %u reads each", name, static_cast<unsigned long long>(reads), static_cast<unsigned long long>(scans), perScan);
    CHECK(Lines("combat_gunconnect_gun_cs").empty() && Lines("combat_gunconnect_waiting").empty() && Count("combat_gunconnect_install") == 1,
          "%s: the module wrote a line", name);
    CHECK(gc::CurrentStats().scopedOut >= 9 && gc::CurrentStats().passes >= 9, "%s: passes %u scoped out %u", name, gc::CurrentStats().passes,
          gc::CurrentStats().scopedOut);
  }
  // A transition: an arena level is still loaded beside a combat map. The arena level wins.
  Reset(1, "mpl_combat_dyson");
  {
    static std::uintptr_t gs2[0x100 / 8];
    std::memset(gs2, 0, sizeof gs2);
    gs2[0x8 / 8] = reinterpret_cast<std::uintptr_t>(g_game);
    gs2[0x58 / 8] = static_cast<std::uintptr_t>(CSymbol64Hash("mpl_arena_a"));
    static std::uintptr_t table2[4];
    table2[0] = g_table[0];
    table2[1] = g_table[1];
    table2[2] = 0x6666;
    table2[3] = reinterpret_cast<std::uintptr_t>(gs2);
    g_game[0x1f48 / 8] = reinterpret_cast<std::uintptr_t>(table2);
    g_game[0x1f78 / 8] = 2;
    Run(10);
    CHECK(g_connects.empty() && g_userActorCalls == 0, "a transition holding an arena level reached the game: %zu connects", g_connects.size());
  }
  // The combat lobby and every combat map act.
  for (const char* name : {"mpl_combat_dyson", "mpl_combat_combustion", "mpl_combat_fission", "mpl_combat_gauss", "mpl_lobby_b_combat"}) {
    Reset(1, name);
    Run(3);
    CHECK(g_connects.size() == 1, "%s: %zu connects", name, g_connects.size());
  }
  // The tick stops (nothing refreshes the answer): a combat map seen a second ago is no longer believed, so nothing is connected however long the
  // module's own callback keeps running (combat_scope.h, kStaleAfterMs).
  Reset(1, "mpl_combat_dyson");
  Tick();  // one look: a combat map
  for (int i = 0; i < 160; ++i) {  // 10 s of the module's callback alone
    gc::Tick(reinterpret_cast<std::uintptr_t>(g_game));
    g_now += 0.0625;
  }
  // In the first second the look is believed and the hold starts; after it every pass (1 Hz) is scoped out, so the 2 s hold never ends in a Connect.
  CHECK(g_connects.empty() && gc::CurrentStats().scopedOut >= 8, "a stale answer acted: %zu connects, %u scoped out", g_connects.size(),
        gc::CurrentStats().scopedOut);
  // An unreadable game is no level.
  Reset(1);
  g_game[0x1f78 / 8] = 100000;  // absurd count
  Run(5);
  CHECK(g_connects.empty() && g_userActorCalls == 0, "an unreadable table acted");
}

void TheInstallOnAnImage() {  // 18
  combat_test::Image image;
  static combat_test::Image* sImage;
  sImage = &image;
  auto lookup = [](const char* module, sentinel::ElfImage* out) {
    if (!sImage->Present() || std::strcmp(module, sentinel::pinned::kLibR15) != 0) return false;
    *out = sImage->Elf();
    return true;
  };
  const nevr_quest::combat::pinned::Pins pins[] = {gt::kPins, gt::kConnectPins, nevr_quest::combat::netgame::kPins, nevr_quest::combat::gamespace::targets::kPins};
  auto put = [&] { combat_test::PutPins(image, pins, 4); };
  // The tick is installed on this image and the scope row registered its refresh (one tick callback), as the shipped table leaves it.
  auto begin = [&] {
    g_lines.clear();
    nevr_quest::combat::tick::ResetForTest();
    scope::ResetForTest();
    gc::ResetForTest();
    gc::SetLegacyDirForTest(g_dir.c_str());
    world::PutTickSlot(image);
    QCHECK(world::StartScope(lookup) && nevr_quest::combat::tick::Count() == 1);
  };
  // the pinned build with every word: installed, a tick registered, the names counted
  image.Build({world::TickRow()});
  put();
  begin();
  g_lookupBase = image.Base();
  g_wrongSym = nullptr;
  nevr_quest::combat::ModuleResult r = gc::InstallAt(lookup, &LookupRight, true);
  CHECK(r.installed && std::strcmp(r.status, "installed") == 0 && !r.off && gc::Phase() == 1 && nevr_quest::combat::tick::Count() == 2, "install on the pinned image");
  CHECK(HasLine("combat_gunconnect_install", "\"named\":4"), "named count not logged");
  // every word and record that differs refuses, names it, registers nothing
  const std::size_t bent = combat_test::ForEachBentPin(image, pins, 4, [&](const char* what) {
    begin();
    r = gc::InstallAt(lookup, &LookupRight, true);
    CHECK(!r.installed && !r.off && std::strcmp(r.status, "pinned_word_mismatch") == 0 && nevr_quest::combat::tick::Count() == 1 && HasLine("combat_gunconnect_pinned_word", what),
          "pin %s: %s", what, r.status);
  });
  CHECK(bent >= 30, "only %zu pins were bent", bent);
  // another build id (the scope was started on the pinned build; the tick keeps running when the image changes)
  image.Build({world::TickRow()});
  put();
  begin();
  image.Build({}, "0123456789abcdef0123456789abcdef01234567");
  put();
  r = gc::InstallAt(lookup, &LookupRight, true);
  CHECK(!r.installed && std::strcmp(r.status, "build_id_mismatch") == 0 && nevr_quest::combat::tick::Count() == 1, "another build id installed");
  image.Build({world::TickRow()});
  put();
  begin();
  image.Build({}, nullptr, /*note=*/false);
  put();
  r = gc::InstallAt(lookup, &LookupRight, true);
  CHECK(!r.installed && std::strcmp(r.status, "no_build_id") == 0, "no build id installed");
  // a name that resolves elsewhere
  image.Build({world::TickRow()});
  put();
  begin();
  g_lookupBase = image.Base();
  g_wrongSym = "_ZN10NRadEngine6CTagCS6HasTagEPNS_10CncaGameMTERKNS_10SActorGUIDENS_9CSymbol64E";
  g_wrongAt = reinterpret_cast<void*>(image.Base() + 0x1000);
  r = gc::InstallAt(lookup, &LookupRight, true);
  CHECK(!r.installed && std::strcmp(r.status, "export_elsewhere") == 0 && nevr_quest::combat::tick::Count() == 1, "a name elsewhere installed");
  g_wrongSym = nullptr;
  // the tick table full: refused
  begin();
  for (std::size_t i = 0; i < nevr_quest::combat::tick::kMaxCallbacks; ++i) nevr_quest::combat::tick::Add([](std::uintptr_t) noexcept {});
  r = gc::InstallAt(lookup, &LookupRight, true);
  CHECK(!r.installed && std::strcmp(r.status, "tick_table_full") == 0 && gc::Phase() == 2, "a full tick table installed");
  nevr_quest::combat::tick::ResetForTest();
  image.Drop();
}

double Seconds(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) { return std::chrono::duration<double>(b - a).count(); }

void TheCostInArena() {  // 19: printed, not asserted (a host number; the module's header says so)
  Reset(1, "mpl_arena_a");
  const int passes = 20000;
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < passes; ++i) {
    g_now += 2.0;  // every call is a due pass
    gc::Tick(reinterpret_cast<std::uintptr_t>(g_game));
  }
  const auto t1 = std::chrono::steady_clock::now();
  const int frames = 2000000;
  for (int i = 0; i < frames; ++i) gc::Tick(reinterpret_cast<std::uintptr_t>(g_game));  // the clock does not move: not due
  const auto t2 = std::chrono::steady_clock::now();
  std::printf("gunconnect_test: arena pass %.2f us (the scope word's load and test, 1 Hz), frame with no pass due %.1f ns (x86 host)\n", Seconds(t0, t1) / passes * 1e6,
              Seconds(t1, t2) / frames * 1e9);
  CHECK(gc::CurrentStats().scopedOut >= static_cast<std::uint32_t>(passes), "the passes were not all scoped out");
}

}  // namespace

int main() {
  nevr_quest::combat::SetReadCounterForTest(&g_safeReads);
  char tmpl[] = "/tmp/combat-gc-XXXXXX";
  if (mkdtemp(tmpl) == nullptr) return 2;
  g_dir = tmpl;
  sentinel::SetLogSink(&Capture);
  TheFix();
  RetryAndGiveUp();
  TheRing();
  TheTag();
  WhichGunsCount();
  TheLocalMember();
  AGunCsThatGoesAway();
  TwoGunSystems();
  TriesRestart();
  ScanRates();
  KeyAndVptr();
  NoLibrary();
  AScopeThatCannotChangeRefusesTheInstall();
  AddressResolution();
  TheSwitchIsOffByDefault();
  TheKillFileIsNotReadAndWarns();
  ArenaAndOtherLevelsAreLeftAlone();
  TheInstallOnAnImage();
  TheCostInArena();
  ClearDir();
  rmdir(g_dir.c_str());
  sentinel::SetLogSink(nullptr);
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "gunconnect_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("gunconnect_test: all checks passed\n");
  return 0;
}
