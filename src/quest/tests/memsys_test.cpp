// Host test of the render-target pool module (src/quest/combat/memsys.cpp, #2175): the real wrappers through the two thunks (kCaller),
// installed by the real GotHook into the two JUMP_SLOTs of a libr15 image built in memory, and called the way the game calls them:
// through the slot. The game side is a model: a fake Alloc that lays the pool out the way the real one does (+0x28 free stack, +0x30 count
// 0, +0x38 stack capacity, +0x84 capacity), InitCommon's inline fill (511..0, count + 512), and a fake CSysJava::Init.
//
// A test cannot make a call return into libr15, so the return address is the gate's only stand-in: the first call from each of two test
// functions teaches the module that function's return address as "the site" (SetSitesForTest), and the other function is the wrong caller.
// That the real sites are base + 0x18818cc and base + 0xf784e8 is the pin table, which got_pinned_test.cpp reads in the real libr15.
//
// What it pins: the pool is 0..1023 exactly once after the inline fill, the pass-through for the wrong caller and the wrong capacity,
// each layout the module does not recognise (and a stack in read-only memory) leaves the pool the stock 512 with one Warning, the witness
// is written once with the System heap the game will read (raised or stock), each refusal (a pinned word, a slot another hook owns)
// leaves its row alone and the other row goes on, the switch, once, and the golden lines.
//
// Built and run by `just test-quest-combat`.

#include "quest/combat/memsys.h"

#include <sys/mman.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "quest/tests/combat_libr15_image.h"
#include "quest/tests/test_check.h"

namespace {

namespace memsys = nevr_quest::combat::memsys;
namespace pinned = sentinel::pinned;
using combat_test::Image;
using combat_test::Reloc;

using AllocFn = std::uint64_t (*)(void*, std::uint32_t, void*);
using InitFn = std::uint64_t (*)(void*);

// ---- the fake engine ------------------------------------------------------------------------------------------------------------

alignas(16) std::uint8_t g_pool[0x100];
alignas(16) std::uint32_t g_stack[0x400 + 8];
int g_allocCalls = 0;
int g_initCalls = 0;
std::uint32_t g_allocCapSeen = 0;
void* g_initAppSeen = nullptr;
// 0 the real layout; 1 count not 0; 2 no stack; 3 capacity read back wrong; 4 stack capacity short; 5 stack unreadable;
// 6 the stack is in read-only memory
int g_mode = 0;
std::uint32_t* g_readOnlyStack = nullptr;
constexpr std::uint64_t kAllocResult = 0x5a5a;
constexpr std::uint64_t kInitResult = 0xa11ce;

extern "C" __attribute__((noinline)) std::uint64_t FakeAlloc(void* pool, std::uint32_t cap, void*) {
  ++g_allocCalls;
  g_allocCapSeen = cap;
  auto* p = static_cast<std::uint8_t*>(pool);
  const std::uint64_t stack = g_mode == 2 ? 0 : g_mode == 5 ? 0x1000 : g_mode == 6 ? reinterpret_cast<std::uint64_t>(g_readOnlyStack)
                                                                                    : reinterpret_cast<std::uint64_t>(g_stack);
  std::memcpy(p + 0x28, &stack, sizeof(stack));
  const std::uint64_t count = g_mode == 1 ? 7 : 0;
  std::memcpy(p + 0x30, &count, sizeof(count));
  const std::uint64_t stackCap = g_mode == 4 ? 0x100 : cap;
  std::memcpy(p + 0x38, &stackCap, sizeof(stackCap));
  const std::uint32_t capWord = g_mode == 3 ? 0x200 : cap;
  std::memcpy(p + 0x84, &capWord, sizeof(capWord));
  return kAllocResult;
}
extern "C" __attribute__((noinline)) std::uint64_t FakeInit(void* app) {
  ++g_initCalls;
  g_initAppSeen = app;
  return kInitResult;
}

// InitCommon's inline fill (libr15 0x18818d8..0x1881914): push 511..0 at stack[count..], count += 512.
void InlineFill(void* pool) {
  auto* p = static_cast<std::uint8_t*>(pool);
  std::uint64_t stackWord = 0;
  std::memcpy(&stackWord, p + 0x28, sizeof(stackWord));
  auto* stack = reinterpret_cast<std::uint32_t*>(stackWord);
  std::uint64_t count = 0;
  std::memcpy(&count, p + 0x30, sizeof(count));
  for (std::uint32_t i = 0; i < 512; ++i) stack[count + i] = 511 - i;
  count += 512;
  std::memcpy(p + 0x30, &count, sizeof(count));
}
std::uint64_t PoolCount() {
  std::uint64_t count = 0;
  std::memcpy(&count, g_pool + 0x30, sizeof(count));
  return count;
}

// Two call sites in two functions: the return address tells them apart.
__attribute__((noinline)) std::uint64_t SiteA(AllocFn f, void* pool, std::uint32_t cap) {
  const std::uint64_t r = f(pool, cap, nullptr);
  asm volatile("" ::: "memory");
  return r;
}
__attribute__((noinline)) std::uint64_t SiteB(AllocFn f, void* pool, std::uint32_t cap) {
  std::uint64_t r = f(pool, cap, nullptr);
  asm volatile("" : "+r"(r)::"memory");
  return r;
}
__attribute__((noinline)) std::uint64_t InitSiteA(InitFn f, void* app) {
  const std::uint64_t r = f(app);
  asm volatile("" ::: "memory");
  return r;
}
__attribute__((noinline)) std::uint64_t InitSiteB(InitFn f, void* app) {
  std::uint64_t r = f(app);
  asm volatile("" : "+r"(r)::"memory");
  return r;
}

// ---- the libr15 image ----------------------------------------------------------------------------------------------------------

Image g_image;
memsys::Pins g_pins = memsys::kPins;
bool Lookup(const char* module, sentinel::ElfImage* out) {
  if (!g_image.Present() || std::strcmp(module, pinned::kLibR15) != 0) return false;
  *out = g_image.Elf();
  return true;
}

struct Line {
  sentinel::LogLevel level;
  std::string text;
};
std::vector<Line> g_lines;
void Capture(sentinel::LogLevel level, const char* line) { g_lines.push_back({level, line}); }
bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }
std::vector<Line> Lines(const char* event) {
  std::vector<Line> out;
  const std::string key = std::string("\"event\":\"") + event + "\"";
  for (const Line& l : g_lines) {
    if (Has(l.text, key)) out.push_back(l);
  }
  return out;
}
std::string Golden(const std::string& line) {
  const std::size_t at = line.find("\"event\"");
  return at == std::string::npos ? line : line.substr(at);
}

