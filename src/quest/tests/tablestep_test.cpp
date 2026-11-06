// Host test of the shader-set CreateInstance module (src/quest/combat/tablestep.cpp, #2177): the real handler through the real thunk, installed
// by the real GotHook into the ABS64 vtable word of a libr15 image built in memory, and called the way the game calls the factory's virtual:
// through the word. The pin word at ELF 0x1907130 is the stock `mov w8,#0x20`; the word holds base + the function; the game's function is a
// stand-in that builds a 0xb0-byte object whose table step at +0x98 is the stock 0x20, as the stock store leaves it.
//
// What it pins: in a combat match the object the game returned has step 1; a step that is not 0x20 is left alone, counted and warned about once;
// a null result is returned as it is; in an Echo Arena level, in a lobby arena players use, in the combat lobby, in a transition out of combat,
// with nothing loaded and with no game known the object is exactly the game's (and the scope is read once per creation); the pinned code word
// refuses the module whole; each refusal (the word, a foreign word, the build, the switch, a second step) leaves the word alone; the plugin's
// kill file is a Warning; the golden lines.
//
// Built and run by `just test-quest-combat`.

#include "quest/combat/tablestep.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "quest/tests/table_step_fixture.h"

namespace {

namespace tbs = nevr_quest::combat::tablestep;
namespace ts = nevr_quest::combat::table_step;
using namespace table_step_test;

constexpr std::uintptr_t kObjStep = 0x98;
constexpr std::uintptr_t kObjSize = 0xb0;

unsigned g_stockCalls = 0;
std::uint64_t g_stockStep = 0x20;  // what the stock store writes (a test changes it to mimic a different build)
bool g_returnNull = false;
void* g_stockUnused = nullptr;
void* g_stockInit = nullptr;

// The stock CreateInstance: a fresh object with the stock step 0x20 at +0x98. The first argument is unused, as in the stock.
extern "C" __attribute__((noinline)) void* FakeCreate(void* unused, void* init) {
  ++g_stockCalls;
  g_stockUnused = unused;
  g_stockInit = init;
  if (g_returnNull) return nullptr;
  auto* obj = static_cast<std::uint8_t*>(std::calloc(1, kObjSize));
  std::memcpy(obj + kObjStep, &g_stockStep, sizeof g_stockStep);
  return obj;
}
using CreateFn = void* (*)(void*, void*);

std::uint64_t StepOf(void* obj) {
  std::uint64_t v = 0;
  std::memcpy(&v, static_cast<std::uint8_t*>(obj) + kObjStep, sizeof v);
  return v;
}

// A function the game does not own, in an executable mapping: a word that holds it is another hook's (or another build's), not CreateInstance.
extern "C" __attribute__((noinline)) void* ForeignCreate(void*, void*) { return nullptr; }

tbs::Pins g_pins = tbs::kPins;

void Build(bool foreignSlot = false) {
  g_pins = tbs::kPins;
  g_image.Build({{tbs::kSymbol, g_pins.slot, false, false, g_pins.fn, /*abs64=*/true}});
  g_pins.fn = reinterpret_cast<std::uintptr_t>(&FakeCreate) - g_image.Base();  // base + fn is the stand-in
  PutWords(tbs::kWords);
  *g_image.Slot(g_pins.slot) = foreignSlot ? reinterpret_cast<void*>(&ForeignCreate) : reinterpret_cast<void*>(&FakeCreate);
}
CreateFn SlotFn() { return reinterpret_cast<CreateFn>(*g_image.Slot(g_pins.slot)); }
bool Hooked() { return *g_image.Slot(g_pins.slot) == tbs::Thunk::EntryAddress(); }

void Begin1() {
  Begin();
  tbs::ResetForTest();
  tbs::SetImageLookupForTest(&Lookup);
  tbs::SetPinsForTest(&g_pins);
  g_stockCalls = 0;
  g_stockStep = 0x20;
  g_returnNull = false;
  g_stockUnused = g_stockInit = nullptr;
}
void End1() {
  tbs::ResetForTest();
  tbs::SetImageLookupForTest(nullptr);
  tbs::SetPinsForTest(nullptr);
  End();
}

void ThePinsAreTheReadelfAndDisassemblyPins() {
  QCHECK(tbs::kPins.fn == 0x190707c && tbs::kPins.slot == 0x365ddd0 && tbs::kPins.stepWord == 0x1907130);
  QCHECK(tbs::kWordCount == 1 && tbs::kWords[0].vaddr == 0x1907130 && tbs::kWords[0].word == 0x52800408);
  QCHECK(std::strcmp(tbs::kSymbol, "_ZN10NRadEngine8CFactoryINS_19CGShaderSetResourceEE14CreateInstanceEPNS_13CPresenceInitE") == 0);
}

// In a combat match the object the game returned has step 1, and the game's function ran once with the game's arguments.
void ACombatMatchSetsTheStepOfTheObjectTheGameReturned() {
  Begin1();
  Build();
  SetWhere(Where::kCombatMap);
  const nevr_quest::combat::ModuleResult r = tbs::InstallAt(&Lookup, g_pins, true);
  QCHECK(r.installed && std::strcmp(r.status, "installed") == 0 && Hooked());
  int unused = 0;
  int init = 0;
  void* const obj = SlotFn()(&unused, &init);
  QCHECK(obj != nullptr);
  if (obj != nullptr) {
    QCHECK(StepOf(obj) == 1);
    std::free(obj);
  }
  QCHECK(g_stockCalls == 1 && g_stockUnused == &unused && g_stockInit == &init);
  QCHECK(tbs::CurrentStats().rewritten == 1 && tbs::CurrentStats().unexpected == 0 && tbs::CurrentStats().stock == 0);
  const std::vector<Line> first = Lines("combat_tablestep_first");
  QCHECK(first.size() == 1U && Lines("combat_tablestep_stock").empty());
  if (first.size() == 1U) {
    QCHECK(first[0].level == sentinel::LogLevel::kInfo);
    QCHECK(Golden(first[0].text) ==
           "\"event\":\"combat_tablestep_first\",\"from\":32,\"to\":1,"
           "\"msg\":\"the first shader set created in a combat match has the table growth step 1\"}");
  }
  std::free(SlotFn()(nullptr, nullptr));
  QCHECK(tbs::CurrentStats().rewritten == 2 && Lines("combat_tablestep_first").size() == 1U);  // the line is written once
  End1();
}

// A step that is not the stock 0x20 (another build): left alone, counted, one Warning.
void AStepThatIsNotTheStockValueIsLeftAloneAndWarnedAboutOnce() {
  Begin1();
  Build();
  SetWhere(Where::kCombatMap);
  QCHECK(tbs::InstallAt(&Lookup, g_pins, true).installed);
  g_stockStep = 0x40;
  void* obj = SlotFn()(nullptr, nullptr);
  QCHECK(obj != nullptr && StepOf(obj) == 0x40);
  std::free(obj);
  obj = SlotFn()(nullptr, nullptr);
  std::free(obj);
  QCHECK(tbs::CurrentStats().rewritten == 0 && tbs::CurrentStats().unexpected == 2);
  const std::vector<Line> warn = Lines("combat_tablestep_not_stock");
  QCHECK(warn.size() == 1U);
  if (warn.size() == 1U) {
    QCHECK(warn[0].level == sentinel::LogLevel::kWarn);
    QCHECK(Golden(warn[0].text) ==
           "\"event\":\"combat_tablestep_not_stock\",\"found\":\"0x0000000000000040\",\"expected\":\"0x20\","
           "\"msg\":\"the step word is not the game's 0x20, so it is left as the game made it (logged once)\"}");
  }
  End1();
}

// The game's own null result is returned unchanged, in a combat match and out of it.
void ANullResultIsReturnedAsItIs() {
  Begin1();
  Build();
  SetWhere(Where::kCombatMap);
  QCHECK(tbs::InstallAt(&Lookup, g_pins, true).installed);
  g_returnNull = true;
  QCHECK(SlotFn()(nullptr, nullptr) == nullptr);
  QCHECK(g_stockCalls == 1 && tbs::CurrentStats().rewritten == 0 && tbs::CurrentStats().unexpected == 0);
  End1();
}

// Arena shares the APK. Everywhere but a combat match the object is exactly the game's, and the cost was one read of the loaded levels.
void EveryOtherScopeLeavesTheGamesObjectAlone() {
  const Where cases[] = {Where::kArena, Where::kLobbyArenaUses, Where::kCombatLobby, Where::kCombatMapDuringArena, Where::kNothingLoaded};
  for (const Where where : cases) {
    Begin1();
    Build();
    SetWhere(where);
    QCHECK(tbs::InstallAt(&Lookup, g_pins, true).installed);
    const std::uint32_t readsBefore = ts::ScopeReadsForTest();
    void* const obj = SlotFn()(nullptr, nullptr);
    QCHECK(ts::ScopeReadsForTest() - readsBefore == 1U);
    QCHECK(obj != nullptr && g_stockCalls == 1);
    if (obj != nullptr) {
      QCHECK(StepOf(obj) == 0x20);  // the game's step
      std::free(obj);
    }
    std::free(SlotFn()(nullptr, nullptr));  // a second one: counted, and the line is still written once
    QCHECK(tbs::CurrentStats().rewritten == 0 && tbs::CurrentStats().stock == 2);
    const std::vector<Line> stock = Lines("combat_tablestep_stock");
    QCHECK(stock.size() == 1U && Lines("combat_tablestep_first").empty());
    if (stock.size() == 1U) QCHECK(Has(stock[0].text, "\"scope\":\"other\""));
    End1();
  }
  Begin1();  // no game verified yet: the game's object, and no read of any level
  Build();
  ts::SetGameForTest(&NoModelGame);
  QCHECK(tbs::InstallAt(&Lookup, g_pins, true).installed);
  void* const obj = SlotFn()(nullptr, nullptr);
  QCHECK(ts::ScopeReadsForTest() == 0U && obj != nullptr && StepOf(obj) == 0x20);
  std::free(obj);
  const std::vector<Line> stock = Lines("combat_tablestep_stock");
  QCHECK(stock.size() == 1U && Has(stock[0].text, "\"scope\":\"unknown\""));
  End1();
}

void AnInstallRefusalLeavesTheWordAlone() {
  {  // the code pin does not read back (`mov w8,#1`, the replacement and not the stock word): the module is refused whole
    Begin1();
    Build();
    g_image.Put<std::uint32_t>(0x1907130, 0x52800028);
    const nevr_quest::combat::ModuleResult r = tbs::InstallAt(&Lookup, g_pins, true);
    QCHECK(!r.installed && std::strcmp(r.status, "pinned_word_mismatch") == 0);
    QCHECK(!Hooked() && *g_image.Slot(g_pins.slot) == reinterpret_cast<void*>(&FakeCreate));
    const std::vector<Line> pin = Lines("combat_tablestep_pin");
    QCHECK(pin.size() == 1U && pin[0].level == sentinel::LogLevel::kWarn);
    if (pin.size() == 1U) QCHECK(Has(pin[0].text, "\"vaddr\":\"0x0000000001907130\"") && Has(pin[0].text, "\"have\":\"0x0000000052800028\""));
    const std::vector<Line> install = Lines("combat_tablestep_install");
    QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kError && Has(install[0].text, "\"reason\":\"pinned_word_mismatch\""));
    End1();
  }
  {  // a word that holds another function (another hook, or another build)
    Begin1();
    Build(/*foreignSlot=*/true);
    const nevr_quest::combat::ModuleResult r = tbs::InstallAt(&Lookup, g_pins, true);
    QCHECK(!r.installed && std::strcmp(r.status, "original_mismatch") == 0);
    QCHECK(!Hooked() && *g_image.Slot(g_pins.slot) == reinterpret_cast<void*>(&ForeignCreate));
    const std::vector<Line> install = Lines("combat_tablestep_install");
    QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kError);
    QCHECK(Has(install[0].text, "\"reason\":\"original_mismatch\""));
    End1();
  }
  {  // the build is not the pinned one; libr15 is not loaded
    Begin1();
    g_pins = tbs::kPins;
    g_image.Build({{tbs::kSymbol, g_pins.slot, false, false, g_pins.fn, true}}, "0000000000000000000000000000000000000000");
    QCHECK(std::strcmp(tbs::InstallAt(&Lookup, g_pins, true).status, "build_id_mismatch") == 0);
    End1();
    Begin1();
    QCHECK(std::strcmp(tbs::InstallAt(&Lookup, g_pins, true).status, "module_not_loaded") == 0);
    End1();
  }
}

