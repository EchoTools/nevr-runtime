// Host test of custctx (src/quest/plugins/q15t/src/custctx.cpp): the three wrappers installed by the real installer on a fake JUMP_SLOT page and called
// through the slots, against fake LoadMeshData / DestroyContext / Grow / Resize functions, a fake draw table laid out as Grow
// reads it, and a fake render-target layout pool. Real SafeRead, real mprotect, real files.
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "custctx.h"
#include "nevr/plugin_interface.h"
#include "pin_check.h"
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
static int CountLines(const char* needle, int level = -1) {
  int c = 0;
  for (const auto& l : g_lines)
    if (l.text.find(needle) != std::string::npos && (level < 0 || (int)l.level == level)) ++c;
  return c;
}

static const uint64_t kDraw = 0x9718;
struct DrawTable {
  uint8_t* data;
  uint64_t size;
  void* allocator;
  uint32_t align, flags;
  uint64_t step, capacity, count;
};

// ---- fake engine ----
static void* const kAlloc = reinterpret_cast<void*>(0xA110C);
static DrawTable g_tbl;
static uint64_t g_load_ret = 0;
static int g_load_calls = 0, g_destroy_calls = 0, g_grow_calls = 0, g_resize_calls = 0;
static void *g_load_ctx = nullptr, *g_load_self = nullptr, *g_load_model = nullptr;
static void* g_destroy_ctx = nullptr;
static void (*g_load_hook)(void*) = nullptr;
static void (*g_destroy_hook)(void*) = nullptr;
static bool g_unsafe_grow = false;
static int g_grow_unreadable = 0;
static DrawTable g_grow_seen;  // the table the last real Grow saw
static void* g_grow_seen_ptr = nullptr;
static void* g_grow_ret = nullptr;
static bool g_resize_ok = true;
static void* g_rs_block = nullptr;
static uint64_t g_rs_bytes = 0;
static uint32_t g_rs_keep = 0;

extern "C" __attribute__((noinline)) uint64_t FakeLoad(void* self, void* ctx, void* model) {
  ++g_load_calls;
  g_load_self = self;
  g_load_ctx = ctx;
  g_load_model = model;
  if (g_load_hook != nullptr) g_load_hook(ctx);
  return g_load_ret;
}
extern "C" __attribute__((noinline)) uint64_t FakeDestroy(void*, void* ctx) {
  ++g_destroy_calls;
  g_destroy_ctx = ctx;
  if (g_destroy_hook != nullptr) g_destroy_hook(ctx);
  return 0;
}
extern "C" __attribute__((noinline)) void* FakeGrow(DrawTable* t) {
  ++g_grow_calls;
  if (reinterpret_cast<uintptr_t>(t) < 0x10000) {
    ++g_grow_unreadable;
    return nullptr;
  }
  g_grow_seen = *t;
  g_grow_seen_ptr = t;
  // the real Grow's room path: it never resizes here and writes the element's slot: a NULL data or no room is the crash
  if (t->data == nullptr || t->count >= t->capacity) {
    g_unsafe_grow = true;
    return nullptr;
  }
  ++t->count;
  g_grow_ret = t->data + (t->count - 1) * kDraw;
  return g_grow_ret;
}
extern "C" __attribute__((noinline)) void FakeOtherCode() {}
static void FakeResize(void* block, uint64_t bytes, uint32_t keep) {
  ++g_resize_calls;
  g_rs_block = block;
  g_rs_bytes = bytes;
  g_rs_keep = keep;
  auto* t = static_cast<DrawTable*>(block);
  if (g_resize_ok) {
    t->data = static_cast<uint8_t*>(std::realloc(t->data, bytes));
    t->size = bytes;
  } else {
    t->data = nullptr;  // an out-of-memory Resize leaves the data pointer NULL
  }
}

alignas(16) static uint8_t g_pool[0x100];
static uintptr_t g_pool_cell = 0;
static void SetPool(uint64_t free_slots) {
  std::memset(g_pool, 0, sizeof g_pool);
  const uint32_t cap = 512;
  std::memcpy(g_pool + 0x84, &cap, sizeof cap);
  std::memcpy(g_pool + 0x30, &free_slots, sizeof free_slots);
  g_pool_cell = reinterpret_cast<uintptr_t>(g_pool);
}

using LoadF = uint64_t (*)(void*, void*, void*);
using DestroyF = uint64_t (*)(void*, void*);
using GrowF = void* (*)(DrawTable*);
static LoadF hLoad;
static DestroyF hDestroy;
static GrowF hGrow;

