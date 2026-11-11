// Host test of the heap step as the sentinel runs it (#2175): the shipped early table (combat::InstallEarly, the real heaps and memsys rows
// through the real GotDataSlot and GotHook) on a libr15 image built in memory, against a model of the start of android_main. It shows
//
//   1. the step runs before the first allocation it governs: after InstallEarly, the model's android_main (CSysJava::Init through its
//      slot, the three heap reads through their slots, then InitCommon's pool Alloc) sees the raised sizes, the witness line is written
//      when Init is reached and before the model reads the heaps, and it names the raised System heap. A step that runs after the model
//      (the control) leaves every size stock and writes no witness, so the test can tell early from late;
//   2. the step runs once: a second InstallEarly runs nothing and writes nothing;
//   3. the step runs only where the gate allows: the q15t and combat variants, and a development build forced with combat.force; a plain
//      release, the measurement build and a release build asked to force install nothing and leave every slot as the loader wrote it;
//   4. the shipped rows read their switch: combat.heaps false leaves the four sizes, combat.memsys false leaves the System heap and the pool.
//
// The ordering of the sentinel's own sequence (after the libr15 patch is verified, before the clock hook, skipped when combat is off) is
// integration_sequence_test.cpp's; this file is the half that follows the step into the game.
//
// Built and run by `just test-quest-combat`.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "quest/combat/combat_active.h"
#include "quest/combat/combat_install.h"
#include "quest/combat/heaps.h"
#include "quest/combat/memsys.h"
#include "quest/tests/combat_libr15_image.h"
#include "quest/tests/test_check.h"

// sendguard's table row reads the sentinel's files folder (the plugin's old switch file); the test names a folder that does not exist.
namespace sentinel {
const char* FilesDir() { return "/nonexistent-nevr-files"; }
}  // namespace sentinel

