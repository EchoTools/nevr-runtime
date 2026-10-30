// Host test of the bvh module (src/quest/plugins/q15t/src/bvh.cpp): the real installer on fake JUMP_SLOT pages (real mprotect, real /proc/self/maps),
// the wrappers called through their slots as the game's PLT stubs do, against fake stock functions and fake game structures
// laid out as the pinned build's code reads them (docs/quest/q15t/modules/bvh.md section 3). Real SafeRead, real files. Synthetic meshes.
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "bvh.h"
#include "bvh_build.h"
#include "nevr/plugin_interface.h"
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
static int Count(const char* needle, int level = -1) {
  int c = 0;
  for (const auto& l : g_lines)
    if (l.text.find(needle) != std::string::npos && (level < 0 || (int)l.level == level)) ++c;
  return c;
}

using q15t::BvhPins;

// ---- fake stock functions (what the pinned build's stubs do) ----
static int g_stock_init, g_stock_shut, g_stock_int, g_stock_occ, g_stock_touch, g_stock_gate;
using IntersectFn = bool (*)(const void*, const float*, uint8_t*);
using OccludedFn = bool (*)(const void*, const float*, const float*);
using GateFn = bool (*)(const void*, const void*, void*, void**);
using TouchFn = bool (*)(const void*, const void*, void*, float*);
using InitFn = void (*)(void*, const void*);
using ShutFn = void (*)(void*);

static uintptr_t* g_page = nullptr;  // the current fake slot page
enum { kSlotInit = 0x20 / 8, kSlotShut, kSlotInt, kSlotOcc, kSlotTouch, kSlotGate };
static void* g_cbvh_self = nullptr;  // the CBVHTree CncaGame::StaticRaycast would pass to Intersect
static bool g_nested = false, g_spawn = false;
static int g_spawn_result = -1;
static bool g_stock_gate_ran_with_args = false;

extern "C" __attribute__((noinline)) void StockInit(void*, const void*) { ++g_stock_init; }
extern "C" __attribute__((noinline)) void StockShut(void*) { ++g_stock_shut; }
extern "C" __attribute__((noinline)) bool StockInt(const void*, const float*, uint8_t*) {
  ++g_stock_int;
  return false;
}
extern "C" __attribute__((noinline)) bool StockOcc(const void*, const float*, const float*) {
  ++g_stock_occ;
  return false;
}
extern "C" __attribute__((noinline)) bool StockTouch(const void*, const void*, void*, float*) {
  ++g_stock_touch;
  return true;  // distinguishable from the replaced one, which answers false
}

__attribute__((noinline)) static bool CallGateAllowedSite(const void* a, const void* b, void* c, void** d) {
  volatile bool r = reinterpret_cast<GateFn>(g_page[kSlotGate])(a, b, c, d);
  return r;
}
__attribute__((noinline)) static bool CallGateOtherSite(const void* a, const void* b, void* c, void** d) {
  volatile bool r = reinterpret_cast<GateFn>(g_page[kSlotGate])(a, b, c, d);
  return r;
}
static bool CallInt(const void* self, const float* ray, uint8_t* hit) { return reinterpret_cast<IntersectFn>(g_page[kSlotInt])(self, ray, hit); }

// CncaGame::StaticRaycast: casts into the gamespace's CBVHTree through the PLT slot
extern "C" __attribute__((noinline)) bool StockGate(const void* self, const void* ray, void* hit, void** gs) {
  ++g_stock_gate;
  g_stock_gate_ran_with_args = self == reinterpret_cast<const void*>(0x77) && ray != nullptr && hit != nullptr;
  if (g_nested) {  // a second cast from an unlisted call site, inside the allowed one
    g_nested = false;
    void* dummy = nullptr;
    uint8_t scratch[0x44];
    CallGateOtherSite(self, ray, scratch, &dummy);
  }
  if (g_spawn) {  // another thread casts directly while this thread is inside an allowed cast
    g_spawn = false;
    std::thread th([&] {
      uint8_t scratch[0x44];
      g_spawn_result = CallInt(g_cbvh_self, static_cast<const float*>(ray), scratch);
    });
    th.join();
  }
  const bool r = CallInt(g_cbvh_self, static_cast<const float*>(ray), static_cast<uint8_t*>(hit));
  if (r && gs != nullptr) *gs = reinterpret_cast<void*>(0x1234);
  return r;
}

