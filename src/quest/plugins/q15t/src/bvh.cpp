#include "bvh.h"

#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "bvh_core.h"
#include "plugin_log.h"
#include "safe_read.h"

namespace q15t {
namespace {

// ---- state: plain atomics and PODs, no constructors ----
constexpr int kMaxTrees = 64;
struct Entry {
  std::uint64_t data;  // the SBVHTreeData address this tree was built from
  bvh::Tree tree;
};
Entry g_trees[kMaxTrees];
int g_ntrees = 0;
std::atomic_flag g_lock = ATOMIC_FLAG_INIT;

struct Lock {
  Lock() {
    while (g_lock.test_and_set(std::memory_order_acquire)) {
    }
  }
  ~Lock() { g_lock.clear(std::memory_order_release); }
};

std::atomic<std::uintptr_t> g_real[6];  // init, shutdown, intersect, occluded, touch, gate
enum { kInit, kShut, kInt, kOcc, kTouch, kGate };
std::atomic<bool> g_installed{false}, g_gate_on{false};
BvhPins g_pins{};
std::uintptr_t g_base = 0;
thread_local bool t_allowed = false;  // this thread is inside a cast the tree answers
std::atomic<std::uintptr_t> g_last_gate_ret{0};

std::atomic<std::uint64_t> g_built{0}, g_refused{0}, g_int{0}, g_int_hit{0}, g_occ{0}, g_occ_hit{0}, g_gated{0}, g_touch{0}, g_stock{0};
std::atomic<std::uint64_t> g_ns{0}, g_max_ns{0};
std::atomic<bool> g_first_hit{false}, g_first_block{false};
double g_next_summary = 0;

char g_dir_override[256] = "";
double (*g_clock)() = nullptr;

double NowS() {
  if (g_clock != nullptr) return g_clock();
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}
std::uint64_t NowNs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
}
void Timed(std::uint64_t t0) {
  const std::uint64_t d = NowNs() - t0;
  g_ns.fetch_add(d, std::memory_order_relaxed);
  std::uint64_t m = g_max_ns.load(std::memory_order_relaxed);
  while (d > m && !g_max_ns.compare_exchange_weak(m, d, std::memory_order_relaxed)) {
  }
}

// Same data directory as weapon_arm.cpp: /sdcard/Android/data/<pkg>/files/q15t (not created here).
bool OptionFile(const char* name) {
  char dir[320];
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
  char path[640];
  std::snprintf(path, sizeof path, "%s/%s", dir, name);
  return access(path, F_OK) == 0;
}

void Hex(char* out, std::size_t n, std::uint64_t v) { std::snprintf(out, n, "0x%llx", static_cast<unsigned long long>(v)); }

// Every page of [addr, addr + bytes) can be read (each page the range touches is probed at its first byte in the range).
bool Readable(std::uint64_t addr, std::uint64_t bytes) {
  constexpr std::uint64_t kPage = 4096;
  std::uint8_t b;
  for (std::uint64_t off = 0; off < bytes; off += kPage - ((addr + off) & (kPage - 1)))
    if (!SafeRead(static_cast<std::uintptr_t>(addr + off), &b, 1)) return false;
  return true;
}

void LogTree(std::uint32_t level, const char* message, std::uint64_t data, const char* why, long long tri4, long long nodes,
             long long root) {
  char d[24], a[24], n[24], r[24];
  Hex(d, sizeof d, data);
  std::snprintf(a, sizeof a, "%lld", tri4);
  std::snprintf(n, sizeof n, "%lld", nodes);
  std::snprintf(r, sizeof r, "0x%llx", static_cast<unsigned long long>(root));
  const NevrLogField f[] = {{"data", d}, {"why", why}, {"triangle4", a}, {"nodes", n}, {"root_ref", r}};
  Log(level, message, f, 5);
}

// ---- R20 / R21 ----
void BuildTree(void* self, const void* data) {
  const std::uint64_t d = reinterpret_cast<std::uintptr_t>(data);
  if (data == nullptr) {
    g_refused.fetch_add(1, std::memory_order_relaxed);
    LogTree(NEVR_LOG_INFO, "bvh tree none", 0, "no data", 0, 0, 0);
    return;
  }
  std::uint8_t hdr[bvh::kHeaderSize];
  if (!SafeRead(static_cast<std::uintptr_t>(d), hdr, sizeof hdr)) {
    g_refused.fetch_add(1, std::memory_order_relaxed);
    LogTree(NEVR_LOG_WARNING, "bvh tree refused", d, "header unreadable", 0, 0, 0);
    return;
  }
  bvh::Tree t;
  bvh::Blocks b;
  const char* why = "";
  if (!bvh::Parse(hdr, d, &t, &b, &why)) {
    g_refused.fetch_add(1, std::memory_order_relaxed);
    // an all-empty resource is what a level without static geometry ships: not a fault
    LogTree(std::strcmp(why, "empty") == 0 ? NEVR_LOG_INFO : NEVR_LOG_WARNING, "bvh tree none", d, why, 0, 0, 0);
    return;
  }
  if (!Readable(b.tri_ptr, b.tri_bytes) || !Readable(b.node_ptr, b.node_bytes)) {
    g_refused.fetch_add(1, std::memory_order_relaxed);
    LogTree(NEVR_LOG_WARNING, "bvh tree refused", d, "block unreadable", t.ntris, t.nnodes, static_cast<std::uint32_t>(t.root));
    return;
  }
  int slot = -1;
  {
    Lock lock;
    for (int i = 0; i < g_ntrees; ++i)
      if (g_trees[i].data == d) slot = i;
    if (slot < 0 && g_ntrees < kMaxTrees) slot = g_ntrees++;
    if (slot >= 0) {
      g_trees[slot].data = d;
      g_trees[slot].tree = t;
    }
  }
  if (slot < 0) {
    g_refused.fetch_add(1, std::memory_order_relaxed);
    LogTree(NEVR_LOG_ERROR, "bvh tree refused", d, "tree table full", t.ntris, t.nnodes, static_cast<std::uint32_t>(t.root));
    return;
  }
  *static_cast<const bvh::Tree**>(self) = &g_trees[slot].tree;
  g_built.fetch_add(1, std::memory_order_relaxed);
  LogTree(NEVR_LOG_INFO, "bvh tree built", d, "", t.ntris, t.nnodes, static_cast<std::uint32_t>(t.root));
}

__attribute__((noinline)) void InitializeWrapper(void* self, const void* data) {
  const auto real = reinterpret_cast<void (*)(void*, const void*)>(g_real[kInit].load(std::memory_order_acquire));
  if (real != nullptr) real(self, data);
  if (self == nullptr) return;
  *static_cast<const bvh::Tree**>(self) = nullptr;
  BuildTree(self, data);
}

__attribute__((noinline)) void ShutdownWrapper(void* self) {
  const auto real = reinterpret_cast<void (*)(void*)>(g_real[kShut].load(std::memory_order_acquire));
  if (real != nullptr) real(self);
  if (self != nullptr) *static_cast<const bvh::Tree**>(self) = nullptr;
}

// The tree a CBVHTree points at: one of our table entries (a word that is not one is a doubt: the stock function answers).
const bvh::Tree* TreeOf(const void* self) {
  std::uintptr_t p = 0;
  if (!SafeRead(reinterpret_cast<std::uintptr_t>(self), &p, sizeof p)) return nullptr;
  const std::uintptr_t lo = reinterpret_cast<std::uintptr_t>(&g_trees[0].tree);
  const std::uintptr_t off = p - lo;
  if (off >= sizeof g_trees || off % sizeof(Entry) != 0) return nullptr;  // (p below the table wraps `off` to a huge value)
  return reinterpret_cast<const bvh::Tree*>(p);
}

// ---- R22 ----
using IntersectFn = bool (*)(const void*, const float*, std::uint8_t*);
__attribute__((noinline)) bool IntersectWrapper(const void* self, const float* ray, std::uint8_t* hit) {
  const auto real = reinterpret_cast<IntersectFn>(g_real[kInt].load(std::memory_order_acquire));
  if (g_gate_on.load(std::memory_order_relaxed) && !t_allowed) {  // not a cast the tree is for: the stock miss
    g_gated.fetch_add(1, std::memory_order_relaxed);
    return real != nullptr ? real(self, ray, hit) : false;
  }
  const bvh::Tree* t = TreeOf(self);
  float r[6];
  if (t == nullptr || !SafeRead(reinterpret_cast<std::uintptr_t>(ray), r, sizeof r)) {
    g_stock.fetch_add(1, std::memory_order_relaxed);
    return real != nullptr ? real(self, ray, hit) : false;
  }
  const std::uint64_t t0 = NowNs();
  bvh::Hit h;
  const bool got = bvh::Trace(*t, bvh::Ray(r, r + 3, 0.0f, INFINITY), false, &h);
  Timed(t0);
  g_int.fetch_add(1, std::memory_order_relaxed);
  if (!got) return false;
  g_int_hit.fetch_add(1, std::memory_order_relaxed);
  std::memcpy(hit + 0x00, &h.id0, 4);
  std::memcpy(hit + 0x04, &h.id1, 4);
  std::memcpy(hit + 0x08, &h.u, 4);
  std::memcpy(hit + 0x0c, &h.v, 4);
  std::memcpy(hit + 0x10, &h.t, 4);
  std::memcpy(hit + 0x38, h.n, 12);
  if (!g_first_hit.exchange(true)) {
    char from[64], dir[64], tt[24], mesh[24], tri[24], nrm[64];
    std::snprintf(from, sizeof from, "%.2f %.2f %.2f", r[0], r[1], r[2]);
    std::snprintf(dir, sizeof dir, "%.2f %.2f %.2f", r[3], r[4], r[5]);
    std::snprintf(tt, sizeof tt, "%.3f", h.t);
    std::snprintf(mesh, sizeof mesh, "%d", h.id0);
    std::snprintf(tri, sizeof tri, "%d", h.id1);
    std::snprintf(nrm, sizeof nrm, "%.2f %.2f %.2f", h.n[0], h.n[1], h.n[2]);
    const NevrLogField f[] = {{"from", from}, {"dir", dir}, {"t", tt}, {"mesh", mesh}, {"tri", tri}, {"normal", nrm}};
    Log(NEVR_LOG_INFO, "bvh first hit", f, 6);
  }
  return true;
}

// ---- R23 ----
using OccludedFn = bool (*)(const void*, const float*, const float*);
__attribute__((noinline)) bool OccludedWrapper(const void* self, const float* a, const float* b) {
  const auto real = reinterpret_cast<OccludedFn>(g_real[kOcc].load(std::memory_order_acquire));
  const bvh::Tree* t = TreeOf(self);
  float pa[3], pb[3];
  if (t == nullptr || !SafeRead(reinterpret_cast<std::uintptr_t>(a), pa, sizeof pa) ||
      !SafeRead(reinterpret_cast<std::uintptr_t>(b), pb, sizeof pb)) {
    g_stock.fetch_add(1, std::memory_order_relaxed);
    return real != nullptr ? real(self, a, b) : false;
  }
  const std::uint64_t t0 = NowNs();
  const float d[3] = {pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]};
  // the segment a..b is t in (0, 1) along the unnormalised direction
  const bool got = bvh::Trace(*t, bvh::Ray(pa, d, 0.0f, 1.0f), true, nullptr);
  Timed(t0);
  g_occ.fetch_add(1, std::memory_order_relaxed);
  if (got) {
    g_occ_hit.fetch_add(1, std::memory_order_relaxed);
    if (!g_first_block.exchange(true)) {
      char s[64], e[64];
      std::snprintf(s, sizeof s, "%.2f %.2f %.2f", pa[0], pa[1], pa[2]);
      std::snprintf(e, sizeof e, "%.2f %.2f %.2f", pb[0], pb[1], pb[2]);
      const NevrLogField f[] = {{"from", s}, {"to", e}};
      Log(NEVR_LOG_INFO, "bvh first blocked segment", f, 2);
    }
  }
  return got;
}