void TheSwitchAndASecondStepInstallNothing() {
  {
    Begin1();
    Build();
    const nevr_quest::combat::ModuleResult r = tbs::InstallAt(&Lookup, g_pins, false);
    QCHECK(!r.installed && std::strcmp(r.status, "off") == 0 && !Hooked());
    const std::vector<Line> install = Lines("combat_tablestep_install");
    QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kInfo);
    if (install.size() == 1U) {
      QCHECK(Golden(install[0].text) ==
             "\"event\":\"combat_tablestep_install\",\"result\":\"off\",\"reason\":\"off_config\",\"status\":\"off\","
             "\"slot_vaddr\":\"0x000000000365ddd0\","
             "\"consequence\":\"combat.tablestep is false: shader sets are the game's, growth step 32\"}");
    }
    End1();
  }
  {
    Begin1();
    Build();
    QCHECK(tbs::InstallAt(&Lookup, g_pins, true).installed);
    const std::vector<Line> install = Lines("combat_tablestep_install");
    QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kInfo);
    if (install.size() == 1U) {
      QCHECK(Golden(install[0].text) ==
             "\"event\":\"combat_tablestep_install\",\"result\":\"installed\",\"reason\":\"ok\",\"status\":\"installed\","
             "\"slot_vaddr\":\"0x000000000365ddd0\","
             "\"consequence\":\"shader sets created by CreateInstance in a combat match have the table growth step 1; arena keeps the game's object\"}");
    }
    const nevr_quest::combat::ModuleResult again = tbs::InstallAt(&Lookup, g_pins, true);
    QCHECK(!again.installed && std::strcmp(again.status, "already_ran") == 0 && Hooked());
    End1();
  }
}

