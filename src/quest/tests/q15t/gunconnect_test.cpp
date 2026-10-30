// Host test of gunconnect.cpp against a fake CR15Game: the two gamespace tables, one or two gun component systems with
// the layout the module reads (cs+0xf2 count, +0xf8 instances of 0x168, +0xb8 -> props of 0xa0, +0x1c0 ring), a fake
// CR15NetGame member table. Real SafeRead, a fake clock; Connect, UserActor, HasTag and the combat tag are fakes.
#include <dirent.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nevr/plugin_interface.h"
#include "gunconnect.h"
#include "plugin_log.h"

static int g_fail = 0;
#define CHECK(cond, ...)                                \
  do {                                                  \
    if (!(cond)) {                                      \
      ++g_fail;                                         \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__);  \
      std::printf(__VA_ARGS__);                         \
      std::printf("\n");                                \
    }                                                   \
  } while (0)

struct Line {
  uint32_t level;
  std::string text;
};
static std::vector<Line> g_lines;
static void FakeLog(const char* id, uint32_t level, const char* msg, const NevrLogField* f, uint32_t n) {
  std::string t = std::string("plugin=") + id + " " + msg;
  for (uint32_t i = 0; i < n; ++i) t += std::string(" ") + f[i].key + "=" + f[i].value;
  g_lines.push_back({level, t});
}
static int CountLines(const char* a, const char* b = "", int level = -1) {
  int n = 0;
  for (const auto& l : g_lines)
    if (l.text.find(a) != std::string::npos && l.text.find(b) != std::string::npos && (level < 0 || (int)l.level == level)) ++n;
  return n;
}
static bool HasLine(const char* needle, int level = -1) { return CountLines(needle, "", level) > 0; }

static const uintptr_t kBase = 0x7000000000ull;
static const uintptr_t kGunVptrVaddr = 0x3696050;
static const uint64_t kGunKey = 0x7a804a0a9b2f5386ull;
static const uint32_t kUser = 3u | (7u << 16);  // slot 3, generation 7

template <typename T>
static void Put(void* base, size_t off, T v) {
  std::memcpy(static_cast<char*>(base) + off, &v, sizeof v);
}
template <typename T>
static T Get(const void* base, size_t off) {
  T v;
  std::memcpy(&v, static_cast<const char*>(base) + off, sizeof v);
  return v;
}

// ---- the fake world ----
struct GunCs {
  alignas(16) uintptr_t cs[0x200 / 8];
  uintptr_t props_ptr;
  alignas(16) char insts[4][0x168];
  alignas(16) char props[4][0xa0];
};
alignas(16) static uintptr_t g_game[0x9000 / 8];
alignas(16) static uintptr_t g_gs[0x500 / 8];
alignas(16) static uintptr_t g_table[2];
alignas(16) static uintptr_t g_sys[4];
alignas(16) static uintptr_t g_net[0x400 / 8];
static GunCs g_gc[2];
static uint64_t g_tag_sym = 0x1122334455667788ull;

static bool g_tag_val = true, g_connect_works = true;
static int g_has_tag_calls = 0, g_user_actor_calls = 0;
static void* g_ua_net = nullptr;
static uint16_t g_ua_midx = 0xffff;
static void* g_tag_game = nullptr;
static q15t::GunGuid g_tag_guid{0, 0};
static uint64_t g_tag_seen = 0;
struct ConnectCall {
  int cs;
  uint32_t user;
  double at;
};
static std::vector<ConnectCall> g_connects;
static double g_now = 100.0;
static double Clock() { return g_now; }