static void* Ctx(int i) { return reinterpret_cast<void*>(0x100000 + static_cast<uintptr_t>(i) * 0x100); }

struct Page {
  uintptr_t* w;
  uintptr_t base;
  q15t::CustCtxAddrs a;
};
static Page MakePage(uintptr_t load, uintptr_t destroy, uintptr_t grow) {
  void* p = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) std::abort();
  auto* w = static_cast<uintptr_t*>(p);
  w[4] = load;
  w[5] = destroy;
  w[6] = grow;
  mprotect(p, 4096, PROT_READ);
  Page pg{w, reinterpret_cast<uintptr_t>(p), {}};
  pg.a = {0x20,
          reinterpret_cast<uintptr_t>(&FakeLoad) - pg.base,
          0x28,
          reinterpret_cast<uintptr_t>(&FakeDestroy) - pg.base,
          0x30,
          reinterpret_cast<uintptr_t>(&FakeGrow) - pg.base,
          0x1000,
          reinterpret_cast<uintptr_t>(&g_pool_cell) - pg.base};
  return pg;
}

static void ResetWorld() {
  g_load_ret = 0;
  g_load_calls = g_destroy_calls = g_grow_calls = g_resize_calls = g_grow_unreadable = 0;
  g_load_hook = nullptr;
  g_destroy_hook = nullptr;
  g_unsafe_grow = false;
  g_resize_ok = true;
  g_load_ctx = g_destroy_ctx = nullptr;
  std::free(g_tbl.data);
  std::memset(&g_tbl, 0, sizeof g_tbl);
  g_tbl.allocator = kAlloc;
  SetPool(100);
  g_lines.clear();
}

static q15t::CustCtxFns Fns() {
  q15t::CustCtxFns f{};
  f.resize = &FakeResize;
  return f;
}

static Page Install() {
  q15t::ResetCustCtxForTest();
  ResetWorld();
  Page pg = MakePage(reinterpret_cast<uintptr_t>(&FakeLoad), reinterpret_cast<uintptr_t>(&FakeDestroy), reinterpret_cast<uintptr_t>(&FakeGrow));
  const auto r = q15t::InstallCustCtxAt(pg.base, pg.a, Fns());
  CHECK(r.status == q15t::SwapStatus::kOk, "install: %s %s", q15t::StatusName(r.status), r.reason);
  hLoad = reinterpret_cast<LoadF>(pg.w[4]);
  hDestroy = reinterpret_cast<DestroyF>(pg.w[5]);
  hGrow = reinterpret_cast<GrowF>(pg.w[6]);
  return pg;
}

// nested calls from inside the fake real functions
static void NestedDestroyInLoad(void* ctx) { hDestroy(nullptr, ctx); }
static int g_depth = 0;
static void NestedDestroyInDestroy(void* ctx) { hDestroy(nullptr, ctx); }
static void ReloadThenDestroyInDestroy(void* ctx) {
  if (g_depth++ != 0) return;
  hLoad(nullptr, ctx, nullptr);
  hDestroy(nullptr, ctx);
}

static int FailingMemalign(void** out, size_t, size_t) {
  *out = nullptr;
  return ENOMEM;
}
static int g_memalign_calls = 0;
static int FailThird(void** out, size_t align, size_t size) {
  if (++g_memalign_calls == 3) {
    *out = nullptr;
    return ENOMEM;
  }
  return posix_memalign(out, align, size);
}

