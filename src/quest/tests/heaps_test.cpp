// Host test of the heap-size module (src/quest/combat/heaps.cpp, #2175): the real module, built without exceptions as the sentinel builds
// it, writing through the real GotDataSlot into the four GLOB_DAT words of a libr15 image built in memory (the build-ID note, a .dynsym
// whose heap symbols are data objects, the relocations, the stock sizes in .rodata and the four reader words at their pinned addresses).
// The game side is a model: a reader that does what libr15 does, `ldr x8,[slot]; ldr x1,[x8]`.
//
// The pins below are this test's own copy of the readelf table (the module's pins must equal it); got_pinned_test.cpp resolves the same
// four relocations, symbol types and words in the real store file.
//
// What it pins: all four slots are pointed at our values and the game reads the raised sizes, the .rodata and reader words are untouched,
// the Quest 2's System heap, each refusal (a slot another writer owns, a non-stock size, a reader that is not the stock load, a symbol
// that is a function) leaves its row alone and the others go on, the two switches, the module runs once, and the golden lines.
//
// Built and run by `just test-quest-combat`.

#include "quest/combat/heaps.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "quest/tests/combat_libr15_image.h"
#include "quest/tests/test_check.h"

namespace {

namespace heaps = nevr_quest::combat::heaps;
namespace pinned = sentinel::pinned;
using combat_test::Image;
using combat_test::Reloc;

constexpr std::uint64_t kMiB = 1ULL << 20;

struct Expect {
  const char* id;
  const char* symbol;
  std::uint64_t slot, ro, reader;
  std::uint32_t readerWord;
  std::uint64_t stock, target;
};
// R06 R07 R08 R09: GLOB_DAT slot -> symbol value (readelf -rW), reader = the disassembly address - 0x100000.
constexpr Expect kExpect[4] = {
    {"R06", "_ZN10NRadEngine22kResourceMemoryAndroidE", 0x370b720, 0x2b9caa0, 0xf78590, 0xf9400101, 206 * kMiB, 320 * kMiB},
    {"R07", "_ZN10NRadEngine20kSystemMemoryAndroidE", 0x371d020, 0x2b9ca90, 0xf78524, 0xf9400101, 400 * kMiB, 640 * kMiB},
    {"R08", "_ZN10NRadEngine17kGpuMemoryAndroidE", 0x3710188, 0x2b9ca98, 0xf78614, 0xf9400101, 160 * kMiB, 512 * kMiB},
    {"R09", "_ZN10NRadEngine24kR15GameSpaceMemoryQuestE", 0x3715198, 0x2b9e1c0, 0x11ffdec, 0xf9400100, 40 * kMiB, 128 * kMiB},
};

Image g_image;
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

void Build(bool functionSymbol = false, const char* buildId = nullptr, bool note = true) {
  std::vector<Reloc> relocs;
  for (int i = 0; i < 4; ++i) relocs.push_back({kExpect[i].symbol, kExpect[i].slot, true, !(functionSymbol && i == 2), kExpect[i].ro});
  g_image.Build(relocs, buildId, note);
  for (const Expect& e : kExpect) {
    g_image.Put<std::uint64_t>(e.ro, e.stock);
    g_image.Put<std::uintptr_t>(e.slot, g_image.Base() + e.ro);  // what the loader stored
    g_image.Put<std::uint32_t>(e.reader, e.readerWord);
  }
}

// What the game does: ldr x8,[slot]; ldr x1,[x8].
std::uint64_t GameRead(int row) {
  const std::uintptr_t word = g_image.Get<std::uintptr_t>(kExpect[row].slot);
  return *reinterpret_cast<const std::uint64_t*>(word);
}
bool SlotIsStock(int row) { return g_image.Get<std::uintptr_t>(kExpect[row].slot) == g_image.Base() + kExpect[row].ro; }

void Begin() {
  g_lines.clear();
  heaps::ResetForTest();
  sentinel::SetLogSink(&Capture);
}
void End() {
  heaps::ResetForTest();
  g_image.Drop();
  sentinel::SetLogSink(nullptr);
}

void ThePinsAreTheReadelfPins() {
  for (int i = 0; i < 4; ++i) {
    const heaps::RowPin& pin = heaps::kRows[i];
    const Expect& e = kExpect[i];
    QCHECK(std::strcmp(pin.id, e.id) == 0 && std::strcmp(pin.symbol, e.symbol) == 0 && pin.slot == e.slot && pin.ro == e.ro &&
           pin.reader == e.reader && pin.readerWord == e.readerWord && pin.stock == e.stock && pin.target == e.target);
  }
  QCHECK(heaps::kRowCount == 4 && heaps::kSystemRow == 1);
}

// A Quest 3: all four slots point at our values, the game reads the raised sizes, and no .rodata or reader word was written.
void AQuest3GetsAllFourSizes() {
  Begin();
  Build();
  const nevr_quest::combat::ModuleResult r = heaps::InstallAt(&Lookup, "eureka", true, true);
  QCHECK(r.installed && std::strcmp(r.status, "installed") == 0);
  for (int i = 0; i < 4; ++i) {
    QCHECK(GameRead(i) == kExpect[i].target);
    QCHECK(!SlotIsStock(i));
    QCHECK(g_image.Get<std::uint64_t>(kExpect[i].ro) == kExpect[i].stock);  // the rodata word was not written
    QCHECK(g_image.Get<std::uint32_t>(kExpect[i].reader) == kExpect[i].readerWord);
  }
  const std::vector<Line> rows = Lines("combat_heaps_row");
  QCHECK(rows.size() == 4U);
  for (const Line& l : rows) QCHECK(l.level == sentinel::LogLevel::kDebug && Has(l.text, "\"status\":\"swapped\""));
  const std::vector<Line> install = Lines("combat_heaps_install");
  QCHECK(install.size() == 1U);
  if (install.size() == 1U) {
    QCHECK(install[0].level == sentinel::LogLevel::kInfo);
    QCHECK(Golden(install[0].text) ==
           "\"event\":\"combat_heaps_install\",\"result\":\"installed\",\"reason\":\"ok\",\"swapped\":4,\"refused\":0,\"left\":0,"
           "\"resource_mib\":320,\"system_mib\":640,\"gpu_mib\":512,\"game_space_mib\":128,\"device\":\"eureka\","
           "\"consequence\":\"the game's heaps are the raised sizes when android_main reads them\"}");
  }
  End();
}

void AQuest2GetsA512MibSystemHeapAndTheOthersAreUnchanged() {
  Begin();
  Build();
  QCHECK(heaps::InstallAt(&Lookup, "hollywood", true, true).installed);
  QCHECK(GameRead(1) == 512 * kMiB);
  QCHECK(GameRead(0) == 320 * kMiB && GameRead(2) == 512 * kMiB && GameRead(3) == 128 * kMiB);
  QCHECK(heaps::SystemBytesFor("hollywood") == 512 * kMiB && heaps::SystemBytesFor("eureka") == 640 * kMiB &&
         heaps::SystemBytesFor("") == 640 * kMiB && heaps::SystemBytesFor(nullptr) == 640 * kMiB);
  End();
  Begin();
  Build();
  QCHECK(heaps::InstallAt(&Lookup, nullptr, true, true).installed);  // no device property at all: the Quest 3 value
  QCHECK(GameRead(1) == 640 * kMiB);
  End();
}

// A slot that does not hold base + its .rodata address (another writer owns it): that row is refused and left alone, the others go on.
void AForeignSlotRefusesItsRowAlone() {
  Begin();
  Build();
  g_image.Put<std::uintptr_t>(kExpect[2].slot, 0x1234560);
  const nevr_quest::combat::ModuleResult r = heaps::InstallAt(&Lookup, "eureka", true, true);
  QCHECK(!r.installed && std::strcmp(r.status, "partial") == 0);
  QCHECK(g_image.Get<std::uintptr_t>(kExpect[2].slot) == 0x1234560);
  QCHECK(GameRead(0) == 320 * kMiB && GameRead(1) == 640 * kMiB && GameRead(3) == 128 * kMiB);
  std::vector<Line> rows = Lines("combat_heaps_row");
  int refused = 0;
  for (const Line& l : rows) {
    if (Has(l.text, "\"status\":\"refused\"")) {
      ++refused;
      QCHECK(l.level == sentinel::LogLevel::kError && Has(l.text, "\"row\":\"R08\"") && Has(l.text, "\"why\":\"original_mismatch\""));
    }
  }
  QCHECK(refused == 1);
  const std::vector<Line> install = Lines("combat_heaps_install");
  QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kError && Has(install[0].text, "\"result\":\"partial\"") &&
         Has(install[0].text, "\"swapped\":3,\"refused\":1"));
  End();
}

