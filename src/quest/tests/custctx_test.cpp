// Host test of custctx (src/quest/combat/custctx.cpp, #2178): the three real handlers through their thunks, installed by the real GotHook into the three
// JUMP_SLOTs of a libr15 image built in memory, and called the way the game calls them: through the slots. The game side is a model: fake
// LoadMeshData / DestroyContext / Grow / Resize functions, a fake draw table laid out as Grow reads it, and a fake render-target layout pool whose
// pointer sits in the image at the pinned GOT word.
//
// The q15t plugin's custctx_test assertions, on the new fixture: the live-context table (a destroy of an unknown, finished, loading or destroying
// context is dropped, a reload during a destroy makes it live again, finished contexts leave the table, the 256 limit, the capped drop lines), the layout
// pool margin (64 free loads, 63 refuses and answers 1, an unreadable pool never refuses, the capped lines), Grow (the room path, the resize with
// max(capacity, step), a failed resize or a torn-down table made empty with a spare element in no table, the round-robin install-time spares, the capped
// lines), and the install (all or none, a foreign or other-code word refused in each slot, a spare that cannot be allocated, no Resize export, the
// Resize address check, every pinned word). New with the move: the switch, the legacy switch file (a Warning, never a switch), the missing image and
// build ID, the removal of the hooks before a refused one proved on the slots and on the thunks, the stats lines and the lines' golden text.
//
// Built and run by `just test-quest-combat`.

#include "quest/combat/custctx.h"
#include "quest/combat/legacy_switch.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "quest/tests/combat_libr15_image.h"
#include "quest/tests/combat_log_capture.h"
#include "quest/tests/combat_pin_check.h"
#include "quest/tests/test_check.h"

