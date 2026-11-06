#include "quest/combat/bvh.h"

#include <pthread.h>
#include <time.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "quest/combat/bvh_core.h"
#include "quest/combat/combat_level.h"
#include "quest/combat/combat_tick.h"
#include "quest/combat/legacy_switch.h"
#include "quest/combat/safe_read.h"
#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/hook_log.h"

namespace nevr_quest::combat::bvh {

namespace {

constexpr int kMaxTrees = 64;
constexpr double kSummaryEverySeconds = 10.0;
constexpr double kLookEverySeconds = 0.25;
constexpr std::uintptr_t kMarkAllowed = 1;

// ---- state: plain atomics and PODs, no constructors ----
struct Entry {
  std::uint64_t data;  // the SBVHTreeData address this tree was built from
  Tree tree;
};
Entry g_trees[kMaxTrees];
int g_treeCount = 0;
pthread_mutex_t g_treeMutex = PTHREAD_MUTEX_INITIALIZER;  // a std::mutex would register a destructor at load time

sentinel::GotHook g_hooks[6];
enum HookIndex { kInit = 0, kShut, kInt, kOcc, kTouch, kGate };

std::atomic<bool> g_ran{false};
std::atomic<bool> g_installed{false};
std::atomic<bool> g_gateOn{false};
std::atomic<std::uintptr_t> g_base{0};
std::atomic<std::uint64_t> g_allowed[4];
std::atomic<int> g_allowedCount{0};
std::atomic<std::uintptr_t> g_lastGateReturn{0};
std::atomic<sentinel::ImageLookup> g_lookup{nullptr};
std::atomic<const Pins*> g_pinsOverride{nullptr};
std::atomic<double (*)() noexcept> g_clock{nullptr};
std::atomic<int> g_lastSwitches{0};  // bit 0 bvh, 1 gate, 2 touch: what the last Install was given (a test seam)

std::atomic<std::uint64_t> g_built{0}, g_refused{0}, g_int{0}, g_intHit{0}, g_occ{0}, g_occHit{0}, g_gated{0}, g_touch{0}, g_stock{0};
std::atomic<std::uint64_t> g_ns{0}, g_maxNs{0};
std::atomic<bool> g_firstHit{false}, g_firstBlock{false};
// The tick's: game thread only.
double g_nextSummary = 0;
double g_nextLook = 0;
std::atomic<std::uint64_t> g_scopeLevel{0};

// One pointer-sized value per thread, for code that runs inside a hooked game call, in place of `thread_local` (the Quest build targets API 26,
// where the compiler emulates thread_local: a thread's first use allocates, and a hooked call must not allocate). A pthread key keeps its value in
// an array of the thread's own static TLS block, so Get and Set never allocate or lock. The key is made once by Create(), before the gate hook
// goes in, and never deleted: nothing uninstalls the gate once it is in. Before Create, or after a Create that failed, Get answers 0 and Set does
// nothing.
class ThreadWord {
 public:
  int Create() noexcept {
    pthread_mutex_lock(&mutex_);
    int error = 0;
    if (!ready_.load(std::memory_order_relaxed)) {
      error = pthread_key_create(&key_, nullptr);
      ready_.store(error == 0, std::memory_order_release);
    }
    pthread_mutex_unlock(&mutex_);
    return error;
  }
  std::uintptr_t Get() const noexcept {
    if (!ready_.load(std::memory_order_acquire)) return 0;
    return reinterpret_cast<std::uintptr_t>(pthread_getspecific(key_));
  }
  void Set(std::uintptr_t value) noexcept {
    if (!ready_.load(std::memory_order_acquire)) return;
    pthread_setspecific(key_, reinterpret_cast<void*>(value));
  }

