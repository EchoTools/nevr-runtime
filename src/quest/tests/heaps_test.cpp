// Host test of the heap-size module (src/quest/combat/heaps.cpp, #2175): the real module, built without exceptions as the sentinel builds
// it, writing through the real GotDataSlot into the four GLOB_DAT words of a libr15 image built in memory (the build-ID note, a .dynsym
// whose heap symbols are data objects, the relocations, the stock sizes in .rodata and the four reader words at their pinned addresses).
// The game side is a model: a reader that does what libr15 does, `ldr x8,[slot]; ldr x1,[x8]`.
//
// The pins below are this test's own copy of the readelf table (the module's pins must equal it); got_pinned_test.cpp resolves the same
// four relocations, symbol types and words in the real store file.
//
// What it pins: all four slots are pointed at our values and the game reads the raised sizes, the .rodata and reader words are untouched,
// the Quest 2's System heap from the identity step's profile (quest/headset_active.h, #2037), the generic size and a Warning when no profile
// was set, each refusal (a slot another writer owns, a non-stock size, a reader that is not the stock load, a symbol
// that is a function) leaves its row alone and the others go on, the two switches, the module runs once, and the golden lines.
//
// Built and run by `just test-quest-combat`.

#include "quest/combat/heaps.h"

#include <sys/mman.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "quest/headset_active.h"
#include "quest/headset_profile.h"
#include "quest/tests/combat_libr15_image.h"
#include "quest/tests/test_check.h"

namespace {

namespace heaps = nevr_quest::combat::heaps;
namespace hp = nevr_quest::headset_profile;
namespace active = nevr_quest::headset_active;
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
  active::SetActive(nullptr);
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
  const nevr_quest::combat::ModuleResult r = heaps::InstallAt(&Lookup, hp::kQuest3, true, true);
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
           "\"event\":\"combat_heaps_install\",\"result\":\"installed\",\"reason\":\"ok\",\"swapped\":4,\"refused\":0,\"left\":0,\"memsys\":1,"
           "\"resource_mib\":320,\"system_mib\":640,\"gpu_mib\":512,\"game_space_mib\":128,\"profile\":\"quest3\","
           "\"consequence\":\"the game's heaps are the raised sizes when android_main reads them\"}");
  }
  End();
}

void AQuest2GetsA512MibSystemHeapAndTheOthersAreUnchanged() {
  Begin();
  Build();
  QCHECK(heaps::InstallAt(&Lookup, hp::kQuest2, true, true).installed);
  QCHECK(GameRead(1) == 512 * kMiB);
  QCHECK(GameRead(0) == 320 * kMiB && GameRead(2) == 512 * kMiB && GameRead(3) == 128 * kMiB);
  // The size is the profile's own value, not a test of which headset it is: a profile with 0 keeps the row's target.
  QCHECK(heaps::SystemBytesFor(hp::kQuest2) == 512 * kMiB && heaps::SystemBytesFor(hp::kQuest3) == 640 * kMiB &&
         heaps::SystemBytesFor(hp::kGeneric) == 640 * kMiB);
  constexpr hp::Profile kOther = {hp::Id::kGeneric, "other", hp::kQuest2Eyes, 300, 90.f};
  QCHECK(heaps::SystemBytesFor(kOther) == 300 * kMiB);
  End();
  Begin();
  Build();
  QCHECK(heaps::InstallAt(&Lookup, hp::kGeneric, true, true).installed);  // a headset no rule names: the row's target
  QCHECK(GameRead(1) == 640 * kMiB);
  End();
}