namespace {

namespace cc = nevr_quest::combat::custctx;
namespace pinned = sentinel::pinned;
using combat_test::Has;
using combat_test::Image;
using combat_test::Line;
using combat_test::Lines;
using cc::DrawTable;
using nevr_quest::combat::ModuleResult;
using nevr_quest::combat::PinnedWord;

constexpr std::uint64_t kDraw = 0x9718;

// ---- the fake engine ------------------------------------------------------------------------------------------------------------

void* const kAlloc = reinterpret_cast<void*>(0xA110C);
DrawTable g_tbl;
std::uint64_t g_loadRet = 0;
constexpr std::uint64_t kDestroyRet = 0x5eed;
int g_loadCalls = 0, g_destroyCalls = 0, g_growCalls = 0, g_resizeCalls = 0;
void *g_loadCtx = nullptr, *g_loadSelf = nullptr, *g_loadModel = nullptr;
void* g_destroyCtx = nullptr;
void (*g_loadHook)(void*) = nullptr;
void (*g_destroyHook)(void*) = nullptr;
bool g_unsafeGrow = false;
int g_growUnreadable = 0;
DrawTable g_growSeen;  // the table the last real Grow saw
void* g_growSeenPtr = nullptr;
void* g_growRet = nullptr;
bool g_resizeOk = true;
void* g_rsBlock = nullptr;
std::uint64_t g_rsBytes = 0;
std::uint32_t g_rsKeep = 0;

extern "C" __attribute__((noinline)) std::uint64_t FakeLoad(void* self, void* ctx, void* model) {
  ++g_loadCalls;
  g_loadSelf = self;
  g_loadCtx = ctx;
  g_loadModel = model;
  if (g_loadHook != nullptr) g_loadHook(ctx);
  return g_loadRet;
}
extern "C" __attribute__((noinline)) std::uint64_t FakeDestroy(void*, void* ctx) {
  ++g_destroyCalls;
  g_destroyCtx = ctx;
  if (g_destroyHook != nullptr) g_destroyHook(ctx);
  return kDestroyRet;
}
extern "C" __attribute__((noinline)) void* FakeGrow(DrawTable* t) {
  ++g_growCalls;
  if (reinterpret_cast<std::uintptr_t>(t) < 0x10000) {
    ++g_growUnreadable;
    return nullptr;
  }
  g_growSeen = *t;
  g_growSeenPtr = t;
  // the real Grow's room path: it never resizes here and writes the element's slot: a NULL data or no room is the crash
  if (t->data == nullptr || t->count >= t->capacity) {
    g_unsafeGrow = true;
    return nullptr;
  }
  ++t->count;
  g_growRet = t->data + (t->count - 1) * kDraw;
  return g_growRet;
}
extern "C" __attribute__((noinline)) void FakeOtherCode() {}
void FakeResize(void* block, std::uint64_t bytes, std::uint32_t keep) {
  ++g_resizeCalls;
  g_rsBlock = block;
  g_rsBytes = bytes;
  g_rsKeep = keep;
  auto* t = static_cast<DrawTable*>(block);
  if (g_resizeOk) {
    t->data = static_cast<std::uint8_t*>(std::realloc(t->data, bytes));
    t->size = bytes;
  } else {
    t->data = nullptr;  // an out-of-memory Resize leaves the data pointer NULL
  }
}

alignas(16) std::uint8_t g_pool[0x100];

using LoadF = std::uint64_t (*)(void*, void*, void*);
using DestroyF = std::uint64_t (*)(void*, void*);
using GrowF = void* (*)(DrawTable*);
LoadF hLoad;
DestroyF hDestroy;
GrowF hGrow;

void* Ctx(int i) { return reinterpret_cast<void*>(0x100000 + static_cast<std::uintptr_t>(i) * 0x100); }

// ---- the libr15 image ----------------------------------------------------------------------------------------------------------

Image g_image;
cc::Pins g_pins = cc::kPins;
bool Lookup(const char* module, sentinel::ElfImage* out) {
  if (!g_image.Present() || std::strcmp(module, pinned::kLibR15) != 0) return false;
  *out = g_image.Elf();
  return true;
}

// The pointer word the guard reads the layout pool through: the pool, null or an unreadable address.
void PoolCell(std::uintptr_t value) { g_image.Put<std::uintptr_t>(g_pins.rtPool, value); }
void SetPool(std::uint64_t freeSlots) {
  std::memset(g_pool, 0, sizeof(g_pool));
  const std::uint32_t cap = 512;
  std::memcpy(g_pool + 0x84, &cap, sizeof(cap));
  std::memcpy(g_pool + 0x30, &freeSlots, sizeof(freeSlots));
  PoolCell(reinterpret_cast<std::uintptr_t>(g_pool));
}

// A libr15 holding the pinned words and the three slots, which hold the given words (the stand-ins when 0).
void Build(std::uintptr_t load = 0, std::uintptr_t destroy = 0, std::uintptr_t grow = 0) {
  g_pins = cc::kPins;
  g_image.Build({{cc::kLoadSymbol, g_pins.slotLoad, false, false, g_pins.fnLoad},
                 {cc::kDestroySymbol, g_pins.slotDestroy, false, false, g_pins.fnDestroy},
                 {cc::kGrowSymbol, g_pins.slotGrow, false, false, g_pins.fnGrow}});
  // base + fn is the stand-in's address: the real functions are not in the image, so the pins hold the stand-ins' offsets.
  g_pins.fnLoad = reinterpret_cast<std::uintptr_t>(&FakeLoad) - g_image.Base();
  g_pins.fnDestroy = reinterpret_cast<std::uintptr_t>(&FakeDestroy) - g_image.Base();
  g_pins.fnGrow = reinterpret_cast<std::uintptr_t>(&FakeGrow) - g_image.Base();
  for (const PinnedWord& w : cc::kWords) g_image.Put<std::uint32_t>(w.vaddr, w.word);
  *g_image.Slot(g_pins.slotLoad) = reinterpret_cast<void*>(load != 0 ? load : reinterpret_cast<std::uintptr_t>(&FakeLoad));
  *g_image.Slot(g_pins.slotDestroy) = reinterpret_cast<void*>(destroy != 0 ? destroy : reinterpret_cast<std::uintptr_t>(&FakeDestroy));
  *g_image.Slot(g_pins.slotGrow) = reinterpret_cast<void*>(grow != 0 ? grow : reinterpret_cast<std::uintptr_t>(&FakeGrow));
  SetPool(100);
}
std::uintptr_t SlotWord(std::uint64_t slot) { return reinterpret_cast<std::uintptr_t>(*g_image.Slot(slot)); }
bool SlotsAreTheGames() {
  return SlotWord(g_pins.slotLoad) == reinterpret_cast<std::uintptr_t>(&FakeLoad) &&
         SlotWord(g_pins.slotDestroy) == reinterpret_cast<std::uintptr_t>(&FakeDestroy) &&
         SlotWord(g_pins.slotGrow) == reinterpret_cast<std::uintptr_t>(&FakeGrow);
}

cc::Fns Fns() {
  cc::Fns f{};
  f.resize = &FakeResize;
  return f;
}

void ResetWorld() {
  g_loadRet = 0;
  g_loadCalls = g_destroyCalls = g_growCalls = g_resizeCalls = g_growUnreadable = 0;
  g_loadHook = nullptr;
  g_destroyHook = nullptr;
  g_unsafeGrow = false;
  g_resizeOk = true;
  g_loadCtx = g_destroyCtx = nullptr;
  std::free(g_tbl.data);
  std::memset(&g_tbl, 0, sizeof(g_tbl));
  g_tbl.allocator = kAlloc;
}

void Begin() {
  combat_test::StartCapture();
  cc::ResetForTest();
  ResetWorld();
  Build();
}
void End() {
  cc::ResetForTest();
  g_image.Drop();
  combat_test::StopCapture();
}

ModuleResult InstallOn(bool on = true, const char* legacyDir = nullptr) {
  const cc::Fns fns = Fns();
  nevr_quest::combat::legacy::SetDirectoryForTest(legacyDir);  // where the plugin's custctx.off would sit (nullptr: production)
  const ModuleResult r = cc::InstallAt(&Lookup, g_pins, &fns, on);
  nevr_quest::combat::legacy::SetDirectoryForTest(nullptr);
  return r;
}

// A fresh world with the guards installed; the handlers are called through the slots.
void Installed() {
  Begin();
  const ModuleResult r = InstallOn();
  QCHECK(r.installed && std::strcmp(r.status, "installed") == 0);
  hLoad = reinterpret_cast<LoadF>(*g_image.Slot(g_pins.slotLoad));
  hDestroy = reinterpret_cast<DestroyF>(*g_image.Slot(g_pins.slotDestroy));
  hGrow = reinterpret_cast<GrowF>(*g_image.Slot(g_pins.slotGrow));
}

// nested calls from inside the fake real functions
void NestedDestroyInLoad(void* ctx) { hDestroy(nullptr, ctx); }
int g_depth = 0;
void ReloadOnlyInDestroy(void* ctx) { hLoad(nullptr, ctx, nullptr); }
void NestedDestroyInDestroy(void* ctx) { hDestroy(nullptr, ctx); }
void ReloadThenDestroyInDestroy(void* ctx) {
  if (g_depth++ != 0) return;
  hLoad(nullptr, ctx, nullptr);
  hDestroy(nullptr, ctx);
}

int FailingMemalign(void** out, std::size_t, std::size_t) {
  *out = nullptr;
  return ENOMEM;
}
std::size_t g_lastAlign = 0, g_lastSize = 0;
int RecordingMemalign(void** out, std::size_t align, std::size_t size) {
  g_lastAlign = align;
  g_lastSize = size;
  return posix_memalign(out, align, size);
}
int g_memalignCalls = 0;
int FailThird(void** out, std::size_t align, std::size_t size) {
  if (++g_memalignCalls == 3) {
    *out = nullptr;
    return ENOMEM;
  }
  return posix_memalign(out, align, size);
}

int CountLines(const char* event, const char* needle = "") {
  int c = 0;
  for (const Line& l : Lines(event)) {
    if (Has(l.text, needle)) ++c;
  }
  return c;
}

// ---- the tests ------------------------------------------------------------------------------------------------------------------

void ThePinsAreTheReadelfAndDisassemblyPins() {
  const cc::Pins& p = cc::kPins;
  QCHECK(p.slotLoad == 0x36cf6f0 && p.fnLoad == 0x1247d90 && p.slotDestroy == 0x36c22f8 && p.fnDestroy == 0x12492fc && p.slotGrow == 0x36ed7f0 &&
         p.fnGrow == 0x124893c && p.fnResize == 0x11dc30c && p.rtPool == 0x379b2c0);
  QCHECK(cc::kWordCount == 34);
  QCHECK(combat_test::Fingerprint(cc::kWords, cc::kWordCount) == 0xe7250efd9cc11821ULL);
  QCHECK(cc::kDrawSize == 0x9718 && cc::kLayoutMargin == 64 && cc::kPoolCapacityOffset == 0x84 && cc::kPoolFreeOffset == 0x30);
  QCHECK(std::strcmp(cc::kResizeSymbol, "_ZN10NRadEngine9CMemBlock6ResizeEyj") == 0);
  QCHECK(std::strcmp(cc::kLoadSymbol,
                     "_ZN10NRadEngine8NRadGame28CR15NetCustomizationRenderer12LoadMeshDataERNS0_27SR15NetCustomizationContextERNS_16CComponentHandleINS_8CModelCSEEE") == 0);
  QCHECK(std::strcmp(cc::kDestroySymbol,
                     "_ZN10NRadEngine8NRadGame28CR15NetCustomizationRenderer14DestroyContextEPNS0_27SR15NetCustomizationContextE") == 0);
  QCHECK(std::strcmp(cc::kGrowSymbol, "_ZN10NRadEngine8CTableXTINS_8NRadGame21SR15NetCustomDrawDataEE4GrowEv") == 0);
}

void TheThunksTakeTheSlotsAndTheLineSaysSo() {
  Installed();
  QCHECK(SlotWord(g_pins.slotLoad) == reinterpret_cast<std::uintptr_t>(cc::LoadThunk::EntryAddress()));
  QCHECK(SlotWord(g_pins.slotDestroy) == reinterpret_cast<std::uintptr_t>(cc::DestroyThunk::EntryAddress()));
  QCHECK(SlotWord(g_pins.slotGrow) == reinterpret_cast<std::uintptr_t>(cc::GrowThunk::EntryAddress()));
  const std::vector<Line> install = Lines("combat_custctx_install");
  QCHECK(install.size() == 1U);
  if (install.size() == 1U) {
    QCHECK(install[0].level == sentinel::LogLevel::kInfo);
    QCHECK(combat_test::Golden(install[0].text) ==
           "\"event\":\"combat_custctx_install\",\"result\":\"installed\",\"reason\":\"ok\",\"load\":\"ok\",\"destroy\":\"ok\",\"grow\":\"ok\","
           "\"layout_margin\":64,\"consequence\":\"loads are refused on a nearly full layout pool, a destroy runs only for a live context with no load "
           "in flight, and a draw table that cannot grow is made empty\"}");
  }
  End();
}

// R32 / R31: the live-context table.
void TheLiveContextTableDecidesWhichDestroysRun() {
  Installed();
  // a destroy of an address no live context holds is dropped
  QCHECK(hDestroy(nullptr, Ctx(1)) == 0);  // a dropped destroy answers 0
  QCHECK(g_destroyCalls == 0 && cc::CurrentStats().destroysDropped == 1);
  const std::vector<Line> dropped = Lines("combat_custctx_dropped");
  QCHECK(dropped.size() == 1U && Has(dropped[0].text, "\"why\":\"not live: no context holds this address\"") &&
         Has(dropped[0].text, "\"ctx\":\"0x0000000000100100\""));
  if (dropped.size() == 1U) {
    QCHECK(dropped[0].level == sentinel::LogLevel::kInfo);
    QCHECK(combat_test::Golden(dropped[0].text) ==
           "\"event\":\"combat_custctx_dropped\",\"ctx\":\"0x0000000000100100\",\"why\":\"not live: no context holds this address\",\"dropped\":1,"
           "\"consequence\":\"the game's destroy was not run for this context\"}");
  }

  // load, destroy: once; a second destroy of the same address is dropped
  g_loadRet = 5;
  const std::uint64_t lr = hLoad(reinterpret_cast<void*>(0x11), Ctx(1), reinterpret_cast<void*>(0x22));
  QCHECK(lr == 5 && g_loadCalls == 1 && g_loadSelf == reinterpret_cast<void*>(0x11) && g_loadCtx == Ctx(1) && g_loadModel == reinterpret_cast<void*>(0x22));
  QCHECK(hDestroy(reinterpret_cast<void*>(0x33), Ctx(1)) == kDestroyRet);  // the game's answer goes back
  QCHECK(g_destroyCalls == 1 && g_destroyCtx == Ctx(1));
  hDestroy(nullptr, Ctx(1));
  QCHECK(g_destroyCalls == 1 && cc::CurrentStats().destroysDropped == 2);

  // a null context goes to the real function (it returns at once), no drop
  hDestroy(nullptr, nullptr);
  QCHECK(g_destroyCalls == 2 && g_destroyCtx == nullptr && cc::CurrentStats().destroysDropped == 2);

  // a destroy that arrives while the same context's LoadMeshData runs is dropped; afterwards it runs
  combat_test::Captured().clear();
  g_loadRet = 0;
  g_loadHook = &NestedDestroyInLoad;
  hLoad(nullptr, Ctx(2), nullptr);
  g_loadHook = nullptr;
  QCHECK(g_destroyCalls == 2 && CountLines("combat_custctx_dropped", "\"why\":\"its LoadMeshData is running\"") == 1);
  hDestroy(nullptr, Ctx(2));
  QCHECK(g_destroyCalls == 3);  // the in-flight count is not stuck

  // a destroy that arrives while the same context is being destroyed is dropped
  combat_test::Captured().clear();
  hLoad(nullptr, Ctx(3), nullptr);
  g_destroyHook = &NestedDestroyInDestroy;
  const int before = g_destroyCalls;
  hDestroy(nullptr, Ctx(3));
  g_destroyHook = nullptr;
  QCHECK(g_destroyCalls == before + 1 && CountLines("combat_custctx_dropped", "\"why\":\"already being destroyed\"") == 1);

  // a load of the same address while it is being destroyed makes it live again: the nested destroy runs
  hLoad(nullptr, Ctx(4), nullptr);
  g_depth = 0;
  g_destroyHook = &ReloadThenDestroyInDestroy;
  const int before2 = g_destroyCalls;
  hDestroy(nullptr, Ctx(4));
  g_destroyHook = nullptr;
  QCHECK(g_destroyCalls == before2 + 2);
  hDestroy(nullptr, Ctx(4));  // the nested destroy finished it: a third one is a stale destroy
  QCHECK(g_destroyCalls == before2 + 2);

  // a load of the same address while it is being destroyed, with no destroy after it: the context is live again, so a later destroy runs
  hLoad(nullptr, Ctx(5), nullptr);
  g_destroyHook = &ReloadOnlyInDestroy;
  const int before3 = g_destroyCalls;
  hDestroy(nullptr, Ctx(5));
  g_destroyHook = nullptr;
  QCHECK(g_destroyCalls == before3 + 1);
  hDestroy(nullptr, Ctx(5));
  QCHECK(g_destroyCalls == before3 + 2);
  End();
}

// The table does not fill up with finished contexts.
void FinishedContextsLeaveTheTable() {
  Installed();
  for (int i = 0; i < 600; ++i) {
    hLoad(nullptr, Ctx(i), nullptr);
    hDestroy(nullptr, Ctx(i));
  }
  QCHECK(g_destroyCalls == 600 && Lines("combat_custctx_full").empty());
  End();
}

// More than 256 live contexts: the game's behaviour for the overflow, one Warning.
void MoreThan256LiveContextsGetTheGamesBehaviour() {
  Installed();
  for (int i = 0; i < 256; ++i) hLoad(nullptr, Ctx(i), nullptr);
  QCHECK(Lines("combat_custctx_full").empty() && g_loadCalls == 256);
  hLoad(nullptr, Ctx(256), nullptr);
  hLoad(nullptr, Ctx(257), nullptr);
  QCHECK(g_loadCalls == 258);  // an untracked load went to the real function
  const std::vector<Line> full = Lines("combat_custctx_full");
  QCHECK(full.size() == 1U && full[0].level == sentinel::LogLevel::kWarn && Has(full[0].text, "\"max\":256"));
  hDestroy(nullptr, Ctx(256));
  QCHECK(g_destroyCalls == 0);  // a destroy of an untracked context is dropped
  hDestroy(nullptr, Ctx(0));
  QCHECK(g_destroyCalls == 1);
  hLoad(nullptr, Ctx(256), nullptr);
  hDestroy(nullptr, Ctx(256));
  QCHECK(g_destroyCalls == 2);  // a freed entry was reused
  End();
}

// Drop log caps: 16 lines, then a total at every 50th drop, at most 16 totals.
void TheDropLinesAreCapped() {
  Installed();
  for (int i = 0; i < 49; ++i) hDestroy(nullptr, Ctx(1));
  QCHECK(CountLines("combat_custctx_dropped") == 16 && CountLines("combat_custctx_drops_total") == 0);
  hDestroy(nullptr, Ctx(1));
  QCHECK(CountLines("combat_custctx_drops_total", "\"dropped\":50") == 1);
  for (int i = 0; i < 850; ++i) hDestroy(nullptr, Ctx(1));
  QCHECK(CountLines("combat_custctx_drops_total") == 16);
  End();
}

// R31 v4: the render-target layout pool.
void ALoadOnANearlyFullLayoutPoolIsRefused() {
  Installed();
  g_loadRet = 7;
  SetPool(100);
  QCHECK(hLoad(nullptr, Ctx(1), nullptr) == 7 && g_loadCalls == 1);  // a roomy pool: passed through
  SetPool(64);
  QCHECK(hLoad(nullptr, Ctx(1), nullptr) == 7 && g_loadCalls == 2);  // 64 free slots still load
  SetPool(63);
  QCHECK(hLoad(nullptr, Ctx(1), nullptr) == 1 && g_loadCalls == 2);  // 63 refuse: answer 1, the real function is not called
  QCHECK(cc::CurrentStats().loadsRefused == 1);
  const std::vector<Line> refused = Lines("combat_custctx_load_refused");
  QCHECK(refused.size() == 1U && refused[0].level == sentinel::LogLevel::kInfo);
  if (refused.size() == 1U) {
    QCHECK(combat_test::Golden(refused[0].text) ==
           "\"event\":\"combat_custctx_load_refused\",\"layouts_used\":449,\"layouts_capacity\":512,\"refused\":1,\"consequence\":\"the render "
           "target layouts are nearly full, so this player's cosmetics are skipped instead of trapping the game\"}");
  }
  hDestroy(nullptr, Ctx(1));
  QCHECK(g_destroyCalls == 1);  // a refused load's context must still be destroyable
  SetPool(0);
  QCHECK(hLoad(nullptr, Ctx(9), nullptr) == 1);  // an empty pool
  PoolCell(0);  // no pool pointer yet
  QCHECK(hLoad(nullptr, Ctx(9), nullptr) == 7);
  PoolCell(0x30);  // unreadable
  QCHECK(hLoad(nullptr, Ctx(9), nullptr) == 7);
  SetPool(0);
  std::memset(g_pool + 0x84, 0, 4);  // capacity 0, free 0: used 0, still low
  // refusal lines: the first 16, then every 50th
  combat_test::Captured().clear();
  SetPool(10);
  const unsigned baseRefused = cc::CurrentStats().loadsRefused;
  for (int i = 0; i < 120; ++i) hLoad(nullptr, Ctx(9), nullptr);
  int want = 0;
  for (unsigned n = baseRefused + 1; n <= baseRefused + 120; ++n) {
    if (n <= 16 || n % 50 == 0) ++want;
  }
  QCHECK(CountLines("combat_custctx_load_refused") == want);
  End();
}

void TheLoadsLineIsWrittenEvery32ndLoad() {
  Installed();
  SetPool(100);
  for (int i = 0; i < 33 * 32; ++i) hLoad(nullptr, Ctx(1), nullptr);
  QCHECK(CountLines("combat_custctx_loads") == 32);  // every 32nd load, at most 32
  const std::vector<Line> loads = Lines("combat_custctx_loads");
  QCHECK(!loads.empty() && loads[0].level == sentinel::LogLevel::kInfo && Has(loads[0].text, "\"loads\":32,\"layouts_used\":412,\"layouts_capacity\":512"));
  End();
}

// R33: Grow.
void GrowResizesWhenThereIsNoRoomAndChecksIt() {
  Installed();
  std::uint8_t* buf = static_cast<std::uint8_t*>(std::calloc(1, 8 * kDraw));
  // room left and data set: the real Grow runs on the same table, no resize, nothing rewritten
  g_tbl.data = buf;
  g_tbl.size = 8 * kDraw;
  g_tbl.step = 4;
  g_tbl.capacity = 8;
  g_tbl.count = 3;
  void* r = hGrow(&g_tbl);
  QCHECK(g_resizeCalls == 0 && g_growCalls == 1 && g_growSeenPtr == &g_tbl && !g_unsafeGrow);
  QCHECK(r == buf + 3 * kDraw && g_tbl.count == 4 && g_tbl.capacity == 8 && g_tbl.data == buf);
  QCHECK(cc::CurrentStats().growFailures == 0);

  // no room (count == capacity): the resize is made here, with max(capacity, step), keep 1, checked, capacity raised
  g_tbl.count = 8;
  g_tbl.step = 4;
  r = hGrow(&g_tbl);
  QCHECK(g_resizeCalls == 1 && g_rsBlock == &g_tbl && g_rsBytes == (8 + 8) * kDraw && g_rsKeep == 1);
  QCHECK(g_tbl.capacity == 16 && g_tbl.count == 9 && g_growSeen.capacity == 16 && !g_unsafeGrow && r != nullptr);
  // a step larger than the capacity
  std::free(g_tbl.data);
  g_tbl.data = static_cast<std::uint8_t*>(std::calloc(1, 4 * kDraw));
  g_tbl.step = 20;
  g_tbl.capacity = 4;
  g_tbl.count = 4;
  g_resizeCalls = 0;
  hGrow(&g_tbl);
  QCHECK(g_rsBytes == (4 + 20) * kDraw && g_tbl.capacity == 24 && !g_unsafeGrow);
  QCHECK(cc::CurrentStats().growFailures == 0);

  // Resize fails (data NULL afterwards, 8 elements in use): an empty consistent table, an element that is in no table, no crash
  std::free(g_tbl.data);
  g_tbl.data = static_cast<std::uint8_t*>(std::calloc(1, 8 * kDraw));
  g_tbl.size = 8 * kDraw;
  g_tbl.step = 32;
  g_tbl.capacity = 8;
  g_tbl.count = 8;
  g_resizeOk = false;
  combat_test::Captured().clear();
  g_growCalls = 0;
  std::uint8_t* const oldData = g_tbl.data;
  r = hGrow(&g_tbl);
  QCHECK(g_tbl.data == nullptr && g_tbl.size == 0 && g_tbl.capacity == 0 && g_tbl.count == 0);
  QCHECK(g_tbl.allocator == kAlloc && g_tbl.step == 32);
  QCHECK(r != nullptr && g_growCalls == 1 && !g_unsafeGrow);
  QCHECK(g_growSeenPtr != &g_tbl && g_growSeen.data != nullptr && g_growSeen.data != oldData && g_growSeen.size == kDraw &&
         g_growSeen.allocator == kAlloc && g_growSeen.step == 1 && g_growSeen.capacity == 1 && g_growSeen.count == 0);
  QCHECK(r == g_growSeen.data);  // the element is the spare slot
  QCHECK(cc::CurrentStats().growFailures == 1);
  const std::vector<Line> fail = Lines("combat_custctx_draw_failure");
  QCHECK(fail.size() == 1U && fail[0].level == sentinel::LogLevel::kWarn && Has(fail[0].text, "\"why\":\"Resize left the data NULL\"") &&
         Has(fail[0].text, "\"entries\":32,") && Has(fail[0].text, "\"in_use\":8,"));
  if (fail.size() == 1U) {
    QCHECK(combat_test::Golden(fail[0].text) ==
           "\"event\":\"combat_custctx_draw_failure\",\"entries\":32,\"bytes\":" + std::to_string((8 + 32) * kDraw) +
               ",\"in_use\":8,\"why\":\"Resize left the data NULL\",\"failure\":1,\"consequence\":\"no memory for cosmetic draw entries: this "
               "player's cosmetics are skipped\"}");
  }
  std::free(oldData);

  // step and capacity 0: nothing to resize
  g_resizeCalls = 0;
  combat_test::Captured().clear();
  std::memset(&g_tbl, 0, sizeof(g_tbl));
  g_tbl.allocator = kAlloc;
  g_resizeOk = true;
  r = hGrow(&g_tbl);
  QCHECK(g_resizeCalls == 0 && r != nullptr && !g_unsafeGrow && CountLines("combat_custctx_draw_failure", "\"why\":\"step and capacity 0\"") == 1);

  // step and capacity 0 on a table that still has a data block: emptied, the block is leaked (never freed by the guard)
  std::uint8_t* leaked = static_cast<std::uint8_t*>(std::calloc(1, kDraw));
  std::memset(&g_tbl, 0, sizeof(g_tbl));
  g_tbl.allocator = kAlloc;
  g_tbl.data = leaked;
  g_tbl.size = kDraw;
  r = hGrow(&g_tbl);
  QCHECK(g_tbl.data == nullptr && g_tbl.size == 0 && r != nullptr && !g_unsafeGrow);
  std::free(leaked);

  // room left but the data is NULL (a torn-down table): the memset(NULL) of the original crash
  combat_test::Captured().clear();
  std::memset(&g_tbl, 0, sizeof(g_tbl));
  g_tbl.allocator = kAlloc;
  g_tbl.step = 4;
  g_tbl.capacity = 32;
  g_tbl.count = 0;
  r = hGrow(&g_tbl);
  QCHECK(r != nullptr && !g_unsafeGrow && g_tbl.capacity == 0 && g_tbl.count == 0 && g_tbl.data == nullptr && g_resizeCalls == 0);
  QCHECK(CountLines("combat_custctx_draw_failure", "\"why\":\"room left but the data is NULL: a torn-down table\"") == 1);
  QCHECK(CountLines("combat_custctx_draw_failure", "\"entries\":0,\"bytes\":0,\"in_use\":0,") == 1);

  // an unreadable table: the real function decides
  g_growUnreadable = 0;
  hGrow(reinterpret_cast<DrawTable*>(0x20));
  QCHECK(g_growUnreadable == 1 && g_resizeCalls == 0);
  End();
}

// Spares: when the allocation fails, the install-time spares are used round robin, never NULL.
void WhenTheAllocationFailsTheInstallTimeSparesAreUsed() {
  Installed();
  cc::SetMemalignForTest(&FailingMemalign);
  std::vector<void*> got;
  for (int i = 0; i < 5; ++i) {
    std::memset(&g_tbl, 0, sizeof(g_tbl));
    g_tbl.allocator = kAlloc;
    g_tbl.step = 4;
    g_tbl.capacity = 32;  // torn down
    g_unsafeGrow = false;
    void* r = hGrow(&g_tbl);
    QCHECK(r != nullptr && !g_unsafeGrow);
    got.push_back(r);
  }
  QCHECK(got[0] != got[1] && got[1] != got[2] && got[2] != got[3] && got[0] != got[2] && got[0] != got[3] && got[1] != got[3]);
  QCHECK(got[4] == got[0]);  // round robin
  // log caps: 16 lines, a total at every 50th failure, at most 16 totals
  combat_test::Captured().clear();
  for (int i = 0; i < 45; ++i) {  // 5 so far: 50 in all after this loop's last call
    std::memset(&g_tbl, 0, sizeof(g_tbl));
    g_tbl.capacity = 32;
    hGrow(&g_tbl);
  }
  QCHECK(CountLines("combat_custctx_draw_failure") == 11 && CountLines("combat_custctx_draw_failures_total", "\"failures\":50") == 1);
  for (int i = 0; i < 850; ++i) {
    std::memset(&g_tbl, 0, sizeof(g_tbl));
    g_tbl.capacity = 32;
    hGrow(&g_tbl);
  }
  QCHECK(CountLines("combat_custctx_draw_failures_total") == 16);
  cc::SetMemalignForTest(nullptr);
  End();
}

// A working allocator: a fresh element each time, in no table.
void AWorkingAllocatorGivesAFreshElementEachTime() {
  Installed();
  cc::SetMemalignForTest(&RecordingMemalign);
  std::vector<void*> seen;
  for (int i = 0; i < 8; ++i) {
    std::memset(&g_tbl, 0, sizeof(g_tbl));
    g_tbl.capacity = 32;
    seen.push_back(hGrow(&g_tbl));
  }
  QCHECK(seen[0] != nullptr && seen[1] != nullptr);
  for (std::size_t i = 0; i < seen.size(); ++i) {
    for (std::size_t j = i + 1; j < seen.size(); ++j) QCHECK(seen[i] != seen[j]);
  }
  QCHECK(g_lastAlign == 64 && g_lastSize == kDraw);  // one element, 64-aligned
  cc::SetMemalignForTest(nullptr);
  End();
}

// Install: all or none. A foreign word in any one slot refuses the module and leaves the other two as they were.
void AForeignWordInAnySlotRefusesAllThreeAndNothingStaysSwapped() {
  struct Case {
    const char* which;
    std::uintptr_t load, destroy, grow;
  };
  const std::uintptr_t fl = reinterpret_cast<std::uintptr_t>(&FakeLoad), fd = reinterpret_cast<std::uintptr_t>(&FakeDestroy),
                       fg = reinterpret_cast<std::uintptr_t>(&FakeGrow), other = reinterpret_cast<std::uintptr_t>(&FakeOtherCode);
  for (const std::uintptr_t foreign : {static_cast<std::uintptr_t>(0x4242420), other}) {
    const Case cases[] = {{"LoadMeshData", foreign, fd, fg}, {"DestroyContext", fl, foreign, fg}, {"Grow", fl, fd, foreign}};
    for (const Case& c : cases) {
      combat_test::StartCapture();
      cc::ResetForTest();
      ResetWorld();
      Build(c.load, c.destroy, c.grow);
      const ModuleResult r = InstallOn();
      QCHECK(!r.installed && std::strcmp(r.status, "original_mismatch") == 0);
      // nothing stays swapped: every slot holds the word it held, and no thunk is left armed (an entry called directly passes straight through)
      QCHECK(SlotWord(g_pins.slotLoad) == c.load && SlotWord(g_pins.slotDestroy) == c.destroy && SlotWord(g_pins.slotGrow) == c.grow);
      g_loadRet = 7;
      SetPool(10);
      if (cc::LoadThunk::Original() != nullptr) {
        reinterpret_cast<LoadF>(cc::LoadThunk::EntryAddress())(nullptr, Ctx(1), nullptr);
        QCHECK(g_loadCalls == 1);  // the pool is nearly full, yet the entry went straight to the original: it is disarmed
        g_loadCalls = 0;
      }
      QCHECK(cc::CurrentStats().loads == 0 && cc::CurrentStats().loadsRefused == 0);
      const std::vector<Line> install = Lines("combat_custctx_install");
      QCHECK(install.size() == 1U);
      if (install.size() == 1U) {
        QCHECK(install[0].level == sentinel::LogLevel::kError);
        QCHECK(Has(install[0].text, "\"result\":\"refused\"") && Has(install[0].text, "\"reason\":\"slot_refused\""));
        QCHECK(Has(install[0].text, std::string("\"which\":\"") + c.which + "\"") && Has(install[0].text, "\"hook\":\"original_mismatch\""));
      }
      cc::ResetForTest();
      g_image.Drop();
      combat_test::StopCapture();
    }
  }
}

// The rollback: the hooks before a refused one are removed with the manager's Remove, and the line says so.
void TheHooksBeforeARefusedOneAreRemovedAndSaidSo() {
  Begin();
  Build(0, 0, 0x4242420);  // Grow refused: LoadMeshData and DestroyContext were in and are removed
  QCHECK(!InstallOn().installed);
  QCHECK(SlotWord(g_pins.slotLoad) == reinterpret_cast<std::uintptr_t>(&FakeLoad) && SlotWord(g_pins.slotDestroy) == reinterpret_cast<std::uintptr_t>(&FakeDestroy));
  std::vector<Line> install = Lines("combat_custctx_install");
  QCHECK(install.size() == 1U && Has(install[0].text, "\"which\":\"Grow\"") && Has(install[0].text, "\"rollback\":\"ok\""));
  End();
  Begin();
  Build(0x4242420, 0, 0);  // the first refused: nothing to remove
  QCHECK(!InstallOn().installed);
  install = Lines("combat_custctx_install");
  QCHECK(install.size() == 1U && Has(install[0].text, "\"which\":\"LoadMeshData\"") && Has(install[0].text, "\"rollback\":\"none\""));
  End();
  // a slot that is installed afterwards works: nothing was left poisoned or held
  Begin();
  Build(0, 0, 0x4242420);
  QCHECK(!InstallOn().installed);
  *g_image.Slot(g_pins.slotGrow) = reinterpret_cast<void*>(&FakeGrow);
  QCHECK(InstallOn().installed);
  End();
}

// A spare that cannot be allocated: nothing swapped, Error.
void ASpareThatCannotBeAllocatedInstallsNothing() {
  Begin();
  g_memalignCalls = 0;
  cc::SetMemalignForTest(&FailThird);
  const ModuleResult r = InstallOn();
  cc::SetMemalignForTest(nullptr);
  QCHECK(!r.installed && std::strcmp(r.status, "spare_alloc_failed") == 0 && SlotsAreTheGames());
  const std::vector<Line> install = Lines("combat_custctx_install");
  QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kError && Has(install[0].text, "\"reason\":\"spare_alloc_failed\""));
  End();
}