static void FConnect(void* cs, uint32_t user) {
  int k = cs == static_cast<void*>(g_gc[0].cs) ? 0 : 1;
  g_connects.push_back({k, user, g_now});
  if (!g_connect_works) return;
  for (int i = 0; i < 4 && i < Get<uint16_t>(g_gc[k].cs, 0xf2); ++i)
    if (Get<uint64_t>(g_gc[k].props[i], 0x20) == (user & 0xffff) && Get<uint32_t>(g_gc[k].props[i], 0x9c) != 0) Put<uint32_t>(g_gc[k].insts[i], 0xd8, 0);
}
static q15t::GunGuid FUserActor(void* net, uint16_t midx) {
  ++g_user_actor_calls;
  g_ua_net = net;
  g_ua_midx = midx;
  return q15t::GunGuid{0x1111u + midx, 0x2222u};
}
static bool FHasTag(void* game, const q15t::GunGuid* guid, uint64_t tag) {
  ++g_has_tag_calls;
  g_tag_game = game;
  g_tag_guid = *guid;
  g_tag_seen = tag;
  return g_tag_val;
}

static void Gun(int k, int i, uint64_t owner, uint32_t flag, uint32_t state) {
  Put<uint16_t>(g_gc[k].insts[i], 0xa0, static_cast<uint16_t>(i));
  Put<uint32_t>(g_gc[k].insts[i], 0xd8, state);
  Put<uint64_t>(g_gc[k].props[i], 0x20, owner);
  Put<uint32_t>(g_gc[k].props[i], 0x9c, flag);
}
static void BuildWorld(int ncs) {
  std::memset(g_game, 0, sizeof g_game);
  std::memset(g_gs, 0, sizeof g_gs);
  std::memset(g_sys, 0, sizeof g_sys);
  std::memset(g_net, 0, sizeof g_net);
  std::memset(g_gc, 0, sizeof g_gc);
  const uintptr_t game = reinterpret_cast<uintptr_t>(g_game), gs = reinterpret_cast<uintptr_t>(g_gs);
  g_table[0] = 0x5555;
  g_table[1] = gs;
  g_game[0x1f48 / 8] = reinterpret_cast<uintptr_t>(g_table);
  g_game[0x1f78 / 8] = 1;
  g_gs[0x8 / 8] = game;
  g_gs[0x420 / 8] = reinterpret_cast<uintptr_t>(g_sys);
  g_gs[0x450 / 8] = static_cast<uintptr_t>(ncs);
  for (int k = 0; k < ncs; ++k) {
    g_sys[2 * k] = kGunKey;
    g_sys[2 * k + 1] = reinterpret_cast<uintptr_t>(g_gc[k].cs);
    g_gc[k].cs[0] = kBase + kGunVptrVaddr;
    g_gc[k].cs[0x80 / 8] = gs;
    Put<uint16_t>(g_gc[k].cs, 0xf2, 2);
    Put<uint32_t>(g_gc[k].cs, 0x1c0, 0);
    g_gc[k].cs[0xf8 / 8] = reinterpret_cast<uintptr_t>(g_gc[k].insts);
    g_gc[k].props_ptr = reinterpret_cast<uintptr_t>(g_gc[k].props);
    g_gc[k].cs[0xb8 / 8] = reinterpret_cast<uintptr_t>(&g_gc[k].props_ptr);
    Gun(k, 0, 3, 1, 3);  // two local guns, never connected (state 3)
    Gun(k, 1, 3, 1, 1);  // and one disconnected (state 1)
  }
  g_game[0x8510 / 8] = reinterpret_cast<uintptr_t>(g_net);
  Put<uint16_t>(g_net, 0xde, 1);
  Put<uint16_t>(g_net, 0xdc, 1);
  Put<uint16_t>(g_net, 0x178, 0x00e3);  // & 0x1f = 3
  Put<uint16_t>(g_net, 0x13a + 3 * 4, 7);
  Put<uint16_t>(g_net, 0x138 + 3 * 4, 0);
  Put<uint16_t>(g_net, 0xe2, 2);
}

static std::string g_dir;
static void ClearDir() {
  DIR* d = opendir(g_dir.c_str());
  if (d == nullptr) return;
  while (dirent* e = readdir(d))
    if (e->d_name[0] != '.') unlink((g_dir + "/" + e->d_name).c_str());
  closedir(d);
}