// A slot that does not hold base + its .rodata address (another writer owns it): that row is refused and left alone, the others go on.
void AForeignSlotRefusesItsRowAlone() {
  Begin();
  Build();
  g_image.Put<std::uintptr_t>(kExpect[2].slot, 0x1234560);
  const nevr_quest::combat::ModuleResult r = heaps::InstallAt(&Lookup, hp::kQuest3, true, true);
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
  const nevr_quest::combat::ModuleResult r = heaps::InstallAt(&Lookup, hp::kQuest3, true, true);
  QCHECK(!r.installed);
  QCHECK(g_image.Get<std::uintptr_t>(kExpect[3].slot) == g_image.Base() + kExpect[3].ro);
  bool found = false;
  for (const Line& l : Lines("combat_heaps_row")) {
    if (Has(l.text, "\"row\":\"R09\"")) {
      found = l.level == sentinel::LogLevel::kWarn && Has(l.text, "\"why\":\"rodata_not_stock\"") &&
              Has(l.text, "\"have\":\"0x0000000008000000\",\"want\":\"0x0000000002800000\"");  // 128 MiB read, 40 MiB wanted
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
  QCHECK(!heaps::InstallAt(&Lookup, hp::kQuest3, true, true).installed);
  QCHECK(SlotIsStock(0));
  bool found = false;
  for (const Line& l : Lines("combat_heaps_row")) {
    if (Has(l.text, "\"row\":\"R06\"")) {
      found = l.level == sentinel::LogLevel::kWarn && Has(l.text, "\"why\":\"reader_word_not_stock\"") &&
              Has(l.text, "\"have\":\"0x00000000d503201f\",\"want\":\"0x00000000f9400101\"");
    }
  }
  QCHECK(found);
  QCHECK(GameRead(1) == 640 * kMiB);
  End();
}

// A slot whose relocation names a function holds a code pointer: that is a hook's business, so the data write refuses it.
void ASlotThatNamesAFunctionIsNeverWritten() {
  Begin();
  Build(/*functionSymbol=*/true);
  QCHECK(!heaps::InstallAt(&Lookup, hp::kQuest3, true, true).installed);
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
  const nevr_quest::combat::ModuleResult off = heaps::InstallAt(&Lookup, hp::kQuest3, /*heapsOn=*/false, true);
  QCHECK(!off.installed && std::strcmp(off.status, "off") == 0);
  for (int i = 0; i < 4; ++i) QCHECK(SlotIsStock(i));
  const std::vector<Line> lines = Lines("combat_heaps_install");
  QCHECK(lines.size() == 1U && Has(lines[0].text, "\"result\":\"off\"") && Has(lines[0].text, "\"reason\":\"combat.heaps\""));
  End();
  // combat.memsys false: only the System heap stays the game's.
  Begin();
  Build();
  const nevr_quest::combat::ModuleResult noSystem = heaps::InstallAt(&Lookup, hp::kQuest3, true, /*systemOn=*/false);
  QCHECK(noSystem.installed && std::strcmp(noSystem.status, "installed") == 0);
  QCHECK(SlotIsStock(1) && GameRead(1) == 400 * kMiB);
  QCHECK(GameRead(0) == 320 * kMiB && GameRead(2) == 512 * kMiB && GameRead(3) == 128 * kMiB);
  const std::vector<Line> install = Lines("combat_heaps_install");
  QCHECK(install.size() == 1U && Has(install[0].text, "\"swapped\":3,\"refused\":0,\"left\":1") && Has(install[0].text, "\"system_mib\":400"));
  End();
}

// The memsys field of every install line is the value the step was handed, true and false alike, on the installed, off and refused paths.
// InstallAt stores the switch before it branches, and ResetForTest does not clear the switch, so a path that stored nothing would log whatever
// the last step left. Each path therefore follows a step that left the other value.
void TheMemsysFieldIsTheValueTheStepWasHanded() {
  for (const bool sys : {false, true}) {
    const char* const want = sys ? "\"memsys\":1," : "\"memsys\":0,";
    Begin();
    Build();
    QCHECK(heaps::InstallAt(&Lookup, hp::kQuest3, true, sys).installed);
    std::vector<Line> install = Lines("combat_heaps_install");
    QCHECK(install.size() == 1U && Has(install[0].text, want));
    End();
    // The switch is left at !sys by a step that installed (so the off call's store, when it makes one, is visible).
    Begin();
    Build();
    QCHECK(heaps::InstallAt(&Lookup, hp::kQuest3, true, !sys).installed);
    End();
    Begin();
    Build();
    QCHECK(std::strcmp(heaps::InstallAt(&Lookup, hp::kQuest3, false, sys).status, "off") == 0);
    install = Lines("combat_heaps_install");
    QCHECK(install.size() == 1U && Has(install[0].text, "\"result\":\"off\"") && Has(install[0].text, want));
    End();
    // The refused step follows an installed step that left !sys in the switch, so a refusal that stores nothing logs !sys and fails here.
    Begin();
    Build();
    QCHECK(heaps::InstallAt(&Lookup, hp::kQuest3, true, !sys).installed);
    End();
    Begin();
    Build();
    QCHECK(std::strcmp(heaps::InstallAt(nullptr, hp::kQuest3, true, sys).status, "module_not_loaded") == 0);
    install = Lines("combat_heaps_install");
    QCHECK(install.size() == 1U && Has(install[0].text, "\"result\":\"refused\"") && Has(install[0].text, want));
    End();
  }
}

void AnImageThatIsNotThePatchedLibr15IsRefusedBeforeAnyWord() {
  Begin();
  QCHECK(std::strcmp(heaps::InstallAt(nullptr, hp::kQuest3, true, true).status, "module_not_loaded") == 0);
  heaps::ResetForTest();
  QCHECK(std::strcmp(heaps::InstallAt(&Lookup, hp::kQuest3, true, true).status, "module_not_loaded") == 0);  // no image
  heaps::ResetForTest();
  Build(false, pinned::kLibR15StockBuildId);  // the stock file's ID: the patched file's is what the sentinel runs on
  QCHECK(std::strcmp(heaps::InstallAt(&Lookup, hp::kQuest3, true, true).status, "build_id_mismatch") == 0);
  for (int i = 0; i < 4; ++i) QCHECK(SlotIsStock(i));
  heaps::ResetForTest();
  Build(false, nullptr, /*note=*/false);
  QCHECK(std::strcmp(heaps::InstallAt(&Lookup, hp::kQuest3, true, true).status, "no_build_id") == 0);
  for (int i = 0; i < 4; ++i) QCHECK(SlotIsStock(i));
  for (const Line& l : Lines("combat_heaps_install")) QCHECK(l.level == sentinel::LogLevel::kError && Has(l.text, "\"result\":\"refused\""));
  End();
}

// The step runs once: a second call writes nothing, whatever the slots hold.
void TheStepRunsOnce() {
  Begin();
  Build();
  QCHECK(heaps::InstallAt(&Lookup, hp::kQuest3, true, true).installed);
  const std::uintptr_t first = g_image.Get<std::uintptr_t>(kExpect[0].slot);
  g_lines.clear();
  const nevr_quest::combat::ModuleResult again = heaps::InstallAt(&Lookup, hp::kQuest2, true, true);
  QCHECK(!again.installed && std::strcmp(again.status, "already_ran") == 0);
  QCHECK(g_image.Get<std::uintptr_t>(kExpect[0].slot) == first && GameRead(1) == 640 * kMiB);  // the Quest 2 value was not applied over it
  QCHECK(Lines("combat_heaps_row").empty());
  End();
}

// Takes a page of the libr15 image away (or gives it back): a word in it cannot be read, as a pinned address outside the mapping cannot.
void Protect(std::uint64_t vaddr, int prot) {
  QCHECK(mprotect(g_image.Slot(vaddr & ~0xfffULL), 0x1000, prot) == 0);
}

// Every line a row writes says which row, what became of it, why, where, how big and for which headset.
void TheRowLinesCarryTheirFields() {
  Begin();
  Build();
  QCHECK(heaps::InstallAt(&Lookup, hp::kQuest3, true, /*systemOn=*/false).installed);
  const std::vector<Line> rows = Lines("combat_heaps_row");
  QCHECK(rows.size() == 4U);
  if (rows.size() == 4U) {
    QCHECK(rows[0].level == sentinel::LogLevel::kDebug && rows[1].level == sentinel::LogLevel::kInfo && rows[2].level == sentinel::LogLevel::kDebug &&
           rows[3].level == sentinel::LogLevel::kDebug);
    QCHECK(Golden(rows[0].text) ==
           "\"event\":\"combat_heaps_row\",\"row\":\"R06\",\"status\":\"swapped\",\"why\":\"slot_points_at_our_value\","
           "\"slot_vaddr\":\"0x000000000370b720\",\"value_mib\":320,\"observed_mib\":320,\"profile\":\"quest3\"}");
    QCHECK(Golden(rows[1].text) ==
           "\"event\":\"combat_heaps_row\",\"row\":\"R07\",\"status\":\"left\",\"why\":\"combat.memsys_is_false\","
           "\"slot_vaddr\":\"0x000000000371d020\",\"value_mib\":400,\"observed_mib\":400,\"profile\":\"quest3\"}");
    QCHECK(Golden(rows[2].text) ==
           "\"event\":\"combat_heaps_row\",\"row\":\"R08\",\"status\":\"swapped\",\"why\":\"slot_points_at_our_value\","
           "\"slot_vaddr\":\"0x0000000003710188\",\"value_mib\":512,\"observed_mib\":512,\"profile\":\"quest3\"}");
    QCHECK(Golden(rows[3].text) ==
           "\"event\":\"combat_heaps_row\",\"row\":\"R09\",\"status\":\"swapped\",\"why\":\"slot_points_at_our_value\","
           "\"slot_vaddr\":\"0x0000000003715198\",\"value_mib\":128,\"observed_mib\":128,\"profile\":\"quest3\"}");
  }
  const std::vector<Line> install = Lines("combat_heaps_install");
  QCHECK(install.size() == 1U);
  if (install.size() == 1U) {
    QCHECK(install[0].level == sentinel::LogLevel::kInfo);
    QCHECK(Golden(install[0].text) ==
           "\"event\":\"combat_heaps_install\",\"result\":\"installed\",\"reason\":\"ok\",\"swapped\":3,\"refused\":0,\"left\":1,\"memsys\":0,"
           "\"resource_mib\":320,\"system_mib\":400,\"gpu_mib\":512,\"game_space_mib\":128,\"profile\":\"quest3\","
           "\"consequence\":\"the game's heaps are the raised sizes when android_main reads them\"}");
  }
  End();
}

// A slot another writer holds (a word that is not base + .rodata, at an address no mapping has): its row is refused with the manager's status and
// reads 0, and the install line says one row kept its stock size.
void ARefusedRowIsNamedWithTheManagersStatusAndReadsZero() {
  Begin();
  Build();
  g_image.Put<std::uintptr_t>(kExpect[2].slot, 0x10);
  QCHECK(!heaps::InstallAt(&Lookup, hp::kQuest3, true, true).installed);
  const std::vector<Line> rows = Lines("combat_heaps_row");
  QCHECK(rows.size() == 4U);
  if (rows.size() == 4U) {
    QCHECK(rows[2].level == sentinel::LogLevel::kError);
    QCHECK(Golden(rows[2].text) ==
           "\"event\":\"combat_heaps_row\",\"row\":\"R08\",\"status\":\"refused\",\"why\":\"original_mismatch\","
           "\"slot_vaddr\":\"0x0000000003710188\",\"value_mib\":512,\"observed_mib\":0,\"profile\":\"quest3\"}");
  }
  const std::vector<Line> install = Lines("combat_heaps_install");
  QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kError);
  if (install.size() == 1U) {
    QCHECK(Golden(install[0].text) ==
           "\"event\":\"combat_heaps_install\",\"result\":\"partial\",\"reason\":\"row_refused\",\"swapped\":3,\"refused\":1,\"left\":0,\"memsys\":1,"
           "\"resource_mib\":320,\"system_mib\":640,\"gpu_mib\":0,\"game_space_mib\":128,\"profile\":\"quest3\","
           "\"consequence\":\"a refused row keeps the game's stock size; the others are raised\"}");
  }
  End();
}

// Every row refused: the result is "refused", not "partial", and the consequence is the stock sizes.
void WhenEveryRowIsRefusedTheResultIsRefused() {
  Begin();
  Build();
  for (const Expect& e : kExpect) g_image.Put<std::uintptr_t>(e.slot, 0x10);
  const nevr_quest::combat::ModuleResult r = heaps::InstallAt(&Lookup, hp::kQuest3, true, true);
  QCHECK(!r.installed && std::strcmp(r.status, "refused") == 0);
  const std::vector<Line> install = Lines("combat_heaps_install");
  QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kError);
  if (install.size() == 1U) {
    QCHECK(Golden(install[0].text) ==
           "\"event\":\"combat_heaps_install\",\"result\":\"refused\",\"reason\":\"row_refused\",\"swapped\":0,\"refused\":4,\"left\":0,\"memsys\":1,"
           "\"resource_mib\":0,\"system_mib\":0,\"gpu_mib\":0,\"game_space_mib\":0,\"profile\":\"quest3\","
           "\"consequence\":\"the game keeps its stock heap sizes (206, 400, 160 and 40 MiB)\"}");
  }
  End();
}