// A .rodata size that is not the stock one (a patched file): refused with a Warning, the slot untouched.
void ANonStockRodataWordRefusesItsRow() {
  Begin();
  Build();
  g_image.Put<std::uint64_t>(kExpect[3].ro, 128 * kMiB);
  const nevr_quest::combat::ModuleResult r = heaps::InstallAt(&Lookup, "eureka", true, true);
  QCHECK(!r.installed);
  QCHECK(g_image.Get<std::uintptr_t>(kExpect[3].slot) == g_image.Base() + kExpect[3].ro);
  bool found = false;
  for (const Line& l : Lines("combat_heaps_row")) {
    if (Has(l.text, "\"row\":\"R09\"")) {
      found = l.level == sentinel::LogLevel::kWarn && Has(l.text, "\"why\":\"rodata_not_stock\"");
    }
  }
  QCHECK(found);
  QCHECK(GameRead(0) == 320 * kMiB);
  End();
}

// The reader is not the stock `ldr xN,[x8]`: the row is refused, the slot untouched.
void AReaderThatIsNotTheStockLoadRefusesItsRow() {
  Begin();
  Build();
  g_image.Put<std::uint32_t>(kExpect[0].reader, 0xd503201f);  // nop
  QCHECK(!heaps::InstallAt(&Lookup, "eureka", true, true).installed);
  QCHECK(SlotIsStock(0));
  bool found = false;
  for (const Line& l : Lines("combat_heaps_row")) {
    if (Has(l.text, "\"row\":\"R06\"")) found = l.level == sentinel::LogLevel::kWarn && Has(l.text, "\"why\":\"reader_word_not_stock\"");
  }
  QCHECK(found);
  QCHECK(GameRead(1) == 640 * kMiB);
  End();
}

