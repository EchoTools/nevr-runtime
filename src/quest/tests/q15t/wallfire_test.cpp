// Host test of the wallfire module (src/quest/plugins/q15t/src/wallfire.cpp) on top of the real bvh module (src/quest/plugins/q15t/src/bvh.cpp): a fake slot page (real mprotect,
// real /proc/self/maps) holds the six bvh slots, the QueryCollision slot and the StaticRaycastOccluded slot; the wrapper is called
// through its slot from two distinct call sites; the engine's stub is a fake that follows CncaGameSpace -> CncaGame -> vtable +0x278
// into a fake CncaGame::StaticRaycastOccluded, which calls CBVHTree::Occluded through bvh's own wrapper on a synthetic room mesh.
#include <sys/mman.h>
#include <unistd.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "bvh.h"
#include "bvh_build.h"
#include "nevr/plugin_interface.h"
#include "plugin_log.h"
#include "safe_read.h"
#include "wallfire.h"

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

static uintptr_t* g_page = nullptr;
// page layout: the six bvh slots (as bvh_test.cpp), then the wallfire slots and a word standing in for FireGunLocal's site
enum { kSlotInit = 0x20 / 8, kSlotShut, kSlotInt, kSlotOcc, kSlotTouch, kSlotGate, kSlotQuery, kSlotOccluded, kSiteWord };

// ---- fakes ----
static int g_stock_query, g_stub_calls, g_game_calls;
static bool g_query_result = false;
static uint64_t g_last_id = 0;
static uintptr_t g_last_gs = 0, g_last_req = 0;
static void* g_cbvh = nullptr;
static float g_seen_seg[6];

extern "C" __attribute__((noinline)) void StockInit(void*, const void*) {}
extern "C" __attribute__((noinline)) void StockShut(void*) {}
extern "C" __attribute__((noinline)) bool StockInt(const void*, const float*, uint8_t*) { return false; }
extern "C" __attribute__((noinline)) bool StockOcc(const void*, const float*, const float*) { return false; }
extern "C" __attribute__((noinline)) bool StockTouch(const void*, const void*, void*, float*) { return true; }
extern "C" __attribute__((noinline)) bool StockGate(const void*, const void*, void*, void**) { return false; }
extern "C" __attribute__((noinline)) bool StockQuery(uintptr_t gs, uintptr_t req, uint64_t id) {
  ++g_stock_query;
  g_last_gs = gs;
  g_last_req = req;
  g_last_id = id;
  return g_query_result;
}
// CncaGame::StaticRaycastOccluded: the level's CBVHTree answers through bvh's wrapper
extern "C" __attribute__((noinline)) bool FakeGameOccluded(const void*, const float* a, const float* b) {
  ++g_game_calls;
  return reinterpret_cast<bool (*)(const void*, const float*, const float*)>(g_page[kSlotOcc])(g_cbvh, a, b);
}
// CncaGameSpace::StaticRaycastOccluded: the stub (ldr x0,[x0,#8]; ldr x8,[x0]; ldr x3,[x8,#0x278]; br x3)
extern "C" __attribute__((noinline)) bool FakeStub(const void* gs, const float* a, const float* b) {
  ++g_stub_calls;
  std::memcpy(g_seen_seg, a, sizeof g_seen_seg);
  const uintptr_t game = reinterpret_cast<const uintptr_t*>(gs)[1];
  const uintptr_t vptr = *reinterpret_cast<const uintptr_t*>(game);
  const uintptr_t fn = *reinterpret_cast<const uintptr_t*>(vptr + 0x278);
  return reinterpret_cast<bool (*)(const void*, const float*, const float*)>(fn)(reinterpret_cast<const void*>(game), a, b);
}

__attribute__((noinline)) static bool CallQuerySite(uintptr_t gs, uintptr_t req, uint64_t id) {
  volatile bool r = reinterpret_cast<bool (*)(uintptr_t, uintptr_t, uint64_t)>(g_page[kSlotQuery])(gs, req, id);
  return r;
}
__attribute__((noinline)) static bool CallQueryOther(uintptr_t gs, uintptr_t req, uint64_t id) {
  volatile bool r = reinterpret_cast<bool (*)(uintptr_t, uintptr_t, uint64_t)>(g_page[kSlotQuery])(gs, req, id);
  return r;
}