// No Resize export: refused, nothing swapped; the production lookup on a host without the game library says so too.
void ANeverFoundResizeInstallsNothing() {
  Begin();
  const cc::Fns none{};
  const ModuleResult r = cc::InstallAt(&Lookup, g_pins, &none, true);
  QCHECK(!r.installed && std::strcmp(r.status, "export_missing") == 0 && SlotsAreTheGames());
  std::vector<Line> install = Lines("combat_custctx_install");
  QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kError && Has(install[0].text, "\"reason\":\"export_missing\""));
  End();
  Begin();
  const ModuleResult p = cc::InstallAt(&Lookup, g_pins, nullptr, true);  // the pins pass, then dlsym finds no libr15
  QCHECK(!p.installed && std::strcmp(p.status, "export_missing") == 0 && SlotsAreTheGames());
  QCHECK(Lines("combat_custctx_install").size() == 1U);
  End();
}

// The install's own lookup of the Resize export: the symbol is asked by name, and only the pinned address is accepted.
const void* g_symbolAnswer = nullptr;
const char* g_symbolAsked = nullptr;
const void* SymbolLookup(const char* symbol) {
  g_symbolAsked = symbol;
  return g_symbolAnswer;
}
void TheInstallsOwnLookupOfResizeAcceptsOnlyThePinnedAddress() {
  Begin();
  cc::SetSymbolLookupForTest(&SymbolLookup);
  g_symbolAnswer = reinterpret_cast<const void*>(g_image.Base() + g_pins.fnResize + 4);  // another function: refused
  QCHECK(std::strcmp(cc::InstallAt(&Lookup, g_pins, nullptr, true).status, "export_missing") == 0);
  QCHECK(g_symbolAsked != nullptr && std::strcmp(g_symbolAsked, cc::kResizeSymbol) == 0 && SlotsAreTheGames());
  g_symbolAnswer = nullptr;  // not exported
  QCHECK(std::strcmp(cc::InstallAt(&Lookup, g_pins, nullptr, true).status, "export_missing") == 0 && SlotsAreTheGames());
  g_symbolAnswer = reinterpret_cast<const void*>(g_image.Base() + g_pins.fnResize);  // exactly the pinned address: installed
  QCHECK(cc::InstallAt(&Lookup, g_pins, nullptr, true).installed && !SlotsAreTheGames());
  End();
}