 private:
  pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
  pthread_key_t key_{};
  std::atomic<bool> ready_{false};
};
ThreadWord g_mark;  // per thread: kMarkAllowed while this thread is inside a cast the tree answers

double NowSeconds() noexcept {
  const auto clock = g_clock.load(std::memory_order_acquire);
  if (clock != nullptr) return clock();
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}
std::uint64_t NowNs() noexcept {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
}
void Timed(std::uint64_t t0) noexcept {
  const std::uint64_t d = NowNs() - t0;
  g_ns.fetch_add(d, std::memory_order_relaxed);
  std::uint64_t m = g_maxNs.load(std::memory_order_relaxed);
  while (d > m && !g_maxNs.compare_exchange_weak(m, d, std::memory_order_relaxed)) {
  }
}

// Every page of [addr, addr + bytes) can be read (each page the range touches is probed at its first byte in the range).
bool Readable(std::uint64_t addr, std::uint64_t bytes) noexcept {
  constexpr std::uint64_t kPage = 4096;
  std::uint8_t b;
  for (std::uint64_t off = 0; off < bytes; off += kPage - ((addr + off) & (kPage - 1))) {
    if (!SafeRead(static_cast<std::uintptr_t>(addr + off), &b, 1)) return false;
  }
  return true;
}

void LogTree(sentinel::LogLevel level, const char* result, std::uint64_t data, const char* why, long long tri4, long long nodes,
             std::uint64_t root) noexcept {
  char d[19], r[19];
  sentinel::LogFields(level, "combat_bvh_tree",
                      {{"result", result}, {"data", sentinel::HexString(d, data)}, {"why", why}, {"triangle4", tri4}, {"nodes", nodes},
                       {"root_ref", sentinel::HexString(r, root)}});
}

// ---- Initialize / Shutdown ----
void BuildTree(void* self, const void* data) noexcept {
  const std::uint64_t d = reinterpret_cast<std::uintptr_t>(data);
  if (data == nullptr) {
    g_refused.fetch_add(1, std::memory_order_relaxed);
    LogTree(sentinel::LogLevel::kInfo, "none", 0, "no data", 0, 0, 0);
    return;
  }
  std::uint8_t hdr[kHeaderSize];
  if (!SafeRead(static_cast<std::uintptr_t>(d), hdr, sizeof(hdr))) {
    g_refused.fetch_add(1, std::memory_order_relaxed);
    LogTree(sentinel::LogLevel::kWarn, "refused", d, "header unreadable", 0, 0, 0);
    return;
  }
  Tree t;
  Blocks b;
  const char* why = "";
  if (!Parse(hdr, d, &t, &b, &why)) {
    g_refused.fetch_add(1, std::memory_order_relaxed);
    // an all-empty resource is what a level without static geometry ships: not a fault
    const bool empty = std::strcmp(why, "empty") == 0;
    LogTree(empty ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn, "none", d, why, 0, 0, 0);
    return;
  }
  if (!Readable(b.tri_ptr, b.tri_bytes) || !Readable(b.node_ptr, b.node_bytes)) {
    g_refused.fetch_add(1, std::memory_order_relaxed);
    LogTree(sentinel::LogLevel::kWarn, "refused", d, "block unreadable", t.ntris, t.nnodes, static_cast<std::uint32_t>(t.root));
    return;
  }
  int slot = -1;
  pthread_mutex_lock(&g_treeMutex);
  for (int i = 0; i < g_treeCount; ++i) {
    if (g_trees[i].data == d) slot = i;
  }
  if (slot < 0 && g_treeCount < kMaxTrees) slot = g_treeCount++;
  if (slot >= 0) {
    g_trees[slot].data = d;
    g_trees[slot].tree = t;
  }
  pthread_mutex_unlock(&g_treeMutex);
  if (slot < 0) {
    g_refused.fetch_add(1, std::memory_order_relaxed);
    LogTree(sentinel::LogLevel::kError, "refused", d, "tree table full", t.ntris, t.nnodes, static_cast<std::uint32_t>(t.root));
    return;
  }
  *static_cast<const Tree**>(self) = &g_trees[slot].tree;
  g_built.fetch_add(1, std::memory_order_relaxed);
  LogTree(sentinel::LogLevel::kInfo, "built", d, "", t.ntris, t.nnodes, static_cast<std::uint32_t>(t.root));
}

// The game's Initialize runs first; then the tree is built from the data it was given. Built whatever the level (see the header).
void OnInitialize(InitThunk::Fn original, void* self, const void* data) noexcept {
  original(self, data);
  if (self == nullptr) return;
  *static_cast<const Tree**>(self) = nullptr;
  BuildTree(self, data);
}

void OnShutdown(ShutThunk::Fn original, void* self) noexcept {
  original(self);
  if (self != nullptr) *static_cast<const Tree**>(self) = nullptr;
}

// The tree a CBVHTree points at: one of our table entries (a word that is not one is a doubt: the game's function answers). The CBVHTree is
// the object the game's caller called a member of, so its first word is read with a plain load.
const Tree* TreeOf(const void* self) noexcept {
  const std::uintptr_t p = *static_cast<const std::uintptr_t*>(self);
  const std::uintptr_t lo = reinterpret_cast<std::uintptr_t>(&g_trees[0].tree);
  const std::uintptr_t off = p - lo;
  if (off >= sizeof(g_trees) || off % sizeof(Entry) != 0) return nullptr;  // (p below the table wraps `off` to a huge value)
  return reinterpret_cast<const Tree*>(p);
}

void LogFirstHit(const float* r, const Hit& h) noexcept {
  char from[64], dir[64], t[24], nrm[64];
  std::snprintf(from, sizeof(from), "%.2f %.2f %.2f", static_cast<double>(r[0]), static_cast<double>(r[1]), static_cast<double>(r[2]));
  std::snprintf(dir, sizeof(dir), "%.2f %.2f %.2f", static_cast<double>(r[3]), static_cast<double>(r[4]), static_cast<double>(r[5]));
  std::snprintf(t, sizeof(t), "%.3f", static_cast<double>(h.t));
  std::snprintf(nrm, sizeof(nrm), "%.2f %.2f %.2f", static_cast<double>(h.n[0]), static_cast<double>(h.n[1]), static_cast<double>(h.n[2]));
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_bvh_first_hit",
                      {{"from", from}, {"dir", dir}, {"t", t}, {"mesh", h.id0}, {"tri", h.id1}, {"normal", nrm},
                       {"msg", "the first cast answered from the game's own static BVH"}});
}

void LogFirstBlock(const float* a, const float* b) noexcept {
  char s[64], e[64];
  std::snprintf(s, sizeof(s), "%.2f %.2f %.2f", static_cast<double>(a[0]), static_cast<double>(a[1]), static_cast<double>(a[2]));
  std::snprintf(e, sizeof(e), "%.2f %.2f %.2f", static_cast<double>(b[0]), static_cast<double>(b[1]), static_cast<double>(b[2]));
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_bvh_first_block",
                      {{"from", s}, {"to", e}, {"msg", "the first segment blocked by the game's own static BVH"}});
}

