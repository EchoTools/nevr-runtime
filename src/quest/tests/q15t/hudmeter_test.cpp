// Host test of hudmeter (src/quest/plugins/q15t/src/hudmeter.cpp, row R57): the real install path (import_hook + got_swap, real mprotect and
// /proc/self/maps) against a fake game-library image (src/quest/tests/q15t/pbhud_image.h) whose JUMP_SLOT 0x36c6ce8 holds base + 0x2726eac.
#include <sys/mman.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nevr/plugin_interface.h"
#include "hudmeter.h"
#include "pbhud_image.h"
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
  if (std::getenv("PBHUD_LOG")) std::printf("LOG level=%u %s\n", level, t.c_str());  // PBHUD_LOG=1: every record as it is emitted
  g_lines.push_back({level, t});
}
static int Count(const char* needle, int level = -1) {
  int n = 0;
  for (const auto& l : g_lines)
    if (l.text.find(needle) != std::string::npos && (level < 0 || (int)l.level == level)) ++n;
  return n;
}

static const uintptr_t kSlot = 0x36c6ce8, kFn = 0x2726eac;
static const uint64_t kComp = 0xe55af932c88ec864ull, kItem = 0x9fd2e8cc0e46df3cull;

// the fake CSetCanvasElementAttributesNode::Enter
static int g_real_calls = 0;
static const void *g_real_node = nullptr, *g_real_data = nullptr;
static float g_real_size[2];
static bool g_fake_reads = true;  // false: the data argument is not dereferenced (unreadable-data cases)
static uint64_t FakeEnter(void* node, const void* data) {
  ++g_real_calls;
  g_real_node = node;
  g_real_data = data;
  if (g_fake_reads) std::memcpy(g_real_size, static_cast<const uint8_t*>(data) + 0x38, 8);
  return 0x1234;
}
using EnterFn = uint64_t (*)(void*, const void*);

struct alignas(16) Data {
  uint8_t b[0x80];
};
static Data MakeData(uint64_t comp, uint64_t item, int32_t size_on, float x, float y) {
  Data d;
  std::memset(d.b, 0xab, sizeof d.b);
  std::memcpy(d.b + 0x18, &comp, 8);
  std::memcpy(d.b + 0x20, &item, 8);
  std::memcpy(d.b + 0x34, &size_on, 4);
  std::memcpy(d.b + 0x38, &x, 4);
  std::memcpy(d.b + 0x3c, &y, 4);
  return d;
}
static float SizeX(const Data& d) {
  float v;
  std::memcpy(&v, d.b + 0x38, 4);
  return v;
}
static float SizeY(const Data& d) {
  float v;
  std::memcpy(&v, d.b + 0x3c, 4);
  return v;
}

static void Fresh(pbhud::Image& img) {
  g_lines.clear();
  g_real_calls = 0;
  g_real_node = g_real_data = nullptr;
  q15t::ResetHudmeterForTest();
  img.Real(kFn, reinterpret_cast<void*>(&FakeEnter));
  img.Slot(kSlot, kFn);
}