// A libr15 holding the pinned words, both slots holding the game's functions (the stand-ins), and the System GLOB_DAT word pointing at a
// u64 of `systemMiB`.
void Build(std::uint64_t systemMiB = 640, bool foreignAllocSlot = false) {
  g_pins = memsys::kPins;
  g_image.Build({{memsys::kAllocSymbol, g_pins.allocSlot, false, false, g_pins.allocFn},
                 {memsys::kInitSymbol, g_pins.initSlot, false, false, g_pins.initFn}});
  // base + fn is the stand-in's address: the real functions are not in the image, so the pins hold the stand-ins' offsets.
  g_pins.allocFn = reinterpret_cast<std::uintptr_t>(&FakeAlloc) - g_image.Base();
  g_pins.initFn = reinterpret_cast<std::uintptr_t>(&FakeInit) - g_image.Base();
  for (const memsys::Word& w : memsys::kWords) g_image.Put<std::uint32_t>(w.vaddr, w.word);
  *g_image.Slot(g_pins.allocSlot) = foreignAllocSlot ? reinterpret_cast<void*>(0x4242420) : reinterpret_cast<void*>(&FakeAlloc);
  *g_image.Slot(g_pins.initSlot) = reinterpret_cast<void*>(&FakeInit);
  g_image.Put<std::uint64_t>(0x2b9ca90, systemMiB << 20);
  g_image.Put<std::uintptr_t>(g_pins.systemSlot, g_image.Base() + 0x2b9ca90);
}
AllocFn AllocSlot() { return reinterpret_cast<AllocFn>(*g_image.Slot(g_pins.allocSlot)); }
InitFn InitSlot() { return reinterpret_cast<InitFn>(*g_image.Slot(g_pins.initSlot)); }
bool AllocHooked() { return *g_image.Slot(g_pins.allocSlot) == memsys::AllocThunk::EntryAddress(); }
bool InitHooked() { return *g_image.Slot(g_pins.initSlot) == memsys::InitThunk::EntryAddress(); }