// The address check on what dlsym would return.
void ResizeIsAcceptedOnlyAtItsPinnedAddress() {
  cc::Fns got{};
  const std::uintptr_t b = 0x7000000000ULL;
  const cc::Pins& pin = cc::kPins;
  QCHECK(cc::ResizeMatches(b, pin, reinterpret_cast<void*>(b + pin.fnResize), &got) &&
         got.resize == reinterpret_cast<void (*)(void*, std::uint64_t, std::uint32_t)>(b + pin.fnResize));
  cc::Fns other{};
  QCHECK(!cc::ResizeMatches(b, pin, reinterpret_cast<void*>(b + pin.fnResize + 4), &other) && other.resize == nullptr);
  QCHECK(!cc::ResizeMatches(b, pin, nullptr, &other));
}

// The plugin's kill file: a Warning that names the key; the module is installed all the same.
void TheLegacySwitchFileIsOnlyToldAbout() {
  char tmpl[] = "/tmp/custctx-legacy-XXXXXX";
  QCHECK(mkdtemp(tmpl) != nullptr);
  const std::string dir = tmpl;
  const std::string file = dir + "/custctx.off";
  Begin();
  ModuleResult r = InstallOn(true, dir.c_str());  // no file: nothing said
  QCHECK(r.installed && Lines("combat_custctx_legacy_switch").empty());
  End();
  std::FILE* f = std::fopen(file.c_str(), "w");
  QCHECK(f != nullptr);
  if (f != nullptr) std::fclose(f);
  Begin();
  r = InstallOn(true, dir.c_str());
  QCHECK(r.installed && !SlotsAreTheGames());  // the file turns nothing off
  const std::vector<Line> legacy = Lines("combat_custctx_legacy_switch");
  QCHECK(legacy.size() == 1U && legacy[0].level == sentinel::LogLevel::kWarn);
  if (legacy.size() == 1U) QCHECK(Has(legacy[0].text, "\"key\":\"combat.custctx\"") && Has(legacy[0].text, "\"file\":\"custctx.off\""));
  End();
  Begin();
  r = InstallOn(false, dir.c_str());  // off by the key, with the file there: still told, still off
  QCHECK(!r.installed && r.off && Lines("combat_custctx_legacy_switch").size() == 1U);
  End();
  unlink(file.c_str());
  rmdir(dir.c_str());
}

