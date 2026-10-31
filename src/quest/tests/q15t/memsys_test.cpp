// Host test of the `memsys` module (src/quest/plugins/q15t/src/memsys.cpp): the layout-pool wrapper on CSimplePool<SGRenderTargetLayout>::Alloc (R37)
// and the android_main timing witness on CSysJava::Init (R38). A fake game library (one anonymous mapping holding the pinned
// code words and the two JUMP_SLOTs, read-only like RELRO), a fake Alloc that builds the pool the way the real one does
// (+0x28 free stack, +0x30 count 0, +0x38 stack capacity, +0x84 capacity), and a fake InitCommon inline fill. The return
// address is the gate: a test cannot return into libr15, so the first call from each call-site function teaches the module
// that function's return address as "the site" (the module's test seam), and the other function is the wrong caller.
#include <dirent.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nevr/plugin_interface.h"
#include "got_swap.h"
#include "memsys.h"
#include "module_switch.h"
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

// ---- the fake engine ----
using AllocFn = uint64_t (*)(void*, uint32_t, void*);
using InitFn = uint64_t (*)(void*);
alignas(16) static uint8_t g_pool[0x100];
alignas(16) static uint32_t g_stack[0x400 + 8];
static int g_alloc_calls = 0, g_init_calls = 0;
static uint32_t g_alloc_cap_seen = 0;
static void* g_init_app_seen = nullptr;
static int g_mode = 0;  // 0 the real layout; 1 count not 0; 2 no stack; 3 capacity read back wrong; 4 stack capacity short; 5 stack unreadable
static const uint64_t kAllocResult = 0x5a5a, kInitResult = 0xa11ce;

extern "C" __attribute__((noinline)) uint64_t FakeAlloc(void* pool, uint32_t cap, void*) {
  ++g_alloc_calls;
  g_alloc_cap_seen = cap;
  auto* p = static_cast<uint8_t*>(pool);
  *reinterpret_cast<uint64_t*>(p + 0x28) = g_mode == 2 ? 0 : g_mode == 5 ? 0x1000 : reinterpret_cast<uint64_t>(g_stack);
  *reinterpret_cast<uint64_t*>(p + 0x30) = g_mode == 1 ? 7 : 0;
  *reinterpret_cast<uint64_t*>(p + 0x38) = g_mode == 4 ? 0x100 : cap;
  *reinterpret_cast<uint32_t*>(p + 0x84) = g_mode == 3 ? 0x200 : cap;
  return kAllocResult;
}
extern "C" __attribute__((noinline)) uint64_t FakeInit(void* app) {
  ++g_init_calls;
  g_init_app_seen = app;
  return kInitResult;
}

// InitCommon's inline fill (libr15 0x18818d8..0x1881914): push 511..0 at stack[count..], count += 512
static void InlineFill(void* pool) {
  auto* p = static_cast<uint8_t*>(pool);
  auto* stack = reinterpret_cast<uint32_t*>(*reinterpret_cast<uint64_t*>(p + 0x28));
  uint64_t& count = *reinterpret_cast<uint64_t*>(p + 0x30);
  for (uint32_t i = 0; i < 512; ++i) stack[count + i] = 511 - i;
  count += 512;
}

// two call sites in two functions: the return address tells them apart
__attribute__((noinline)) uint64_t SiteA(AllocFn f, void* pool, uint32_t cap) {
  uint64_t r = f(pool, cap, nullptr);
  asm volatile("" ::: "memory");
  return r;
}
__attribute__((noinline)) uint64_t SiteB(AllocFn f, void* pool, uint32_t cap) {
  uint64_t r = f(pool, cap, nullptr);
  asm volatile("" : "+r"(r)::"memory");
  return r;
}
__attribute__((noinline)) uint64_t InitSiteA(InitFn f, void* app) {
  uint64_t r = f(app);
  asm volatile("" ::: "memory");
  return r;
}
__attribute__((noinline)) uint64_t InitSiteB(InitFn f, void* app) {
  uint64_t r = f(app);
  asm volatile("" : "+r"(r)::"memory");
  return r;
}

static const size_t kLibSize = 0x3800000;
static uintptr_t g_base = 0;
static q15t::MemsysPins g_pins;

static void ResetPool() {
  std::memset(g_pool, 0, sizeof g_pool);
  std::memset(g_stack, 0xee, sizeof g_stack);
  g_alloc_calls = g_init_calls = 0;
  g_alloc_cap_seen = 0;
  g_init_app_seen = nullptr;
  g_mode = 0;
}