void ResetPool() {
  std::memset(g_pool, 0, sizeof(g_pool));
  std::memset(g_stack, 0xee, sizeof(g_stack));
  g_allocCalls = g_initCalls = 0;
  g_allocCapSeen = 0;
  g_initAppSeen = nullptr;
  g_mode = 0;
}

void Begin() {
  g_lines.clear();
  memsys::ResetForTest();
  ResetPool();
  sentinel::SetLogSink(&Capture);
}
void End() {
  memsys::ResetForTest();
  g_image.Drop();
  sentinel::SetLogSink(nullptr);
}

// The two sites, learned from the first call of each test function.
struct Sites {
  std::uintptr_t allocA = 0, allocB = 0, initA = 0, initB = 0;
};
Sites Learn() {
  Sites s;
  AllocFn alloc = AllocSlot();
  InitFn init = InitSlot();
  std::uintptr_t a = 0, i = 0;
  ResetPool();
  SiteA(alloc, g_pool, 0x200);
  memsys::LastCallersForTest(&a, &i);
  s.allocA = a;
  SiteB(alloc, g_pool, 0x200);
  memsys::LastCallersForTest(&s.allocB, nullptr);
  InitSiteA(init, nullptr);
  memsys::LastCallersForTest(nullptr, &s.initA);
  InitSiteB(init, nullptr);
  memsys::LastCallersForTest(nullptr, &s.initB);
  ResetPool();
  return s;
}

// ---- the tests ------------------------------------------------------------------------------------------------------------------

void ThePinsAreTheReadelfAndDisassemblyPins() {
  const memsys::Pins& d = memsys::kPins;
  QCHECK(d.allocSlot == 0x36e9b48 && d.allocFn == 0x1901b5c && d.allocSite == 0x18818cc);
  QCHECK(d.initSlot == 0x36de5c0 && d.initFn == 0xf7e8e8 && d.initSite == 0xf784e8 && d.systemSlot == 0x371d020);
  static const struct {
    std::uint64_t va;
    std::uint32_t word;
    int row;
  } kWant[21] = {{0x18818b8, 0x52804001, 37}, {0x18818c8, 0x97da8bfe, 37}, {0x18818cc, 0xa942ae6a, 37}, {0x18818d8, 0x52803f6c, 37},
                 {0x18818e0, 0x4f00c420, 37}, {0x18818e8, 0x91080169, 37}, {0x1881904, 0xf120011f, 37}, {0x1881914, 0xf9001a69, 37},
                 {0xf248c0, 0xb0013e30, 37},  {0xf248c4, 0xf945a611, 37},  {0x1901b5c, 0xd10143ff, 37}, {0x1901c24, 0xad0002c0, 37},
                 {0x1901c30, 0xf9001e75, 37}, {0x1901c4c, 0xf9001660, 37}, {0xf784e4, 0x97fe5633, 38},  {0xf784e8, 0x97fdb586, 38},
                 {0xf0ddb0, 0xb0013e90, 38},  {0xf0ddb4, 0xf942e211, 38},  {0xf78510, 0xb0013d28, 38},  {0xf78514, 0xf9401108, 38},
                 {0xf78524, 0xf9400101, 38}};
  QCHECK(memsys::kWordCount == 21);
  for (std::size_t i = 0; i < 21; ++i) {
    QCHECK(memsys::kWords[i].vaddr == kWant[i].va && memsys::kWords[i].word == kWant[i].word && memsys::kWords[i].row == kWant[i].row);
  }
  QCHECK(std::strcmp(memsys::kAllocSymbol, "_ZN10NRadEngine11CSimplePoolINS_20SGRenderTargetLayoutEjE5AllocEjPNS_10CAllocatorE") == 0);
  QCHECK(std::strcmp(memsys::kInitSymbol, "_ZN10NRadEngine8CSysJava4InitEP11android_app") == 0);
}

void BothSlotsTakeTheirThunkAndTheLinesSayWhich() {
  Begin();
  Build();
  const nevr_quest::combat::ModuleResult r = memsys::InstallAt(&Lookup, g_pins, true);
  QCHECK(r.installed && std::strcmp(r.status, "installed") == 0);
  QCHECK(AllocHooked() && InitHooked());
  const std::vector<Line> hooks = Lines("combat_memsys_hook");
  QCHECK(hooks.size() == 2U);
  for (const Line& l : hooks) QCHECK(l.level == sentinel::LogLevel::kInfo && Has(l.text, "\"status\":\"ok\""));
  QCHECK(hooks.size() == 2U && Has(hooks[0].text, "\"row\":\"R37\"") && Has(hooks[1].text, "\"row\":\"R38\""));
  const std::vector<Line> install = Lines("combat_memsys_install");
  QCHECK(install.size() == 1U);
  if (install.size() == 1U) {
    QCHECK(install[0].level == sentinel::LogLevel::kInfo);
    QCHECK(Golden(install[0].text) ==
           "\"event\":\"combat_memsys_install\",\"result\":\"installed\",\"reason\":\"ok\",\"r37\":\"ok\",\"r38\":\"ok\","
           "\"consequence\":\"the layout pool is raised to 1024 slots and android_main's start is witnessed\"}");
  }
  End();
}