// The refusals that write no row: the lines say what was refused, what the game keeps, and for which headset.
void TheRefusalAndOffLinesSayWhatTheGameKeeps() {
  Begin();
  QCHECK(std::strcmp(heaps::InstallAt(nullptr, hp::kQuest3, true, true).status, "module_not_loaded") == 0);
  std::vector<Line> install = Lines("combat_heaps_install");
  QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kError);
  if (install.size() == 1U) {
    QCHECK(Golden(install[0].text) ==
           "\"event\":\"combat_heaps_install\",\"result\":\"refused\",\"reason\":\"module_not_loaded\",\"swapped\":0,\"refused\":0,\"left\":0,\"memsys\":1,"
           "\"resource_mib\":0,\"system_mib\":0,\"gpu_mib\":0,\"game_space_mib\":0,\"profile\":\"quest3\","
           "\"consequence\":\"the game keeps its stock heap sizes (206, 400, 160 and 40 MiB)\"}");
  }
  End();
  Begin();
  Build();
  QCHECK(std::strcmp(heaps::InstallAt(&Lookup, hp::kQuest3, false, true).status, "off") == 0);
  install = Lines("combat_heaps_install");
  QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kInfo);
  if (install.size() == 1U) {
    QCHECK(Golden(install[0].text) ==
           "\"event\":\"combat_heaps_install\",\"result\":\"off\",\"reason\":\"combat.heaps\",\"swapped\":0,\"refused\":0,\"left\":0,\"memsys\":1,"
           "\"resource_mib\":0,\"system_mib\":0,\"gpu_mib\":0,\"game_space_mib\":0,\"profile\":\"quest3\","
           "\"consequence\":\"combat.heaps is false: the game keeps its stock heap sizes (206, 400, 160 and 40 MiB)\"}");
  }
  End();
  // The lines name the profile they were given.
  Begin();
  Build();
  QCHECK(heaps::InstallAt(&Lookup, hp::kGeneric, false, true).status != nullptr);
  install = Lines("combat_heaps_install");
  QCHECK(install.size() == 1U && Has(install[0].text, "\"profile\":\"generic\""));
  End();
}