static q15t::GunConnectFns Fns() {
  q15t::GunConnectFns f{};
  f.connect = &FConnect;
  f.user_actor = &FUserActor;
  f.has_tag = &FHasTag;
  f.tag_combat_addr = reinterpret_cast<uintptr_t>(&g_tag_sym);
  return f;
}
static void Reset(int ncs) {
  g_lines.clear();
  ClearDir();
  BuildWorld(ncs);
  g_tag_val = true;
  g_connect_works = true;
  g_tag_sym = 0x1122334455667788ull;
  g_has_tag_calls = g_user_actor_calls = 0;
  g_connects.clear();
  g_now = 100.0;
  q15t::ResetGunConnectForTest();
  q15t::InitGunConnect(kBase, Fns());
}
static void Tick() {
  q15t::GunConnectTick(reinterpret_cast<uintptr_t>(g_game));
  g_now += 0.0625;  // exact in binary: a pass every 0.25 s lands on a multiple of 0.25
}
static void Run(double seconds) {
  const double end = g_now + seconds;
  while (g_now < end) Tick();
}

// the world's gun 0 of cs k, made unconnected again
static void Unconnect(int k) {
  for (int i = 0; i < 2; ++i) Put<uint32_t>(g_gc[k].insts[i], 0xd8, 3);
}
static bool ConnectedAll(int k) {
  return Get<uint32_t>(g_gc[k].insts[0], 0xd8) == 0 && Get<uint32_t>(g_gc[k].insts[1], 0xd8) == 0;
}

static void* g_lookup_wrong = nullptr;
static const char* g_lookup_wrong_sym = nullptr;
static uintptr_t g_lookup_base = 0;
static void* LookupRight(const char* s) {
  static const struct {
    const char* sym;
    uintptr_t vaddr;
  } t[] = {{"_ZN10NRadEngine8NRadGame12CR15NetGunCS7ConnectENS0_13SR15NetUserIdE", 0x2153438},
           {"_ZNK10NRadEngine8NRadGame11CR15NetGame9UserActorEt", 0x1251654},
           {"_ZN10NRadEngine6CTagCS6HasTagEPNS_10CncaGameMTERKNS_10SActorGUIDENS_9CSymbol64E", 0x1723760},
           {"_ZN10NRadEngine8NRadGame14CR15ActorUtils15kActorTagCombatE", 0x37af288}};
  for (const auto& e : t)
    if (std::strcmp(s, e.sym) == 0) return std::strcmp(s, g_lookup_wrong_sym != nullptr ? g_lookup_wrong_sym : "") == 0 ? g_lookup_wrong : reinterpret_cast<void*>(g_lookup_base + e.vaddr);
  return nullptr;
}