// ---- R25: CR15TouchInteractQuery::RayQueryStatic as on the stock Quest: a miss, nothing written (disassembly 0x21cf458: cbz w0 -> epilogue) ----
__attribute__((noinline)) bool TouchWrapper(const void*, const void*, void*, float*) {
  g_touch.fetch_add(1, std::memory_order_relaxed);
  return false;
}

// ---- R24: the callers' gate. The PLT stub branches with br, so the return address is the caller's. ----
using StaticRaycastFn = bool (*)(const void*, const void*, void*, void**);
__attribute__((noinline)) bool GateWrapper(const void* self, const void* ray, void* hit, void** gs) {
  const std::uintptr_t ret = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)) - g_base;
  g_last_gate_ret.store(ret, std::memory_order_relaxed);
  bool allowed = false;
  for (int i = 0; i < g_pins.n_allowed; ++i) allowed |= g_pins.allowed[i] == ret;
  const auto real = reinterpret_cast<StaticRaycastFn>(g_real[kGate].load(std::memory_order_acquire));
  if (real == nullptr) return false;
  const bool outer = t_allowed;
  t_allowed = allowed;
  const bool got = real(self, ray, hit, gs);
  t_allowed = outer;
  return got;
}

// ---- install ----
struct Hook {
  const char* name;
  const char* symbol;
  int index;
  std::uintptr_t wrapper;
  BvhHookSlot s;
};