// A second step is refused with a Warning that says nothing was written.
void ASecondStepIsAWarningThatWroteNothing() {
  Begin();
  Build();
  QCHECK(heaps::InstallAt(&Lookup, hp::kQuest3, true, true).installed);
  g_lines.clear();
  QCHECK(std::strcmp(heaps::InstallAt(&Lookup, hp::kQuest3, true, true).status, "already_ran") == 0);
  const std::vector<Line> refused = Lines("combat_heaps_refused");
  QCHECK(refused.size() == 1U && refused[0].level == sentinel::LogLevel::kWarn);
  if (refused.size() == 1U) {
    QCHECK(Golden(refused[0].text) ==
           "\"event\":\"combat_heaps_refused\",\"why\":\"already_ran\",\"consequence\":\"a second step would find its slots already ours; nothing was written\"}");
  }
  End();
}

// The step the sentinel runs reads the identity step's headset profile (quest/headset_active.h, ADR 0031 decision 4, #2037) and finds
// libr15 through the seam (production: the loader's lookup). It reads no system property of its own.
void TheInstallEntryReadsTheIdentityStepsProfileAndTheLookup() {
  Begin();
  Build();
  heaps::SetImageLookupForTest(&Lookup);
  active::SetActive(&hp::kQuest2);
  const nevr_quest::combat::ModuleResult r = heaps::Install(true, true);
  QCHECK(r.installed && std::strcmp(r.status, "installed") == 0);
  QCHECK(GameRead(1) == 512 * kMiB && GameRead(0) == 320 * kMiB);
  const std::vector<Line> install = Lines("combat_heaps_install");
  QCHECK(install.size() == 1U && Has(install[0].text, "\"profile\":\"quest2\"") && Has(install[0].text, "\"system_mib\":512"));
  QCHECK(Lines("combat_heaps_profile_missing").empty());
  heaps::SetImageLookupForTest(nullptr);
  End();
  // A Quest 3 and a headset no rule names: the row's 640 MiB.
  for (const hp::Profile* profile : {&hp::kQuest3, &hp::kGeneric}) {
    Begin();
    Build();
    heaps::SetImageLookupForTest(&Lookup);
    active::SetActive(profile);
    QCHECK(heaps::Install(true, true).installed);
    QCHECK(GameRead(1) == 640 * kMiB);
    const std::vector<Line> lines = Lines("combat_heaps_install");
    QCHECK(lines.size() == 1U && Has(lines[0].text, std::string("\"profile\":\"") + profile->name + "\""));
    QCHECK(Lines("combat_heaps_profile_missing").empty());
    heaps::SetImageLookupForTest(nullptr);
    End();
  }
  // The two switches reach the right rows through the entry: the System heap alone left, then the whole module off.
  Begin();
  Build();
  heaps::SetImageLookupForTest(&Lookup);
  active::SetActive(&hp::kQuest3);
  QCHECK(heaps::Install(true, /*systemOn=*/false).installed);
  QCHECK(SlotIsStock(1) && GameRead(1) == 400 * kMiB && GameRead(0) == 320 * kMiB);
  heaps::SetImageLookupForTest(nullptr);
  End();
  Begin();
  Build();
  heaps::SetImageLookupForTest(&Lookup);
  QCHECK(std::strcmp(heaps::Install(/*heapsOn=*/false, true).status, "off") == 0);
  for (int i = 0; i < 4; ++i) QCHECK(SlotIsStock(i));
  heaps::SetImageLookupForTest(nullptr);
  End();
  // With no lookup of the test's own, the loader's is used: there is no libr15 on a host.
  Begin();
  Build();
  active::SetActive(&hp::kQuest3);
  QCHECK(std::strcmp(heaps::Install(true, true).status, "module_not_loaded") == 0);
  for (int i = 0; i < 4; ++i) QCHECK(SlotIsStock(i));
  End();
}