static void Protect(uintptr_t base, std::initializer_list<uintptr_t> addrs) {
  for (uintptr_t a : addrs) mprotect(reinterpret_cast<void*>((base + a) & ~uintptr_t(4095)), 4096, PROT_READ);
}
static void Poke32(uintptr_t addr, uint32_t v) {
  void* pg = reinterpret_cast<void*>(addr & ~uintptr_t(4095));
  mprotect(pg, 4096, PROT_READ | PROT_WRITE);
  *reinterpret_cast<uint32_t*>(addr) = v;
  mprotect(pg, 4096, PROT_READ);
}
static void Poke64(uintptr_t addr, uint64_t v) {
  void* pg = reinterpret_cast<void*>(addr & ~uintptr_t(4095));
  mprotect(pg, 4096, PROT_READ | PROT_WRITE);
  *reinterpret_cast<uint64_t*>(addr) = v;
  mprotect(pg, 4096, PROT_READ);
}

// A fake libr15: the pinned words, both slots holding the real functions, the System GLOB_DAT -> a 640 MiB u64.
static uintptr_t NewLib() {
  void* p = mmap(nullptr, kLibSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (p == MAP_FAILED) std::abort();
  const uintptr_t base = reinterpret_cast<uintptr_t>(p);
  g_base = base;
  int n = 0;
  const q15t::MemsysWord* w = q15t::MemsysWords(&n);
  for (int i = 0; i < n; ++i) *reinterpret_cast<uint32_t*>(base + w[i].vaddr) = w[i].word;
  g_pins = q15t::DefaultMemsysPins();
  g_pins.fn_alloc = reinterpret_cast<uintptr_t>(&FakeAlloc) - base;  // base + fn_alloc == &FakeAlloc
  g_pins.fn_init = reinterpret_cast<uintptr_t>(&FakeInit) - base;
  *reinterpret_cast<uintptr_t*>(base + g_pins.got_alloc) = reinterpret_cast<uintptr_t>(&FakeAlloc);
  *reinterpret_cast<uintptr_t*>(base + g_pins.got_init) = reinterpret_cast<uintptr_t>(&FakeInit);
  *reinterpret_cast<uint64_t*>(base + 0x2b9ca90) = 640ull << 20;
  *reinterpret_cast<uintptr_t*>(base + g_pins.got_sys) = base + 0x2b9ca90;
  std::initializer_list<uintptr_t> pages = {g_pins.got_alloc, g_pins.got_init, g_pins.got_sys, 0x2b9ca90};
  Protect(base, pages);
  for (int i = 0; i < n; ++i) Protect(base, {w[i].vaddr});
  q15t::ResetMemsysForTest();
  q15t::SetMemsysPinsForTest(g_pins);
  return base;
}
static void Drop(uintptr_t base) { munmap(reinterpret_cast<void*>(base), kLibSize); }
static uintptr_t SlotWord(uintptr_t addr) { return *reinterpret_cast<uintptr_t*>(g_base + addr); }

static std::string g_dir;
static void ClearDir() {
  DIR* d = opendir(g_dir.c_str());
  if (d == nullptr) return;
  while (dirent* e = readdir(d))
    if (e->d_name[0] != '.') unlink((g_dir + "/" + e->d_name).c_str());
  closedir(d);
}
static void Fresh() {
  g_lines.clear();
  ClearDir();
  ResetPool();
}

int main() {
  char tmpl[] = "/tmp/q15t-memsys-XXXXXX";
  if (mkdtemp(tmpl) == nullptr) return 2;
  g_dir = tmpl;
  q15t::SetModuleSwitchDirForTest(g_dir.c_str());
  NvrGameContext ctx{};
  ctx.ctx_size = sizeof ctx;
  ctx.plugin_id = "q15t";
  ctx.log = &FakeLog;
  q15t::SetLogContext(&ctx);

  // 0. the pins are the readelf / disassembly pins (independent copy: the source build's memsys and its readelf note, not in this tree)
  const q15t::MemsysPins d = q15t::DefaultMemsysPins();
  CHECK(d.got_alloc == 0x36e9b48 && d.fn_alloc == 0x1901b5c && d.site_ret == 0x18818cc, "R37 pins");
  CHECK(d.got_init == 0x36de5c0 && d.fn_init == 0xf7e8e8 && d.init_ret == 0xf784e8 && d.got_sys == 0x371d020, "R38 pins");
  static const struct {
    uintptr_t va;
    uint32_t word;
    int row;
  } kWant[21] = {{0x18818b8, 0x52804001, 37}, {0x18818c8, 0x97da8bfe, 37}, {0x18818cc, 0xa942ae6a, 37}, {0x18818d8, 0x52803f6c, 37},
                 {0x18818e0, 0x4f00c420, 37}, {0x18818e8, 0x91080169, 37}, {0x1881904, 0xf120011f, 37}, {0x1881914, 0xf9001a69, 37},
                 {0xf248c0, 0xb0013e30, 37},  {0xf248c4, 0xf945a611, 37},  {0x1901b5c, 0xd10143ff, 37}, {0x1901c24, 0xad0002c0, 37},
                 {0x1901c30, 0xf9001e75, 37}, {0x1901c4c, 0xf9001660, 37}, {0xf784e4, 0x97fe5633, 38},  {0xf784e8, 0x97fdb586, 38},
                 {0xf0ddb0, 0xb0013e90, 38},  {0xf0ddb4, 0xf942e211, 38},  {0xf78510, 0xb0013d28, 38},  {0xf78514, 0xf9401108, 38},
                 {0xf78524, 0xf9400101, 38}};
  int nw = 0;
  const q15t::MemsysWord* words = q15t::MemsysWords(&nw);
  CHECK(nw == 21, "word count %d", nw);
  for (int i = 0; i < nw && i < 21; ++i)
    CHECK(words[i].vaddr == kWant[i].va && words[i].word == kWant[i].word && words[i].row == kWant[i].row, "word %d differs", i);

  // 1. install, then learn the call sites
  Fresh();
  uintptr_t base = NewLib();
  q15t::InstallResult r = q15t::InstallMemsys(base);
  CHECK(r.status == q15t::SwapStatus::kOk, "install: %s %s", q15t::StatusName(r.status), r.reason);
  CHECK(SlotWord(g_pins.got_alloc) != reinterpret_cast<uintptr_t>(&FakeAlloc), "Alloc slot not swapped");
  CHECK(SlotWord(g_pins.got_init) != reinterpret_cast<uintptr_t>(&FakeInit), "Init slot not swapped");
  CHECK(Count("memsys hook install", 1) == 2 && Count("row=R37") >= 1 && Count("row=R38") >= 1, "want 2 INFO install lines");
  auto alloc = reinterpret_cast<AllocFn>(SlotWord(g_pins.got_alloc));
  auto init = reinterpret_cast<InitFn>(SlotWord(g_pins.got_init));

  // a call from some other place before the site is known: the real function sees the stock request, nothing is pre-pushed
  ResetPool();
  CHECK(SiteA(alloc, g_pool, 0x200) == kAllocResult && g_alloc_cap_seen == 0x200, "pass-through result or capacity (%u)", g_alloc_cap_seen);
  uintptr_t ret_a = 0, ret_init_a = 0;
  q15t::GetMemsysLastReturnsForTest(&ret_a, &ret_init_a);
  CHECK(ret_a != 0, "no return address learnt");
  // learn B's return address too
  SiteB(alloc, g_pool, 0x200);
  uintptr_t ret_b = 0;
  q15t::GetMemsysLastReturnsForTest(&ret_b, nullptr);
  CHECK(ret_b != 0 && ret_b != ret_a, "two call sites share a return address");
  InitSiteA(init, nullptr);
  q15t::GetMemsysLastReturnsForTest(nullptr, &ret_init_a);
  InitSiteB(init, nullptr);
  uintptr_t ret_init_b = 0;
  q15t::GetMemsysLastReturnsForTest(nullptr, &ret_init_b);
  CHECK(ret_init_a != 0 && ret_init_b != ret_init_a, "init call sites");
  CHECK(q15t::GetMemsysStats().init_witnessed == 0 && Count("android_main reached") == 0, "witnessed before the site was set");

  // 2. the site call with the stock capacity: Alloc is asked for 0x400, the free stack holds 1023..512 below the inline fill
  q15t::SetMemsysSitesForTest(ret_a, ret_init_a);
  ResetPool();
  g_lines.clear();
  CHECK(SiteA(alloc, g_pool, 0x200) == kAllocResult, "site call result");
  CHECK(g_alloc_cap_seen == 0x400 && g_alloc_calls == 1, "real Alloc asked for %u (%d calls)", g_alloc_cap_seen, g_alloc_calls);
  CHECK(*reinterpret_cast<uint64_t*>(g_pool + 0x30) == 512, "pre-pushed count %llu", (unsigned long long)*reinterpret_cast<uint64_t*>(g_pool + 0x30));
  InlineFill(g_pool);
  CHECK(*reinterpret_cast<uint64_t*>(g_pool + 0x30) == 1024, "free count after the inline fill");
  bool seen[1024] = {};
  bool unique = true;
  for (uint32_t i = 0; i < 1024; ++i) {
    if (g_stack[i] >= 1024 || seen[g_stack[i]]) unique = false;
    else seen[g_stack[i]] = true;
  }
  CHECK(unique, "the 1024 free indices are not 0..1023 exactly once");
  CHECK(g_stack[0] == 1023 && g_stack[511] == 512 && g_stack[512] == 511 && g_stack[1023] == 0, "free stack order %u %u %u %u",
        g_stack[0], g_stack[511], g_stack[512], g_stack[1023]);
  CHECK(Count("layout pool raised", 1) == 1 && Count("slots=1024") == 1 && Count("pre_pushed=512") == 1, "no raised line");
  CHECK(q15t::GetMemsysStats().pool_raised == 1, "pool_raised counter");

  // 3. the same call from another function: not the site, the real function gets the stock request untouched
  ResetPool();
  CHECK(SiteB(alloc, g_pool, 0x200) == kAllocResult && g_alloc_cap_seen == 0x200, "other caller got capacity %u", g_alloc_cap_seen);
  CHECK(*reinterpret_cast<uint64_t*>(g_pool + 0x30) == 0 && g_stack[0] == 0xeeeeeeeeu, "other caller's pool was touched");

  // 4. the site, but not the stock capacity (another pool made there): untouched
  ResetPool();
  CHECK(SiteA(alloc, g_pool, 0x100) == kAllocResult && g_alloc_cap_seen == 0x100, "capacity 0x100 changed to %u", g_alloc_cap_seen);
  CHECK(*reinterpret_cast<uint64_t*>(g_pool + 0x30) == 0 && g_stack[0] == 0xeeeeeeeeu, "capacity 0x100 pool was touched");

  // 5. the site, but a layout the module does not know: Alloc ran, nothing is pushed, one WARNING (once)
  for (int mode = 1; mode <= 5; ++mode) {
    ResetPool();
    g_lines.clear();
    q15t::ResetMemsysForTest();  // the once-only line and counters; the pins and sites again below
    q15t::SetMemsysPinsForTest(g_pins);
    base = g_base;
    // the hooks stay installed; the module state was reset only for its flags
    q15t::SetMemsysSitesForTest(ret_a, ret_init_a);
    g_mode = mode;
    // the real pointer was cleared by the reset: reinstall over a fresh lib
    Drop(base);
    base = NewLib();
    q15t::InstallMemsys(base);
    alloc = reinterpret_cast<AllocFn>(SlotWord(g_pins.got_alloc));
    q15t::SetMemsysSitesForTest(ret_a, ret_init_a);
    g_lines.clear();
    ResetPool();
    g_mode = mode;
    SiteA(alloc, g_pool, 0x200);
    const uint64_t count = *reinterpret_cast<uint64_t*>(g_pool + 0x30);
    CHECK(g_stack[0] == 0xeeeeeeeeu, "mode %d: the free stack was written", mode);
    CHECK(count == (mode == 1 ? 7u : 0u), "mode %d: the count was changed to %llu", mode, (unsigned long long)count);
    CHECK(Count("layout pool NOT raised", 2) == 1, "mode %d: want one WARNING, got %d", mode, Count("layout pool NOT raised", 2));
    CHECK(q15t::GetMemsysStats().pool_not_raised == 1 && q15t::GetMemsysStats().pool_raised == 0, "mode %d: counters", mode);
    SiteA(alloc, g_pool, 0x200);
    CHECK(Count("layout pool NOT raised", 2) == 1, "mode %d: the WARNING repeated", mode);
  }
  Drop(g_base);

  // 6. the Init witness: the first call from android_main's site logs once, with the System value the game will read
  Fresh();
  base = NewLib();
  q15t::InstallMemsys(base);
  init = reinterpret_cast<InitFn>(SlotWord(g_pins.got_init));
  q15t::SetMemsysSitesForTest(0, ret_init_a);  // the init site only (the Alloc site stays base + pin)
  // the return address of InitSiteA is stable across installs (same code), so ret_init_a is still it
  void* app = reinterpret_cast<void*>(0x7777);
  g_lines.clear();
  CHECK(InitSiteB(init, app) == kInitResult && g_init_app_seen == app, "init from another caller: result or argument");
  CHECK(Count("android_main reached") == 0, "witness from the wrong caller");
  CHECK(InitSiteA(init, app) == kInitResult && g_init_app_seen == app && g_init_calls == 2, "init at the site: result, argument, calls %d", g_init_calls);
  CHECK(Count("android_main reached CSysJava::Init", 1) == 1, "want one witness line, got %d", Count("android_main reached"));
  CHECK(Count("system_heap_mib_next_read=640") == 1 && Count("plugin_armed_before_android_main=1") == 1, "witness fields");
  InitSiteA(init, app);
  CHECK(Count("android_main reached") == 1 && q15t::GetMemsysStats().init_calls == 3 && q15t::GetMemsysStats().init_witnessed == 1,
        "the witness repeated or the counters are off");
  Drop(base);

  // 7. a pinned code word that differs refuses its own row only
  Fresh();
  base = NewLib();
  Poke32(base + 0x18818e8, 0x91080129);  // InitCommon's `add x9,x11,#0x200` changed (R37 word)
  r = q15t::InstallMemsys(base);
  CHECK(r.status != q15t::SwapStatus::kOk && std::strcmp(r.reason, "R37") == 0, "R37 word: %s %s", q15t::StatusName(r.status), r.reason);
  CHECK(SlotWord(g_pins.got_alloc) == reinterpret_cast<uintptr_t>(&FakeAlloc), "R37 slot swapped although a word differs");
  CHECK(SlotWord(g_pins.got_init) != reinterpret_cast<uintptr_t>(&FakeInit), "R38 not installed after R37's refusal");
  CHECK(Count("a pinned code word differs", 2) == 1 && Count("vaddr=0x18818e8") == 1 && Count("have=91080129") == 1, "no WARNING naming the word");
  Drop(base);
  Fresh();
  base = NewLib();
  Poke32(base + 0xf784e4, 0xd503201f);
  r = q15t::InstallMemsys(base);
  CHECK(r.status != q15t::SwapStatus::kOk && std::strcmp(r.reason, "R38") == 0, "R38 word: %s", r.reason);
  CHECK(SlotWord(g_pins.got_init) == reinterpret_cast<uintptr_t>(&FakeInit), "R38 slot swapped although a word differs");
  CHECK(SlotWord(g_pins.got_alloc) != reinterpret_cast<uintptr_t>(&FakeAlloc), "R37 not installed after R38's refusal");
  Drop(base);

  // 8. a slot another hook already owns: refused loudly, left alone
  Fresh();
  base = NewLib();
  Poke64(base + g_pins.got_alloc, 0x4242420);
  r = q15t::InstallMemsys(base);
  CHECK(r.status == q15t::SwapStatus::kUnexpectedOriginal && std::strcmp(r.reason, "R37") == 0, "foreign slot: %s %s", q15t::StatusName(r.status), r.reason);
  CHECK(SlotWord(g_pins.got_alloc) == 0x4242420, "foreign slot word changed");
  CHECK(Count("memsys hook install", 3) == 1 && SlotWord(g_pins.got_init) != reinterpret_cast<uintptr_t>(&FakeInit), "R38 or the ERROR line");
  Drop(base);

  // 9. memsys.off: nothing installed, one line
  Fresh();
  base = NewLib();
  FILE* f = std::fopen((g_dir + "/memsys.off").c_str(), "w");
  std::fclose(f);
  r = q15t::InstallMemsys(base);
  CHECK(r.status == q15t::SwapStatus::kOk && std::strcmp(r.reason, "memsys_off") == 0, "memsys.off: %s", r.reason);
  CHECK(SlotWord(g_pins.got_alloc) == reinterpret_cast<uintptr_t>(&FakeAlloc) && SlotWord(g_pins.got_init) == reinterpret_cast<uintptr_t>(&FakeInit),
        "a slot was swapped although memsys.off");
  CHECK(Count("memsys off", 1) == 1, "no INFO line for memsys.off");
  Drop(base);

  // 10. no base
  Fresh();
  q15t::ResetMemsysForTest();
  r = q15t::InstallMemsys(0);
  CHECK(r.status == q15t::SwapStatus::kNullSlot && std::strcmp(r.reason, "no_base") == 0 && Count("no game base", 3) == 1, "base 0");

  ClearDir();
  rmdir(g_dir.c_str());
  if (g_fail == 0) std::printf("memsys_test: all checks passed\n");
  return g_fail == 0 ? 0 : 1;
}