void TheSwitchOffInstallsNothingAndSaysSo() {
  Begin();
  const ModuleResult r = InstallOn(false);
  QCHECK(!r.installed && r.off && std::strcmp(r.status, "off_config") == 0 && SlotsAreTheGames());
  const std::vector<Line> install = Lines("combat_custctx_install");
  QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kInfo);
  if (install.size() == 1U) {
    QCHECK(combat_test::Golden(install[0].text) ==
           "\"event\":\"combat_custctx_install\",\"result\":\"off\",\"reason\":\"off_config\",\"consequence\":\"combat.custctx is false: the "
           "cosmetics renderer runs as the game wrote it\"}");
  }
  End();
}

// The image: not loaded, no build ID, another build.
void AMissingOrOtherImageInstallsNothing() {
  Begin();
  const cc::Fns fns = Fns();
  QCHECK(std::strcmp(cc::InstallAt(nullptr, g_pins, &fns, true).status, "module_not_loaded") == 0);
  g_image.Drop();
  QCHECK(std::strcmp(InstallOn().status, "module_not_loaded") == 0);
  Build();
  g_image.Build({}, pinned::kLibR15StockBuildId);
  QCHECK(std::strcmp(InstallOn().status, "build_id_mismatch") == 0);
  g_image.Build({}, nullptr, /*note=*/false);
  QCHECK(std::strcmp(InstallOn().status, "no_build_id") == 0);
  const std::vector<Line> lines = Lines("combat_custctx_install");
  QCHECK(lines.size() == 4U);
  for (const Line& l : lines) QCHECK(l.level == sentinel::LogLevel::kError && Has(l.text, "\"result\":\"refused\""));
  End();
}