int main() {
  NvrGameContext ctx{};
  ctx.ctx_size = sizeof ctx;
  ctx.plugin_id = "q15t";
  ctx.log = &FakeLog;
  q15t::SetLogContext(&ctx);

  pbhud::Image img;
  Fresh(img);
  void* node = reinterpret_cast<void*>(0x5000);

  // 1. install: the slot holds the wrapper, one INFO line
  const uintptr_t stock = img.base + kFn;
  const q15t::InstallResult ir = q15t::InstallHudmeter(img.base);
  CHECK(ir.status == q15t::SwapStatus::kOk, "install: %s", q15t::StatusName(ir.status));
  CHECK(img.Get(kSlot) != stock && img.Get(kSlot) != 0, "slot not swapped");
  CHECK(Count("hudmeter: on", 1) == 1, "no INFO install line");
  const EnterFn enter = reinterpret_cast<EnterFn>(img.Get(kSlot));

  // 2. a 0 x 0 write to weapon_meter on the lobby helmet's canvas: the real Enter sees 86 x 88, the node's value is put back
  Data d = MakeData(kComp, kItem, 1, 0.0f, 0.0f);
  uint64_t r = enter(node, &d);
  CHECK(g_real_calls == 1 && r == 0x1234, "real not called once / result not passed: %d %llx", g_real_calls, (unsigned long long)r);
  CHECK(g_real_size[0] == 86.0f && g_real_size[1] == 88.0f, "real saw %g x %g", (double)g_real_size[0], (double)g_real_size[1]);
  CHECK(g_real_node == node && g_real_data == &d, "arguments changed");
  CHECK(SizeX(d) == 0.0f && SizeY(d) == 0.0f, "node's value not put back: %g x %g", (double)SizeX(d), (double)SizeY(d));
  CHECK(d.b[0x34] == 1 && d.b[0x40] == 0xab && d.b[0x20] == (kItem & 0xff), "other data bytes changed");
  CHECK(Count("hudmeter: full size written") == 1 && Count("n=1") == 1, "no full #1 line");
  CHECK(Count("another canvas component") == 0, "lobby helmet's canvas named as another");
  CHECK(q15t::GetHudmeterStats().full_writes == 1, "full_writes");

  // 3. the next size to the same item (the fill) is passed on untouched; the first one logs a pass line once
  d = MakeData(kComp, kItem, 1, 86.0f, 0.0f);
  r = enter(node, &d);
  CHECK(g_real_size[0] == 86.0f && g_real_size[1] == 0.0f && g_real_data == &d, "fill write changed");
  CHECK(Count("hudmeter: pass") == 1, "no pass line");
  for (float y : {10.0f, 10.0f, 20.0f, 30.0f}) {
    d = MakeData(kComp, kItem, 1, 86.0f, y);
    enter(node, &d);
    CHECK(g_real_size[1] == y, "fill height changed");
  }
  CHECK(Count("hudmeter: pass") == 1, "pass line repeated");
  CHECK(Count("hudmeter: fill") == 3, "fill lines for changes only: %d", Count("hudmeter: fill"));  // y 0 -> 10 -> 20 -> 30 (10 twice = one line)

  // the fill lines are capped at 8 for good (3 are out; heights that keep changing add 5 more and no more)
  g_lines.clear();
  for (int i = 0; i < 40; ++i) {
    d = MakeData(kComp, kItem, 1, 86.0f, 100.0f + static_cast<float>(i));
    enter(node, &d);
  }
  CHECK(Count("hudmeter: fill") == 5, "fill line cap: %d more lines (want 5)", Count("hudmeter: fill"));

  // 4. a later helmet start (0 x 0 again) is guarded again and a new pass line follows
  d = MakeData(kComp, kItem, 1, 0.0f, 0.0f);
  enter(node, &d);
  CHECK(g_real_size[0] == 86.0f && g_real_size[1] == 88.0f, "second start not guarded");
  d = MakeData(kComp, kItem, 1, 86.0f, 44.0f);
  enter(node, &d);
  CHECK(Count("hudmeter: pass") == 1, "no pass line after a second start (lines were cleared before it): %d", Count("hudmeter: pass"));

  // 5. full-write line thinning: 1st, 2nd, 4th ... (the 3rd above already counted: n so far 2)
  g_lines.clear();
  for (int i = 0; i < 6; ++i) {
    d = MakeData(kComp, kItem, 1, 0.0f, 0.0f);
    enter(node, &d);
  }
  CHECK(Count("hudmeter: full size written") == 2, "thinning: %d lines for n=3..8 (want n=4 and n=8)", Count("hudmeter: full size written"));

  // 6. any canvas component is guarded; the first other one is named once
  g_lines.clear();
  d = MakeData(0x1111, kItem, 1, 0.0f, 0.0f);
  enter(node, &d);
  CHECK(g_real_size[0] == 86.0f && g_real_size[1] == 88.0f, "other canvas component not guarded");
  CHECK(Count("another canvas component") == 1 && Count("comp=0x1111") >= 1, "other component not named");
  d = MakeData(0x2222, kItem, 1, 0.0f, 0.0f);
  enter(node, &d);
  CHECK(Count("another canvas component") == 1, "other component named twice");

  // 7. everything else passes untouched and is not logged: another item, size flag off, only one coordinate 0
  g_lines.clear();
  const unsigned guarded = q15t::GetHudmeterStats().calls_guarded;
  d = MakeData(kComp, kItem + 1, 1, 0.0f, 0.0f);
  enter(node, &d);
  CHECK(g_real_size[0] == 0.0f && g_real_size[1] == 0.0f && g_real_data == &d, "another item was changed");
  d = MakeData(kComp, kItem, 0, 0.0f, 0.0f);
  enter(node, &d);
  CHECK(g_real_size[0] == 0.0f && g_real_size[1] == 0.0f, "a call with the size flag off was changed");
  d = MakeData(kComp, kItem, 1, 0.0f, 5.0f);
  enter(node, &d);
  CHECK(g_real_size[0] == 0.0f && g_real_size[1] == 5.0f, "(0, 5) was changed");
  d = MakeData(kComp, kItem, 1, 7.0f, 0.0f);
  enter(node, &d);
  CHECK(g_real_size[0] == 7.0f && g_real_size[1] == 0.0f, "(7, 0) was changed");
  CHECK(g_lines.size() <= 2 && q15t::GetHudmeterStats().calls_guarded == guarded + 2, "stray lines / guard count for foreign calls");

  // 8. no per-call logging on the hot path
  g_lines.clear();
  d = MakeData(kComp, kItem + 7, 1, 1.0f, 2.0f);
  for (int i = 0; i < 2000; ++i) enter(node, &d);
  CHECK(g_lines.empty(), "per-call lines: %zu", g_lines.size());

  // 9. a data argument that cannot be read: the real function runs with the very same arguments, nothing is written or faults
  g_fake_reads = false;
  g_real_calls = 0;
  unsigned unread = q15t::GetHudmeterStats().unreadable;
  r = enter(node, nullptr);
  CHECK(g_real_calls == 1 && r == 0x1234 && g_real_data == nullptr && g_real_node == node, "null data: real not called with the same arguments");
  CHECK(q15t::GetHudmeterStats().unreadable == unread + 1, "unreadable data not counted");
  r = enter(node, reinterpret_cast<const void*>(0x1000));
  CHECK(g_real_calls == 2 && g_real_data == reinterpret_cast<const void*>(0x1000), "unmapped data: real not called with the same arguments");

  // 10. the data right at the end of a mapping: exactly 0x40 readable bytes is enough; one word less is passed on untouched
  uint8_t* two = static_cast<uint8_t*>(mmap(nullptr, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  mprotect(two + 4096, 4096, PROT_NONE);
  const Data src = MakeData(kComp, kItem, 1, 0.0f, 0.0f);
  uint8_t* edge = two + 4096 - 0x40;
  std::memcpy(edge, src.b, 0x40);
  g_real_calls = 0;
  unread = q15t::GetHudmeterStats().unreadable;
  g_fake_reads = true;
  enter(node, edge);
  CHECK(g_real_calls == 1 && g_real_size[0] == 86.0f && g_real_size[1] == 88.0f, "data ending exactly at 0x40 not guarded");
  CHECK(q15t::GetHudmeterStats().unreadable == unread, "readable edge counted as unreadable");
  uint8_t* short_edge = two + 4096 - 0x3c;
  std::memcpy(short_edge, src.b, 0x3c);
  g_fake_reads = false;
  enter(node, short_edge);
  CHECK(q15t::GetHudmeterStats().unreadable == unread + 1 && std::memcmp(short_edge, src.b, 0x3c) == 0, "short readable range not passed on untouched");
  g_fake_reads = true;
  munmap(two, 8192);

  // 11. a slot that does not hold the pinned function is refused, left alone, and logged as an ERROR
  Fresh(img);
  img.SetSlot(kSlot, 0x4242420);
  const q15t::InstallResult bad = q15t::InstallHudmeter(img.base);
  CHECK(bad.status == q15t::SwapStatus::kUnexpectedOriginal && img.Get(kSlot) == 0x4242420, "foreign slot word: %s", q15t::StatusName(bad.status));
  CHECK(Count("hudmeter: OFF", 3) == 1, "no ERROR line for a refused slot");

  if (g_fail == 0) std::printf("hudmeter_test: all checks passed\n");
  return g_fail == 0 ? 0 : 1;
}