// A slot whose relocation names a function holds a code pointer: that is a hook's business, so the data write refuses it.
void ASlotThatNamesAFunctionIsNeverWritten() {
  Begin();
  Build(/*functionSymbol=*/true);
  QCHECK(!heaps::InstallAt(&Lookup, "eureka", true, true).installed);
  QCHECK(SlotIsStock(2));
  bool found = false;
  for (const Line& l : Lines("combat_heaps_row")) {
    if (Has(l.text, "\"row\":\"R08\"")) found = Has(l.text, "\"why\":\"executable_mapping\"");
  }
  QCHECK(found);
  End();
}

void TheSwitchesTurnTheModuleOrTheSystemHeapOff() {
  // combat.heaps false: nothing is written and the line says why.
  Begin();
  Build();
  const nevr_quest::combat::ModuleResult off = heaps::InstallAt(&Lookup, "eureka", /*heapsOn=*/false, true);
  QCHECK(!off.installed && std::strcmp(off.status, "off") == 0);
  for (int i = 0; i < 4; ++i) QCHECK(SlotIsStock(i));
  const std::vector<Line> lines = Lines("combat_heaps_install");
  QCHECK(lines.size() == 1U && Has(lines[0].text, "\"result\":\"off\"") && Has(lines[0].text, "\"reason\":\"combat.heaps\""));
  End();
  // combat.memsys false: only the System heap stays the game's.
  Begin();
  Build();
  const nevr_quest::combat::ModuleResult noSystem = heaps::InstallAt(&Lookup, "eureka", true, /*systemOn=*/false);
  QCHECK(noSystem.installed && std::strcmp(noSystem.status, "installed") == 0);
  QCHECK(SlotIsStock(1) && GameRead(1) == 400 * kMiB);
  QCHECK(GameRead(0) == 320 * kMiB && GameRead(2) == 512 * kMiB && GameRead(3) == 128 * kMiB);
  const std::vector<Line> install = Lines("combat_heaps_install");
  QCHECK(install.size() == 1U && Has(install[0].text, "\"swapped\":3,\"refused\":0,\"left\":1") && Has(install[0].text, "\"system_mib\":400"));
  End();
}