void LogHook(const Hook& h, const InstallResult& r) {
  char slot[24], real[24];
  Hex(slot, sizeof slot, h.s.slot);
  Hex(real, sizeof real, h.s.fn);
  const NevrLogField f[] = {{"hook", h.name}, {"slot_vaddr", slot}, {"real_vaddr", real}, {"status", StatusName(r.status)}, {"reason", r.reason}};
  if (r.status == SwapStatus::kOk) Log(NEVR_LOG_INFO, "bvh hook installed", f, 5);
  else Log(NEVR_LOG_ERROR, "bvh hook refused", f, 5);
}

void Rollback(std::uintptr_t base, const Hook* hooks, int n_done) {
  for (int i = 0; i < n_done; ++i) {
    const std::uintptr_t real = g_real[hooks[i].index].load(std::memory_order_acquire);
    const SwapResult r = SwapWord(base + hooks[i].s.slot, hooks[i].wrapper, real);
    const NevrLogField f[] = {{"hook", hooks[i].name}, {"status", StatusName(r.status)}};
    Log(r.status == SwapStatus::kOk ? NEVR_LOG_INFO : NEVR_LOG_ERROR, "bvh hook rolled back", f, 2);
  }
}

InstallResult Refuse(const char* reason, const char* detail, SwapStatus status) {
  InstallResult r;
  r.status = status;
  r.reason = reason;
  const NevrLogField f[] = {{"reason", reason}, {"detail", detail}, {"status", StatusName(status)}};
  Log(NEVR_LOG_ERROR, "bvh refused: nothing changed", f, 3);
  return r;
}

}  // namespace