namespace {

namespace bv = nevr_quest::build_variant;
namespace combat = nevr_quest::combat;
namespace heaps = nevr_quest::combat::heaps;
namespace memsys = nevr_quest::combat::memsys;
namespace pinned = sentinel::pinned;
using combat_test::Image;
using combat_test::Reloc;

constexpr std::uint64_t kMiB = 1ULL << 20;
using AllocFn = std::uint64_t (*)(void*, std::uint32_t, void*);
using InitFn = std::uint64_t (*)(void*);

// ---- the game's stand-ins -------------------------------------------------------------------------------------------------------

alignas(16) std::uint8_t g_pool[0x100];
alignas(16) std::uint32_t g_stack[0x400 + 8];
std::uint32_t g_allocCapSeen = 0;

extern "C" __attribute__((noinline)) std::uint64_t FakeAlloc(void* pool, std::uint32_t cap, void*) {
  g_allocCapSeen = cap;
  auto* p = static_cast<std::uint8_t*>(pool);
  const std::uint64_t stack = reinterpret_cast<std::uint64_t>(g_stack);
  std::memcpy(p + 0x28, &stack, sizeof(stack));
  const std::uint64_t zero = 0;
  std::memcpy(p + 0x30, &zero, sizeof(zero));
  const std::uint64_t stackCap = cap;
  std::memcpy(p + 0x38, &stackCap, sizeof(stackCap));
  const std::uint32_t capWord = cap;
  std::memcpy(p + 0x84, &capWord, sizeof(capWord));
  return 1;
}
extern "C" __attribute__((noinline)) std::uint64_t FakeInit(void*) { return 2; }

// ---- the libr15 image -----------------------------------------------------------------------------------------------------------

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
int IndexOf(const char* event) {
  const std::string key = std::string("\"event\":\"") + event + "\"";
  for (std::size_t i = 0; i < g_lines.size(); ++i) {
    if (Has(g_lines[i].text, key)) return static_cast<int>(i);
  }
  return -1;
}
int CountOf(const char* event) {
  const std::string key = std::string("\"event\":\"") + event + "\"";
  int n = 0;
  for (const Line& l : g_lines) n += Has(l.text, key) ? 1 : 0;
  return n;
}

void BuildGame() {
  g_pins = memsys::kPins;
  std::vector<Reloc> relocs;
  for (const heaps::RowPin& row : heaps::kRows) relocs.push_back({row.symbol, row.slot, true, true, row.ro});
  relocs.push_back({memsys::kAllocSymbol, g_pins.allocSlot, false, false, g_pins.allocFn});
  relocs.push_back({memsys::kInitSymbol, g_pins.initSlot, false, false, g_pins.initFn});
  g_image.Build(relocs);
  g_pins.allocFn = reinterpret_cast<std::uintptr_t>(&FakeAlloc) - g_image.Base();
  g_pins.initFn = reinterpret_cast<std::uintptr_t>(&FakeInit) - g_image.Base();
  for (const heaps::RowPin& row : heaps::kRows) {
    g_image.Put<std::uint64_t>(row.ro, row.stock);
    g_image.Put<std::uintptr_t>(row.slot, g_image.Base() + row.ro);  // what the loader stored
    g_image.Put<std::uint32_t>(row.reader, row.readerWord);
  }
  for (const memsys::Word& w : memsys::kWords) g_image.Put<std::uint32_t>(w.vaddr, w.word);
  *g_image.Slot(g_pins.allocSlot) = reinterpret_cast<void*>(&FakeAlloc);
  *g_image.Slot(g_pins.initSlot) = reinterpret_cast<void*>(&FakeInit);
}

// The two call sites of the model, as the module learns them (a test cannot return into libr15).
__attribute__((noinline)) std::uint64_t ModelInitCall(InitFn f) {
  const std::uint64_t r = f(nullptr);
  asm volatile("" ::: "memory");
  return r;
}
__attribute__((noinline)) std::uint64_t ModelPoolCall(AllocFn f, void* pool) {
  const std::uint64_t r = f(pool, 0x200, nullptr);
  asm volatile("" ::: "memory");
  return r;
}

struct Seen {
  std::uint64_t resource = 0, system = 0, gpu = 0, gameSpace = 0;
  std::uint32_t poolCapacity = 0;
};

// What android_main does at its start (libr15 0x10784c4): CSysJava::Init first, then the resource, System and GPU heap sizes through their
// slots, then (later, in InitCommon) the layout pool; GameSpaceAllocatorSize reads the fourth size. Each action leaves a marker line so the
// test can put the step's lines before or after it.
std::uint64_t ReadThroughSlot(std::uint64_t slot) {
  const std::uintptr_t word = g_image.Get<std::uintptr_t>(slot);
  return *reinterpret_cast<const std::uint64_t*>(word);
}
void Marker(const char* step) { sentinel::LogFields(sentinel::LogLevel::kInfo, "model_android_main", {{"step", step}}); }
Seen ModelAndroidMain() {
  Seen seen;
  ModelInitCall(reinterpret_cast<InitFn>(*g_image.Slot(g_pins.initSlot)));
  Marker("init_called");
  seen.resource = ReadThroughSlot(heaps::kRows[0].slot);
  seen.system = ReadThroughSlot(heaps::kRows[1].slot);
  seen.gpu = ReadThroughSlot(heaps::kRows[2].slot);
  Marker("heaps_read");
  seen.gameSpace = ReadThroughSlot(heaps::kRows[3].slot);
  std::memset(g_pool, 0, sizeof(g_pool));
  ModelPoolCall(reinterpret_cast<AllocFn>(*g_image.Slot(g_pins.allocSlot)), g_pool);
  seen.poolCapacity = g_allocCapSeen;
  Marker("pool_made");
  return seen;
}

// The game image and the modules' seams: the shipped rows find the image through the lookup and point memsys at the stand-in functions.
void SetUpGame() {
  BuildGame();
  memsys::SetPinsForTest(&g_pins);
  memsys::SetImageLookupForTest(&Lookup);
  heaps::SetImageLookupForTest(&Lookup);
  heaps::SetDeviceForTest("eureka");
}

void Begin(bv::Variant variant, bool devBuild = false, bool forceAsked = false) {
  g_lines.clear();
  combat::ResetForTest();
  combat::ResetInstallForTest();
  heaps::ResetForTest();
  memsys::ResetForTest();
  SetUpGame();
  combat::SetVariant(variant);
  sentinel::SetLogSink(&Capture);
  combat::Resolve(devBuild, forceAsked);
  g_lines.clear();  // combat_active's line is combat_active_test's
}
void End() {
  heaps::ResetForTest();
  memsys::ResetForTest();
  memsys::SetPinsForTest(nullptr);
  memsys::SetImageLookupForTest(nullptr);
  heaps::SetImageLookupForTest(nullptr);
  heaps::SetDeviceForTest(nullptr);
  g_image.Drop();
  sentinel::SetLogSink(nullptr);
}
bool AllSlotsStock() {
  for (const heaps::RowPin& row : heaps::kRows) {
    if (g_image.Get<std::uintptr_t>(row.slot) != g_image.Base() + row.ro) return false;
  }
  return *g_image.Slot(g_pins.allocSlot) == reinterpret_cast<void*>(&FakeAlloc) &&
         *g_image.Slot(g_pins.initSlot) == reinterpret_cast<void*>(&FakeInit);
}

// Teaches the memsys wrappers the model's two call sites: one throwaway android_main on the installed hooks reads the return addresses.
void TeachSites() {
  memsys::SetSitesForTest(0, 0);
  ModelAndroidMain();
  std::uintptr_t allocReturn = 0, initReturn = 0;
  memsys::LastCallersForTest(&allocReturn, &initReturn);
  memsys::SetSitesForTest(allocReturn, initReturn);
}

// ---- the tests ------------------------------------------------------------------------------------------------------------------

// 1. The step runs before the first allocation it governs.
void TheStepRunsBeforeAndroidMainReadsAnySize() {
  Begin(bv::Variant::kCombat);
  const combat::Outcome early = combat::InstallEarly();
  // heaps, memsys and the texel-buffer guard (#1650: its row is early too; it only registers requests with vk_ext_window)
  QCHECK(early.ran && early.modules == 3 && early.installed == 3 && early.failed == 0);
  TeachSites();
  g_lines.clear();
  const Seen seen = ModelAndroidMain();
  QCHECK(seen.resource == 320 * kMiB && seen.system == 640 * kMiB && seen.gpu == 512 * kMiB && seen.gameSpace == 128 * kMiB);
  QCHECK(seen.poolCapacity == 0x400);  // InitCommon asked for 0x200, the pool was made with 0x400
  // The witness is written when android_main reaches Init, which is before the model reads any heap, and it names the raised System heap.
  const int witness = IndexOf("combat_memsys_witness");
  const int initCalled = IndexOf("model_android_main");
  QCHECK(witness >= 0 && initCalled >= 0 && witness < initCalled);
  if (witness >= 0) {
    QCHECK(Has(g_lines[static_cast<std::size_t>(witness)].text, "\"system_heap\":\"raised\"") &&
           Has(g_lines[static_cast<std::size_t>(witness)].text, "\"system_heap_mib_next_read\":640"));
  }
  QCHECK(CountOf("combat_memsys_pool") == 1);
  End();
}

// The control: a step that runs after android_main's start changes nothing the game already read and writes no witness.
void AStepThatRunsAfterAndroidMainChangesNothingTheGameRead() {
  Begin(bv::Variant::kCombat);
  const Seen seen = ModelAndroidMain();  // the game starts first, on the loader's slots
  QCHECK(seen.resource == 206 * kMiB && seen.system == 400 * kMiB && seen.gpu == 160 * kMiB && seen.gameSpace == 40 * kMiB);
  QCHECK(seen.poolCapacity == 0x200);
  QCHECK(combat::InstallEarly().ran);  // late: the slots are rewritten, but the reads are done
  QCHECK(IndexOf("combat_memsys_witness") < 0);
  End();
}

// 2. Once.
void TheStepRunsOnce() {
  Begin(bv::Variant::kCombat);
  QCHECK(combat::InstallEarly().ran);
  const std::uintptr_t resource = g_image.Get<std::uintptr_t>(heaps::kRows[0].slot);
  void* const initSlot = *g_image.Slot(g_pins.initSlot);
  const int rows = CountOf("combat_heaps_row");
  const int installs = CountOf("combat_install");
  QCHECK(!combat::InstallEarly().ran);
  QCHECK(g_image.Get<std::uintptr_t>(heaps::kRows[0].slot) == resource && *g_image.Slot(g_pins.initSlot) == initSlot);
  QCHECK(CountOf("combat_heaps_row") == rows && CountOf("combat_install") == installs && CountOf("combat_memsys_hook") == 2);
  // The modules guard themselves too: a second call that did reach them writes nothing.
  QCHECK(std::strcmp(heaps::Install(true, true).status, "already_ran") == 0);
  QCHECK(std::strcmp(memsys::Install(true).status, "already_ran") == 0);
  QCHECK(g_image.Get<std::uintptr_t>(heaps::kRows[0].slot) == resource && *g_image.Slot(g_pins.initSlot) == initSlot);
  End();
}

// 3. Only where the gate allows.
void TheStepRunsOnlyWhereTheGateAllows() {
  struct Case {
    bv::Variant variant;
    bool dev;
    bool force;
    bool runs;
  };
  const Case cases[] = {
      {bv::Variant::kQ15t, false, false, true},       {bv::Variant::kCombat, false, false, true},
      {bv::Variant::kNone, false, false, false},      {bv::Variant::kPerfProbe, false, false, false},
      {bv::Variant::kNone, true, true, true},         // a development build forced on
      {bv::Variant::kNone, false, true, false},       // a release build asked to force: refused
      {bv::Variant::kPerfProbe, false, true, false},
  };
  for (const Case& c : cases) {
    Begin(c.variant, c.dev, c.force);
    const combat::Outcome o = combat::InstallEarly();
    QCHECK(o.ran == c.runs);
    if (c.runs) {
      QCHECK(!AllSlotsStock() && o.installed == 3);  // heaps, memsys and the texel-buffer guard
    } else {
      QCHECK(AllSlotsStock());  // every slot is as the loader wrote it
      QCHECK(CountOf("combat_heaps_row") == 0 && CountOf("combat_memsys_hook") == 0 && CountOf("combat_heaps_install") == 0);
      QCHECK(CountOf("combat_install") == 1);  // the one `skipped` line
    }
    End();
  }
}

// 4. The shipped rows read their switch.
void TheShippedRowsReadTheirSwitch() {
  nevr_quest::CombatConfig cfg;
  cfg.heaps.on = false;
  Begin(bv::Variant::kCombat);
  combat::SetConfig(cfg);
  combat::Outcome o = combat::InstallEarly();
  QCHECK(o.ran && o.modules == 3 && o.installed == 2 && o.failed == 0 && o.off == 1);  // heaps is off (counted apart), memsys and the texel-buffer guard install
  for (const heaps::RowPin& row : heaps::kRows) QCHECK(g_image.Get<std::uintptr_t>(row.slot) == g_image.Base() + row.ro);
  QCHECK(*g_image.Slot(g_pins.initSlot) == memsys::InitThunk::EntryAddress());
  End();

  cfg = nevr_quest::CombatConfig{};
  cfg.memsys.on = false;
  Begin(bv::Variant::kCombat);
  combat::SetConfig(cfg);
  o = combat::InstallEarly();
  QCHECK(o.ran && o.modules == 3 && o.installed == 2 && o.failed == 0 && o.off == 1);  // memsys is off (counted apart), heaps installs without the System heap, the texel-buffer guard installs
  QCHECK(g_image.Get<std::uintptr_t>(heaps::kRows[1].slot) == g_image.Base() + heaps::kRows[1].ro);
  QCHECK(g_image.Get<std::uintptr_t>(heaps::kRows[0].slot) != g_image.Base() + heaps::kRows[0].ro);
  QCHECK(*g_image.Slot(g_pins.initSlot) == reinterpret_cast<void*>(&FakeInit) && *g_image.Slot(g_pins.allocSlot) == reinterpret_cast<void*>(&FakeAlloc));
  End();
}

}  // namespace

int main() {
  TheStepRunsBeforeAndroidMainReadsAnySize();
  AStepThatRunsAfterAndroidMainChangesNothingTheGameRead();
  TheStepRunsOnce();
  TheStepRunsOnlyWhereTheGateAllows();
  TheShippedRowsReadTheirSwitch();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "heap_step_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::puts("heap_step_test: all checks passed");
  return 0;
}