void AnImageThatIsNotThePatchedLibr15IsRefusedBeforeAnyWord() {
  Begin();
  QCHECK(std::strcmp(heaps::InstallAt(nullptr, "eureka", true, true).status, "module_not_loaded") == 0);
  heaps::ResetForTest();
  QCHECK(std::strcmp(heaps::InstallAt(&Lookup, "eureka", true, true).status, "module_not_loaded") == 0);  // no image
  heaps::ResetForTest();
  Build(false, pinned::kLibR15StockBuildId);  // the stock file's ID: the patched file's is what the sentinel runs on
  QCHECK(std::strcmp(heaps::InstallAt(&Lookup, "eureka", true, true).status, "build_id_mismatch") == 0);
  for (int i = 0; i < 4; ++i) QCHECK(SlotIsStock(i));
  heaps::ResetForTest();
  Build(false, nullptr, /*note=*/false);
  QCHECK(std::strcmp(heaps::InstallAt(&Lookup, "eureka", true, true).status, "no_build_id") == 0);
  for (int i = 0; i < 4; ++i) QCHECK(SlotIsStock(i));
  for (const Line& l : Lines("combat_heaps_install")) QCHECK(l.level == sentinel::LogLevel::kError && Has(l.text, "\"result\":\"refused\""));
  End();
}

// The step runs once: a second call writes nothing, whatever the slots hold.
void TheStepRunsOnce() {
  Begin();
  Build();
  QCHECK(heaps::InstallAt(&Lookup, "eureka", true, true).installed);
  const std::uintptr_t first = g_image.Get<std::uintptr_t>(kExpect[0].slot);
  g_lines.clear();
  const nevr_quest::combat::ModuleResult again = heaps::InstallAt(&Lookup, "hollywood", true, true);
  QCHECK(!again.installed && std::strcmp(again.status, "already_ran") == 0);
  QCHECK(g_image.Get<std::uintptr_t>(kExpect[0].slot) == first && GameRead(1) == 640 * kMiB);  // the Quest 2 value was not applied over it
  QCHECK(Lines("combat_heaps_row").empty());
  End();
}

}  // namespace

int main() {
  ThePinsAreTheReadelfPins();
  AQuest3GetsAllFourSizes();
  AQuest2GetsA512MibSystemHeapAndTheOthersAreUnchanged();
  AForeignSlotRefusesItsRowAlone();
  ANonStockRodataWordRefusesItsRow();
  AReaderThatIsNotTheStockLoadRefusesItsRow();
  ASlotThatNamesAFunctionIsNeverWritten();
  TheSwitchesTurnTheModuleOrTheSystemHeapOff();
  AnImageThatIsNotThePatchedLibr15IsRefusedBeforeAnyWord();
  TheStepRunsOnce();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "heaps_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::puts("heaps_test: all checks passed");
  return 0;
}