InstallResult InstallBvhAt(std::uintptr_t base, const BvhPins& pins) {
  if (OptionFile("bvh.off")) {
    const NevrLogField f[] = {{"why", "bvh.off present in the q15t data directory"}};
    Log(NEVR_LOG_INFO, "bvh off", f, 1);
    InstallResult r;
    r.reason = "off_file";
    return r;
  }
  if (base == 0) return Refuse("no_base", "", SwapStatus::kNullSlot);
  const bool want_gate = !OptionFile("bvhgate.off");
  const bool want_touch = !OptionFile("bvhtouch.keep");
  const Hook tree[4] = {
      {"CBVHTree::Initialize", "_ZN10NRadEngine8CBVHTree10InitializeERKNS_12SBVHTreeDataE", kInit,
       reinterpret_cast<std::uintptr_t>(&InitializeWrapper), pins.init},
      {"CBVHTree::Shutdown", "_ZN10NRadEngine8CBVHTree8ShutdownEv", kShut, reinterpret_cast<std::uintptr_t>(&ShutdownWrapper), pins.shutdown},
      {"CBVHTree::Intersect", "_ZNK10NRadEngine8CBVHTree9IntersectERKNS_4CRayERNS_7SBVHHitE", kInt,
       reinterpret_cast<std::uintptr_t>(&IntersectWrapper), pins.intersect},
      {"CBVHTree::Occluded", "_ZNK10NRadEngine8CBVHTree8OccludedERKNS_8C3VectorES3_", kOcc, reinterpret_cast<std::uintptr_t>(&OccludedWrapper),
       pins.occluded},
  };
  const Hook gate = {"CncaGameSpace::StaticRaycast", "_ZNK10NRadEngine13CncaGameSpace13StaticRaycastERKNS_4CRayERNS_7SBVHHitERPNS_15CncaGameSpaceMTE",
                     kGate, reinterpret_cast<std::uintptr_t>(&GateWrapper), pins.gate};
  const Hook touch = {"CR15TouchInteractQuery::RayQueryStatic",
                      "_ZN10NRadEngine8NRadGame22CR15TouchInteractQuery14RayQueryStaticEPKNS_15CncaGameSpaceMTERKNS_4CRayERNS0_8SMeshHitERf", kTouch,
                      reinterpret_cast<std::uintptr_t>(&TouchWrapper), pins.touch};

  // all or none: every slot must hold its pinned function before any is swapped
  const Hook* check[5] = {&tree[0], &tree[1], &tree[2], &tree[3], &gate};
  for (int i = 0; i < (want_gate ? 5 : 4); ++i) {
    std::uintptr_t word = 0;
    if (!SafeRead(base + check[i]->s.slot, &word, sizeof word)) return Refuse("slot_unreadable", check[i]->name, SwapStatus::kUnmapped);
    if (word != base + check[i]->s.fn) return Refuse("slot_mismatch", check[i]->name, SwapStatus::kUnexpectedOriginal);
  }

  g_base = base;
  g_pins = pins;
  // the gate first: until its wrapper is in, no cast is marked, so a tree installed a moment later answers none
  Hook done[5];
  int n_done = 0;
  g_gate_on.store(want_gate, std::memory_order_release);
  if (want_gate) {
    const InstallResult r = InstallImportHook(gate.symbol, base, gate.s.slot, gate.wrapper, &g_real[kGate], gate.s.fn);
    LogHook(gate, r);
    if (r.status != SwapStatus::kOk) {
      g_gate_on.store(false, std::memory_order_release);
      return r;
    }
    done[n_done++] = gate;
  } else {
    const NevrLogField f[] = {{"why", "bvhgate.off present: every cast uses the tree"}};
    Log(NEVR_LOG_WARNING, "bvh callers' gate off", f, 1);
  }
  for (const Hook& h : tree) {
    const InstallResult r = InstallImportHook(h.symbol, base, h.s.slot, h.wrapper, &g_real[h.index], h.s.fn);
    LogHook(h, r);
    if (r.status != SwapStatus::kOk) {
      Rollback(base, done, n_done);
      g_gate_on.store(false, std::memory_order_release);
      return r;
    }
    done[n_done++] = h;
  }
  g_installed.store(true, std::memory_order_release);

  if (want_touch) {
    const InstallResult r = InstallImportHook(touch.symbol, base, touch.s.slot, touch.wrapper, &g_real[kTouch], touch.s.fn);
    LogHook(touch, r);  // a refusal here is an ERROR line but not fatal: with the gate on, touch casts miss anyway
  } else {
    const NevrLogField f[] = {{"why", "bvhtouch.keep present: RayQueryStatic is left alone"}};
    Log(NEVR_LOG_INFO, "bvh touch rays kept", f, 1);
  }
  char s[16];
  std::snprintf(s, sizeof s, "%d", pins.n_allowed);
  const NevrLogField f[] = {{"gate", want_gate ? "on" : "off"}, {"allowed_casts", s}};
  Log(NEVR_LOG_INFO, "bvh on: CBVHTree answers from the game's own static BVH", f, 2);
  InstallResult ok;
  ok.status = SwapStatus::kOk;
  return ok;
}

