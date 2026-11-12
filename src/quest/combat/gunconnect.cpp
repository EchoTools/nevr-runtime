#include "quest/combat/gunconnect.h"

#include <dlfcn.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "quest/combat/combat_scope.h"
#include "quest/combat/combat_tick.h"
#include "quest/combat/gamespace.h"
#include "quest/combat/gamespace_targets.h"
#include "quest/combat/gunconnect_targets.h"
#include "quest/combat/legacy_switch.h"
#include "quest/combat/netgame_targets.h"
#include "quest/combat/safe_read.h"
#include "quest/sentinel/hook_log.h"
#include "quest/sentinel/pinned_targets.h"

namespace nevr_quest::combat::gunconnect {

namespace {

namespace gt = targets;
constexpr int kMaxGuns = 4096, kMaxCs = 8;
constexpr double kFixEveryS = 0.25, kIdleEveryS = 1.0, kHoldS = 2.0, kRetryS = 1.0;
constexpr int kMaxTries = 10;

struct FixState {
  std::uintptr_t cs = 0;
  double since = 0, nextTry = 0;
  int tries = 0;
  bool gaveUp = false;
  bool seen = false;
};

struct Local {
  bool ok = false;
  const char* why = "";
  std::uintptr_t net = 0;
  std::uint32_t slot = 0, gen = 0, midx = 0, userId = 0;
  Guid guid{0, 0};
};

struct GunCounts {
  bool ok = true;
  std::uint32_t guns = 0, local = 0, unconnected = 0;
  std::uint64_t ring = 0;  // the game loads it as a u64 (ExclusiveUpdateConnect)
};

// The module's state. The install fills it on the thread that ran the normal step and registers the callback after, so the game thread's first
// pass is ordered after it by the callback table's release store; the pass is the game thread alone.
Fns g_f{};
std::uintptr_t g_base = 0;
std::atomic<int> g_phase{0};
FixState g_st[kMaxCs];
double g_nextFix = 0;
char g_why[96] = "";
double (*g_clock)() = nullptr;
std::atomic<std::uint32_t> g_passes{0}, g_scopedOut{0}, g_connects{0};

double Now() noexcept {
  if (g_clock != nullptr) return g_clock();
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

void* DlLookup(const char* symbol) noexcept {
  void* const lib = dlopen("libr15.so", RTLD_NOW | RTLD_NOLOAD);
  return lib != nullptr ? dlsym(lib, symbol) : nullptr;
}

bool Pick(SymbolLookup lookup, const char* name, std::uintptr_t want, std::uintptr_t* out, int* named, const char** why) noexcept {
  void* const p = lookup != nullptr ? lookup(name) : nullptr;
  if (p != nullptr && reinterpret_cast<std::uintptr_t>(p) != want) {  // the name resolves elsewhere: not the pinned build
    *why = name;
    return false;
  }
  if (p != nullptr) ++*named;
  *out = want;
  return true;
}

void Waiting(const char* why) noexcept {  // one line per distinct reason
  if (std::strcmp(why, g_why) == 0) return;
  std::snprintf(g_why, sizeof(g_why), "%s", why);
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_gunconnect_waiting",
                      {{"why", why}, {"msg", "the local member's gun connect is waiting for the game; nothing is called until it can be read"}});
}

FixState& StateFor(std::uintptr_t cs) noexcept {
  for (FixState& s : g_st) {
    if (s.cs == cs) return s;
  }
  for (FixState& s : g_st) {
    if (s.cs == 0) {
      s = FixState{};
      s.cs = cs;
      return s;
    }
  }
  g_st[0] = FixState{};  // more gun CSs than slots (cannot happen: at most kMaxCs are found): recycle
  g_st[0].cs = cs;
  return g_st[0];
}

// The local member (member 0) of CR15NetGame, as OnGameSpaceLoaded builds its user id (afk_gunconnect.cpp:222-240).
Local ReadLocal(std::uintptr_t game) noexcept {
  Local L;
  bool ok = true;
  L.net = SafeRead<std::uintptr_t>(game + netgame::kNetGameOffset, &ok);
  if (!ok || L.net == 0) {
    L.why = "no netgame";
    return L;
  }
  const auto de = SafeRead<std::uint16_t>(L.net + gt::kNetReady, &ok);
  const auto dc = SafeRead<std::uint16_t>(L.net + gt::kMemberCountCheck, &ok);
  if (!ok || de == 0 || dc == 0) {
    L.why = "netgame +0xde/+0xdc zero";
    return L;
  }
  L.slot = SafeRead<std::uint16_t>(L.net + gt::kMemberRowSlot, &ok) & gt::kSlotMask;
  L.gen = SafeRead<std::uint16_t>(L.net + gt::kSlotGeneration + L.slot * gt::kSlotStride, &ok);
  L.midx = SafeRead<std::uint16_t>(L.net + gt::kSlotMember + L.slot * gt::kSlotStride, &ok);
  if (!ok) {
    L.why = "member read failed";
    return L;
  }
  if (L.gen == 0xffff || L.midx == 0xffff) {
    L.why = "member slot unused";
    return L;
  }
  const auto members = SafeRead<std::uint16_t>(L.net + gt::kMemberCount, &ok);  // UserActor indexes net + midx*0x250 unchecked
  if (!ok || L.midx >= members) {
    L.why = "member index out of range";
    return L;
  }
  L.userId = L.slot | L.gen << 16;
  L.guid = g_f.userActor(reinterpret_cast<void*>(L.net), static_cast<std::uint16_t>(L.midx));
  L.ok = true;
  return L;
}

bool HasCombatTag(std::uintptr_t game, const Guid& guid) noexcept {
  bool ok = true;
  const auto tag = SafeRead<std::uint64_t>(g_f.tagCombatAddr, &ok);
  if (!ok || tag == 0) return false;
  return g_f.hasTag(reinterpret_cast<void*>(game), &guid, tag);
}

// The gun CS's guns owned by member slot `slot` (Connect's own match) and how many are not connected.
GunCounts CountGuns(std::uintptr_t cs, std::uint32_t slot) noexcept {
  GunCounts c;
  bool ok = true;
  c.guns = SafeRead<std::uint16_t>(cs + gt::kGunCount, &ok);
  c.ring = SafeRead<std::uint64_t>(cs + gt::kConnectRing, &ok);
  const auto insts = SafeRead<std::uintptr_t>(cs + gt::kInstances, &ok);
  const auto props = SafeRead<std::uintptr_t>(SafeRead<std::uintptr_t>(cs + gt::kProperties, &ok), &ok);
  if (!ok || c.guns > kMaxGuns) {
    c.ok = false;
    return c;
  }
  for (std::uint32_t i = 0; i < c.guns; ++i) {
    bool ok2 = true;
    const std::uintptr_t inst = insts + i * gt::kInstanceStride;
    const std::uintptr_t p = props + gt::kPropertyStride * SafeRead<std::uint16_t>(inst + gt::kPropertyIndex, &ok2);
    if (SafeRead<std::uint64_t>(p + gt::kPropertyOwner, &ok2) != slot || !ok2) continue;
    if (SafeRead<std::uint32_t>(p + gt::kPropertyFlag, &ok2) == 0 || !ok2) continue;
    ++c.local;
    if (SafeRead<std::uint32_t>(inst + gt::kInstanceState, &ok2) != 0 && ok2) ++c.unconnected;
  }
  return c;
}

void FixPass(std::uintptr_t game, double now) noexcept {
  // A combat map or the combat lobby with no Echo Arena level, as the combat level answer last saw it (combat_scope.h): a load and a test on an
  // arena pass, no read of the game.
  if (!scope::InCombatOrLobby()) {
    g_scopedOut.fetch_add(1, std::memory_order_relaxed);
    for (FixState& s : g_st) s = FixState{};
    g_why[0] = 0;
    return;
  }
  gamespace::CompSys found[kMaxCs];
  const int n = gamespace::FindComponentSystems(game, gt::kGunCsKey, g_base + gt::kGunCsVptrVaddr, found, kMaxCs);
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
    if (st.gaveUp) continue;
    char csText[19];
    const GunCounts c = CountGuns(found[k].cs, L.slot);
    const bool a = c.ok && c.unconnected > 0, b = c.ok && c.ring == 0;
    if (!st.seen) {
      st.seen = true;
      sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_gunconnect_gun_cs",
                          {{"cs", sentinel::HexString(csText, found[k].cs)}, {"state", c.ok ? "found" : "found, unreadable"},
                           {"unconnected", c.unconnected}, {"local_guns", c.local}});
    }
    if (a || !c.ok) g_nextFix = std::min(g_nextFix, now + kFixEveryS);  // something to fix (or unread): 4 Hz
    if (!a && st.tries) {
      sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_gunconnect_connected",
                          {{"cs", sentinel::HexString(csText, found[k].cs)}, {"tries", st.tries}, {"local_guns", c.local},
                           {"msg", "every local gun is connected after the call; the gun fires"}});
      st.tries = 0;
    }
    if (!a || !b || !HasCombatTag(game, L.guid)) {
      st.since = 0;
      continue;
    }
    if (!st.since) st.since = now;
    if (now - st.since < kHoldS) continue;
    if (now < st.nextTry) continue;
    if (st.tries >= kMaxTries) {
      sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_gunconnect_gave_up",
                          {{"cs", sentinel::HexString(csText, found[k].cs)}, {"tries", st.tries}, {"unconnected", c.unconnected},
                           {"local_guns", c.local},
                           {"msg", "local guns are still unconnected after the last Connect; the module stops for this gun system and the guns do not fire"}});
      st.gaveUp = true;
      continue;
    }
    ++st.tries;
    st.nextTry = now + kRetryS;
    char userText[19], hiText[19], loText[19];
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_gunconnect_connect",
                        {{"cs", sentinel::HexString(csText, found[k].cs)}, {"tries", st.tries}, {"unconnected", c.unconnected},
                         {"local_guns", c.local}, {"user", sentinel::HexString(userText, L.userId)}, {"slot", L.slot}, {"gen", L.gen},
                         {"midx", L.midx}, {"actor_hi", sentinel::HexString(hiText, L.guid.hi)}, {"actor_lo", sentinel::HexString(loText, L.guid.lo)},
                         {"msg", "Connect(gun system, local user) is called for guns the game left unconnected"}});
    g_connects.fetch_add(1, std::memory_order_relaxed);
    g_f.connect(reinterpret_cast<void*>(found[k].cs), L.userId);
  }
}