// ---- Intersect ----
// With no combat map loaded the game's function answers (the arena path). The ray is the caller's CRay (six floats: origin, direction).
bool OnIntersect(IntersectThunk::Fn original, const void* self, const float* ray, std::uint8_t* hit) noexcept {
  if (!InCombatMap()) return original(self, ray, hit);
  if (g_gateOn.load(std::memory_order_relaxed) && g_mark.Get() == 0) {  // not a cast the tree is for: the stock miss
    g_gated.fetch_add(1, std::memory_order_relaxed);
    return original(self, ray, hit);
  }
  const Tree* t = TreeOf(self);
  if (t == nullptr) {
    g_stock.fetch_add(1, std::memory_order_relaxed);
    return original(self, ray, hit);
  }
  float r[6];
  std::memcpy(r, ray, sizeof(r));
  const std::uint64_t t0 = NowNs();
  Hit h;
  const bool got = Trace(*t, Ray(r, r + 3, 0.0f, INFINITY), false, &h);
  Timed(t0);
  g_int.fetch_add(1, std::memory_order_relaxed);
  if (!got) return false;
  g_intHit.fetch_add(1, std::memory_order_relaxed);
  std::memcpy(hit + 0x00, &h.id0, 4);
  std::memcpy(hit + 0x04, &h.id1, 4);
  std::memcpy(hit + 0x08, &h.u, 4);
  std::memcpy(hit + 0x0c, &h.v, 4);
  std::memcpy(hit + 0x10, &h.t, 4);
  std::memcpy(hit + 0x38, h.n, 12);
  if (!g_firstHit.exchange(true, std::memory_order_relaxed)) LogFirstHit(r, h);
  return true;
}