// The pinned words: every word flipped in turn (and an unreadable one) refuses the install before any slot is touched.
void EveryPinnedWordGatesTheInstall() {
  Begin();
  combat_test::EveryPinIsRefused(
      "combat_custctx_install", cc::kWords, cc::kWordCount, g_image,
      [] {
        combat_test::StartCapture();
        cc::ResetForTest();
        Build();
      },
      [] { return InstallOn(); }, [] { return SlotsAreTheGames(); });
  End();
}

// A second install: the slots already hold the hooks, nothing is installed again.
void ASecondInstallDoesNothing() {
  Installed();
  const ModuleResult again = InstallOn();
  QCHECK(!again.installed && std::strcmp(again.status, "already_installed") == 0);
  const std::vector<Line> lines = Lines("combat_custctx_install");
  QCHECK(lines.size() == 2U && lines[1].level == sentinel::LogLevel::kError && Has(lines[1].text, "\"reason\":\"already_installed\""));
  QCHECK(SlotWord(g_pins.slotLoad) == reinterpret_cast<std::uintptr_t>(cc::LoadThunk::EntryAddress()));
  End();
}

// The stats: a Debug line every kStatsEvery loads, the Info line at the end of the process only when the guards are installed.
void TheStatsAreWrittenEveryKStatsEveryLoadsAndAtTheEnd() {
  Installed();
  g_loadRet = 0;
  for (unsigned i = 0; i < cc::kStatsEvery - 1; ++i) hLoad(nullptr, Ctx(1), nullptr);
  QCHECK(Lines("combat_custctx_stats").empty());
  hLoad(nullptr, Ctx(1), nullptr);
  std::vector<Line> stats = Lines("combat_custctx_stats");
  QCHECK(stats.size() == 1U && stats[0].level == sentinel::LogLevel::kDebug && Has(stats[0].text, "\"final\":0") &&
         Has(stats[0].text, "\"loads\":1024,\"loads_refused\":0,\"destroys_dropped\":0,\"grow_failures\":0"));
  hDestroy(nullptr, Ctx(77));  // one drop
  cc::NoteEnd();
  stats = Lines("combat_custctx_stats");
  QCHECK(stats.size() == 2U && stats[1].level == sentinel::LogLevel::kInfo && Has(stats[1].text, "\"final\":1") &&
         Has(stats[1].text, "\"loads\":1024,\"loads_refused\":0,\"destroys_dropped\":1,\"grow_failures\":0"));
  End();
  Begin();  // not installed: no final line
  cc::NoteEnd();
  QCHECK(Lines("combat_custctx_stats").empty());
  End();
}

}  // namespace