// The site call with the stock capacity: Alloc is asked for 0x400, the free stack holds 1023..512 below the inline fill's 511..0, and the
// count reads 1024 after it. A call from anywhere else, or with another capacity, is the game's own.
void ThePoolIsRaisedOnlyAtTheSiteWithTheStockCapacity() {
  Begin();
  Build();
  QCHECK(memsys::InstallAt(&Lookup, g_pins, true).installed);
  const Sites s = Learn();
  QCHECK(s.allocA != 0 && s.allocB != 0 && s.allocA != s.allocB && s.initA != 0 && s.initB != 0 && s.initA != s.initB);
  // Before the site is known (it is base + the pin, which no test function returns to): the stock request, nothing pre-pushed.
  memsys::SetSitesForTest(0, 0);
  ResetPool();
  QCHECK(SiteA(AllocSlot(), g_pool, 0x200) == kAllocResult && g_allocCapSeen == 0x200 && PoolCount() == 0);

  memsys::SetSitesForTest(s.allocA, s.initA);
  ResetPool();
  g_lines.clear();
  AllocFn alloc = AllocSlot();
  QCHECK(SiteA(alloc, g_pool, 0x200) == kAllocResult);  // the game's result goes back untouched
  QCHECK(g_allocCapSeen == 0x400 && g_allocCalls == 1);
  QCHECK(PoolCount() == 512);
  InlineFill(g_pool);
  QCHECK(PoolCount() == 1024);
  bool seen[1024] = {};
  bool unique = true;
  for (std::uint32_t i = 0; i < 1024; ++i) {
    if (g_stack[i] >= 1024 || seen[g_stack[i]]) {
      unique = false;
    } else {
      seen[g_stack[i]] = true;
    }
  }
  QCHECK(unique);
  QCHECK(g_stack[0] == 1023 && g_stack[511] == 512 && g_stack[512] == 511 && g_stack[1023] == 0);
  const std::vector<Line> pool = Lines("combat_memsys_pool");
  QCHECK(pool.size() == 1U);
  if (pool.size() == 1U) {
    QCHECK(pool[0].level == sentinel::LogLevel::kInfo);
    QCHECK(Golden(pool[0].text) ==
           "\"event\":\"combat_memsys_pool\",\"result\":\"raised\",\"slots\":1024,\"pre_pushed\":512,\"cap_read_back\":1024,"
           "\"consequence\":\"the layout pool holds 1024 render-target layouts once the inline fill has run\"}");
  }
  QCHECK(memsys::CurrentStats().poolRaised == 1);

  // The same call from another function: the game's own request.
  ResetPool();
  QCHECK(SiteB(alloc, g_pool, 0x200) == kAllocResult && g_allocCapSeen == 0x200);
  QCHECK(PoolCount() == 0 && g_stack[0] == 0xeeeeeeeeU);
  // The site, another capacity: untouched.
  ResetPool();
  QCHECK(SiteA(alloc, g_pool, 0x100) == kAllocResult && g_allocCapSeen == 0x100);
  QCHECK(PoolCount() == 0 && g_stack[0] == 0xeeeeeeeeU);
  End();
}