// ---- Occluded ----
// The segment from `a` to `b` (C3Vectors, three floats each).
bool OnOccluded(OccludedThunk::Fn original, const void* self, const float* a, const float* b) noexcept {
  if (!InCombatMap()) return original(self, a, b);
  const Tree* t = TreeOf(self);
  if (t == nullptr) {
    g_stock.fetch_add(1, std::memory_order_relaxed);
    return original(self, a, b);
  }
  float pa[3], pb[3];
  std::memcpy(pa, a, sizeof(pa));
  std::memcpy(pb, b, sizeof(pb));
  const std::uint64_t t0 = NowNs();
  const float d[3] = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
  // the segment a..b is t in (0, 1) along the unnormalised direction
  const bool got = Trace(*t, Ray(pa, d, 0.0f, 1.0f), true, nullptr);
  Timed(t0);
  g_occ.fetch_add(1, std::memory_order_relaxed);
  if (got) {
    g_occHit.fetch_add(1, std::memory_order_relaxed);
    if (!g_firstBlock.exchange(true, std::memory_order_relaxed)) LogFirstBlock(pa, pb);
  }
  return got;
}

// ---- Touch: CR15TouchInteractQuery::RayQueryStatic as on the stock Quest: a miss, nothing written (disassembly 0x21cf458: cbz w0 -> epilogue) ----
bool OnTouch(TouchThunk::Fn original, const void* a, const void* b, void* c, float* d) noexcept {
  if (!InCombatMap()) return original(a, b, c, d);
  g_touch.fetch_add(1, std::memory_order_relaxed);
  return false;
}

// ---- Gate: the callers' gate. `caller` is the instruction after the game's call. ----
bool OnGate(GateThunk::Fn original, const void* caller, const void* self, const void* ray, void* hit, void** gs) noexcept {
  if (!InCombatMap()) return original(self, ray, hit, gs);
  const std::uint64_t ret = reinterpret_cast<std::uintptr_t>(caller) - g_base.load(std::memory_order_relaxed);
  g_lastGateReturn.store(ret, std::memory_order_relaxed);
  bool allowed = false;
  const int count = g_allowedCount.load(std::memory_order_acquire);
  for (int i = 0; i < count; ++i) allowed |= g_allowed[i].load(std::memory_order_relaxed) == ret;
  const std::uintptr_t outer = g_mark.Get();
  g_mark.Set(allowed ? kMarkAllowed : 0);
  const bool got = original(self, ray, hit, gs);
  g_mark.Set(outer);
  return got;
}

NEVR_HOOK_RECORD(kInitRecord, InitThunk, &OnInitialize);
NEVR_HOOK_RECORD(kShutRecord, ShutThunk, &OnShutdown);
NEVR_HOOK_RECORD(kIntersectRecord, IntersectThunk, &OnIntersect);
NEVR_HOOK_RECORD(kOccludedRecord, OccludedThunk, &OnOccluded);
NEVR_HOOK_RECORD(kTouchRecord, TouchThunk, &OnTouch);
NEVR_HOOK_RECORD(kGateRecord, GateThunk, &OnGate);

// ---- install ----
void WarnLegacySwitches() noexcept {
  struct Legacy {
    const char* file;
    const char* key;
  };
  static constexpr Legacy kFiles[] = {{"bvh.off", "combat.bvh: false"}, {"bvhgate.off", "combat.bvh_gate: false"},
                                      {"bvhtouch.keep", "combat.bvh_touch: false"}};
  for (const Legacy& l : kFiles) {
    if (!legacy::Present(l.file)) continue;
    sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_bvh_legacy_switch",
                        {{"file", l.file}, {"replaced_by", l.key},
                         {"msg", "the plugin's kill file is no longer read and changes nothing; set the combat key in config.yaml instead"}});
  }
}