ModuleResult Refused(const char* reason, const char* detail) noexcept {
  g_phase.store(2, std::memory_order_release);
  sentinel::LogFields(sentinel::LogLevel::kError, "combat_gunconnect_install",
                      {{"result", "refused"}, {"reason", reason}, {"detail", detail},
                       {"msg", "gun connect is not installed: a local practice gun that was never connected does not fire"}});
  return {false, reason};
}

void WarnLegacySwitch() noexcept {
  if (!legacy::Present("gunconnect.off")) return;
  sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_gunconnect_legacy_switch",
                      {{"file", "gunconnect.off"}, {"replaced_by", "combat.gunconnect"},
                       {"msg", "the plugin's kill file is no longer read and changes nothing; combat.gunconnect in config.yaml decides (on unless it says false)"}});
}

}  // namespace

bool Resolve(std::uintptr_t base, SymbolLookup lookup, Fns* out, const char** why) noexcept {
  Fns f{};
  std::uintptr_t connect = 0, userActor = 0, hasTag = 0;
  if (!Pick(lookup, gt::kConnectSymbol, base + gt::kConnectVaddr, &connect, &f.named, why)) return false;
  if (!Pick(lookup, netgame::kUserActorSymbol, base + netgame::kUserActorVaddr, &userActor, &f.named, why)) return false;
  if (!Pick(lookup, gt::kHasTagSymbol, base + gt::kHasTagVaddr, &hasTag, &f.named, why)) return false;
  if (!Pick(lookup, gt::kTagCombatSymbol, base + gt::kTagCombatVaddr, &f.tagCombatAddr, &f.named, why)) return false;
  f.connect = reinterpret_cast<void (*)(void*, std::uint32_t)>(connect);
  f.userActor = reinterpret_cast<Guid (*)(void*, std::uint16_t)>(userActor);
  f.hasTag = reinterpret_cast<bool (*)(void*, const Guid*, std::uint64_t)>(hasTag);
  *out = f;
  return true;
}