InstallResult InstallBvh(std::uintptr_t base) { return InstallBvhAt(base, kBvhPinned); }

bool BvhTreeInstalled() { return g_installed.load(std::memory_order_acquire); }

BvhCounters BvhGetCounters() {
  return {g_built.load(),  g_refused.load(), g_int.load(), g_int_hit.load(), g_occ.load(),
          g_occ_hit.load(), g_gated.load(),   g_touch.load(), g_stock.load()};
}

int BvhTreeCount() {
  Lock lock;
  return g_ntrees;
}

void BvhTick(std::uintptr_t) {
  const double now = NowS();
  if (g_next_summary == 0) g_next_summary = now + 10.0;
  if (now < g_next_summary) return;
  g_next_summary = now + 10.0;
  const std::uint64_t ni = g_int.exchange(0), nih = g_int_hit.exchange(0), no = g_occ.exchange(0), noh = g_occ_hit.exchange(0);
  const std::uint64_t nt = g_touch.exchange(0), ng = g_gated.exchange(0), ns = g_stock.exchange(0);
  const std::uint64_t t = g_ns.exchange(0), mx = g_max_ns.exchange(0);
  if (!ni && !no && !nt && !ng && !ns) return;
  char b[8][32];
  std::snprintf(b[0], sizeof b[0], "%llu", static_cast<unsigned long long>(ni));
  std::snprintf(b[1], sizeof b[1], "%llu", static_cast<unsigned long long>(nih));
  std::snprintf(b[2], sizeof b[2], "%llu", static_cast<unsigned long long>(no));
  std::snprintf(b[3], sizeof b[3], "%llu", static_cast<unsigned long long>(noh));
  std::snprintf(b[4], sizeof b[4], "%llu", static_cast<unsigned long long>(nt));
  std::snprintf(b[5], sizeof b[5], "%llu", static_cast<unsigned long long>(ng));
  char avg[32], max[32], stock[32];
  std::snprintf(avg, sizeof avg, "%.2f", (ni + no) ? static_cast<double>(t) / 1000.0 / static_cast<double>(ni + no) : 0.0);
  std::snprintf(max, sizeof max, "%.1f", static_cast<double>(mx) / 1000.0);
  std::snprintf(stock, sizeof stock, "%llu", static_cast<unsigned long long>(ns));
  const NevrLogField f[] = {{"window_s", "10"}, {"intersect", b[0]}, {"intersect_hit", b[1]}, {"occluded", b[2]},
                            {"occluded_blocked", b[3]}, {"touch_kept_off", b[4]}, {"gated_off", b[5]}, {"stock", stock},
                            {"us_per_ray_avg", avg}};
  Log(NEVR_LOG_INFO, "bvh summary", f, 9);
  const NevrLogField g[] = {{"us_per_ray_max", max}};
  Log(NEVR_LOG_DEBUG, "bvh summary max", g, 1);
}

void SetBvhDirForTest(const char* dir) { std::snprintf(g_dir_override, sizeof g_dir_override, "%s", dir != nullptr ? dir : ""); }
void SetBvhClockForTest(double (*clock)()) { g_clock = clock; }
void ResetBvhForTest() {
  {
    Lock lock;
    std::memset(g_trees, 0, sizeof g_trees);
    g_ntrees = 0;
  }
  for (auto& r : g_real) r.store(0);
  g_installed.store(false);
  g_gate_on.store(false);
  g_built = g_refused = g_int = g_int_hit = g_occ = g_occ_hit = g_gated = g_touch = g_stock = 0;
  g_ns = g_max_ns = 0;
  g_first_hit = g_first_block = false;
  g_next_summary = 0;
  g_last_gate_ret = 0;
  t_allowed = false;
}
std::uintptr_t BvhLastGateRetForTest() { return g_last_gate_ret.load(); }

}  // namespace q15t