void Refused(const char* reason, const char* hook, const char* status, int error) noexcept {
  sentinel::LogFields(sentinel::LogLevel::kError, "combat_bvh_install",
                      {{"result", "refused"}, {"reason", reason}, {"hook", hook}, {"status", status}, {"error", error},
                       {"msg", "the static BVH is not installed: nothing was changed, so no cast is answered from the level and the game keeps its stub"}});
}

struct Planned {
  const char* name;
  const char* symbol;
  const Slot* slot;
  int index;
};

void Undo(int doneCount, const int* doneIndexes) noexcept {
  for (int i = doneCount - 1; i >= 0; --i) g_hooks[doneIndexes[i]].Remove();
  g_gateOn.store(false, std::memory_order_release);
  g_allowedCount.store(0, std::memory_order_release);
}

}  // namespace

ModuleResult InstallAt(sentinel::ImageLookup lookup, const Pins& pins, const Switches& switches) noexcept {
  g_lastSwitches.store((switches.bvh ? 1 : 0) | (switches.gate ? 2 : 0) | (switches.touch ? 4 : 0), std::memory_order_relaxed);
  WarnLegacySwitches();
  if (!switches.bvh) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_bvh_install",
                        {{"result", "off"}, {"reason", "off_config"}, {"hooks", 0}, {"gate", 0}, {"touch", 0}, {"allowed_casts", 0},
                         {"msg", "combat.bvh is false: the game keeps its stubbed static BVH and no cast is answered from the level"}});
    return {false, "off_config"};
  }
  if (g_ran.exchange(true, std::memory_order_acq_rel)) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_bvh_install",
                        {{"result", "refused"}, {"reason", "already_ran"}, {"msg", "a second step installs nothing"}});
    return {false, "already_ran"};
  }
  sentinel::ElfImage image;
  if (lookup == nullptr || !lookup(sentinel::pinned::kLibR15, &image)) {
    Refused("module_not_loaded", "(none)", "module_not_loaded", 0);
    return {false, "module_not_loaded"};
  }
  const std::uintptr_t base = image.base;
  if (const int keyError = g_mark.Create(); keyError != 0) {  // the allowed mark is per thread, in a pthread key
    Refused("no_thread_key", "(none)", "no_thread_key", keyError);
    return {false, "no_thread_key"};
  }
  // The scope is refreshed by the combat tick: with no tick callback it would never turn on and the module would do nothing.
  if (!tick::Add(&Tick)) {
    Refused("tick_table_full", "(none)", "tick_table_full", 0);
    return {false, "tick_table_full"};
  }
  g_base.store(base, std::memory_order_release);
  for (int i = 0; i < pins.allowedCount && i < 4; ++i) g_allowed[i].store(pins.allowed[i], std::memory_order_relaxed);
  g_allowedCount.store(pins.allowedCount < 4 ? pins.allowedCount : 4, std::memory_order_release);

  InitThunk::Arm(kInitRecord);
  ShutThunk::Arm(kShutRecord);
  IntersectThunk::Arm(kIntersectRecord);
  OccludedThunk::Arm(kOccludedRecord);
  TouchThunk::Arm(kTouchRecord);
  GateThunk::Arm(kGateRecord);

  // The gate first: until its hook is in, no cast is marked, so a tree installed a moment later answers none.
  g_gateOn.store(switches.gate, std::memory_order_release);
  int done[5];
  int doneCount = 0;
  const auto status = [&](int index, const sentinel::GotTarget& target) -> sentinel::GotStatus {
    switch (index) {
      case kGate: return sentinel::InstallThunk<GateThunk>(g_hooks[index], target, lookup);
      case kInit: return sentinel::InstallThunk<InitThunk>(g_hooks[index], target, lookup);
      case kShut: return sentinel::InstallThunk<ShutThunk>(g_hooks[index], target, lookup);
      case kInt: return sentinel::InstallThunk<IntersectThunk>(g_hooks[index], target, lookup);
      case kOcc: return sentinel::InstallThunk<OccludedThunk>(g_hooks[index], target, lookup);
      default: return sentinel::InstallThunk<TouchThunk>(g_hooks[index], target, lookup);
    }
  };
  const Planned plan[5] = {{"CncaGameSpace::StaticRaycast", kGateSymbol, &pins.gate, kGate},
                           {"CBVHTree::Initialize", kInitSymbol, &pins.init, kInit},
                           {"CBVHTree::Shutdown", kShutSymbol, &pins.shutdown, kShut},
                           {"CBVHTree::Intersect", kIntersectSymbol, &pins.intersect, kInt},
                           {"CBVHTree::Occluded", kOccludedSymbol, &pins.occluded, kOcc}};
  for (const Planned& p : plan) {
    if (p.index == kGate && !switches.gate) continue;
    const sentinel::GotStatus got = status(p.index, TargetFor(p.symbol, *p.slot, base));
    if (got != sentinel::GotStatus::kOk) {
      Undo(doneCount, done);
      Refused("hook_refused", p.name, sentinel::GotStatusName(got), 0);
      return {false, sentinel::GotStatusName(got)};
    }
    done[doneCount++] = p.index;
  }
  g_installed.store(true, std::memory_order_release);

  int hooks = doneCount;
  bool touchOn = false;
  if (switches.touch) {
    const sentinel::GotStatus got = status(kTouch, TargetFor(kTouchSymbol, pins.touch, base));
    touchOn = got == sentinel::GotStatus::kOk;
    if (touchOn) {
      ++hooks;
    } else {
      // Not fatal: with the gate on, touch casts miss anyway.
      sentinel::LogFields(sentinel::LogLevel::kError, "combat_bvh_touch",
                          {{"result", "refused"}, {"status", sentinel::GotStatusName(got)},
                           {"msg", "RayQueryStatic is left alone: touch rays keep the game's own answer"}});
    }
  }
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_bvh_install",
                      {{"result", "installed"}, {"reason", "ok"}, {"hooks", hooks}, {"gate", switches.gate ? 1 : 0}, {"touch", touchOn ? 1 : 0},
                       {"allowed_casts", g_allowedCount.load(std::memory_order_relaxed)},
                       {"msg", "CBVHTree answers from the game's own static BVH on a combat map; every other level keeps the game's own answers"}});
  return {true, "installed"};
}