ModuleResult InstallResolved(std::uintptr_t base, const Fns& fns) noexcept {
  g_base = base;
  g_f = fns;
  g_nextFix = 0;
  if (!tick::Add(&Tick)) return Refused("tick_table_full", "");
  g_phase.store(1, std::memory_order_release);
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_gunconnect_install",
                      {{"result", "installed"}, {"reason", "ok"}, {"named", fns.named},
                       {"msg", "Connect(local user) is called 2 s after a combat-level gun is left unconnected; it installs no hook"}});
  return {true, "installed"};
}

ModuleResult InstallAt(sentinel::ImageLookup lookup, SymbolLookup symbols, bool on) noexcept {
  WarnLegacySwitch();
  if (!on) {
    g_phase.store(2, std::memory_order_release);
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_gunconnect_install",
                        {{"result", "off"}, {"reason", "off_config"}, {"switch", "combat.gunconnect"},
                         {"msg", "combat.gunconnect is false: local practice guns the game left unconnected stay as they are"}});
    return {false, "off_config", true};
  }
  // The pass acts only while the combat level answer says a combat map or the combat lobby is loaded: when that answer cannot change (the tick or the
  // scope row's refresh is not installed) the module would never act, so it is not installed (combat_scope.h `Live`).
  if (!scope::Live()) return Refused("scope_not_live", "");
  sentinel::ElfImage image;
  if (lookup == nullptr || !lookup(sentinel::pinned::kLibR15, &image)) return Refused("module_not_loaded", "");
  char id[64] = {};
  if (!sentinel::ReadBuildId(image, id, sizeof(id))) return Refused("no_build_id", "");
  if (std::strcmp(id, sentinel::pinned::LibR15PatchedBuildId()) != 0) return Refused("build_id_mismatch", "");
  const pinned::Pins lists[] = {gt::kPins, gt::kConnectPins, netgame::kPins, gamespace::targets::kPins};
  pinned::Failure failure;
  if (pinned::FirstFailure(image.base, lists, sizeof(lists) / sizeof(lists[0]), &failure)) {
    char at[19], haveText[19], wantText[19];
    sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_gunconnect_pinned_word",
                        {{"vaddr", sentinel::HexString(at, failure.vaddr)}, {"have", failure.readable ? sentinel::HexString(haveText, failure.have) : "unreadable"},
                         {"want", sentinel::HexString(wantText, failure.want)}, {"what", failure.what}});
    return Refused("pinned_word_mismatch", failure.what);
  }
  Fns fns{};
  const char* why = "";
  if (!Resolve(image.base, symbols, &fns, &why)) return Refused("export_elsewhere", why);
  return InstallResolved(image.base, fns);
}

ModuleResult Install(bool on) noexcept { return InstallAt(&sentinel::FindLoadedImage, &DlLookup, on); }

void Tick(std::uintptr_t game) noexcept {
  if (g_phase.load(std::memory_order_acquire) != 1) return;
  const double now = Now();
  if (now < g_nextFix) return;
  g_nextFix = now + kIdleEveryS;  // FixPass shortens it while there is something to fix
  g_passes.fetch_add(1, std::memory_order_relaxed);
  FixPass(game, now);
}

int Phase() noexcept { return g_phase.load(std::memory_order_acquire); }

Stats CurrentStats() noexcept { return {g_passes.load(), g_scopedOut.load(), g_connects.load()}; }

void SetClockForTest(double (*clock)()) noexcept { g_clock = clock; }
void SetLegacyDirForTest(const char* dir) noexcept { legacy::SetDirectoryForTest(dir); }

void ResetForTest() noexcept {
  g_f = Fns{};
  g_base = 0;
  g_phase.store(0);
  for (FixState& s : g_st) s = FixState{};
  g_nextFix = 0;
  g_why[0] = 0;
  g_passes.store(0);
  g_scopedOut.store(0);
  g_connects.store(0);
}

}  // namespace nevr_quest::combat::gunconnect