int main() {
  ThePinsAreTheReadelfAndDisassemblyPins();
  TheThunksTakeTheSlotsAndTheLineSaysSo();
  TheLiveContextTableDecidesWhichDestroysRun();
  FinishedContextsLeaveTheTable();
  MoreThan256LiveContextsGetTheGamesBehaviour();
  TheDropLinesAreCapped();
  ALoadOnANearlyFullLayoutPoolIsRefused();
  TheLoadsLineIsWrittenEvery32ndLoad();
  GrowResizesWhenThereIsNoRoomAndChecksIt();
  WhenTheAllocationFailsTheInstallTimeSparesAreUsed();
  AWorkingAllocatorGivesAFreshElementEachTime();
  AForeignWordInAnySlotRefusesAllThreeAndNothingStaysSwapped();
  TheHooksBeforeARefusedOneAreRemovedAndSaidSo();
  ASpareThatCannotBeAllocatedInstallsNothing();
  ANeverFoundResizeInstallsNothing();
  ResizeIsAcceptedOnlyAtItsPinnedAddress();
  TheInstallsOwnLookupOfResizeAcceptsOnlyThePinnedAddress();
  TheLegacySwitchFileIsOnlyToldAbout();
  TheSwitchOffInstallsNothingAndSaysSo();
  AMissingOrOtherImageInstallsNothing();
  EveryPinnedWordGatesTheInstall();
  ASecondInstallDoesNothing();
  TheStatsAreWrittenEveryKStatsEveryLoadsAndAtTheEnd();
  std::free(g_tbl.data);
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "custctx_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::puts("custctx_test: all checks passed");
  return 0;
}