// No profile set (a sequence that did not run the identity step before this one): the System heap takes the generic profile's size, never
// the Quest 2's, and one Warning says so. With combat.heaps false nothing reads the profile, so nothing is said.
void WithNoProfileTheSystemHeapIsTheGenericOneAndAWarningSaysSo() {
  Begin();
  Build();
  heaps::SetImageLookupForTest(&Lookup);
  active::SetActive(nullptr);
  QCHECK(heaps::Install(true, true).installed);
  QCHECK(GameRead(1) == 640 * kMiB && GameRead(0) == 320 * kMiB);
  const std::vector<Line> missing = Lines("combat_heaps_profile_missing");
  QCHECK(missing.size() == 1U);
  if (missing.size() == 1U) {
    QCHECK(missing[0].level == sentinel::LogLevel::kWarn);
    QCHECK(Golden(missing[0].text) ==
           "\"event\":\"combat_heaps_profile_missing\",\"profile\":\"generic\",\"system_mib\":640,"
           "\"consequence\":\"the identity step set no headset profile before this step, so the System heap takes the generic profile's size\"}");
  }
  const std::vector<Line> install = Lines("combat_heaps_install");
  QCHECK(install.size() == 1U && Has(install[0].text, "\"profile\":\"generic\""));
  heaps::SetImageLookupForTest(nullptr);
  End();
  Begin();
  Build();
  heaps::SetImageLookupForTest(&Lookup);
  QCHECK(std::strcmp(heaps::Install(/*heapsOn=*/false, true).status, "off") == 0);
  QCHECK(Lines("combat_heaps_profile_missing").empty());
  heaps::SetImageLookupForTest(nullptr);
  End();
}

