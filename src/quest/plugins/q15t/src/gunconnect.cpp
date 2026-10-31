#include "gunconnect.h"

#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "gamespace.h"
#include "plugin_log.h"
#include "safe_read.h"

namespace q15t {
namespace {

// ELF vaddrs of the pinned build (libr15 b243509c, disassembly VA = ELF vaddr + 0x100000). Source constants:
// <source build>/the source build's gunconnect:91-92. disassembly: Connect 0x2253438 (size 484), UserActor
// 0x1351654 (56), HasTag 0x1823760 (148), symbol kActorTagCombat 0x38af288; gun CS vtable 0x3696040 + 0x10 (readelf GLOB_DAT).
constexpr std::uintptr_t kFnConnect = 0x2153438, kFnHasTag = 0x1723760, kFnUserActor = 0x1251654, kTagCombat = 0x37af288;
constexpr std::uintptr_t kGunCsVptr = 0x3696050;
constexpr std::uint64_t kGunCsKey = 0x7a804a0a9b2f5386ull;  // source constant, not re-read in the disassembly (see docs/quest/q15t/modules/gunconnect.md)
constexpr int kMaxGuns = 4096, kMaxCs = 8;
constexpr double kFixEveryS = 0.25, kIdleEveryS = 1.0, kHoldS = 2.0, kRetryS = 1.0;
constexpr int kMaxTries = 10;

const char kSymConnect[] = "_ZN10NRadEngine8NRadGame12CR15NetGunCS7ConnectENS0_13SR15NetUserIdE";
const char kSymUserActor[] = "_ZNK10NRadEngine8NRadGame11CR15NetGame9UserActorEt";
const char kSymHasTag[] = "_ZN10NRadEngine6CTagCS6HasTagEPNS_10CncaGameMTERKNS_10SActorGUIDENS_9CSymbol64E";
const char kSymTagCombat[] = "_ZN10NRadEngine8NRadGame14CR15ActorUtils15kActorTagCombatE";

struct FixState {
  std::uintptr_t cs = 0;
  double since = 0, next_try = 0;
  int tries = 0;
  bool gave_up = false;
  bool seen = false;
};

struct Local {
  bool ok = false;
  const char* why = "";
  std::uintptr_t net = 0;
  std::uint32_t slot = 0, gen = 0, midx = 0, user_id = 0;
  GunGuid guid{0, 0};
};

struct GunCounts {
  bool ok = true;
  std::uint32_t guns = 0, local = 0, unconnected = 0, ring = 0;
};

GunConnectFns g_f{};
bool g_have_fns = false;
int g_phase = 0;
std::uintptr_t g_base = 0;
FixState g_st[kMaxCs];
double g_next_fix = 0;
char g_why[96] = "", g_dir_override[256] = "";
double (*g_clock)() = nullptr;
void* g_lib = nullptr;

double Now() {
  if (g_clock != nullptr) return g_clock();
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

void* LibLookup(const char* symbol) { return g_lib != nullptr ? dlsym(g_lib, symbol) : nullptr; }

bool Pick(void* (*lookup)(const char*), const char* name, std::uintptr_t want, std::uintptr_t* out, int* named, const char** why) {
  void* p = lookup != nullptr ? lookup(name) : nullptr;
  if (p != nullptr && reinterpret_cast<std::uintptr_t>(p) != want) {  // the name resolves elsewhere: not the pinned build
    *why = name;
    return false;
  }
  if (p != nullptr) ++*named;
  *out = want;
  return true;
}

// <data dir>/gunconnect.off (the plugin's own directory; made by the T-26 module, not here)
bool OffSwitchPresent() {
  char dir[320] = "";
  if (g_dir_override[0] != 0) {
    std::snprintf(dir, sizeof dir, "%s", g_dir_override);
  } else {
    char cmd[256] = "";
    FILE* f = std::fopen("/proc/self/cmdline", "re");
    if (f == nullptr) return false;
    const std::size_t r = std::fread(cmd, 1, sizeof cmd - 1, f);
    std::fclose(f);
    cmd[r] = 0;
    if (char* colon = std::strchr(cmd, ':')) *colon = 0;
    if (std::strchr(cmd, '.') == nullptr || std::strchr(cmd, '/') != nullptr || std::strlen(cmd) > 160) return false;
    std::snprintf(dir, sizeof dir, "/sdcard/Android/data/%s/files/q15t", cmd);
  }
  char path[400];
  std::snprintf(path, sizeof path, "%s/gunconnect.off", dir);
  return access(path, F_OK) == 0;
}

void LogGc(std::uint32_t level, const char* message, const char* why, std::uintptr_t cs, long long tries, long long unconnected,
           long long local, const char* detail) {
  char c[24], t[24], u[24], l[24];
  std::snprintf(c, sizeof c, "0x%llx", static_cast<unsigned long long>(cs));
  std::snprintf(t, sizeof t, "%lld", tries);
  std::snprintf(u, sizeof u, "%lld", unconnected);
  std::snprintf(l, sizeof l, "%lld", local);
  const NevrLogField f[] = {{"why", why}, {"cs", c}, {"tries", t}, {"unconnected", u}, {"local_guns", l}, {"detail", detail}};
  Log(level, message, f, 6);
}

void Waiting(const char* why) {  // one line per distinct reason
  if (std::strcmp(why, g_why) == 0) return;
  std::snprintf(g_why, sizeof g_why, "%s", why);
  LogGc(NEVR_LOG_INFO, "gunconnect waiting", why, 0, -1, -1, -1, "");
}

FixState& StateFor(std::uintptr_t cs) {
  for (FixState& s : g_st)
    if (s.cs == cs) return s;
  for (FixState& s : g_st)
    if (s.cs == 0) {
      s = FixState{};
      s.cs = cs;
      return s;
    }
  g_st[0] = FixState{};  // more gun CSs than slots (cannot happen: at most kMaxCs are found): recycle
  g_st[0].cs = cs;
  return g_st[0];
}

// The local member (member 0) of CR15NetGame, as OnGameSpaceLoaded builds its user id (source ReadLocal, the source build's gunconnect:222-240).
Local ReadLocal(std::uintptr_t game) {
  Local L;
  bool ok = true;
  L.net = Rd<std::uintptr_t>(game + 0x8510, &ok);  // CR15Game::NetGameMT: mov w8,#0x8510; ldr x0,[x0,x8] (disassembly 0x13027d4)
  if (!ok || L.net == 0) {
    L.why = "no netgame";
    return L;
  }
  const auto de = Rd<std::uint16_t>(L.net + 0xde, &ok);
  const auto dc = Rd<std::uint16_t>(L.net + 0xdc, &ok);
  if (!ok || de == 0 || dc == 0) {
    L.why = "netgame +0xde/+0xdc zero";
    return L;
  }
  L.slot = Rd<std::uint16_t>(L.net + 0x178, &ok) & 0x1f;
  L.gen = Rd<std::uint16_t>(L.net + 0x13a + L.slot * 4ull, &ok);
  L.midx = Rd<std::uint16_t>(L.net + 0x138 + L.slot * 4ull, &ok);
  if (!ok) {
    L.why = "member read failed";
    return L;
  }
  if (L.gen == 0xffff || L.midx == 0xffff) {
    L.why = "member slot unused";
    return L;
  }
  const auto members = Rd<std::uint16_t>(L.net + 0xe2, &ok);  // UserActor indexes net + midx*0x250 unchecked (disassembly 0x1351660-0x1351668)
  if (!ok || L.midx >= members) {
    L.why = "member index out of range";
    return L;
  }
  L.user_id = L.slot | L.gen << 16;
  L.guid = g_f.user_actor(reinterpret_cast<void*>(L.net), static_cast<std::uint16_t>(L.midx));
  L.ok = true;
  return L;
}

bool HasCombatTag(std::uintptr_t game, const GunGuid& guid) {
  bool ok = true;
  const auto tag = Rd<std::uint64_t>(g_f.tag_combat_addr, &ok);
  if (!ok || tag == 0) return false;
  return g_f.has_tag(reinterpret_cast<void*>(game), &guid, tag);
}

// The gun CS's guns owned by member slot `slot` (Connect's own match: disassembly 0x22534c4-0x22534ec) and how many are not connected.
GunCounts CountGuns(std::uintptr_t cs, std::uint32_t slot) {
  GunCounts c;
  bool ok = true;
  c.guns = Rd<std::uint16_t>(cs + 0xf2, &ok);
  c.ring = Rd<std::uint32_t>(cs + 0x1c0, &ok);
  const auto insts = Rd<std::uintptr_t>(cs + 0xf8, &ok);
  const auto props = Rd<std::uintptr_t>(Rd<std::uintptr_t>(cs + 0xb8, &ok), &ok);
  if (!ok || c.guns > kMaxGuns) {
    c.ok = false;
    return c;
  }
  for (std::uint32_t i = 0; i < c.guns; ++i) {
    bool ok2 = true;
    const std::uintptr_t inst = insts + i * 0x168ull;
    const std::uintptr_t p = props + 0xa0ull * Rd<std::uint16_t>(inst + 0xa0, &ok2);
    if (Rd<std::uint64_t>(p + 0x20, &ok2) != slot || !ok2) continue;
    if (Rd<std::uint32_t>(p + 0x9c, &ok2) == 0 || !ok2) continue;
    ++c.local;
    if (Rd<std::uint32_t>(inst + 0xd8, &ok2) != 0 && ok2) ++c.unconnected;
  }
  return c;
}

void FixPass(std::uintptr_t game, double now) {
  CompSys found[kMaxCs];
  const int n = FindComponentSystems(game, kGunCsKey, g_base + kGunCsVptr, found, kMaxCs);
  for (FixState& s : g_st) {  // forget gun CSs that are gone (a level change)
    bool live = false;
    for (int k = 0; k < n; ++k) live = live || found[k].cs == s.cs;
    if (!live) s = FixState{};
  }
  if (n == 0) return;
  const Local L = ReadLocal(game);
  if (!L.ok) {
    Waiting(L.why);
    for (FixState& s : g_st) s.since = 0;
    return;
  }
  g_why[0] = 0;  // a later "waiting" reason is a new event
  for (int k = 0; k < n; ++k) {
    FixState& st = StateFor(found[k].cs);
    if (st.gave_up) continue;
    const GunCounts c = CountGuns(found[k].cs, L.slot);
    const bool a = c.ok && c.unconnected > 0, b = c.ok && c.ring == 0;
    if (!st.seen) {
      st.seen = true;
      LogGc(NEVR_LOG_INFO, "gunconnect gun cs", c.ok ? "found" : "found, unreadable", found[k].cs, 0, c.unconnected, c.local, "");
    }
    if (a || !c.ok) g_next_fix = std::min(g_next_fix, now + kFixEveryS);  // something to fix (or unread): 4 Hz
    if (!a && st.tries) {
      LogGc(NEVR_LOG_INFO, "gunconnect connected", "all local guns connected", found[k].cs, st.tries, 0, c.local, "");
      st.tries = 0;
    }
    if (!a || !b || !HasCombatTag(game, L.guid)) {
      st.since = 0;
      continue;
    }
    if (!st.since) st.since = now;
    if (now - st.since < kHoldS) continue;
    if (now < st.next_try) continue;
    if (st.tries >= kMaxTries) {
      LogGc(NEVR_LOG_WARNING, "gunconnect gave up", "local guns still unconnected after the last Connect", found[k].cs, st.tries,
            c.unconnected, c.local, "");
      st.gave_up = true;
      continue;
    }
    ++st.tries;
    st.next_try = now + kRetryS;
    char detail[128];
    std::snprintf(detail, sizeof detail, "user %x slot %u gen %u midx %u actor %016llx%016llx", L.user_id, L.slot, L.gen, L.midx,
                  static_cast<unsigned long long>(L.guid.hi), static_cast<unsigned long long>(L.guid.lo));
    LogGc(NEVR_LOG_INFO, "gunconnect Connect", "Connect(cs, local user)", found[k].cs, st.tries, c.unconnected, c.local, detail);
    g_f.connect(reinterpret_cast<void*>(found[k].cs), L.user_id);
  }
}

bool Start() {
  g_phase = 2;
  if (!g_have_fns) {
    g_lib = dlopen("libr15.so", RTLD_NOW | RTLD_NOLOAD);
    const char* why = "";
    if (g_lib == nullptr) {
      LogGc(NEVR_LOG_ERROR, "gunconnect off", "libr15.so is not loaded", 0, -1, -1, -1, "");
      return false;
    }
    if (!ResolveGunConnectFns(g_base, &LibLookup, &g_f, &why)) {
      LogGc(NEVR_LOG_ERROR, "gunconnect off", "an export does not sit at the pinned address (not the pinned build?)", 0, -1, -1, -1, why);
      return false;
    }
    g_have_fns = true;
  }
  if (OffSwitchPresent()) {
    LogGc(NEVR_LOG_INFO, "gunconnect off", "gunconnect.off present", 0, -1, -1, -1, "");
    return false;
  }
  char detail[48];
  std::snprintf(detail, sizeof detail, "%d of 4 addresses confirmed by exported name", g_f.named);
  LogGc(NEVR_LOG_INFO, "gunconnect on", "Connect(local user) after 2 s for unconnected local guns", 0, -1, -1, -1, detail);
  g_phase = 1;
  return true;
}

}  // namespace

bool ResolveGunConnectFns(std::uintptr_t base, void* (*lookup)(const char*), GunConnectFns* out, const char** why) {
  GunConnectFns f{};
  std::uintptr_t connect = 0, user_actor = 0, has_tag = 0;
  if (!Pick(lookup, kSymConnect, base + kFnConnect, &connect, &f.named, why)) return false;
  if (!Pick(lookup, kSymUserActor, base + kFnUserActor, &user_actor, &f.named, why)) return false;
  if (!Pick(lookup, kSymHasTag, base + kFnHasTag, &has_tag, &f.named, why)) return false;
  if (!Pick(lookup, kSymTagCombat, base + kTagCombat, &f.tag_combat_addr, &f.named, why)) return false;
  f.connect = reinterpret_cast<void (*)(void*, std::uint32_t)>(connect);
  f.user_actor = reinterpret_cast<GunGuid (*)(void*, std::uint16_t)>(user_actor);
  f.has_tag = reinterpret_cast<bool (*)(void*, const GunGuid*, std::uint64_t)>(has_tag);
  *out = f;
  return true;
}

void InitGunConnect(std::uintptr_t base, const GunConnectFns& fns) {
  g_base = base;
  g_f = fns;
  g_have_fns = fns.connect != nullptr;
}

void GunConnectTick(std::uintptr_t game) {
  if (g_phase == 2) return;
  if (g_phase == 0 && !Start()) return;
  const double now = Now();
  if (now < g_next_fix) return;
  g_next_fix = now + kIdleEveryS;  // FixPass shortens it while there is something to fix
  FixPass(game, now);
}

int GunConnectPhase() { return g_phase; }

void SetGunConnectClockForTest(double (*clock)()) { g_clock = clock; }
void SetGunConnectDirForTest(const char* dir) { std::snprintf(g_dir_override, sizeof g_dir_override, "%s", dir != nullptr ? dir : ""); }
void ResetGunConnectForTest() {
  g_f = GunConnectFns{};
  g_have_fns = false;
  g_phase = 0;
  g_base = 0;
  for (FixState& s : g_st) s = FixState{};
  g_next_fix = 0;
  g_why[0] = 0;
  g_lib = nullptr;
}

}  // namespace q15t