// A layout the module does not recognise: Alloc ran, nothing is written, one Warning (once).
void ALayoutItDoesNotKnowIsLeftTheStock512() {
  g_readOnlyStack = static_cast<std::uint32_t*>(mmap(nullptr, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  QCHECK(g_readOnlyStack != MAP_FAILED);
  for (int mode = 1; mode <= 6; ++mode) {
    Begin();
    Build();
    QCHECK(memsys::InstallAt(&Lookup, g_pins, true).installed);
    const Sites s = Learn();
    memsys::SetSitesForTest(s.allocA, s.initA);
    ResetPool();
    g_lines.clear();
    g_mode = mode;
    SiteA(AllocSlot(), g_pool, 0x200);
    QCHECK(g_stack[0] == 0xeeeeeeeeU);
    QCHECK(PoolCount() == (mode == 1 ? 7U : 0U));
    const std::vector<Line> pool = Lines("combat_memsys_pool");
    QCHECK(pool.size() == 1U);
    if (pool.size() == 1U) {
      QCHECK(pool[0].level == sentinel::LogLevel::kWarn && Has(pool[0].text, "\"result\":\"not_raised\""));
      QCHECK(Has(pool[0].text, "the layout pool is the stock 512 slots"));
    }
    QCHECK(memsys::CurrentStats().poolNotRaised == 1 && memsys::CurrentStats().poolRaised == 0);
    g_mode = mode;
    SiteA(AllocSlot(), g_pool, 0x200);
    QCHECK(Lines("combat_memsys_pool").size() == 1U);  // the Warning is once
    End();
  }
  munmap(g_readOnlyStack, 4096);
}

// android_main's first call writes one line with the System heap the game will read, whether that is the raised size or the stock one.
void TheWitnessIsWrittenOnceAtAndroidMainsCall() {
  Begin();
  Build(/*systemMiB=*/640);
  QCHECK(memsys::InstallAt(&Lookup, g_pins, true).installed);
  const Sites s = Learn();
  memsys::SetSitesForTest(0, s.initA);
  g_lines.clear();
  void* app = reinterpret_cast<void*>(0x7777);
  QCHECK(InitSiteB(InitSlot(), app) == kInitResult && g_initAppSeen == app);  // another caller: the game's call, no line
  QCHECK(Lines("combat_memsys_witness").empty());
  QCHECK(InitSiteA(InitSlot(), app) == kInitResult && g_initAppSeen == app);
  const std::vector<Line> witness = Lines("combat_memsys_witness");
  QCHECK(witness.size() == 1U);
  if (witness.size() == 1U) {
    QCHECK(witness[0].level == sentinel::LogLevel::kInfo);
    char slot[19];
    QCHECK(Golden(witness[0].text) == std::string("\"event\":\"combat_memsys_witness\",\"msg\":\"android_main reached CSysJava::Init\","
                                                  "\"system_heap_mib_next_read\":640,\"system_heap\":\"raised\",\"system_slot_vaddr\":\"") +
                                          sentinel::HexString(slot, g_pins.systemSlot) + "\"}");
  }
  InitSiteA(InitSlot(), app);
  QCHECK(Lines("combat_memsys_witness").size() == 1U && memsys::CurrentStats().initCalls == 5 &&  // Learn made two, then three more
          memsys::CurrentStats().initWitnessed == 1);
  End();
  // The System heap the game will read is the stock one: the witness says so.
  Begin();
  Build(/*systemMiB=*/400);
  QCHECK(memsys::InstallAt(&Lookup, g_pins, true).installed);
  const Sites t = Learn();
  memsys::SetSitesForTest(0, t.initA);
  g_lines.clear();
  InitSiteA(InitSlot(), nullptr);
  const std::vector<Line> stock = Lines("combat_memsys_witness");
  QCHECK(stock.size() == 1U && Has(stock[0].text, "\"system_heap_mib_next_read\":400") && Has(stock[0].text, "\"system_heap\":\"stock\""));
  End();
}

// A pinned code word that differs refuses its own row only.
void APinnedWordThatDiffersRefusesItsRowAlone() {
  Begin();
  Build();
  g_image.Put<std::uint32_t>(0x18818e8, 0x91080129);  // InitCommon's `add x9,x11,#0x200` changed (an R37 word)
  nevr_quest::combat::ModuleResult r = memsys::InstallAt(&Lookup, g_pins, true);
  QCHECK(!r.installed && std::strcmp(r.status, "partial") == 0);
  QCHECK(!AllocHooked() && *g_image.Slot(g_pins.allocSlot) == reinterpret_cast<void*>(&FakeAlloc));
  QCHECK(InitHooked());
  std::vector<Line> hooks = Lines("combat_memsys_hook");
  bool found = false;
  for (const Line& l : hooks) {
    if (Has(l.text, "\"row\":\"R37\"")) found = l.level == sentinel::LogLevel::kWarn && Has(l.text, "\"status\":\"pinned_word_mismatch\"") && Has(l.text, "\"vaddr\":\"0x00000000018818e8\"");
  }
  QCHECK(found);
  End();
  Begin();
  Build();
  g_image.Put<std::uint32_t>(0xf784e4, 0xd503201f);  // android_main's first call replaced (an R38 word)
  r = memsys::InstallAt(&Lookup, g_pins, true);
  QCHECK(!r.installed && std::strcmp(r.status, "partial") == 0);
  QCHECK(AllocHooked() && !InitHooked() && *g_image.Slot(g_pins.initSlot) == reinterpret_cast<void*>(&FakeInit));
  End();
}

// A slot another hook already owns: refused, left alone, the other row installs.
void AForeignSlotIsRefusedAndLeftAlone() {
  Begin();
  Build(640, /*foreignAllocSlot=*/true);
  const nevr_quest::combat::ModuleResult r = memsys::InstallAt(&Lookup, g_pins, true);
  QCHECK(!r.installed && std::strcmp(r.status, "partial") == 0);
  QCHECK(*g_image.Slot(g_pins.allocSlot) == reinterpret_cast<void*>(0x4242420));
  QCHECK(InitHooked());
  bool found = false;
  for (const Line& l : Lines("combat_memsys_hook")) {
    if (Has(l.text, "\"row\":\"R37\"")) found = l.level == sentinel::LogLevel::kError && Has(l.text, "\"status\":\"original_mismatch\"");
  }
  QCHECK(found);
  const std::vector<Line> install = Lines("combat_memsys_install");
  QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kError && Has(install[0].text, "\"result\":\"partial\"") &&
         Has(install[0].text, "the witness is written but the pool is the stock 512 slots"));
  End();
}

void TheSwitchTheMissingImageAndASecondStepInstallNothing() {
  Begin();
  Build();
  const nevr_quest::combat::ModuleResult off = memsys::InstallAt(&Lookup, g_pins, /*on=*/false);
  QCHECK(!off.installed && std::strcmp(off.status, "off") == 0 && !AllocHooked() && !InitHooked());
  const std::vector<Line> lines = Lines("combat_memsys_install");
  QCHECK(lines.size() == 1U && Has(lines[0].text, "\"result\":\"off\"") && Has(lines[0].text, "\"reason\":\"combat.memsys\""));
  End();

  Begin();
  QCHECK(std::strcmp(memsys::InstallAt(nullptr, g_pins, true).status, "module_not_loaded") == 0);
  memsys::ResetForTest();
  Build();
  g_image.Drop();
  QCHECK(std::strcmp(memsys::InstallAt(&Lookup, g_pins, true).status, "module_not_loaded") == 0);
  memsys::ResetForTest();
  Build();
  g_image.Build({}, pinned::kLibR15StockBuildId);
  QCHECK(std::strcmp(memsys::InstallAt(&Lookup, g_pins, true).status, "build_id_mismatch") == 0);
  memsys::ResetForTest();
  g_image.Build({}, nullptr, /*note=*/false);
  QCHECK(std::strcmp(memsys::InstallAt(&Lookup, g_pins, true).status, "no_build_id") == 0);
  for (const Line& l : Lines("combat_memsys_install")) QCHECK(l.level == sentinel::LogLevel::kError && Has(l.text, "\"result\":\"refused\""));
  End();

  // A second step: the hooks are in, nothing is installed again.
  Begin();
  Build();
  QCHECK(memsys::InstallAt(&Lookup, g_pins, true).installed);
  g_lines.clear();
  const nevr_quest::combat::ModuleResult again = memsys::InstallAt(&Lookup, g_pins, true);
  QCHECK(!again.installed && std::strcmp(again.status, "already_ran") == 0 && Lines("combat_memsys_hook").empty());
  QCHECK(AllocHooked() && InitHooked());
  End();
}

}  // namespace

int main() {
  ThePinsAreTheReadelfAndDisassemblyPins();
  BothSlotsTakeTheirThunkAndTheLinesSayWhich();
  ThePoolIsRaisedOnlyAtTheSiteWithTheStockCapacity();
  ALayoutItDoesNotKnowIsLeftTheStock512();
  TheWitnessIsWrittenOnceAtAndroidMainsCall();
  APinnedWordThatDiffersRefusesItsRowAlone();
  AForeignSlotIsRefusedAndLeftAlone();
  TheSwitchTheMissingImageAndASecondStepInstallNothing();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "memsys_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::puts("memsys_test: all checks passed");
  return 0;
}