ModuleResult Install(const Switches& switches) noexcept {
  const sentinel::ImageLookup lookup = g_lookup.load(std::memory_order_acquire);
  const Pins* const pins = g_pinsOverride.load(std::memory_order_acquire);
  return InstallAt(lookup != nullptr ? lookup : &sentinel::FindLoadedImage, pins != nullptr ? *pins : kPins, switches);
}

bool TreeInstalled() noexcept { return g_installed.load(std::memory_order_acquire); }

Counters GetCounters() noexcept {
  return {g_built.load(),  g_refused.load(), g_int.load(), g_intHit.load(), g_occ.load(),
          g_occHit.load(), g_gated.load(),   g_touch.load(), g_stock.load()};
}

int TreeCount() noexcept {
  pthread_mutex_lock(&g_treeMutex);
  const int n = g_treeCount;
  pthread_mutex_unlock(&g_treeMutex);
  return n;
}

namespace {

void LogSummary() noexcept {
  const std::uint64_t ni = g_int.exchange(0), nih = g_intHit.exchange(0), no = g_occ.exchange(0), noh = g_occHit.exchange(0);
  const std::uint64_t nt = g_touch.exchange(0), ng = g_gated.exchange(0), ns = g_stock.exchange(0);
  const std::uint64_t t = g_ns.exchange(0), mx = g_maxNs.exchange(0);
  if (ni == 0 && no == 0 && nt == 0 && ng == 0 && ns == 0) return;
  char avg[32], max[32];
  std::snprintf(avg, sizeof(avg), "%.2f", (ni + no) != 0 ? static_cast<double>(t) / 1000.0 / static_cast<double>(ni + no) : 0.0);
  std::snprintf(max, sizeof(max), "%.1f", static_cast<double>(mx) / 1000.0);
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_bvh_summary",
                      {{"window_s", 10}, {"intersect", ni}, {"intersect_hit", nih}, {"occluded", no}, {"occluded_blocked", noh},
                       {"touch_miss", nt}, {"gated_off", ng}, {"stock", ns}, {"us_per_cast_avg", avg}, {"us_per_cast_max", max},
                       {"msg", "casts answered from the level's static BVH in this window; stock counts calls the game's function answered"}});
}