void TheKillFileIsOnlyAWarning() {
  Begin1();
  Build();
  const std::string dir = MakeLegacyDir("tablestep.off");
  ts::SetLegacyDirForTest(dir.c_str());
  QCHECK(tbs::InstallAt(&Lookup, g_pins, true).installed && Hooked());
  const std::vector<Line> w = Lines("combat_tablestep_legacy_switch");
  QCHECK(w.size() == 1U);
  if (w.size() == 1U) {
    QCHECK(w[0].level == sentinel::LogLevel::kWarn);
    QCHECK(Has(w[0].text, "\"file\":\"tablestep.off\"") && Has(w[0].text, "\"replaced_by\":\"combat.tablestep: false\""));
  }
  RemoveLegacyDir(dir, "tablestep.off");
  End1();
  Begin1();
  Build();
  QCHECK(tbs::InstallAt(&Lookup, g_pins, true).installed && Lines("combat_tablestep_legacy_switch").empty());
  End1();
}

}  // namespace

int main() {
  ThePinsAreTheReadelfAndDisassemblyPins();
  ACombatMatchSetsTheStepOfTheObjectTheGameReturned();
  AStepThatIsNotTheStockValueIsLeftAloneAndWarnedAboutOnce();
  ANullResultIsReturnedAsItIs();
  EveryOtherScopeLeavesTheGamesObjectAlone();
  AnInstallRefusalLeavesTheWordAlone();
  TheSwitchAndASecondStepInstallNothing();
  TheKillFileIsOnlyAWarning();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "tablestep_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::puts("tablestep_test: all checks passed");
  return 0;
}