// the game: a gamespace whose +8 is a CncaGame whose vtable +0x278 is FakeGameOccluded
struct FakeGame {
  uintptr_t vptr;
  uint8_t pad[64];
};
struct FakeGs {
  uintptr_t w0, game, w2, w3;
};
static uintptr_t g_vtable[0x300 / 8];
static FakeGame g_game;
static FakeGs g_gs;

struct World {
  uint8_t* mem = nullptr;
  uintptr_t base = 0;
  q15t::BvhPins bvh{};
  q15t::WallfirePins wf{};
};
static World MakeWorld(uint32_t site_word, uintptr_t site_abs, bool exec_query_page) {
  World w;
  w.mem = static_cast<uint8_t*>(mmap(nullptr, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (w.mem == MAP_FAILED) std::abort();
  w.base = reinterpret_cast<uintptr_t>(w.mem);
  auto off = [&](const void* f) { return reinterpret_cast<uintptr_t>(f) - w.base; };
  const uintptr_t fns[8] = {off(reinterpret_cast<const void*>(&StockInit)), off(reinterpret_cast<const void*>(&StockShut)),
                            off(reinterpret_cast<const void*>(&StockInt)),  off(reinterpret_cast<const void*>(&StockOcc)),
                            off(reinterpret_cast<const void*>(&StockTouch)), off(reinterpret_cast<const void*>(&StockGate)),
                            off(reinterpret_cast<const void*>(&StockQuery)), off(reinterpret_cast<const void*>(&FakeStub))};
  const uintptr_t slots[8] = {0x20, 0x28, 0x30, 0x38, 0x40, 0x48, exec_query_page ? 0x1050u : 0x50u, 0x58};
  for (int i = 0; i < 8; ++i) *reinterpret_cast<uintptr_t*>(w.mem + slots[i]) = w.base + fns[i];
  *reinterpret_cast<uint32_t*>(w.mem + 0x60) = site_word;
  w.bvh.init = {slots[0], fns[0]};
  w.bvh.shutdown = {slots[1], fns[1]};
  w.bvh.intersect = {slots[2], fns[2]};
  w.bvh.occluded = {slots[3], fns[3]};
  w.bvh.touch = {slots[4], fns[4]};
  w.bvh.gate = {slots[5], fns[5]};
  w.bvh.n_allowed = 0;
  w.wf.query_slot = slots[6];
  w.wf.query_fn = fns[6];
  w.wf.site = site_abs != 0 ? site_abs - w.base : 0x60;
  w.wf.site_word = site_word;
  w.wf.occluded_slot = slots[7];
  w.wf.occluded_fn = fns[7];
  w.wf.game_occluded = off(reinterpret_cast<const void*>(&FakeGameOccluded));
  mprotect(w.mem, 4096, PROT_READ);
  mprotect(w.mem + 4096, 4096, exec_query_page ? (PROT_READ | PROT_EXEC) : PROT_READ);
  return w;
}

static std::string g_dir;
static void ClearDir() {
  const char* names[] = {"bvh.off", "bvhgate.off", "bvhtouch.keep", "wallfire.off"};
  for (const char* n : names) unlink((g_dir + "/" + n).c_str());
}
static void Touch(const char* name) {
  FILE* f = std::fopen((g_dir + "/" + name).c_str(), "w");
  if (f) std::fclose(f);
}

static bt::Resource g_room;
static bt::Resource RoomResource() {
  const float lo[3] = {-5, -5, -5}, hi[3] = {5, 5, 5};
  std::vector<bt::TriIn> in;
  bt::AddBox(in, lo, hi, 100, true);
  return bt::MakeResource(bt::Builder(in, 4).Build(), true);
}

static void Reset() {
  g_lines.clear();
  q15t::ResetBvhForTest();
  q15t::ResetWallfireForTest();
  g_stock_query = g_stub_calls = g_game_calls = 0;
  g_query_result = false;
  ClearDir();
  g_cbvh = nullptr;
}

// the bvh module on the world's page (gate off: the tree answers every cast, as Occluded is not gated anyway), then its tree
static uint64_t g_cbvh_word = 0;
static void InstallBvhAndTree(const World& w) {
  Touch("bvhgate.off");
  g_page = reinterpret_cast<uintptr_t*>(w.mem);
  const auto r = q15t::InstallBvhAt(w.base, w.bvh);
  CHECK(r.status == q15t::SwapStatus::kOk, "bvh install: %s", q15t::StatusName(r.status));
  g_cbvh_word = 0;
  g_room = RoomResource();
  reinterpret_cast<void (*)(void*, const void*)>(g_page[kSlotInit])(&g_cbvh_word, g_room.bytes.data());
  g_cbvh = &g_cbvh_word;
  unlink((g_dir + "/bvhgate.off").c_str());
}

static void SetGame(uintptr_t base, const World& w) {
  std::memset(g_vtable, 0, sizeof g_vtable);
  g_vtable[0x278 / 8] = base + w.wf.game_occluded;
  g_game.vptr = reinterpret_cast<uintptr_t>(g_vtable);
  g_gs = {0, reinterpret_cast<uintptr_t>(&g_game), 0, 0};
}

// SPhQueryCollisionRequest as FireGunLocal builds it: the first 24 bytes are the segment
static float g_req[16];
static uintptr_t Req(float a0, float a1, float a2, float b0, float b1, float b2) {
  std::memset(g_req, 0, sizeof g_req);
  const float s[6] = {a0, a1, a2, b0, b1, b2};
  std::memcpy(g_req, s, sizeof s);
  return reinterpret_cast<uintptr_t>(g_req);
}

int main() {
  char tmpl[] = "/tmp/q15t-wf-XXXXXX";
  if (mkdtemp(tmpl) == nullptr) return 2;
  g_dir = tmpl;
  NvrGameContext ctx{};
  ctx.ctx_size = sizeof ctx;
  ctx.plugin_id = "q15t";
  ctx.log = &FakeLog;
  q15t::SetLogContext(&ctx);
  q15t::SetBvhDirForTest(g_dir.c_str());
  q15t::SetWallfireDirForTest(g_dir.c_str());

  // ---------------------------------------------------------------- the pinned constants (readelf -rW and the disassembly: docs/quest/q15t/modules/wallfire.md section 1)
  {
    const q15t::WallfirePins& p = q15t::kWallfirePinned;
    CHECK(p.query_slot == 0x36e0c38 && p.query_fn == 0x1806784, "R26 pin");
    CHECK(p.site == 0x215566c && p.site_word == 0x3941b3e8, "R27 pin");
    CHECK(p.occluded_slot == 0x36ce380 && p.occluded_fn == 0x18066f4, "R28 pin");
    CHECK(p.game_occluded == 0x17f2614, "CncaGame::StaticRaycastOccluded pin");
    CHECK(q15t::kGsGame == 0x8 && q15t::kGameVtblSlot == 0x278 && q15t::kReqSegFloats == 6, "structure offsets");
  }

  // ---------------------------------------------------------------- 0. learn FireGunLocal's site: the return address of CallQuerySite
  uintptr_t site_abs = 0;
  uint32_t site_word = 0;
  {
    Reset();
    World w = MakeWorld(0xdeadbeef, 0, false);
    InstallBvhAndTree(w);
    SetGame(w.base, w);
    const auto r = q15t::InstallWallfireAt(w.base, w.wf);
    CHECK(r.status == q15t::SwapStatus::kOk, "learning install: %s", q15t::StatusName(r.status));
    CallQuerySite(reinterpret_cast<uintptr_t>(&g_gs), Req(0, 0, 0, 0, 0, 1), 0xffffffff);
    site_abs = q15t::WallfireLastRetForTest() + w.base;
    CHECK(site_abs != w.base, "no return address recorded");
    CHECK(q15t::SafeRead(site_abs, &site_word, sizeof site_word), "the site is unreadable");
  }

  // ---------------------------------------------------------------- 1. installs
  Reset();
  {  // without bvh nothing is installed (the segment would never be blocked)
    World w = MakeWorld(site_word, site_abs, false);
    g_page = reinterpret_cast<uintptr_t*>(w.mem);
    const uintptr_t before = g_page[kSlotQuery];
    const auto r = q15t::InstallWallfireAt(w.base, w.wf);
    CHECK(r.status != q15t::SwapStatus::kOk && std::strcmp(r.reason, "bvh_not_installed") == 0, "no bvh: %s / %s", q15t::StatusName(r.status), r.reason);
    CHECK(g_page[kSlotQuery] == before && Count("wallfire refused", 2) == 1, "no-bvh refusal: slot or log");
  }
  Reset();
  {
    World w = MakeWorld(site_word, site_abs, false);
    InstallBvhAndTree(w);
    SetGame(w.base, w);
    const uintptr_t before = g_page[kSlotQuery];
    const auto r = q15t::InstallWallfireAt(w.base, w.wf);
    CHECK(r.status == q15t::SwapStatus::kOk, "install: %s / %s", q15t::StatusName(r.status), r.reason);
    CHECK(g_page[kSlotQuery] != before, "the QueryCollision slot was not replaced");
    CHECK(g_page[kSlotOccluded] == w.base + w.wf.occluded_fn, "the StaticRaycastOccluded slot must only be read");
    CHECK(Count("wallfire on", 1) == 1, "no 'on' line");

    const uintptr_t gs = reinterpret_cast<uintptr_t>(&g_gs);
    // ------------------------------------------------------------ 2. shots from FireGunLocal's site
    // the stock query's own answer passes through, the static level is not asked
    g_query_result = true;
    CHECK(CallQuerySite(gs, Req(0, 0, 0, 0, 0, 5.2f), 0xffffffff) && g_stub_calls == 0, "a stock hit was not passed through");
    g_query_result = false;
    CHECK(g_stock_query == 1 && g_last_gs == gs && g_last_id == 0xffffffffull, "stock query args: gs %llx id %llx", (unsigned long long)g_last_gs, (unsigned long long)g_last_id);
    CHECK(g_last_req == reinterpret_cast<uintptr_t>(g_req), "stock query got another request");
    // a segment inside the room: not blocked; the stock answer (false) stands
    CHECK(!CallQuerySite(gs, Req(0, 0, 0, 0, 0, 4.9f), 0xffffffff), "a clear segment was blocked");
    CHECK(g_stub_calls == 1 && g_game_calls == 1, "stub %d game %d", g_stub_calls, g_game_calls);
    // the segment arguments are start xyz and end xyz
    const float want[6] = {1, 2, 3, 1, 2, 4};
    CallQuerySite(gs, Req(1, 2, 3, 1, 2, 4), 0xffffffff);
    CHECK(std::memcmp(g_seen_seg, want, sizeof want) == 0, "the stub saw %.1f %.1f %.1f %.1f %.1f %.1f", g_seen_seg[0], g_seen_seg[1], g_seen_seg[2], g_seen_seg[3], g_seen_seg[4], g_seen_seg[5]);
    // through a wall: blocked, so the shot is refused (QueryCollision answers 1)
    CHECK(CallQuerySite(gs, Req(0, 0, 0, 0, 0, 5.2f), 0xffffffff), "a segment through the +z wall was not blocked");
    CHECK(CallQuerySite(gs, Req(0, 0, 0, 6, 0, 0), 0xffffffff), "a segment through the +x wall was not blocked");
    CHECK(CallQuerySite(gs, Req(3, 1, 0, 3, 7, 0), 0xffffffff), "a segment through the +y wall was not blocked");
    CHECK(Count("wallfire blocked a shot", 1) == 3, "blocked lines: %d", Count("wallfire blocked a shot", 1));
    // other callers of QueryCollision get the stock answer, whatever the segment
    CHECK(!CallQueryOther(gs, Req(0, 0, 0, 0, 0, 5.2f), 0xffffffff), "another call site was blocked");
    // a null gamespace, a non-finite or unreadable segment, an unreadable request: the stock answer
    CHECK(!CallQuerySite(0, Req(0, 0, 0, 0, 0, 5.2f), 0xffffffff), "gs 0 was asked");
    CHECK(!CallQuerySite(gs, Req(0, 0, 0, NAN, 0, 5.2f), 0xffffffff), "a NaN segment was asked");
    CHECK(!CallQuerySite(gs, Req(0, 0, 0, 0, INFINITY, 5.2f), 0xffffffff), "an infinite segment was asked");
    CHECK(!CallQuerySite(gs, 0x1000, 0xffffffff), "an unreadable request was asked");
    auto c = q15t::WallfireGetCounters();
    CHECK(c.at_site == 8 && c.checked == 5 && c.blocked == 3 && c.skipped_nan == 3 && c.skipped_game == 0,
          "counters site %llu checked %llu blocked %llu nan %llu game %llu", (unsigned long long)c.at_site, (unsigned long long)c.checked,
          (unsigned long long)c.blocked, (unsigned long long)c.skipped_nan, (unsigned long long)c.skipped_game);

    // ------------------------------------------------------------ 3. the game's structures
    FakeGs bad_gs = {0, 0, 0, 0};  // no CncaGame
    CHECK(!CallQuerySite(reinterpret_cast<uintptr_t>(&bad_gs), Req(0, 0, 0, 0, 0, 5.2f), 0xffffffff), "a gamespace without a game was asked");
    FakeGs wild = {0, 0x1000, 0, 0};  // an unreadable CncaGame
    CHECK(!CallQuerySite(reinterpret_cast<uintptr_t>(&wild), Req(0, 0, 0, 0, 0, 5.2f), 0xffffffff), "an unreadable game was asked");
    FakeGame nov = {0, {}};
    FakeGs gs_nov = {0, reinterpret_cast<uintptr_t>(&nov), 0, 0};
    CHECK(!CallQuerySite(reinterpret_cast<uintptr_t>(&gs_nov), Req(0, 0, 0, 0, 0, 5.2f), 0xffffffff), "a game without a vtable was asked");
    uintptr_t other_vt[0x300 / 8] = {};
    other_vt[0x278 / 8] = w.base + 0x1234;  // another class's virtual
    FakeGame other = {reinterpret_cast<uintptr_t>(other_vt), {}};
    FakeGs gs_other = {0, reinterpret_cast<uintptr_t>(&other), 0, 0};
    CHECK(!CallQuerySite(reinterpret_cast<uintptr_t>(&gs_other), Req(0, 0, 0, 0, 0, 5.2f), 0xffffffff), "a game with another virtual was asked");
    CHECK(Count("wallfire skipped a shot check", 2) == 1, "the skip warning: %d", Count("wallfire skipped a shot check", 2));
    c = q15t::WallfireGetCounters();
    CHECK(c.skipped_game == 4 && c.checked == 5 && g_stub_calls == 5 + 0, "game skips %llu checked %llu stub %d", (unsigned long long)c.skipped_game, (unsigned long long)c.checked, g_stub_calls);
    // the pinned game still works after the others, and a vtable whose slot changes later is checked again
    CHECK(CallQuerySite(gs, Req(0, 0, 0, 0, 0, 5.2f), 0xffffffff), "the pinned game stopped working after the others");
    CHECK(!CallQuerySite(reinterpret_cast<uintptr_t>(&gs_other), Req(0, 0, 0, 0, 0, 5.2f), 0xffffffff), "a vtable that was not the pinned one was trusted");
    const uintptr_t saved = g_vtable[0x278 / 8];
    g_vtable[0x278 / 8] = w.base + 0x1234;
    CHECK(!CallQuerySite(gs, Req(0, 0, 0, 0, 0, 5.2f), 0xffffffff), "a vtable changed after a good shot was still trusted");
    g_vtable[0x278 / 8] = saved;

    // ------------------------------------------------------------ 4. log volume: the first 16 blocked shots, then powers of two
    g_lines.clear();
    for (int i = 0; i < 60; ++i) CallQuerySite(gs, Req(0, 0, 0, 0, 0, 5.2f), 0xffffffff);
    // blocked so far: 3 + 1 + 60 = 64 total; lines for n = 5..16 (12) + 32 + 64 = 14
    CHECK(Count("wallfire blocked a shot", 1) == 14, "blocked lines %d", Count("wallfire blocked a shot", 1));
    CHECK(q15t::WallfireGetCounters().blocked == 64, "blocked %llu", (unsigned long long)q15t::WallfireGetCounters().blocked);
  }

  // ---------------------------------------------------------------- 4b. a game whose vtable pointer is null, as the very first shot
  {
    Reset();
    World w = MakeWorld(site_word, site_abs, false);
    InstallBvhAndTree(w);
    SetGame(w.base, w);
    CHECK(q15t::InstallWallfireAt(w.base, w.wf).status == q15t::SwapStatus::kOk, "install");
    FakeGame nov = {0, {}};
    FakeGs gs_nov = {0, reinterpret_cast<uintptr_t>(&nov), 0, 0};
    CHECK(!CallQuerySite(reinterpret_cast<uintptr_t>(&gs_nov), Req(0, 0, 0, 0, 0, 5.2f), 0xffffffff) && g_stub_calls == 0, "a null vtable pointer was trusted");
    CHECK(q15t::WallfireGetCounters().skipped_game == 1, "skip not counted");
  }

  // ---------------------------------------------------------------- 5. refusals change nothing
  {
    Reset();
    World w = MakeWorld(site_word ^ 1u, site_abs, false);  // the pinned word does not match what the site holds
    InstallBvhAndTree(w);
    const uintptr_t before = g_page[kSlotQuery];
    w.wf.site_word = site_word ^ 1u;
    const auto r = q15t::InstallWallfireAt(w.base, w.wf);
    CHECK(std::strcmp(r.reason, "site_word") == 0 && g_page[kSlotQuery] == before, "site word: %s", r.reason);
    CHECK(Count("wallfire refused", 3) == 1, "site word refusal not an ERROR");
  }
  {
    Reset();
    World w = MakeWorld(site_word, site_abs, false);
    InstallBvhAndTree(w);
    const uintptr_t before = g_page[kSlotQuery];
    mprotect(w.mem, 4096, PROT_READ | PROT_WRITE);
    g_page[kSlotOccluded] = 0x4242420;
    mprotect(w.mem, 4096, PROT_READ);
    const auto r = q15t::InstallWallfireAt(w.base, w.wf);
    CHECK(std::strcmp(r.reason, "occluded_slot") == 0 && g_page[kSlotQuery] == before, "occluded slot: %s", r.reason);
  }
  {
    Reset();
    World w = MakeWorld(site_word, site_abs, false);
    InstallBvhAndTree(w);
    mprotect(w.mem, 4096, PROT_READ | PROT_WRITE);
    g_page[kSlotQuery] = 0x4242420;
    mprotect(w.mem, 4096, PROT_READ);
    const auto r = q15t::InstallWallfireAt(w.base, w.wf);
    CHECK(r.status == q15t::SwapStatus::kUnexpectedOriginal && g_page[kSlotQuery] == 0x4242420 && Count("wallfire hook refused", 3) == 1,
          "query slot mismatch: %s", q15t::StatusName(r.status));
  }
  {  // the slot holds some other mapped function (not the pinned one): refused, not chained
    Reset();
    World w = MakeWorld(site_word, site_abs, false);
    InstallBvhAndTree(w);
    mprotect(w.mem, 4096, PROT_READ | PROT_WRITE);
    g_page[kSlotQuery] = reinterpret_cast<uintptr_t>(&StockInit);
    mprotect(w.mem, 4096, PROT_READ);
    const auto r = q15t::InstallWallfireAt(w.base, w.wf);
    CHECK(r.status == q15t::SwapStatus::kUnexpectedOriginal && g_page[kSlotQuery] == reinterpret_cast<uintptr_t>(&StockInit), "another function in the slot: %s",
          q15t::StatusName(r.status));
  }
  {
    Reset();
    World w = MakeWorld(site_word, site_abs, true);  // the QueryCollision slot's page is executable: never written
    InstallBvhAndTree(w);
    const uintptr_t before = *reinterpret_cast<uintptr_t*>(w.mem + 0x1050);
    const auto r = q15t::InstallWallfireAt(w.base, w.wf);
    CHECK(r.status == q15t::SwapStatus::kExecutablePage && *reinterpret_cast<uintptr_t*>(w.mem + 0x1050) == before, "exec page: %s", q15t::StatusName(r.status));
    CHECK(Count("wallfire hook refused", 3) == 1, "exec page refusal not an ERROR");
  }
  {
    Reset();
    World w = MakeWorld(site_word, site_abs, false);
    InstallBvhAndTree(w);
    Touch("wallfire.off");
    const uintptr_t before = g_page[kSlotQuery];
    const auto r = q15t::InstallWallfireAt(w.base, w.wf);
    CHECK(std::strcmp(r.reason, "off_file") == 0 && g_page[kSlotQuery] == before && Count("wallfire off", 1) == 1, "wallfire.off: %s", r.reason);
  }
  {
    Reset();
    World w = MakeWorld(site_word, site_abs, false);
    InstallBvhAndTree(w);
    const auto r = q15t::InstallWallfireAt(0, w.wf);
    CHECK(std::strcmp(r.reason, "no_base") == 0, "no base: %s", r.reason);
  }

  ClearDir();
  rmdir(g_dir.c_str());
  if (g_fail == 0) std::printf("wallfire_test: all checks passed\n");
  else std::printf("wallfire_test: %d failure(s)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
