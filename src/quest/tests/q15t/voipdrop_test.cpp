// Host test of voipdrop (src/quest/plugins/q15t/src/voipdrop.cpp): the wrapper installed by the real installer on a fake JUMP_SLOT page and called
// through the slot, against a fake CR15NetVoipReceiverCS (free head, page size, allocator block) and a fake allocator with a
// vtable (+0x10 alloc, +0x38 free) behind fake CMemoryContext functions. Real SafeRead, real mprotect, real files.
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nevr/plugin_interface.h"
#include "pin_check.h"
#include "plugin_log.h"
#include "voipdrop.h"

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

// ---- the fake receiver and allocator ----
alignas(16) static uint8_t g_cs[0x200];
static void* const kSentinel = reinterpret_cast<void*>(0x7777);  // the memory context's current allocator before the probe
static void* g_cur = kSentinel;
static void* g_alloc_obj[2];  // [0] = vtable
static uintptr_t g_vt[10];
static char g_page[16];
static bool g_alloc_ok = true, g_no_allocator = false;
static int g_probe_allocs = 0, g_probe_frees = 0, g_set_calls = 0;
static uint32_t g_probe_bytes = 0;
static const void* g_block_seen = nullptr;
static void* g_freed = nullptr;

static void* FBlock(const void* block) {
  g_block_seen = block;
  return g_no_allocator ? nullptr : static_cast<void*>(g_alloc_obj);
}
static void* FCurPtr() { return g_cur; }
static void FSetCur(void* a) {
  g_cur = a;
  ++g_set_calls;
}
static void* FCur() { return g_cur; }
static void* FAlloc(void*, uint32_t bytes) {
  ++g_probe_allocs;
  g_probe_bytes = bytes;
  return g_alloc_ok ? static_cast<void*>(g_page) : nullptr;
}
static void FFree(void*, void* p) {
  ++g_probe_frees;
  g_freed = p;
}

static int g_real_calls = 0;
static void* g_r_self = nullptr;
static uint64_t g_r_handle = 0;
static const void* g_r_event = nullptr;
static const void* g_r_data = nullptr;
static uint32_t g_r_bytes = 0;
extern "C" __attribute__((noinline)) void RealRecv(void* self, uint64_t handle, const void* event, const void* data, uint32_t bytes) {
  ++g_real_calls;
  g_r_self = self;
  g_r_handle = handle;
  g_r_event = event;
  g_r_data = data;
  g_r_bytes = bytes;
}
extern "C" __attribute__((noinline)) void OtherRecv(void*, uint64_t, const void*, const void*, uint32_t) {}
using Fn = void (*)(void*, uint64_t, const void*, const void*, uint32_t);

static void World(int32_t free_head, uint32_t page_bytes) {
  std::memset(g_cs, 0, sizeof g_cs);
  std::memcpy(g_cs + 0x1b8, &free_head, sizeof free_head);
  std::memcpy(g_cs + 0x1cc, &page_bytes, sizeof page_bytes);
  g_alloc_obj[0] = g_vt;
  std::memset(g_vt, 0, sizeof g_vt);
  g_vt[0x10 / 8] = reinterpret_cast<uintptr_t>(&FAlloc);
  g_vt[0x38 / 8] = reinterpret_cast<uintptr_t>(&FFree);
  g_cur = kSentinel;
  g_alloc_ok = true;
  g_no_allocator = false;
  g_probe_allocs = g_probe_frees = g_set_calls = g_real_calls = 0;
  g_probe_bytes = 0;
  g_block_seen = nullptr;
  g_freed = nullptr;
}

static q15t::VoipDropFns Fns() {
  q15t::VoipDropFns f{};
  f.block_allocator = &FBlock;
  f.current_ptr = &FCurPtr;
  f.set_current = &FSetCur;
  f.current = &FCur;
  return f;
}