int main() {
  char tmpl[] = "/tmp/q15t-gc-XXXXXX";
  if (mkdtemp(tmpl) == nullptr) return 2;
  g_dir = tmpl;
  NvrGameContext ctx{};
  ctx.ctx_size = sizeof ctx;
  ctx.plugin_id = "q15t";
  ctx.log = &FakeLog;
  q15t::SetLogContext(&ctx);
  q15t::SetGunConnectClockForTest(&Clock);
  q15t::SetGunConnectDirForTest(g_dir.c_str());

  // 1. the fix: nothing before the 2 s hold, then one Connect(cs, slot | gen << 16), then "connected" within a quarter second
  Reset(1);
  Run(1.9);
  CHECK(g_connects.empty(), "Connect before the 2 s hold: %zu", g_connects.size());
  CHECK(HasLine("gunconnect on") && HasLine("gunconnect gun cs"), "no start / gun cs records");
  Run(0.3);
  CHECK(g_connects.size() == 1, "calls %zu", g_connects.size());
  CHECK(g_connects.size() == 1 && g_connects[0].user == kUser && g_connects[0].cs == 0, "user %x", g_connects.empty() ? 0 : g_connects[0].user);
  CHECK(g_connects.size() == 1 && g_connects[0].at == 102.0, "first Connect at %.4f, wanted 102.0", g_connects.empty() ? 0.0 : g_connects[0].at);
  CHECK(CountLines("gunconnect Connect", "tries=1 ") == 1 && HasLine("detail=user 70003 slot 3 gen 7 midx 0 actor 0000000000002222"), "no Connect record");
  CHECK(g_ua_net == static_cast<void*>(g_net) && g_ua_midx == 0, "UserActor args");
  CHECK(g_tag_game == static_cast<void*>(g_game) && g_tag_seen == g_tag_sym && g_tag_guid.lo == 0x1111 && g_tag_guid.hi == 0x2222, "HasTag args");
  Run(0.5);
  CHECK(ConnectedAll(0) && CountLines("gunconnect connected", "tries=1 ") == 1, "no 'connected' record within 0.5 s of the Connect");
  Run(5);
  CHECK(g_connects.size() == 1, "Connect repeated after the guns connected: %zu", g_connects.size());

  // 2. Connect has no effect: retried every second, 10 times, then one WARNING and no more
  Reset(1);
  g_connect_works = false;
  Run(30);
  CHECK(g_connects.size() == 10, "tries: %zu, wanted 10", g_connects.size());
  for (size_t i = 0; i < g_connects.size(); ++i) CHECK(g_connects[i].at == 102.0 + (double)i, "try %zu at %.4f", i, g_connects[i].at);
  CHECK(CountLines("gunconnect gave up", "", 2) == 1, "no WARNING after the 10th try (%d)", CountLines("gunconnect gave up"));

  // 3. the connect ring is not empty: nothing; once it is empty the 2 s hold starts then
  Reset(1);
  Put<uint32_t>(g_gc[0].cs, 0x1c0, 1);
  Run(5);
  CHECK(g_connects.empty(), "Connect while the ring is not empty");
  const double t_ring = g_now;
  Put<uint32_t>(g_gc[0].cs, 0x1c0, 0);
  Run(3);
  CHECK(g_connects.size() == 1 && g_connects[0].at >= t_ring + 2.0 && g_connects[0].at <= t_ring + 2.3125, "ring cleared at %.4f, Connect at %.4f", t_ring,
        g_connects.empty() ? 0.0 : g_connects[0].at);

  // 4. the local actor has no combat tag: nothing; the hold restarts when the tag comes back
  Reset(1);
  g_tag_val = false;
  Run(5);
  CHECK(g_connects.empty() && g_has_tag_calls > 0, "Connect without the tag (calls %zu, tag checks %d)", g_connects.size(), g_has_tag_calls);
  g_tag_val = true;
  Run(1.5);
  g_tag_val = false;
  Run(0.5);
  g_tag_val = true;
  const double t_tag = g_now;
  Run(3);
  CHECK(g_connects.size() == 1 && g_connects[0].at >= t_tag + 2.0, "hold did not restart after the tag dropped (Connect at %.4f, tag back at %.4f)",
        g_connects.empty() ? 0.0 : g_connects[0].at, t_tag);
  Reset(1);
  g_tag_sym = 0;  // the tag symbol reads 0: no check is made
  Run(5);
  CHECK(g_connects.empty() && g_has_tag_calls == 0, "zero tag symbol used: calls %zu, checks %d", g_connects.size(), g_has_tag_calls);

  // 5. which guns count: only local (owner slot == local slot), flagged (props+0x9c), not connected (state != 0)
  Reset(1);
  Gun(0, 0, 9, 1, 3);  // someone else's
  Gun(0, 1, 3, 0, 3);  // local but flag 0
  Run(5);
  CHECK(g_connects.empty() && g_has_tag_calls == 0, "Connect for guns that are not local+flagged: %zu", g_connects.size());
  Gun(0, 1, 3, 1, 0);  // local, flagged, connected
  Run(5);
  CHECK(g_connects.empty() && g_has_tag_calls == 0, "Connect for a connected gun: %zu", g_connects.size());
  Gun(0, 1, 3, 1, 1);  // disconnected (1)
  Run(3);
  CHECK(g_connects.size() == 1, "state 1 (disconnected) not fixed: %zu", g_connects.size());
  Reset(1);
  Gun(0, 0, 3, 1, 0);
  Gun(0, 1, 3, 1, 3);  // never processed (3)
  Run(3);
  CHECK(g_connects.size() == 1, "state 3 (never processed) not fixed: %zu", g_connects.size());
  Reset(1);
  Put<uint16_t>(g_gc[0].cs, 0xf2, 4097);  // more guns than the bound: unreadable, nothing
  Run(5);
  CHECK(g_connects.empty(), "Connect for an out-of-bound gun count");

  // 6. the local member cannot be read: nothing, and the hold restarts when it can
  Reset(1);
  Run(1.5);
  Put<uint16_t>(g_net, 0xde, 0);
  Run(1);
  CHECK(g_connects.empty() && HasLine("netgame +0xde/+0xdc zero"), "no waiting record for a zero +0xde");
  Put<uint16_t>(g_net, 0xde, 1);
  const double t_ok = g_now;
  Run(3);
  CHECK(g_connects.size() == 1 && g_connects[0].at >= t_ok + 2.0, "hold survived an unreadable local member (Connect at %.4f, readable at %.4f)",
        g_connects.empty() ? 0.0 : g_connects[0].at, t_ok);
  struct Bad {
    const char* what;
    const char* reason;
  };
  const Bad bads[] = {{"netgame", "no netgame"},       {"dc", "netgame +0xde/+0xdc zero"}, {"midx", "member slot unused"},
                      {"gen", "member slot unused"},   {"range", "member index out of range"}};
  for (const Bad& b : bads) {
    Reset(1);
    if (!std::strcmp(b.what, "netgame")) g_game[0x8510 / 8] = 0;
    if (!std::strcmp(b.what, "dc")) Put<uint16_t>(g_net, 0xdc, 0);
    if (!std::strcmp(b.what, "midx")) Put<uint16_t>(g_net, 0x138 + 3 * 4, 0xffff);
    if (!std::strcmp(b.what, "gen")) Put<uint16_t>(g_net, 0x13a + 3 * 4, 0xffff);
    if (!std::strcmp(b.what, "range")) Put<uint16_t>(g_net, 0x138 + 3 * 4, 2);  // members == 2: index 2 is out
    Run(5);
    CHECK(g_connects.empty() && HasLine(b.reason) && g_user_actor_calls == 0, "%s: calls %zu, UserActor %d", b.what, g_connects.size(), g_user_actor_calls);
  }

  // 7. a gun CS that goes away (level change) is forgotten; a new one starts from scratch
  Reset(1);
  g_connect_works = false;
  Run(30);
  CHECK(g_connects.size() == 10, "setup: %zu", g_connects.size());
  g_gs[0x450 / 8] = 0;  // no component systems
  Run(2);
  g_gs[0x450 / 8] = 1;
  g_connect_works = true;
  Run(4);
  CHECK(g_connects.size() == 11 && ConnectedAll(0), "gun CS back after a level change not served: %zu", g_connects.size());
  CHECK(CountLines("gunconnect Connect", "tries=1 ") == 2, "tries not restarted for the new gun CS");

  // 8. two gun CSs keep their own state: both are served in the same pass
  Reset(2);
  Run(3);
  CHECK(g_connects.size() == 2 && g_connects[0].cs != g_connects[1].cs && g_connects[0].at == g_connects[1].at, "two gun CSs: %zu calls", g_connects.size());
  CHECK(ConnectedAll(0) && ConnectedAll(1), "not both connected");

  // 9. tries restart after the guns connected: a later disconnect gets try 1 again
  Reset(1);
  Run(3);
  CHECK(ConnectedAll(0), "setup");
  Run(2);
  Unconnect(0);
  Run(3);
  CHECK(g_connects.size() == 2 && CountLines("gunconnect Connect", "tries=1 ") == 2, "second disconnect: %zu calls, %d try-1 records", g_connects.size(),
        CountLines("gunconnect Connect", "tries=1 "));

  // 10. scan rate: 1 Hz while nothing is unconnected, 4 Hz while something is (or cannot be read)
  Reset(1);
  Put<uint32_t>(g_gc[0].insts[0], 0xd8, 0);
  Put<uint32_t>(g_gc[0].insts[1], 0xd8, 0);
  Run(10);
  CHECK(g_user_actor_calls >= 9 && g_user_actor_calls <= 11 && g_has_tag_calls == 0, "idle passes in 10 s: %d (tag checks %d)", g_user_actor_calls,
        g_has_tag_calls);
  Reset(1);
  Put<uint32_t>(g_gc[0].cs, 0x1c0, 1);  // unconnected, ring busy: 4 Hz, nothing sent
  Run(4);
  CHECK(g_user_actor_calls >= 15, "active passes in 4 s: %d", g_user_actor_calls);
  Reset(1);
  g_gc[0].cs[0xb8 / 8] = 0;  // the property table cannot be read
  Run(4);
  CHECK(g_connects.empty() && g_user_actor_calls >= 15, "unreadable gun CS: %zu calls, %d passes in 4 s", g_connects.size(), g_user_actor_calls);

  // 11. the gun CS is found by key, vptr and gamespace link only
  Reset(1);
  g_sys[0] = 0x1234;
  Run(5);
  CHECK(g_connects.empty(), "wrong key");
  Reset(1);
  g_gc[0].cs[0] = kBase + 0x1111;
  Run(5);
  CHECK(g_connects.empty(), "wrong vptr");

  // 12. gunconnect.off
  Reset(1);
  FILE* m = std::fopen((g_dir + "/gunconnect.off").c_str(), "w");
  std::fclose(m);
  Run(5);
  CHECK(g_connects.empty() && q15t::GunConnectPhase() == 2 && HasLine("gunconnect off") && HasLine("gunconnect.off present"), "gunconnect.off not honoured");

  // 13. no libr15 to resolve from (host): one ERROR, nothing runs
  Reset(1);
  q15t::InitGunConnect(kBase, q15t::GunConnectFns{});
  Run(5);
  CHECK(g_connects.empty() && q15t::GunConnectPhase() == 2 && HasLine("gunconnect off", 3), "missing library not loud");

  // 14. address resolution: fixed addresses (the four constants), exported names must agree with them
  {
    q15t::GunConnectFns f{};
    const char* why = "";
    CHECK(q15t::ResolveGunConnectFns(kBase, nullptr, &f, &why) && f.named == 0, "fixed-address resolve");
    CHECK(reinterpret_cast<uintptr_t>(f.connect) == kBase + 0x2153438, "Connect %llx", (unsigned long long)reinterpret_cast<uintptr_t>(f.connect));
    CHECK(reinterpret_cast<uintptr_t>(f.user_actor) == kBase + 0x1251654, "UserActor");
    CHECK(reinterpret_cast<uintptr_t>(f.has_tag) == kBase + 0x1723760, "HasTag");
    CHECK(f.tag_combat_addr == kBase + 0x37af288, "kActorTagCombat");
    g_lookup_base = kBase;
    g_lookup_wrong_sym = nullptr;
    CHECK(q15t::ResolveGunConnectFns(kBase, &LookupRight, &f, &why) && f.named == 4, "named resolve: %d", f.named);
    static const char* const syms[] = {"_ZN10NRadEngine8NRadGame12CR15NetGunCS7ConnectENS0_13SR15NetUserIdE", "_ZNK10NRadEngine8NRadGame11CR15NetGame9UserActorEt",
                                       "_ZN10NRadEngine6CTagCS6HasTagEPNS_10CncaGameMTERKNS_10SActorGUIDENS_9CSymbol64E",
                                       "_ZN10NRadEngine8NRadGame14CR15ActorUtils15kActorTagCombatE"};
    for (const char* s : syms) {
      g_lookup_wrong_sym = s;
      g_lookup_wrong = reinterpret_cast<void*>(kBase + 0x1000);
      q15t::GunConnectFns g{};
      const char* w = "";
      CHECK(!q15t::ResolveGunConnectFns(kBase, &LookupRight, &g, &w) && std::strcmp(w, s) == 0 && g.connect == nullptr, "a name at the wrong address accepted: %s", s);
    }
  }

  ClearDir();
  rmdir(g_dir.c_str());
  if (g_fail == 0) std::printf("gunconnect_test: all checks passed\n");
  return g_fail == 0 ? 0 : 1;
}