// ---- the fake slot page ----
struct World {
  uint8_t* mem = nullptr;  // 2 pages
  uintptr_t base = 0;
  BvhPins pins{};
};
static World MakeWorld(uintptr_t allowed_abs_ret, bool exec_intersect_page) {
  World w;
  w.mem = static_cast<uint8_t*>(mmap(nullptr, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (w.mem == MAP_FAILED) std::abort();
  w.base = reinterpret_cast<uintptr_t>(w.mem);
  auto* p = reinterpret_cast<uintptr_t*>(w.mem);
  auto fn = [&](const void* f) { return reinterpret_cast<uintptr_t>(f) - w.base; };
  const uintptr_t fns[6] = {fn(reinterpret_cast<const void*>(&StockInit)), fn(reinterpret_cast<const void*>(&StockShut)),
                            fn(reinterpret_cast<const void*>(&StockInt)),  fn(reinterpret_cast<const void*>(&StockOcc)),
                            fn(reinterpret_cast<const void*>(&StockTouch)), fn(reinterpret_cast<const void*>(&StockGate))};
  const uintptr_t slots[6] = {0x20, 0x28, exec_intersect_page ? 0x1030u : 0x30u, 0x38, 0x40, 0x48};
  for (int i = 0; i < 6; ++i) *reinterpret_cast<uintptr_t*>(w.mem + slots[i]) = w.base + fns[i];
  (void)p;
  w.pins.init = {slots[0], fns[0]};
  w.pins.shutdown = {slots[1], fns[1]};
  w.pins.intersect = {slots[2], fns[2]};
  w.pins.occluded = {slots[3], fns[3]};
  w.pins.touch = {slots[4], fns[4]};
  w.pins.gate = {slots[5], fns[5]};
  w.pins.n_allowed = 0;
  if (allowed_abs_ret != 0) {
    w.pins.allowed[0] = allowed_abs_ret - w.base;
    w.pins.n_allowed = 1;
  }
  mprotect(w.mem, 4096, PROT_READ);
  mprotect(w.mem + 4096, 4096, exec_intersect_page ? (PROT_READ | PROT_EXEC) : PROT_READ);
  return w;
}
// the slot words of the page as the plugin's wrappers see them
static uintptr_t SlotWord(const World& w, uintptr_t slot) { return *reinterpret_cast<const uintptr_t*>(w.mem + slot); }

static std::string g_dir;
static void ClearDir() {
  const char* names[] = {"bvh.off", "bvhgate.off", "bvhtouch.keep"};
  for (const char* n : names) unlink((g_dir + "/" + n).c_str());
}
static void Touch(const char* name) {
  FILE* f = std::fopen((g_dir + "/" + name).c_str(), "w");
  if (f) std::fclose(f);
}

static double g_now = 100.0;
static double Clock() { return g_now; }

// ---- fake game structures ----
static bt::Resource RoomResource(bool fixed_ptrs, int leaf = 4, uint32_t pad = 0) {
  const float lo[3] = {-5, -5, -5}, hi[3] = {5, 5, 5};
  static std::vector<bt::TriIn> in;
  in.clear();
  bt::AddBox(in, lo, hi, 100, true);
  const bt::Built b = bt::Builder(in, leaf).Build(static_cast<int>(pad));
  return bt::MakeResource(b, fixed_ptrs);
}

// two parallel squares in one leaf, the far one (z = 5, id0 200) listed before the near one (z = 2, id0 201): the nearest hit is not the first found
static bt::Resource TwoQuadResource() {
  std::vector<bt::TriIn> in;
  int id1 = 0;
  const float far_[4][3] = {{-1, -1, 5}, {1, -1, 5}, {1, 1, 5}, {-1, 1, 5}}, near_[4][3] = {{-1, -1, 2}, {1, -1, 2}, {1, 1, 2}, {-1, 1, 2}};
  const float toward[3] = {0, 0, -10};
  bt::AddQuad(in, far_[0], far_[1], far_[2], far_[3], toward, 200, id1);
  bt::AddQuad(in, near_[0], near_[1], near_[2], near_[3], toward, 201, id1);
  return bt::MakeResource(bt::Builder(in, 8).BuildLeafRoot(), true);
}

static void Reset() {
  g_lines.clear();
  q15t::ResetBvhForTest();
  g_stock_init = g_stock_shut = g_stock_int = g_stock_occ = g_stock_touch = g_stock_gate = 0;
  g_nested = g_spawn = false;
  g_spawn_result = -1;
  g_cbvh_self = nullptr;
  ClearDir();
}

// a world with the module installed (gate on unless the option file says otherwise)
static World Install(uintptr_t allowed_abs_ret = 0, uintptr_t second_abs_ret = 0) {
  World w = MakeWorld(allowed_abs_ret, false);
  if (second_abs_ret != 0) {
    w.pins.allowed[1] = second_abs_ret - w.base;
    w.pins.n_allowed = 2;
  }
  g_page = reinterpret_cast<uintptr_t*>(w.mem);
  const auto r = q15t::InstallBvhAt(w.base, w.pins);
  CHECK(r.status == q15t::SwapStatus::kOk, "install: %s", q15t::StatusName(r.status));
  return w;
}

static const float kDown[6] = {0, 0, 0, 0, -1, 0};  // from the room's centre, straight down: hits the floor (id0 102) at t = 5

int main() {
  char tmpl[] = "/tmp/q15t-bvh-XXXXXX";
  if (mkdtemp(tmpl) == nullptr) return 2;
  g_dir = tmpl;
  NvrGameContext ctx{};
  ctx.ctx_size = sizeof ctx;
  ctx.plugin_id = "q15t";
  ctx.log = &FakeLog;
  q15t::SetLogContext(&ctx);
  q15t::SetBvhDirForTest(g_dir.c_str());
  q15t::SetBvhClockForTest(&Clock);

  // ---------------------------------------------------------------- 0. the pinned constants (readelf -rW: docs/quest/q15t/modules/bvh.md section 1)
  {
    const q15t::BvhPins& p = q15t::kBvhPinned;
    CHECK(p.init.slot == 0x36ec548 && p.init.fn == 0x1a45a88, "R20 pin");
    CHECK(p.shutdown.slot == 0x36eb648 && p.shutdown.fn == 0x1a45a8c, "R21 pin");
    CHECK(p.intersect.slot == 0x36d38f0 && p.intersect.fn == 0x1a45ae4, "R22 pin");
    CHECK(p.occluded.slot == 0x36e8430 && p.occluded.fn == 0x1a45b38, "R23 pin");
    CHECK(p.gate.slot == 0x36c0288 && p.gate.fn == 0x18066e4, "R24 pin");
    CHECK(p.touch.slot == 0x36f37d8 && p.touch.fn == 0x20cf414, "R25 pin");
    CHECK(p.n_allowed == 3 && p.allowed[0] == 0x12b2b4c && p.allowed[1] == 0x12b2bc4 && p.allowed[2] == 0x12c0a54, "allowed call sites");
  }

  // ---------------------------------------------------------------- 1. install: six slots, one line each, stock words kept
  Reset();
  Touch("bvhgate.off");  // the tree alone first; the gate has its own section
  {
    World w = MakeWorld(0, false);
    g_page = reinterpret_cast<uintptr_t*>(w.mem);
    uintptr_t before[6];
    for (int i = 0; i < 6; ++i) before[i] = g_page[0x20 / 8 + i];
    const auto r = q15t::InstallBvhAt(w.base, w.pins);
    CHECK(r.status == q15t::SwapStatus::kOk && q15t::BvhTreeInstalled(), "install: %s", q15t::StatusName(r.status));
    for (int i : {0, 1, 2, 3, 4}) CHECK(g_page[0x20 / 8 + i] != before[i], "slot %d not replaced", i);
    CHECK(g_page[kSlotGate] == before[5], "the gate slot was swapped although bvhgate.off is present");
    CHECK(Count("bvh hook installed", 1) == 5, "hook lines: %d", Count("bvh hook installed", 1));
    CHECK(Count("bvh on", 1) == 1 && Count("gate=off") == 1, "no summary line");
    CHECK(Count("bvh callers' gate off", 2) == 1, "gate-off not reported as WARNING");

    // ------------------------------------------------------------ 2. Initialize / Intersect / Occluded / Shutdown through the slots
    bt::Resource res = RoomResource(true);
    uint64_t cbvh = 0xdeadbeef;  // the stock constructor zeroes it; a garbage value must not matter
    g_cbvh_self = &cbvh;
    reinterpret_cast<InitFn>(g_page[kSlotInit])(&cbvh, res.bytes.data());
    CHECK(g_stock_init == 1, "the stock Initialize was not called");
    CHECK(cbvh != 0 && cbvh != 0xdeadbeef, "no tree stored in the CBVHTree");
    CHECK(Count("bvh tree built", 1) == 1 && Count("triangle4=") == 1, "no tree line");
    CHECK(q15t::BvhTreeCount() == 1, "tree count %d", q15t::BvhTreeCount());

    uint8_t hit[0x44];
    std::memset(hit, 0xab, sizeof hit);
    const int32_t m1 = -1;
    std::memcpy(hit + 0, &m1, 4);
    std::memcpy(hit + 4, &m1, 4);
    const bool got = CallInt(&cbvh, kDown, hit);
    CHECK(got, "no hit on the floor");
    int32_t id0, id1;
    float u, v, t, n[3];
    std::memcpy(&id0, hit + 0, 4);
    std::memcpy(&id1, hit + 4, 4);
    std::memcpy(&u, hit + 8, 4);
    std::memcpy(&v, hit + 12, 4);
    std::memcpy(&t, hit + 0x10, 4);
    std::memcpy(n, hit + 0x38, 12);
    CHECK(id0 == 102 && id1 >= 0 && t > 4.99f && t < 5.01f, "hit fields id0 %d id1 %d t %.3f", id0, id1, t);
    CHECK(u >= 0 && v >= 0 && u + v <= 1.0001f, "u %.3f v %.3f", u, v);
    CHECK(n[1] > 0.99f && n[0] * n[0] + n[1] * n[1] + n[2] * n[2] > 0.99f, "normal (%.2f %.2f %.2f)", n[0], n[1], n[2]);
    bool untouched = true;
    for (int i = 0x14; i < 0x38; ++i) untouched &= hit[i] == 0xab;
    CHECK(untouched, "bytes 0x14..0x37 of the hit were written (they are the caller's)");
    CHECK(Count("bvh first hit", 1) == 1, "first-hit line %d", Count("bvh first hit", 1));
    CallInt(&cbvh, kDown, hit);
    CHECK(Count("bvh first hit") == 1, "first-hit line repeated");
    // a miss writes nothing
    uint8_t clean[0x44];
    std::memset(clean, 0xcd, sizeof clean);
    const float up_out[6] = {0, 20, 0, 0, 1, 0};  // from outside, away from the room
    CHECK(!CallInt(&cbvh, up_out, clean), "a ray leaving into empty space hit");
    bool all_cd = true;
    for (uint8_t b : clean) all_cd &= b == 0xcd;
    CHECK(all_cd && g_stock_int == 0, "a miss wrote the hit, or went to the stock function (%d)", g_stock_int);

    // Occluded: the segment a..b is t in (0, 1): clear inside, blocked through a wall
    const float a[3] = {0, 0, 0}, in_[3] = {0, 0, 4.9f}, out[3] = {0, 0, 5.2f};
    auto occ = [&](const void* self, const float* p, const float* q) { return reinterpret_cast<OccludedFn>(g_page[kSlotOcc])(self, p, q); };
    CHECK(!occ(&cbvh, a, in_), "a clear segment is blocked");
    CHECK(occ(&cbvh, a, out), "a segment through the wall is not blocked");
    CHECK(Count("bvh first blocked segment", 1) == 1, "first-blocked line %d", Count("bvh first blocked segment"));
    occ(&cbvh, a, out);
    CHECK(Count("bvh first blocked segment") == 1, "first-blocked line repeated");
    const float a3[3] = {0, 0, 3}, out2[3] = {0, 0, 5.5f};  // blocked going +z, but not if the direction were reversed
    CHECK(occ(&cbvh, a3, out2), "a segment from inside to past the wall is not blocked");
    auto c = q15t::BvhGetCounters();
    CHECK(c.intersect == 3 && c.intersect_hit == 2 && c.occluded == 4 && c.occluded_blocked == 3 && c.trees_built == 1 && c.stock == 0,
          "counters int %llu/%llu occ %llu/%llu built %llu stock %llu", (unsigned long long)c.intersect, (unsigned long long)c.intersect_hit,
          (unsigned long long)c.occluded, (unsigned long long)c.occluded_blocked, (unsigned long long)c.trees_built, (unsigned long long)c.stock);

    // the file-form resource (both header pointers 0: the blocks follow the header) and a root that is not node 0
    bt::Resource file_form = RoomResource(false, 2, 5);
    uint64_t cbvh2 = 0;
    reinterpret_cast<InitFn>(g_page[kSlotInit])(&cbvh2, file_form.bytes.data());
    uint8_t hit2[0x44];
    std::memset(hit2, 0, sizeof hit2);
    CHECK(cbvh2 != 0 && CallInt(&cbvh2, kDown, hit2), "file-form resource with root at node 5 gave no tree / hit");
    CHECK(Count("root_ref=0x28") == 1, "root reference not logged");

    // the nearest hit, not the first one found: a leaf that lists the far square first
    bt::Resource two = TwoQuadResource();
    uint64_t cbvh3 = 0;
    reinterpret_cast<InitFn>(g_page[kSlotInit])(&cbvh3, two.bytes.data());
    const float ray3[6] = {0, 0, -10, 0, 0, 1};
    uint8_t hit3[0x44];
    std::memset(hit3, 0, sizeof hit3);
    int32_t id3 = -1;
    float t3 = 0;
    CHECK(cbvh3 != 0 && CallInt(&cbvh3, ray3, hit3), "two squares: no hit");
    std::memcpy(&id3, hit3, 4);
    std::memcpy(&t3, hit3 + 0x10, 4);
    CHECK(id3 == 201 && t3 > 11.99f && t3 < 12.01f, "the nearest square was not returned: id0 %d t %.3f", id3, t3);

    // Shutdown clears; the next Intersect goes to the stock function
    reinterpret_cast<ShutFn>(g_page[kSlotShut])(&cbvh);
    CHECK(g_stock_shut == 1 && cbvh == 0, "Shutdown: stock %d word %llx", g_stock_shut, (unsigned long long)cbvh);
    g_cbvh_self = &cbvh;
    CHECK(!CallInt(&cbvh, kDown, hit) && g_stock_int == 1, "after Shutdown the stock Intersect must answer (%d)", g_stock_int);
    CHECK(!occ(&cbvh, a, out) && g_stock_occ == 1, "after Shutdown the stock Occluded must answer");

    // a CBVHTree word that is not one of our table entries is a doubt: the stock function answers, nothing is dereferenced
    uint64_t garbage = 0x1234567;
    CHECK(!CallInt(&garbage, kDown, hit) && g_stock_int == 2, "a foreign tree word was followed");
    uint64_t misaligned = cbvh2 + 1;
    CHECK(!CallInt(&misaligned, kDown, hit) && g_stock_int == 3, "a misaligned tree word was followed");
    uint64_t kept = cbvh2;
    CHECK(CallInt(&kept, kDown, hit), "a copy of a valid CBVHTree (same word) does not answer");

    // an argument that cannot be read: the stock function, no fault
    CHECK(!CallInt(&kept, reinterpret_cast<const float*>(0x1000), hit) && g_stock_int == 4, "unreadable ray not handed to the stock function");
    CHECK(!occ(&kept, reinterpret_cast<const float*>(0x1000), out) && g_stock_occ == 2, "unreadable segment start not handed to the stock function");
    CHECK(!occ(&kept, a, reinterpret_cast<const float*>(0x1000)) && g_stock_occ == 3, "unreadable segment end not handed to the stock function");
    CHECK(q15t::BvhGetCounters().stock == 7, "stock counter %llu", (unsigned long long)q15t::BvhGetCounters().stock);

    // ------------------------------------------------------------ 3. touch rays: a miss, as the stock Quest
    CHECK(g_page[kSlotTouch] != reinterpret_cast<uintptr_t>(&StockTouch), "touch slot not replaced");
    CHECK(!reinterpret_cast<TouchFn>(g_page[kSlotTouch])(nullptr, nullptr, nullptr, nullptr) && g_stock_touch == 0, "touch ray not a miss");
    CHECK(q15t::BvhGetCounters().touch == 1, "touch counter");
  }

  // ---------------------------------------------------------------- 4. Initialize refusals: no tree, a reason, the stock answer
  Reset();
  Touch("bvhgate.off");
  {
    World w = Install();
    uint64_t cbvh = 0x55;
    const uint8_t zeros_for_null_self[0x50] = {};
    auto init = [&](const void* data) { reinterpret_cast<InitFn>(g_page[kSlotInit])(&cbvh, data); };
    init(nullptr);
    CHECK(cbvh == 0 && Count("why=no data", 1) == 1, "null data: word %llx", (unsigned long long)cbvh);
    reinterpret_cast<InitFn>(g_page[kSlotInit])(nullptr, zeros_for_null_self);  // a null CBVHTree: nothing to store into, no fault
    CHECK(q15t::BvhTreeCount() == 0, "a tree was built for a null CBVHTree");
    cbvh = 0x55;
    init(reinterpret_cast<const void*>(0x1000));
    CHECK(cbvh == 0 && Count("header unreadable", 2) == 1, "unreadable header: word %llx", (unsigned long long)cbvh);
    uint8_t zeros[0x50] = {};
    cbvh = 0x55;
    init(zeros);
    CHECK(cbvh == 0 && Count("why=empty", 1) == 1, "empty resource: word %llx", (unsigned long long)cbvh);
    uint8_t bad[0x50] = {};
    const uint64_t s0 = 0xe1, s1 = 0x40;
    std::memcpy(bad + 8, &s0, 8);
    std::memcpy(bad + 0x28, &s1, 8);
    cbvh = 0x55;
    init(bad);
    CHECK(cbvh == 0 && Count("why=block sizes", 2) == 1, "bad sizes: word %llx", (unsigned long long)cbvh);
    uint8_t far_blocks[0x50] = {};  // a header whose blocks are in unmapped memory
    const uint64_t p0 = 0x10000, p1 = 0x20000, e0 = 0xe0;
    std::memcpy(far_blocks + 0, &p0, 8);
    std::memcpy(far_blocks + 8, &e0, 8);
    std::memcpy(far_blocks + 0x20, &p1, 8);
    std::memcpy(far_blocks + 0x28, &s1, 8);
    cbvh = 0x55;
    init(far_blocks);
    CHECK(cbvh == 0 && Count("block unreadable", 2) == 1, "unreadable blocks: word %llx", (unsigned long long)cbvh);
    // a block whose FIRST page reads but whose later pages do not: the whole block is probed
    void* two = mmap(nullptr, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    munmap(static_cast<uint8_t*>(two) + 4096, 4096);
    uint8_t half[0x50] = {};
    const uint64_t pp = reinterpret_cast<uintptr_t>(two) + 4096 - 0xe0, big = 4 * 0xe0;  // 2 records straddle into the unmapped page
    std::memcpy(half + 0, &pp, 8);
    std::memcpy(half + 8, &big, 8);
    const uint64_t pn = reinterpret_cast<uintptr_t>(two);
    std::memcpy(half + 0x20, &pn, 8);
    std::memcpy(half + 0x28, &s1, 8);
    cbvh = 0x55;
    g_lines.clear();
    init(half);
    CHECK(cbvh == 0 && Count("block unreadable", 2) == 1, "a block whose tail is unmapped was accepted: word %llx", (unsigned long long)cbvh);
    // the same for the node block
    uint8_t half_nodes[0x50] = {};
    const uint64_t one_tri = 0xe0, two_nodes = 2 * 0x40;
    const uint64_t pnn = reinterpret_cast<uintptr_t>(two) + 4096 - 0x40;  // the second node is in the unmapped page
    std::memcpy(half_nodes + 0, &pn, 8);
    std::memcpy(half_nodes + 8, &one_tri, 8);
    std::memcpy(half_nodes + 0x20, &pnn, 8);
    std::memcpy(half_nodes + 0x28, &two_nodes, 8);
    cbvh = 0x55;
    g_lines.clear();
    init(half_nodes);
    CHECK(cbvh == 0 && Count("block unreadable", 2) == 1, "a node block whose tail is unmapped was accepted: word %llx", (unsigned long long)cbvh);
    munmap(two, 4096);
    CHECK(q15t::BvhGetCounters().trees_refused == 7 && q15t::BvhGetCounters().trees_built == 0, "refused %llu built %llu",
          (unsigned long long)q15t::BvhGetCounters().trees_refused, (unsigned long long)q15t::BvhGetCounters().trees_built);
    CHECK(q15t::BvhTreeCount() == 0, "a refused tree took a table entry");
    (void)w;
  }

  // ---------------------------------------------------------------- 5. the table: 64 trees, the same data reuses its entry, the 65th is refused loudly
  Reset();
  Touch("bvhgate.off");
  {
    World w = Install();
    std::vector<bt::Resource> res;
    for (int i = 0; i < 66; ++i) res.push_back(RoomResource(true, 8));
    std::vector<uint64_t> cb(66, 0);
    for (int i = 0; i < 64; ++i) reinterpret_cast<InitFn>(g_page[kSlotInit])(&cb[i], res[i].bytes.data());
    CHECK(q15t::BvhTreeCount() == 64, "tree count %d", q15t::BvhTreeCount());
    bool all = true;
    for (int i = 0; i < 64; ++i) all &= cb[i] != 0;
    CHECK(all, "one of the first 64 got no tree");
    reinterpret_cast<InitFn>(g_page[kSlotInit])(&cb[64], res[64].bytes.data());
    CHECK(cb[64] == 0 && Count("tree table full", 3) == 1, "the 65th tree: word %llx", (unsigned long long)cb[64]);
    uint64_t again = 0;
    reinterpret_cast<InitFn>(g_page[kSlotInit])(&again, res[3].bytes.data());  // the same data: the same entry, even with the table full
    CHECK(again == cb[3] && q15t::BvhTreeCount() == 64, "re-initialising the same data: %llx vs %llx", (unsigned long long)again, (unsigned long long)cb[3]);
    // a CBVHTree word that is the right distance from the table but past its last entry is not a tree
    const uint64_t stride = cb[63] - cb[62];
    uint64_t past = cb[63] + stride;
    const int stock_before = g_stock_int;
    uint8_t hit_p[0x44];
    std::memset(hit_p, 0, sizeof hit_p);
    CHECK(!CallInt(&past, kDown, hit_p) && g_stock_int == stock_before + 1, "a word past the end of the table was followed");
    uint64_t below = cb[0] - stride;
    CHECK(!CallInt(&below, kDown, hit_p) && g_stock_int == stock_before + 2, "a word before the start of the table was followed");
    uint64_t last = cb[63];
    CHECK(CallInt(&last, kDown, hit_p) && g_stock_int == stock_before + 2, "the last table entry does not answer");
    (void)w;
  }

  // ---------------------------------------------------------------- 6. the callers' gate
  Reset();
  uintptr_t allowed_abs = 0, other_abs = 0;
  {  // pass 1: no allowed casts; learn the return address of CallGateAllowedSite
    World w = Install(0);
    uint64_t cbvh = 0;
    bt::Resource res = RoomResource(true);
    reinterpret_cast<InitFn>(g_page[kSlotInit])(&cbvh, res.bytes.data());
    g_cbvh_self = &cbvh;
    uint8_t hit[0x44];
    std::memset(hit, 0, sizeof hit);
    void* gs = nullptr;
    CHECK(Count("gate=on") == 1, "gate not on");
    const bool r = CallGateAllowedSite(reinterpret_cast<const void*>(0x77), kDown, hit, &gs);
    CHECK(!r && gs == nullptr, "a cast from an unlisted call site hit the tree");
    CHECK(g_stock_gate == 1 && g_stock_gate_ran_with_args, "the real StaticRaycast did not run with the caller's arguments");
    CHECK(g_stock_int == 1 && q15t::BvhGetCounters().gated == 1 && q15t::BvhGetCounters().intersect == 0, "gated cast: stock %d gated %llu", g_stock_int,
          (unsigned long long)q15t::BvhGetCounters().gated);
    allowed_abs = q15t::BvhLastGateRetForTest() + w.base;
    CHECK(allowed_abs != w.base, "the gate recorded no return address");
    CallGateOtherSite(reinterpret_cast<const void*>(0x77), kDown, hit, &gs);
    other_abs = q15t::BvhLastGateRetForTest() + w.base;
    CHECK(other_abs != allowed_abs, "the two call sites have the same return address");
    // a direct cast (not through the gate: LocalStaticRaycast) is also kept off the tree
    CHECK(!CallInt(&cbvh, kDown, hit), "a direct cast hit the tree with the gate on");
  }
  Reset();
  {
    World w = Install(allowed_abs);
    CHECK(w.pins.n_allowed == 1, "pins");
    uint64_t cbvh = 0;
    bt::Resource res = RoomResource(true);
    reinterpret_cast<InitFn>(g_page[kSlotInit])(&cbvh, res.bytes.data());
    g_cbvh_self = &cbvh;
    uint8_t hit[0x44];
    std::memset(hit, 0, sizeof hit);
    void* gs = nullptr;
    // the listed call site: the tree answers; the other call site (same slot, same args): the stock miss
    const bool r = CallGateAllowedSite(reinterpret_cast<const void*>(0x77), kDown, hit, &gs);
    CHECK(r && gs == reinterpret_cast<void*>(0x1234), "the listed call site did not get the tree's answer");
    float t;
    std::memcpy(&t, hit + 0x10, 4);
    CHECK(t > 4.99f && t < 5.01f, "t %.3f", t);
    std::memset(hit, 0, sizeof hit);
    gs = nullptr;
    CHECK(!CallGateOtherSite(reinterpret_cast<const void*>(0x77), kDown, hit, &gs), "an unlisted call site got the tree's answer");
    // the mark is restored when the allowed cast returns
    CHECK(!CallInt(&cbvh, kDown, hit), "the allowed mark leaked past the cast");
    // nested: an unlisted cast inside an allowed one is gated, and the allowed one still is allowed afterwards
    g_nested = true;
    std::memset(hit, 0, sizeof hit);
    CHECK(CallGateAllowedSite(reinterpret_cast<const void*>(0x77), kDown, hit, &gs), "the outer allowed mark was lost by a nested cast");
    CHECK(!CallInt(&cbvh, kDown, hit), "the mark leaked after the nested casts");
    // another thread casting directly during an allowed cast is gated: the mark is per thread
    g_spawn = true;
    CHECK(CallGateAllowedSite(reinterpret_cast<const void*>(0x77), kDown, hit, &gs), "outer cast lost");
    CHECK(g_spawn_result == 0, "a cast on another thread saw the allowed mark (%d)", g_spawn_result);
    // Occluded is not gated
    const float a[3] = {0, 0, 0}, out[3] = {0, 0, 5.2f};
    CHECK(reinterpret_cast<OccludedFn>(g_page[kSlotOcc])(&cbvh, a, out), "Occluded was gated");
    CHECK(Count("allowed_casts=1") == 1, "gate line");
  }
  // two allowed call sites: the second entry of the list counts as much as the first
  Reset();
  {
    World w = Install(allowed_abs + 0x1000, allowed_abs);
    CHECK(w.pins.n_allowed == 2, "pins");
    uint64_t cbvh = 0;
    bt::Resource res = RoomResource(true);
    reinterpret_cast<InitFn>(g_page[kSlotInit])(&cbvh, res.bytes.data());
    g_cbvh_self = &cbvh;
    uint8_t hit[0x44];
    std::memset(hit, 0, sizeof hit);
    void* gs = nullptr;
    CHECK(CallGateAllowedSite(reinterpret_cast<const void*>(0x77), kDown, hit, &gs), "the second allowed call site did not get the tree's answer");
    CHECK(!CallGateOtherSite(reinterpret_cast<const void*>(0x77), kDown, hit, &gs), "an unlisted site got the tree's answer");
    (void)other_abs;
    // after the module's state is reset (the slots stay swapped) the wrappers have no real function: they answer a miss and do not fault
    q15t::ResetBvhForTest();
    CHECK(!CallGateAllowedSite(reinterpret_cast<const void*>(0x77), kDown, hit, &gs), "the gate without a real function did not answer a miss");
    CHECK(!CallInt(&cbvh, kDown, hit), "Intersect without a real function did not answer a miss");
    const float a[3] = {0, 0, 0}, out[3] = {0, 0, 5.2f};
    CHECK(!reinterpret_cast<OccludedFn>(g_page[kSlotOcc])(&cbvh, a, out), "Occluded without a real function did not answer a miss");
    uint64_t word = 5;
    reinterpret_cast<InitFn>(g_page[kSlotInit])(&word, res.bytes.data());  // no stock function to call: the tree is still built
    reinterpret_cast<ShutFn>(g_page[kSlotShut])(&word);
    CHECK(word == 0, "Shutdown without a real function did not clear the word");
  }

  // ---------------------------------------------------------------- 7. option files
  Reset();
  Touch("bvh.off");
  {
    World w = MakeWorld(0, false);
    g_page = reinterpret_cast<uintptr_t*>(w.mem);
    const auto r = q15t::InstallBvhAt(w.base, w.pins);
    CHECK(r.status != q15t::SwapStatus::kOk && std::strcmp(r.reason, "off_file") == 0 && !q15t::BvhTreeInstalled(), "bvh.off not honoured");
    CHECK(Count("bvh off", 1) == 1 && g_page[kSlotInt] == reinterpret_cast<uintptr_t>(&StockInt), "a slot was swapped although bvh.off is present");
  }
  Reset();
  Touch("bvhtouch.keep");
  Touch("bvhgate.off");
  {
    World w = Install();
    CHECK(g_page[kSlotTouch] == reinterpret_cast<uintptr_t>(&StockTouch), "touch slot swapped although bvhtouch.keep is present");
    CHECK(Count("bvh touch rays kept", 1) == 1, "touch-kept line");
    (void)w;
  }

  // ---------------------------------------------------------------- 8. all or none
  for (int which : {2, 3, 0, 1, 5}) {  // intersect, occluded, init, shutdown, gate: a slot that does not hold the pinned function
    Reset();
    World w = MakeWorld(0, false);
    g_page = reinterpret_cast<uintptr_t*>(w.mem);
    uintptr_t before[6];
    for (int i = 0; i < 6; ++i) before[i] = g_page[0x20 / 8 + i];
    mprotect(w.mem, 4096, PROT_READ | PROT_WRITE);
    g_page[0x20 / 8 + which] = 0x4242420;
    before[which] = 0x4242420;
    mprotect(w.mem, 4096, PROT_READ);
    const auto r = q15t::InstallBvhAt(w.base, w.pins);
    bool same = true;
    for (int i = 0; i < 6; ++i) same &= g_page[0x20 / 8 + i] == before[i];
    CHECK(r.status == q15t::SwapStatus::kUnexpectedOriginal && std::strcmp(r.reason, "slot_mismatch") == 0, "slot %d mismatch: %s / %s", which,
          q15t::StatusName(r.status), r.reason);
    CHECK(same && !q15t::BvhTreeInstalled(), "slot %d mismatch: some other slot was swapped", which);
    CHECK(Count("bvh refused", 3) == 1, "slot %d mismatch not an ERROR line", which);
  }
  {  // the gate's slot does not matter when the gate is off
    Reset();
    Touch("bvhgate.off");
    World w = MakeWorld(0, false);
    g_page = reinterpret_cast<uintptr_t*>(w.mem);
    mprotect(w.mem, 4096, PROT_READ | PROT_WRITE);
    g_page[kSlotGate] = 0x4242420;
    mprotect(w.mem, 4096, PROT_READ);
    CHECK(q15t::InstallBvhAt(w.base, w.pins).status == q15t::SwapStatus::kOk, "gate-off install with a foreign gate slot");
  }
  {  // a slot whose page is executable cannot be swapped: what was swapped before it is put back
    Reset();
    World w = MakeWorld(0, true);
    g_page = reinterpret_cast<uintptr_t*>(w.mem);
    uintptr_t before[6];
    for (int i = 0; i < 6; ++i) before[i] = g_page[i == 2 ? 0x1030 / 8 : 0x20 / 8 + i];
    const auto r = q15t::InstallBvhAt(w.base, w.pins);
    CHECK(r.status == q15t::SwapStatus::kExecutablePage, "executable slot page: %s", q15t::StatusName(r.status));
    bool same = true;
    for (int i = 0; i < 6; ++i) same &= SlotWord(w, i == 2 ? 0x1030 : 0x20 + 8 * i) == before[i];
    CHECK(same, "a slot was left swapped after the failed install");
    CHECK(!q15t::BvhTreeInstalled(), "installed flag set after a failed install");
    CHECK(Count("bvh hook refused", 3) == 1 && Count("bvh hook rolled back", 1) == 3, "refusal %d rollbacks %d", Count("bvh hook refused", 3),
          Count("bvh hook rolled back", 1));
    // and no cast is answered from a tree now: Initialize was never wrapped, so nothing built one
    CHECK(q15t::BvhTreeCount() == 0, "a tree exists");
  }

  // ---------------------------------------------------------------- 9. the 10 s summary
  Reset();
  Touch("bvhgate.off");
  {
    World w = Install();
    bt::Resource res = RoomResource(true);
    uint64_t cbvh = 0;
    reinterpret_cast<InitFn>(g_page[kSlotInit])(&cbvh, res.bytes.data());
    uint8_t hit[0x44];
    std::memset(hit, 0, sizeof hit);
    g_now = 100.0;
    CallInt(&cbvh, kDown, hit);
    q15t::BvhTick(0);  // arms the window; a counter has moved, but the window has not run out
    CHECK(Count("bvh summary") == 0, "a summary at the first tick");
    g_now = 105.0;
    q15t::BvhTick(0);
    CHECK(Count("bvh summary") == 0, "a summary inside the window");
    g_now = 111.0;
    q15t::BvhTick(0);
    CHECK(Count("bvh summary", 1) == 1 && Count("intersect=1 intersect_hit=1") == 1, "no summary after 10 s: %d", Count("bvh summary"));
    g_now = 125.0;
    q15t::BvhTick(0);
    CHECK(Count("bvh summary", 1) == 1, "a summary with nothing to report");
    CallInt(&cbvh, kDown, hit);
    g_now = 130.0;
    q15t::BvhTick(0);
    CHECK(Count("bvh summary", 1) == 1, "a second summary inside the 10 s window");
    g_now = 140.0;
    q15t::BvhTick(0);
    CHECK(Count("bvh summary", 1) == 2, "no second summary: %d", Count("bvh summary", 1));
    (void)w;
  }

  ClearDir();
  rmdir(g_dir.c_str());
  if (g_fail == 0) std::printf("bvh_test: all checks passed\n");
  else std::printf("bvh_test: %d failure(s)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