struct Slot {
  uintptr_t* w;
  uintptr_t base;
};
static Slot MakePage(uintptr_t word) {
  void* p = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) std::abort();
  auto* w = static_cast<uintptr_t*>(p);
  w[4] = word;
  mprotect(p, 4096, PROT_READ);
  return {w, reinterpret_cast<uintptr_t>(p)};
}
static Fn Install() {
  q15t::ResetVoipDropForTest();
  Slot s = MakePage(reinterpret_cast<uintptr_t>(&RealRecv));
  g_lines.clear();
  const auto r = q15t::InstallVoipDropAt(s.base, 0x20, reinterpret_cast<uintptr_t>(&RealRecv) - s.base, Fns());
  CHECK(r.status == q15t::SwapStatus::kOk, "install: %s", q15t::StatusName(r.status));
  return reinterpret_cast<Fn>(s.w[4]);
}

int main() {
  NvrGameContext ctx{};
  ctx.ctx_size = sizeof ctx;
  ctx.plugin_id = "q15t";
  ctx.log = &FakeLog;
  q15t::SetLogContext(&ctx);
  char tmpl[] = "/tmp/q15t-voipdrop-XXXXXX";
  if (mkdtemp(tmpl) == nullptr) return 2;
  const std::string dir = tmpl;
  q15t::SetVoipDropDirForTest(dir.c_str());

  // the pinned addresses (R0_readelf_r.txt line 109636; disassembly 0x22c3238 - 0x100000)
  CHECK(q15t::kVoipDropSlot == 0x36c5d80 && q15t::kVoipDropReal == 0x21c3238, "pinned addresses changed");

  const Fn hook = Install();
  CHECK(CountLines("voipdrop on", 1) == 1, "no INFO install line");
  void* const ev = reinterpret_cast<void*>(0xe0e0);
  const void* const dat = reinterpret_cast<void*>(0xdada);

  // 1. the table has a free page (head != -1): the real function, no probe at all
  World(5, 0x80000);
  hook(g_cs, 0x1122334455ull, ev, dat, 321);
  CHECK(g_real_calls == 1 && g_probe_allocs == 0 && g_set_calls == 0, "free page: real %d probes %d sets %d", g_real_calls, g_probe_allocs, g_set_calls);
  CHECK(g_r_self == g_cs && g_r_handle == 0x1122334455ull && g_r_event == ev && g_r_data == dat && g_r_bytes == 321, "arguments changed");
  World(0, 0x80000);
  hook(g_cs, 1, ev, dat, 1);
  CHECK(g_real_calls == 1 && g_probe_allocs == 0, "head 0 is a valid page, not 'needs a page'");

  // 2. head -1 and the allocator can give the page: probed with the table's size and block, given back, context restored, real called
  World(-1, 0x80000);
  hook(g_cs, 7, ev, dat, 99);
  CHECK(g_probe_allocs == 1 && g_probe_bytes == 0x80000, "probe size %u (allocs %d)", g_probe_bytes, g_probe_allocs);
  CHECK(g_block_seen == g_cs + 0x180, "probe on the wrong allocator block %p (cs+0x180 = %p)", g_block_seen, (void*)(g_cs + 0x180));
  CHECK(g_probe_frees == 1 && g_freed == g_page, "probe block not given back");
  CHECK(g_cur == kSentinel, "memory context not restored");
  CHECK(g_real_calls == 1 && g_r_handle == 7 && g_r_event == ev && g_r_data == dat && g_r_bytes == 99, "real not called with the same arguments");
  CHECK(q15t::VoipDropCount() == 0, "counted a drop");

  // 3. head -1 and the allocator is out of memory: the packet is dropped, the real function is not called
  World(-1, 0x80000);
  g_alloc_ok = false;
  hook(g_cs, 7, ev, dat, 99);
  CHECK(g_real_calls == 0, "real called although the page cannot be allocated");
  CHECK(g_probe_frees == 0, "freed a block that was never given");
  CHECK(g_cur == kSentinel, "memory context not restored after a failed probe");
  CHECK(q15t::VoipDropCount() == 1, "drop count %u", q15t::VoipDropCount());
  CHECK(CountLines("voipdrop no memory for a voice table page", 1) == 1 && CountLines("page_bytes=524288 dropped=1") == 1, "no drop record");

  // 4. no allocator at all behind the block (Current() is null): dropped too, context restored
  World(-1, 0x80000);
  g_no_allocator = true;
  hook(g_cs, 7, ev, dat, 99);
  CHECK(g_real_calls == 0 && q15t::VoipDropCount() == 2 && g_cur == kSentinel, "null allocator: real %d drops %u", g_real_calls, q15t::VoipDropCount());

  // 5. doubt: self null or unreadable, vtable unreadable or without the two entries: the real function runs
  World(-1, 0x80000);
  hook(nullptr, 1, ev, dat, 1);
  hook(reinterpret_cast<void*>(0x20), 1, ev, dat, 1);
  CHECK(g_real_calls == 2 && g_probe_allocs == 0, "null/unreadable self: real %d", g_real_calls);
  World(-1, 0x80000);
  g_alloc_obj[0] = reinterpret_cast<void*>(0x30);  // vtable pointer unreadable
  g_alloc_ok = false;
  hook(g_cs, 1, ev, dat, 1);
  CHECK(g_real_calls == 1 && g_cur == kSentinel, "unreadable vtable: real %d", g_real_calls);
  World(-1, 0x80000);
  g_vt[0x10 / 8] = 0;
  hook(g_cs, 1, ev, dat, 1);
  CHECK(g_real_calls == 1 && g_cur == kSentinel, "no alloc entry: real %d", g_real_calls);
  World(-1, 0x80000);
  g_vt[0x38 / 8] = 0;
  hook(g_cs, 1, ev, dat, 1);
  CHECK(g_real_calls == 1 && g_probe_allocs == 0, "no free entry: probed anyway (could not give the block back)");

  // 5b. the free head reads -1 but the page size is unreadable (the receiver ends on a page boundary): doubt, real runs
  {
    auto* two = static_cast<uint8_t*>(mmap(nullptr, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    mprotect(two + 4096, 4096, PROT_NONE);
    uint8_t* edge = two + 4096 - 0x1bc;  // edge+0x1b8 is the last readable word; edge+0x1cc is in the PROT_NONE page
    World(-1, 0x80000);
    const int32_t minus1 = -1;
    std::memcpy(edge + 0x1b8, &minus1, sizeof minus1);
    g_alloc_ok = false;
    hook(edge, 1, ev, dat, 1);
    CHECK(g_real_calls == 1 && g_probe_allocs == 0 && q15t::VoipDropCount() == 2,
          "unreadable page size: real %d probes %d drops %u", g_real_calls, g_probe_allocs, q15t::VoipDropCount());
    munmap(two, 8192);
  }

  // 6. log caps: 16 lines, then a total at every 50th drop, at most 16 totals
  Fn h2 = Install();
  World(-1, 0x80000);
  g_alloc_ok = false;
  for (int i = 0; i < 49; ++i) h2(g_cs, 1, ev, dat, 1);
  CHECK(CountLines("no memory for a voice table page") == 16 && CountLines("dropped so far") == 0, "after 49 drops: %d lines, %d totals",
        CountLines("no memory for a voice table page"), CountLines("dropped so far"));
  h2(g_cs, 1, ev, dat, 1);
  CHECK(CountLines("dropped so far") == 1 && CountLines("dropped=50") == 1, "no total at the 50th drop");
  for (int i = 0; i < 850; ++i) h2(g_cs, 1, ev, dat, 1);
  CHECK(CountLines("dropped so far") == 16, "totals %d (cap 16)", CountLines("dropped so far"));
  CHECK(q15t::VoipDropCount() == 900 && g_real_calls == 0, "drops %u", q15t::VoipDropCount());

  // 7. a slot that does not hold the pinned function is refused and left alone, with an ERROR line
  g_lines.clear();
  Slot bad = MakePage(0x4242420);
  const auto rb = q15t::InstallVoipDropAt(bad.base, 0x20, reinterpret_cast<uintptr_t>(&RealRecv) - bad.base, Fns());
  CHECK(rb.status == q15t::SwapStatus::kUnexpectedOriginal && bad.w[4] == 0x4242420, "foreign word: %s", q15t::StatusName(rb.status));
  {
    char named[128];
    std::snprintf(named, sizeof named, "pin=0x20 found=0x4242420 expected=0x%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&RealRecv)));
    CHECK(CountLines("voipdrop refused", 3) == 1 && CountLines(named) == 1, "refusal not loud / does not name pin, found, expected");
  }

  // 7b. a word that points at real executable code but not at the pinned function is refused too (no by-name acceptance)
  Slot other = MakePage(reinterpret_cast<uintptr_t>(&OtherRecv));
  const auto rc = q15t::InstallVoipDropAt(other.base, 0x20, reinterpret_cast<uintptr_t>(&RealRecv) - other.base, Fns());
  CHECK(rc.status == q15t::SwapStatus::kUnexpectedOriginal && other.w[4] == reinterpret_cast<uintptr_t>(&OtherRecv),
        "other function's word accepted: %s", q15t::StatusName(rc.status));

  // 8. a missing engine function: refused, nothing swapped, ERROR line
  g_lines.clear();
  Slot noexp = MakePage(reinterpret_cast<uintptr_t>(&RealRecv));
  q15t::VoipDropFns f = Fns();
  f.set_current = nullptr;
  const auto rn = q15t::InstallVoipDropAt(noexp.base, 0x20, reinterpret_cast<uintptr_t>(&RealRecv) - noexp.base, f);
  CHECK(std::strcmp(rn.reason, "export_missing") == 0 && noexp.w[4] == reinterpret_cast<uintptr_t>(&RealRecv), "missing export installed");
  CHECK(CountLines("voipdrop refused", 3) == 1, "missing export not loud");
  // (no game library on the host: the resolving installer refuses the same way)
  g_lines.clear();
  pincheck::Text pt = pincheck::Make(q15t::kVoipDropWordPins, q15t::kVoipDropWordPinCount);  // the pins pass, then the exports are looked up
  const auto rr = q15t::InstallVoipDrop(pt.base());
  CHECK(std::strcmp(rr.reason, "export_missing") == 0 && CountLines("voipdrop refused", 3) == 1, "resolver on a host without libr15 not loud");
  pincheck::Free(&pt);

  // 9. voipdrop.off in the data directory: nothing swapped, one INFO line
  g_lines.clear();
  FILE* m = std::fopen((dir + "/voipdrop.off").c_str(), "w");
  std::fclose(m);
  Slot off = MakePage(reinterpret_cast<uintptr_t>(&RealRecv));
  const auto ro = q15t::InstallVoipDropAt(off.base, 0x20, reinterpret_cast<uintptr_t>(&RealRecv) - off.base, Fns());
  CHECK(std::strcmp(ro.reason, "off_file") == 0 && off.w[4] == reinterpret_cast<uintptr_t>(&RealRecv), "voipdrop.off not honoured");
  CHECK(CountLines("voipdrop off", 1) == 1, "no INFO off line");
  unlink((dir + "/voipdrop.off").c_str());

  rmdir(dir.c_str());
  // ---- code-word pins: every word read, a changed or unreadable one refused loudly, no file hash anywhere ----
  {
    auto count = [](const char* needle, int level) { return CountLines(needle, level); };
    auto clear = []() { g_lines.clear(); };
    g_fail += pincheck::Run("voipdrop", q15t::kVoipDropWordPins, q15t::kVoipDropWordPinCount, 10, 0xa2a0b8aa8d6d97a8ull,
                            [](std::uintptr_t b) { return q15t::VoipDropWordPinsMatch(b); }, count, clear);
    // the real installer refuses on a changed word, before anything else (reason word_pin_mismatch, one ERROR line, nothing swapped)
    pincheck::Text t = pincheck::Make(q15t::kVoipDropWordPins, q15t::kVoipDropWordPinCount);
    *t.word(q15t::kVoipDropWordPins[0].va) ^= 1u;
    g_lines.clear();
    const auto pr = q15t::InstallVoipDrop(t.base());
    CHECK(std::strcmp(pr.reason, "word_pin_mismatch") == 0 && CountLines("code word pin mismatch", 3) == 1, "installer ignored a changed word");
    CHECK(CountLines("voipdrop on", -1) == 0, "installer went on after a changed word");
    pincheck::Free(&t);
  }

  if (g_fail == 0) std::printf("voipdrop_test: all checks passed\n");
  return g_fail == 0 ? 0 : 1;
}