void LookAtLevels(std::uintptr_t game) noexcept {
  const level::Loaded loaded = level::Scan(game);
  const bool in = loaded.combatMap && !loaded.echoArena;
  const bool before = detail::g_inCombatMap.load(std::memory_order_relaxed);
  if (in == before) return;
  detail::g_inCombatMap.store(in, std::memory_order_relaxed);
  const std::uint64_t levelId = in ? loaded.combatMapLevel : 0;
  g_scopeLevel.store(levelId, std::memory_order_relaxed);
  char lv[19];
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_bvh_scope",
                      {{"from", before ? 1 : 0}, {"to", in ? 1 : 0}, {"level", sentinel::HexString(lv, levelId)},
                       {"msg", in ? "a combat map is loaded: casts are answered from its static BVH"
                                  : "no combat map is loaded: the game's own answers stand"}});
}

}  // namespace

void Tick(std::uintptr_t game) noexcept {
  if (!g_installed.load(std::memory_order_acquire)) return;
  const double now = NowSeconds();
  if (now >= g_nextLook) {
    g_nextLook = now + kLookEverySeconds;
    LookAtLevels(game);
  }
  if (now < g_nextSummary) return;
  g_nextSummary = now + kSummaryEverySeconds;
  LogSummary();
}

void SetImageLookupForTest(sentinel::ImageLookup lookup) noexcept { g_lookup.store(lookup, std::memory_order_release); }
void SetPinsForTest(const Pins* pins) noexcept { g_pinsOverride.store(pins, std::memory_order_release); }
void SetClockForTest(double (*clock)() noexcept) noexcept { g_clock.store(clock, std::memory_order_release); }
std::uintptr_t LastGateReturnForTest() noexcept { return g_lastGateReturn.load(std::memory_order_relaxed); }
Switches LastSwitchesForTest() noexcept {
  const int bits = g_lastSwitches.load(std::memory_order_relaxed);
  Switches s;
  s.bvh = (bits & 1) != 0;
  s.gate = (bits & 2) != 0;
  s.touch = (bits & 4) != 0;
  return s;
}

void ResetForTest() noexcept {
  for (sentinel::GotHook& hook : g_hooks) hook.Forget();
  InitThunk::Reset();
  ShutThunk::Reset();
  IntersectThunk::Reset();
  OccludedThunk::Reset();
  TouchThunk::Reset();
  GateThunk::Reset();
  pthread_mutex_lock(&g_treeMutex);
  std::memset(g_trees, 0, sizeof(g_trees));
  g_treeCount = 0;
  pthread_mutex_unlock(&g_treeMutex);
  g_ran.store(false);
  g_installed.store(false);
  g_gateOn.store(false);
  g_base.store(0);
  g_allowedCount.store(0);
  g_lastGateReturn.store(0);
  g_lastSwitches.store(0);
  g_lookup.store(nullptr);
  g_pinsOverride.store(nullptr);
  g_clock.store(nullptr);
  g_built = g_refused = g_int = g_intHit = g_occ = g_occHit = g_gated = g_touch = g_stock = 0;
  g_ns = g_maxNs = 0;
  g_firstHit = g_firstBlock = false;
  g_nextSummary = 0;
  g_nextLook = 0;
  g_scopeLevel.store(0);
  detail::g_inCombatMap.store(false);
  g_mark.Set(0);
}

}  // namespace nevr_quest::combat::bvh