int main() {
  NvrGameContext ctx{};
  ctx.ctx_size = sizeof ctx;
  ctx.plugin_id = "q15t";
  ctx.log = &FakeLog;
  q15t::SetLogContext(&ctx);
  char tmpl[] = "/tmp/q15t-custctx-XXXXXX";
  if (mkdtemp(tmpl) == nullptr) return 2;
  const std::string dir = tmpl;
  q15t::SetCustCtxDirForTest(dir.c_str());

  // the pinned addresses (R0_readelf_r.txt lines 114546, 107763, 129938; disassembly VA - 0x100000; RegisterRTLayout 0x18829c4)
  {
    const q15t::CustCtxAddrs& p = q15t::kCustCtxPinned;
    CHECK(p.got_load == 0x36cf6f0 && p.fn_load == 0x1247d90 && p.got_destroy == 0x36c22f8 && p.fn_destroy == 0x12492fc &&
              p.got_grow == 0x36ed7f0 && p.fn_grow == 0x124893c && p.fn_resize == 0x11dc30c && p.rt_pool == 0x379b2c0,
          "pinned addresses changed");
  }

  // ---- R32 / R31: the live-context table ----
  {
    Page pg = Install();
    (void)pg;
    CHECK(CountLines("custctx on", 1) == 1, "no INFO install line");
    CHECK(pg.w[4] != reinterpret_cast<uintptr_t>(&FakeLoad) && pg.w[5] != reinterpret_cast<uintptr_t>(&FakeDestroy) &&
              pg.w[6] != reinterpret_cast<uintptr_t>(&FakeGrow),
          "a slot was not replaced");

    // a destroy of an address no live context holds is dropped
    hDestroy(nullptr, Ctx(1));
    CHECK(g_destroy_calls == 0 && q15t::GetCustCtxStats().destroys_dropped == 1, "unknown context destroyed: %d", g_destroy_calls);
    CHECK(CountLines("custctx dropped destroy") == 1 && CountLines("why=not live: no context holds this address") == 1 &&
              CountLines("ctx=0x100100") == 1,
          "no drop record");

    // load, destroy: once; a second destroy of the same address is dropped
    g_load_ret = 5;
    const uint64_t lr = hLoad(reinterpret_cast<void*>(0x11), Ctx(1), reinterpret_cast<void*>(0x22));
    CHECK(lr == 5 && g_load_calls == 1 && g_load_self == reinterpret_cast<void*>(0x11) && g_load_ctx == Ctx(1) &&
              g_load_model == reinterpret_cast<void*>(0x22),
          "load not passed through: ret %llu", (unsigned long long)lr);
    hDestroy(reinterpret_cast<void*>(0x33), Ctx(1));
    CHECK(g_destroy_calls == 1 && g_destroy_ctx == Ctx(1), "live context not destroyed");
    hDestroy(nullptr, Ctx(1));
    CHECK(g_destroy_calls == 1 && q15t::GetCustCtxStats().destroys_dropped == 2, "second destroy ran: %d", g_destroy_calls);

    // a null context goes to the real function (it returns at once), no drop
    hDestroy(nullptr, nullptr);
    CHECK(g_destroy_calls == 2 && g_destroy_ctx == nullptr && q15t::GetCustCtxStats().destroys_dropped == 2, "null context: calls %d", g_destroy_calls);

    // a destroy that arrives while the same context's LoadMeshData runs is dropped; afterwards it runs
    g_lines.clear();
    g_load_ret = 0;
    g_load_hook = &NestedDestroyInLoad;
    hLoad(nullptr, Ctx(2), nullptr);
    g_load_hook = nullptr;
    CHECK(g_destroy_calls == 2 && CountLines("why=its LoadMeshData is running") == 1, "destroy during a load ran or was not recorded");
    hDestroy(nullptr, Ctx(2));
    CHECK(g_destroy_calls == 3, "destroy after the load finished did not run (in-flight count stuck)");

    // a destroy that arrives while the same context is being destroyed is dropped
    g_lines.clear();
    hLoad(nullptr, Ctx(3), nullptr);
    g_destroy_hook = &NestedDestroyInDestroy;
    const int before = g_destroy_calls;
    hDestroy(nullptr, Ctx(3));
    g_destroy_hook = nullptr;
    CHECK(g_destroy_calls == before + 1 && CountLines("why=already being destroyed") == 1, "nested destroy: calls %d", g_destroy_calls - before);

    // a load of the same address while it is being destroyed makes it live again: the nested destroy runs
    hLoad(nullptr, Ctx(4), nullptr);
    g_depth = 0;
    g_destroy_hook = &ReloadThenDestroyInDestroy;
    const int before2 = g_destroy_calls;
    hDestroy(nullptr, Ctx(4));
    g_destroy_hook = nullptr;
    CHECK(g_destroy_calls == before2 + 2, "reload during a destroy: %d real destroys (want 2)", g_destroy_calls - before2);
    hDestroy(nullptr, Ctx(4));  // the nested destroy finished it: a third one is a stale destroy
    CHECK(g_destroy_calls == before2 + 2, "destroy of a finished context ran: %d", g_destroy_calls - before2);
  }

  // the table does not fill up with finished contexts
  {
    Install();
    for (int i = 0; i < 600; ++i) {
      hLoad(nullptr, Ctx(i), nullptr);
      hDestroy(nullptr, Ctx(i));
    }
    CHECK(g_destroy_calls == 600 && CountLines("more than") == 0, "a finished context kept its table entry: %d destroys, %d full lines", g_destroy_calls,
          CountLines("more than"));
  }

  // more than 256 live contexts: stock behaviour for the overflow, one line
  {
    Install();
    for (int i = 0; i < 256; ++i) hLoad(nullptr, Ctx(i), nullptr);
    CHECK(CountLines("more than") == 0 && g_load_calls == 256, "full before 256 contexts");
    hLoad(nullptr, Ctx(256), nullptr);
    hLoad(nullptr, Ctx(257), nullptr);
    CHECK(g_load_calls == 258, "an untracked load was not passed to the real function");
    CHECK(CountLines("more than", 2) == 1, "overflow lines: %d", CountLines("more than"));
    hDestroy(nullptr, Ctx(256));
    CHECK(g_destroy_calls == 0, "destroy of an untracked context ran");
    hDestroy(nullptr, Ctx(0));
    CHECK(g_destroy_calls == 1, "tracked context not destroyed");
    hLoad(nullptr, Ctx(256), nullptr);
    hDestroy(nullptr, Ctx(256));
    CHECK(g_destroy_calls == 2, "a freed entry was not reused");
  }

  // drop log caps: 16 lines, then a total at every 50th drop, at most 16 totals
  {
    Install();
    for (int i = 0; i < 49; ++i) hDestroy(nullptr, Ctx(1));
    CHECK(CountLines("custctx dropped destroy") == 16 && CountLines("destroys dropped so far") == 0, "after 49 drops: %d / %d",
          CountLines("custctx dropped destroy"), CountLines("destroys dropped so far"));
    hDestroy(nullptr, Ctx(1));
    CHECK(CountLines("destroys dropped so far") == 1 && CountLines("dropped=50") == 1, "no total at the 50th drop");
    for (int i = 0; i < 850; ++i) hDestroy(nullptr, Ctx(1));
    CHECK(CountLines("destroys dropped so far") == 16, "totals %d", CountLines("destroys dropped so far"));
  }

  // ---- R31 v4: the render-target layout pool ----
  {
    Install();
    g_load_ret = 7;
    SetPool(100);
    CHECK(hLoad(nullptr, Ctx(1), nullptr) == 7 && g_load_calls == 1, "a roomy pool: load not passed through");
    SetPool(64);
    CHECK(hLoad(nullptr, Ctx(1), nullptr) == 7 && g_load_calls == 2, "64 free slots must still load");
    SetPool(63);
    CHECK(hLoad(nullptr, Ctx(1), nullptr) == 1 && g_load_calls == 2, "63 free slots must refuse (returns 1, real not called)");
    CHECK(q15t::GetCustCtxStats().loads_refused == 1, "refusal not counted");
    CHECK(CountLines("custctx cosmetic load refused", 1) == 1 && CountLines("layouts_used=449 layouts_capacity=512 refused=1") == 1, "refusal record");
    hDestroy(nullptr, Ctx(1));
    CHECK(g_destroy_calls == 1, "a refused load's context must still be destroyable");
    SetPool(0);
    CHECK(hLoad(nullptr, Ctx(9), nullptr) == 1, "empty pool");
    g_pool_cell = 0;  // no pool pointer yet
    CHECK(hLoad(nullptr, Ctx(9), nullptr) == 7, "a null pool pointer must not refuse");
    g_pool_cell = 0x30;  // unreadable
    CHECK(hLoad(nullptr, Ctx(9), nullptr) == 7, "an unreadable pool pointer must not refuse");
    SetPool(0);
    std::memset(g_pool + 0x84, 0, 4);  // capacity 0, free 0: used 0, still "low"
    // refusal logs: first 16, then every 50th
    g_lines.clear();
    SetPool(10);
    const unsigned base_refused = q15t::GetCustCtxStats().loads_refused;
    for (int i = 0; i < 120; ++i) hLoad(nullptr, Ctx(9), nullptr);
    const unsigned total = base_refused + 120;
    int want = 0;
    for (unsigned n = base_refused + 1; n <= total; ++n)
      if (n <= 16 || n % 50 == 0) ++want;
    CHECK(CountLines("custctx cosmetic load refused") == want, "refusal lines %d, want %d", CountLines("custctx cosmetic load refused"), want);
  }
  {
    Install();
    SetPool(100);
    for (int i = 0; i < 33 * 32; ++i) hLoad(nullptr, Ctx(1), nullptr);
    CHECK(CountLines("custctx cosmetic loads") == 32, "pool-use lines %d (every 32nd load, at most 32)", CountLines("custctx cosmetic loads"));
    CHECK(CountLines("loads=32 layouts_used=412 layouts_capacity=512") == 1, "first pool-use line");
  }

  // ---- R33: Grow ----
  {
    Install();
    uint8_t* buf = static_cast<uint8_t*>(std::calloc(1, 8 * kDraw));
    // room left and data set: the real Grow runs on the same table, no resize, nothing rewritten
    g_tbl.data = buf;
    g_tbl.size = 8 * kDraw;
    g_tbl.step = 4;
    g_tbl.capacity = 8;
    g_tbl.count = 3;
    void* r = hGrow(&g_tbl);
    CHECK(g_resize_calls == 0 && g_grow_calls == 1 && g_grow_seen_ptr == &g_tbl && !g_unsafe_grow, "room path: resizes %d", g_resize_calls);
    CHECK(r == buf + 3 * kDraw && g_tbl.count == 4 && g_tbl.capacity == 8 && g_tbl.data == buf, "room path result");
    CHECK(q15t::GetCustCtxStats().grow_failures == 0, "room path counted as a failure");

    // no room (count == capacity): the resize is made here, with max(capacity, step), keep 1, checked, capacity raised
    g_tbl.count = 8;
    g_tbl.step = 4;
    r = hGrow(&g_tbl);
    CHECK(g_resize_calls == 1 && g_rs_block == &g_tbl && g_rs_bytes == (8 + 8) * kDraw && g_rs_keep == 1, "resize args: block %p bytes %llu keep %u",
          g_rs_block, (unsigned long long)g_rs_bytes, g_rs_keep);
    CHECK(g_tbl.capacity == 16 && g_tbl.count == 9 && g_grow_seen.capacity == 16 && !g_unsafe_grow && r != nullptr, "capacity not raised before the real Grow");
    // a step larger than the capacity
    std::free(g_tbl.data);
    g_tbl.data = static_cast<uint8_t*>(std::calloc(1, 4 * kDraw));
    g_tbl.step = 20;
    g_tbl.capacity = 4;
    g_tbl.count = 4;
    g_resize_calls = 0;
    hGrow(&g_tbl);
    CHECK(g_rs_bytes == (4 + 20) * kDraw && g_tbl.capacity == 24 && !g_unsafe_grow, "step larger than capacity: bytes %llu cap %llu",
          (unsigned long long)g_rs_bytes, (unsigned long long)g_tbl.capacity);
    CHECK(q15t::GetCustCtxStats().grow_failures == 0, "successful growth counted as a failure");

    // Resize fails (data NULL afterwards, 8 elements in use): empty consistent table, an element that is in no table, no crash
    std::free(g_tbl.data);
    g_tbl.data = static_cast<uint8_t*>(std::calloc(1, 8 * kDraw));
    g_tbl.size = 8 * kDraw;
    g_tbl.step = 32;
    g_tbl.capacity = 8;
    g_tbl.count = 8;
    g_resize_ok = false;
    g_lines.clear();
    g_grow_calls = 0;
    uint8_t* const old_data = g_tbl.data;
    r = hGrow(&g_tbl);
    CHECK(g_tbl.data == nullptr && g_tbl.size == 0 && g_tbl.capacity == 0 && g_tbl.count == 0, "table not emptied: cap %llu count %llu",
          (unsigned long long)g_tbl.capacity, (unsigned long long)g_tbl.count);
    CHECK(g_tbl.allocator == kAlloc && g_tbl.step == 32, "allocator / step of the table changed");
    CHECK(r != nullptr && g_grow_calls == 1 && !g_unsafe_grow, "no spare element: ret %p calls %d unsafe %d", r, g_grow_calls, (int)g_unsafe_grow);
    CHECK(g_grow_seen_ptr != &g_tbl && g_grow_seen.data != nullptr && g_grow_seen.data != old_data && g_grow_seen.size == kDraw &&
              g_grow_seen.allocator == kAlloc && g_grow_seen.step == 1 && g_grow_seen.capacity == 1 && g_grow_seen.count == 0,
          "one-slot table: seen cap %llu (count %llu)", (unsigned long long)g_grow_seen.capacity,
          (unsigned long long)g_grow_seen.count);
    CHECK(r == g_grow_seen.data, "the element is not the spare slot");
    CHECK(q15t::GetCustCtxStats().grow_failures == 1, "failure not counted");
    CHECK(CountLines("custctx no memory for cosmetic draw entries", 2) == 1 && CountLines("why=Resize left the data NULL") == 1 &&
              CountLines("entries=32 bytes=") == 1 && CountLines("in_use=8") == 1,
          "failure record");
    std::free(old_data);

    // step and capacity 0: nothing to resize
    g_resize_calls = 0;
    g_lines.clear();
    std::memset(&g_tbl, 0, sizeof g_tbl);
    g_tbl.allocator = kAlloc;
    g_resize_ok = true;
    r = hGrow(&g_tbl);
    CHECK(g_resize_calls == 0 && r != nullptr && !g_unsafe_grow && CountLines("why=step and capacity 0") == 1, "add 0: resizes %d", g_resize_calls);

    // step and capacity 0 on a table that still has a data block: emptied, the block is leaked (never freed by the guard)
    uint8_t* leaked = static_cast<uint8_t*>(std::calloc(1, kDraw));
    std::memset(&g_tbl, 0, sizeof g_tbl);
    g_tbl.allocator = kAlloc;
    g_tbl.data = leaked;
    g_tbl.size = kDraw;
    r = hGrow(&g_tbl);
    CHECK(g_tbl.data == nullptr && g_tbl.size == 0 && r != nullptr && !g_unsafe_grow, "data block kept in an emptied table");
    std::free(leaked);

    // room left but the data is NULL (a torn-down table): the memset(NULL) of the original crash
    g_lines.clear();
    std::memset(&g_tbl, 0, sizeof g_tbl);
    g_tbl.allocator = kAlloc;
    g_tbl.step = 4;
    g_tbl.capacity = 32;
    g_tbl.count = 0;
    r = hGrow(&g_tbl);
    CHECK(r != nullptr && !g_unsafe_grow && g_tbl.capacity == 0 && g_tbl.count == 0 && g_tbl.data == nullptr && g_resize_calls == 0,
          "torn-down table: cap %llu resizes %d", (unsigned long long)g_tbl.capacity, g_resize_calls);
    CHECK(CountLines("why=room left but the data is NULL") == 1, "no torn-down record");

    // an unreadable table: the real function decides
    g_grow_unreadable = 0;
    hGrow(reinterpret_cast<DrawTable*>(0x20));
    CHECK(g_grow_unreadable == 1 && g_resize_calls == 0, "unreadable table not handed to the real function");
  }

  // spares: when the allocation fails, the install-time spares are used round robin, never NULL
  {
    Install();
    q15t::SetCustCtxMemalignForTest(&FailingMemalign);
    std::vector<void*> got;
    for (int i = 0; i < 5; ++i) {
      std::memset(&g_tbl, 0, sizeof g_tbl);
      g_tbl.allocator = kAlloc;
      g_tbl.step = 4;
      g_tbl.capacity = 32;  // torn down
      g_unsafe_grow = false;
      void* r = hGrow(&g_tbl);
      CHECK(r != nullptr && !g_unsafe_grow, "spare %d: ret %p", i, r);
      got.push_back(r);
    }
    CHECK(got[0] != got[1] && got[1] != got[2] && got[2] != got[3] && got[0] != got[2] && got[0] != got[3] && got[1] != got[3], "spares not distinct");
    CHECK(got[4] == got[0], "spares not reused round robin");
    // log caps: 16 lines, a total at every 50th failure, at most 16 totals
    g_lines.clear();
    for (int i = 0; i < 45; ++i) {  // 5 so far: 50 in all after this loop's last call
      std::memset(&g_tbl, 0, sizeof g_tbl);
      g_tbl.capacity = 32;
      hGrow(&g_tbl);
    }
    CHECK(CountLines("custctx no memory for cosmetic draw entries") == 11 && CountLines("draw-table failures so far") == 1 &&
              CountLines("failures=50") == 1,
          "after 50 failures: %d lines, %d totals", CountLines("custctx no memory for cosmetic draw entries"), CountLines("draw-table failures so far"));
    for (int i = 0; i < 850; ++i) {
      std::memset(&g_tbl, 0, sizeof g_tbl);
      g_tbl.capacity = 32;
      hGrow(&g_tbl);
    }
    CHECK(CountLines("draw-table failures so far") == 16, "failure totals %d", CountLines("draw-table failures so far"));
    q15t::SetCustCtxMemalignForTest(nullptr);
  }
  // a working allocator: a fresh element each time, in no table
  {
    Install();
    std::memset(&g_tbl, 0, sizeof g_tbl);
    g_tbl.capacity = 32;
    void* a = hGrow(&g_tbl);
    std::memset(&g_tbl, 0, sizeof g_tbl);
    g_tbl.capacity = 32;
    void* b = hGrow(&g_tbl);
    CHECK(a != nullptr && b != nullptr && a != b, "fresh elements expected");
    std::vector<void*> seen = {a, b};
    for (int i = 0; i < 6; ++i) {
      std::memset(&g_tbl, 0, sizeof g_tbl);
      g_tbl.capacity = 32;
      seen.push_back(hGrow(&g_tbl));
    }
    for (size_t i = 0; i < seen.size(); ++i)
      for (size_t j = i + 1; j < seen.size(); ++j) CHECK(seen[i] != seen[j], "element %zu reused as %zu while the allocator works", i, j);
  }

  // ---- install: all or none ----
  {
    q15t::ResetCustCtxForTest();
    ResetWorld();
    // the grow slot holds a foreign word: nothing stays swapped, ERROR line
    Page p = MakePage(reinterpret_cast<uintptr_t>(&FakeLoad), reinterpret_cast<uintptr_t>(&FakeDestroy), 0x4242420);
    const auto r = q15t::InstallCustCtxAt(p.base, p.a, Fns());
    CHECK(r.status == q15t::SwapStatus::kUnexpectedOriginal, "foreign grow word: %s", q15t::StatusName(r.status));
    CHECK(p.w[4] == reinterpret_cast<uintptr_t>(&FakeLoad) && p.w[5] == reinterpret_cast<uintptr_t>(&FakeDestroy) && p.w[6] == 0x4242420,
          "slots not back as found after a Grow refusal");
    char named[128];
    std::snprintf(named, sizeof named, "pin=0x30 found=0x4242420 expected=0x%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&FakeGrow)));
    CHECK(CountLines("custctx refused (Grow)", 3) == 1 &&
              CountLines(named) == 1 && CountLines("status=unexpected_original") == 1,
          "refusal not loud / does not name the pin, found and expected");
    // the destroy slot
    g_lines.clear();
    Page p2 = MakePage(reinterpret_cast<uintptr_t>(&FakeLoad), 0x4242420, reinterpret_cast<uintptr_t>(&FakeGrow));
    q15t::InstallCustCtxAt(p2.base, p2.a, Fns());
    CHECK(p2.w[4] == reinterpret_cast<uintptr_t>(&FakeLoad) && p2.w[5] == 0x4242420 && p2.w[6] == reinterpret_cast<uintptr_t>(&FakeGrow),
          "slots not back as found after a DestroyContext refusal");
    CHECK(CountLines("custctx refused (DestroyContext)", 3) == 1, "no refusal for DestroyContext");
    // the load slot
    g_lines.clear();
    Page p3 = MakePage(0x4242420, reinterpret_cast<uintptr_t>(&FakeDestroy), reinterpret_cast<uintptr_t>(&FakeGrow));
    q15t::InstallCustCtxAt(p3.base, p3.a, Fns());
    CHECK(p3.w[4] == 0x4242420 && p3.w[5] == reinterpret_cast<uintptr_t>(&FakeDestroy) && p3.w[6] == reinterpret_cast<uintptr_t>(&FakeGrow) &&
              CountLines("custctx refused (LoadMeshData)", 3) == 1,
          "load refusal");

    // a word that points at real executable code but not at the pinned function is refused, for each of the three slots
    uintptr_t other = reinterpret_cast<uintptr_t>(&FakeOtherCode);
    for (int which = 0; which < 3; ++which) {
      uintptr_t v[3] = {reinterpret_cast<uintptr_t>(&FakeLoad), reinterpret_cast<uintptr_t>(&FakeDestroy), reinterpret_cast<uintptr_t>(&FakeGrow)};
      v[which] = other;
      Page q = MakePage(v[0], v[1], v[2]);
      const auto rq = q15t::InstallCustCtxAt(q.base, q.a, Fns());
      CHECK(rq.status == q15t::SwapStatus::kUnexpectedOriginal && q.w[4 + which] == other, "other function's word accepted in slot %d: %s", which,
            q15t::StatusName(rq.status));
    }

    // a spare that cannot be allocated: nothing swapped, ERROR line
    g_lines.clear();
    g_memalign_calls = 0;
    q15t::SetCustCtxMemalignForTest(&FailThird);
    Page p4 = MakePage(reinterpret_cast<uintptr_t>(&FakeLoad), reinterpret_cast<uintptr_t>(&FakeDestroy), reinterpret_cast<uintptr_t>(&FakeGrow));
    const auto r4 = q15t::InstallCustCtxAt(p4.base, p4.a, Fns());
    q15t::SetCustCtxMemalignForTest(nullptr);
    CHECK(std::strcmp(r4.reason, "spare_alloc_failed") == 0 && p4.w[4] == reinterpret_cast<uintptr_t>(&FakeLoad) &&
              p4.w[5] == reinterpret_cast<uintptr_t>(&FakeDestroy) && p4.w[6] == reinterpret_cast<uintptr_t>(&FakeGrow),
          "swapped although a spare could not be allocated");
    CHECK(CountLines("custctx refused", 3) == 1, "spare failure not loud");

    // no Resize export: refused, nothing swapped (and the resolving installer on a host without the game library says so too)
    g_lines.clear();
    Page p5 = MakePage(reinterpret_cast<uintptr_t>(&FakeLoad), reinterpret_cast<uintptr_t>(&FakeDestroy), reinterpret_cast<uintptr_t>(&FakeGrow));
    const auto r5 = q15t::InstallCustCtxAt(p5.base, p5.a, q15t::CustCtxFns{});
    CHECK(std::strcmp(r5.reason, "export_missing") == 0 && p5.w[4] == reinterpret_cast<uintptr_t>(&FakeLoad), "no Resize installed");
    CHECK(CountLines("custctx refused", 3) == 1, "missing Resize not loud");
    g_lines.clear();
    pincheck::Text pt = pincheck::Make(q15t::kCustCtxWordPins, q15t::kCustCtxWordPinCount);  // the pins pass, then the export is looked up
    const auto r6 = q15t::InstallCustCtx(pt.base());
    CHECK(std::strcmp(r6.reason, "export_missing") == 0 && CountLines("custctx refused", 3) == 1, "resolver on a host without libr15 not loud");
    pincheck::Free(&pt);

    // the address check on what dlsym would return
    {
      q15t::CustCtxFns got{};
      const uintptr_t b = 0x7000000000ull;
      const auto& pin = q15t::kCustCtxPinned;
      CHECK(q15t::CustCtxResizeMatches(b, pin, reinterpret_cast<void*>(b + pin.fn_resize), &got) &&
                got.resize == reinterpret_cast<void (*)(void*, uint64_t, uint32_t)>(b + pin.fn_resize),
            "Resize at the pinned address refused");
      CHECK(!q15t::CustCtxResizeMatches(b, pin, reinterpret_cast<void*>(b + pin.fn_resize + 4), &got), "Resize elsewhere accepted");
      CHECK(!q15t::CustCtxResizeMatches(b, pin, nullptr, &got), "null Resize accepted");
    }

    // custctx.off in the data directory: nothing swapped, one INFO line
    g_lines.clear();
    FILE* m = std::fopen((dir + "/custctx.off").c_str(), "w");
    std::fclose(m);
    Page p7 = MakePage(reinterpret_cast<uintptr_t>(&FakeLoad), reinterpret_cast<uintptr_t>(&FakeDestroy), reinterpret_cast<uintptr_t>(&FakeGrow));
    const auto r7 = q15t::InstallCustCtxAt(p7.base, p7.a, Fns());
    CHECK(std::strcmp(r7.reason, "off_file") == 0 && p7.w[4] == reinterpret_cast<uintptr_t>(&FakeLoad) && p7.w[6] == reinterpret_cast<uintptr_t>(&FakeGrow),
          "custctx.off not honoured");
    CHECK(CountLines("custctx off", 1) == 1, "no INFO off line");
    unlink((dir + "/custctx.off").c_str());
  }

  std::free(g_tbl.data);
  rmdir(dir.c_str());
  // ---- code-word pins: every word read, a changed or unreadable one refused loudly, no file hash anywhere ----
  {
    auto count = [](const char* needle, int level) { return CountLines(needle, level); };
    auto clear = []() { g_lines.clear(); };
    g_fail += pincheck::Run("custctx", q15t::kCustCtxWordPins, q15t::kCustCtxWordPinCount, 34, 0xe7250efd9cc11821ull,
                            [](std::uintptr_t b) { return q15t::CustCtxWordPinsMatch(b); }, count, clear);
    // the real installer refuses on a changed word, before anything else (reason word_pin_mismatch, one ERROR line, nothing swapped)
    pincheck::Text t = pincheck::Make(q15t::kCustCtxWordPins, q15t::kCustCtxWordPinCount);
    *t.word(q15t::kCustCtxWordPins[0].va) ^= 1u;
    g_lines.clear();
    const auto pr = q15t::InstallCustCtx(t.base());
    CHECK(std::strcmp(pr.reason, "word_pin_mismatch") == 0 && CountLines("code word pin mismatch", 3) == 1, "installer ignored a changed word");
    CHECK(CountLines("custctx on", -1) == 0, "installer went on after a changed word");
    pincheck::Free(&t);
  }

  if (g_fail == 0) std::printf("custctx_test: all checks passed\n");
  return g_fail == 0 ? 0 : 1;
}