// A pinned address the game's mapping does not give back reads as "unreadable", not as a word.
void AWordThatCannotBeReadIsNamedUnreadable() {
  Begin();
  Build();
  Protect(kExpect[0].reader, PROT_NONE);  // R06, R07 and R08's readers share this page
  QCHECK(!heaps::InstallAt(&Lookup, hp::kQuest3, true, true).installed);
  Protect(kExpect[0].reader, PROT_READ | PROT_WRITE);
  const std::vector<Line> rows = Lines("combat_heaps_row");
  QCHECK(rows.size() == 4U);
  if (rows.size() == 4U) {
    QCHECK(rows[0].level == sentinel::LogLevel::kWarn);
    QCHECK(Golden(rows[0].text) ==
           "\"event\":\"combat_heaps_row\",\"row\":\"R06\",\"status\":\"refused\",\"why\":\"reader_word_not_stock\","
           "\"slot_vaddr\":\"0x000000000370b720\",\"have\":\"unreadable\",\"want\":\"0x00000000f9400101\",\"value_mib\":320,\"profile\":\"quest3\"}");
    QCHECK(Has(rows[3].text, "\"status\":\"swapped\""));  // R09's reader is on another page
  }
  QCHECK(SlotIsStock(0) && SlotIsStock(1) && SlotIsStock(2) && !SlotIsStock(3));
  End();
  Begin();
  Build();
  Protect(kExpect[0].ro, PROT_NONE);  // R06, R07 and R08's .rodata sizes share this page
  QCHECK(!heaps::InstallAt(&Lookup, hp::kQuest3, true, true).installed);
  Protect(kExpect[0].ro, PROT_READ | PROT_WRITE);
  const std::vector<Line> ro = Lines("combat_heaps_row");
  QCHECK(ro.size() == 4U);
  if (ro.size() == 4U) {
    QCHECK(Golden(ro[0].text) ==
           "\"event\":\"combat_heaps_row\",\"row\":\"R06\",\"status\":\"refused\",\"why\":\"rodata_not_stock\","
           "\"slot_vaddr\":\"0x000000000370b720\",\"have\":\"unreadable\",\"want\":\"0x000000000ce00000\",\"value_mib\":320,\"profile\":\"quest3\"}");
  }
  QCHECK(SlotIsStock(0) && SlotIsStock(1) && SlotIsStock(2));
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
  TheMemsysFieldIsTheValueTheStepWasHanded();
  AnImageThatIsNotThePatchedLibr15IsRefusedBeforeAnyWord();
  TheStepRunsOnce();
  TheRowLinesCarryTheirFields();
  ARefusedRowIsNamedWithTheManagersStatusAndReadsZero();
  WhenEveryRowIsRefusedTheResultIsRefused();
  TheRefusalAndOffLinesSayWhatTheGameKeeps();
  ASecondStepIsAWarningThatWroteNothing();
  TheInstallEntryReadsTheIdentityStepsProfileAndTheLookup();
  WithNoProfileTheSystemHeapIsTheGenericOneAndAWarningSaysSo();
  AWordThatCannotBeReadIsNamedUnreadable();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "heaps_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::puts("heaps_test: all checks passed");
  return 0;
}
