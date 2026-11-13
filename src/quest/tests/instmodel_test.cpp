// Host test of the instanced-model table-step module (src/quest/combat/instmodel.cpp, #2177): the real handler through the real thunk, installed
// by the real GotHook into the JUMP_SLOT of a libr15 image built in memory, and called the way the game's PLT stub calls it: through the slot. The
// game's constructor is a stand-in that writes what disassembly 0x1a078dc writes into the fields the module reads (the eight 0x20 steps, zeroed
// capacities and counts).
//
// What it pins: in a combat match the object is the game's constructor's with the eight growth steps set to 1 and nothing else changed, a step
// that does not hold 0x20 is left as it is, and no step holding 0x20 means no write; in an Echo Arena level, in a lobby arena players use, in
// the combat lobby, in a transition out of combat, with nothing loaded and with no game known the object is exactly the game's (and the scope is
// read once per construction); the log cap; each refusal (a pinned word, a foreign slot word, the build, the switch, a second step) leaves the slot
// alone; the plugin's kill file is a Warning; the golden lines.
//
// Built and run by `just test-quest-combat`.

#include "quest/combat/instmodel.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "quest/tests/table_step_fixture.h"

namespace {

namespace im = nevr_quest::combat::instmodel;
namespace ts = nevr_quest::combat::table_step;
using namespace table_step_test;

constexpr unsigned long kMembers[] = {0x78, 0xb0, 0xe8, 0x120, 0x158, 0x190, 0x1c8, 0x200};
constexpr unsigned long kSteps[] = {0x98, 0xd0, 0x108, 0x140, 0x178, 0x1b0, 0x1e8, 0x220};
constexpr unsigned long kObjSize = 0x240;

alignas(16) unsigned char g_obj[kObjSize];
int g_stockCalls = 0;
const void* g_stockInit = nullptr;
// What the stock constructor leaves in the fields this module reads.
unsigned long g_stockStepValue = 0x20;
unsigned long g_stockStepIndex = ~0UL;  // when not ~0UL, that step gets g_stockStepOverride instead
unsigned long g_stockStepOverride = 0x21;

extern "C" __attribute__((noinline)) void FakeStockCtor(void* self, const void* init) {
  ++g_stockCalls;
  g_stockInit = init;
  if (self == nullptr) return;  // the stand-in for the game's own handling of a null object
  unsigned char* o = static_cast<unsigned char*>(self);
  std::memset(o, 0xAB, kObjSize);
  for (const unsigned long m : kMembers) {
    const unsigned long zero = 0;
    std::memcpy(o + m + 0x28, &zero, sizeof zero);  // the capacity the constructor zeroes
  }
  for (std::size_t i = 0; i < sizeof kSteps / sizeof kSteps[0]; ++i) {
    unsigned long v = g_stockStepValue;
    if (i == g_stockStepIndex) v = g_stockStepOverride;
    std::memcpy(o + kSteps[i], &v, sizeof v);
  }
}

// A function the game does not own, in an executable mapping: a slot that holds it is another hook's (or another build's), not the constructor.
extern "C" __attribute__((noinline)) void ForeignCtor(void*, const void*) {}

im::Pins g_pins = im::kPins;
using CtorFn = void (*)(void*, const void*);

void Build(bool foreignSlot = false) {
  g_pins = im::kPins;
  g_image.Build({{im::kSymbol, g_pins.slot, false, false, g_pins.ctor}});
  g_pins.ctor = reinterpret_cast<std::uintptr_t>(&FakeStockCtor) - g_image.Base();  // base + ctor is the stand-in
  PutWords(im::kWords);
  *g_image.Slot(g_pins.slot) = foreignSlot ? reinterpret_cast<void*>(&ForeignCtor) : reinterpret_cast<void*>(&FakeStockCtor);
}
CtorFn SlotFn() { return reinterpret_cast<CtorFn>(*g_image.Slot(g_pins.slot)); }
bool Hooked() { return *g_image.Slot(g_pins.slot) == im::Thunk::EntryAddress(); }

void Begin1() {
  Begin();
  im::ResetForTest();
  im::SetImageLookupForTest(&Lookup);
  im::SetPinsForTest(&g_pins);
  g_stockCalls = 0;
  g_stockStepValue = 0x20;
  g_stockStepIndex = ~0UL;
  g_stockStepOverride = 0x21;
  std::memset(g_obj, 0, sizeof g_obj);
}
void End1() {
  im::ResetForTest();
  im::SetImageLookupForTest(nullptr);
  im::SetPinsForTest(nullptr);
  End();
}
unsigned long Read(unsigned long off) {
  unsigned long v = 0;
  std::memcpy(&v, g_obj + off, sizeof v);
  return v;
}
// The stock object, built by the stand-in alone.
void StockObject(unsigned char* out) {
  const int calls = g_stockCalls;
  FakeStockCtor(out, nullptr);
  g_stockCalls = calls;
}
const int kInitMarker = 0;
void Construct() { SlotFn()(g_obj, &kInitMarker); }

void ThePinsAreTheReadelfAndDisassemblyPins() {
  QCHECK(im::kPins.ctor == 0x19078dc && im::kPins.slot == 0x36c5c68 && im::kPins.stepWord == 0x1907958);
  QCHECK(im::kWordCount == 2 && im::kWords[0].vaddr == 0x19078dc && im::kWords[0].word == 0xf81e0ff4);
  QCHECK(im::kWords[1].vaddr == 0x1907958 && im::kWords[1].word == 0x52800414);
  QCHECK(std::strcmp(im::kSymbol, "_ZN10NRadEngine24CGInstancedModelResourceC2ERKNS_13CPresenceInitE") == 0);
}

// In a combat match the game's constructor runs once and the eight steps read 1; nothing else of the object changes.
void ACombatMatchSetsTheEightStepsAndNothingElse() {
  Begin1();
  Build();
  SetWhere(Where::kCombatMap);
  const nevr_quest::combat::ModuleResult r = im::InstallAt(&Lookup, g_pins, true);
  QCHECK(r.installed && std::strcmp(r.status, "installed") == 0 && Hooked());
  unsigned char expect[kObjSize];
  StockObject(expect);
  for (unsigned long s : kSteps) {
    unsigned long one = 1;
    std::memcpy(expect + s, &one, sizeof one);
  }
  Construct();
  QCHECK(g_stockCalls == 1 && g_stockInit == &kInitMarker);  // the stock constructor runs exactly once, with the game's init
  for (unsigned long s : kSteps) QCHECK(Read(s) == 1);
  QCHECK(std::memcmp(g_obj, expect, kObjSize) == 0);  // the stock object with the eight steps set to 1, nothing else
  for (unsigned long m : kMembers) QCHECK(Read(m + 0x28) == 0);  // the capacities the constructor zeroes stay zero
  QCHECK(im::CurrentStats().objects == 1 && im::CurrentStats().stepsSet == 8 && im::CurrentStats().mismatch == 0 && im::CurrentStats().stock == 0);
  const std::vector<Line> steps = Lines("combat_instmodel_steps");
  QCHECK(steps.size() == 1U && Lines("combat_instmodel_stock").empty() && Lines("combat_instmodel_mismatch").empty());
  if (steps.size() == 1U) {
    QCHECK(steps[0].level == sentinel::LogLevel::kDebug);  // per-item detail is Debug; combat_instmodel_counts is the Info summary
    QCHECK(Golden(steps[0].text) ==
           "\"event\":\"combat_instmodel_steps\",\"steps_set\":8,\"steps_mismatch\":0,\"step\":1,"
           "\"msg\":\"an instanced-model resource built in a combat match has the table growth step 1\"}");
  }
  End1();
}

void AStepThatDoesNotHoldTheStockValueIsLeftAlone() {
  Begin1();
  Build();
  SetWhere(Where::kCombatMap);
  QCHECK(im::InstallAt(&Lookup, g_pins, true).installed);
  g_stockStepIndex = 6;  // the step at 0x1e8 holds 0x21 after the stock constructor
  Construct();
  QCHECK(Read(0x1e8) == 0x21);  // left as it is
  for (std::size_t i = 0; i < sizeof kSteps / sizeof kSteps[0]; ++i) {
    if (i != 6) QCHECK(Read(kSteps[i]) == 1);  // the other seven still become 1
  }
  QCHECK(im::CurrentStats().stepsSet == 7 && im::CurrentStats().mismatch == 1);
  const std::vector<Line> mismatch = Lines("combat_instmodel_mismatch");
  QCHECK(mismatch.size() == 1U);
  if (mismatch.size() == 1U) {
    QCHECK(mismatch[0].level == sentinel::LogLevel::kWarn);
    QCHECK(Golden(mismatch[0].text) ==
           "\"event\":\"combat_instmodel_mismatch\",\"offset\":\"0x00000000000001e8\",\"found\":\"0x0000000000000021\","
           "\"expected\":\"0x20\",\"msg\":\"a growth step does not hold the stock value, so it is left as the game made it\"}");
  }
  End1();
}

void NoStepHoldingTheStockValueChangesNothing() {
  Begin1();
  Build();
  SetWhere(Where::kCombatMap);
  QCHECK(im::InstallAt(&Lookup, g_pins, true).installed);
  g_stockStepValue = 0x40;  // no step holds 0x20: nothing is written
  Construct();
  for (unsigned long s : kSteps) QCHECK(Read(s) == 0x40);
  QCHECK(im::CurrentStats().stepsSet == 0 && im::CurrentStats().mismatch == 8);
  End1();
}

// Arena shares the APK. Everywhere but a combat match the object is exactly the game's, the game's constructor ran once, and the cost was one
// read of the loaded levels.
void EveryOtherScopeLeavesTheGamesObjectAlone() {
  const Where cases[] = {Where::kArena, Where::kLobbyArenaUses, Where::kCombatLobby, Where::kCombatMapDuringArena, Where::kNothingLoaded};
  for (const Where where : cases) {
    Begin1();
    Build();
    SetWhere(where);
    QCHECK(im::InstallAt(&Lookup, g_pins, true).installed);
    unsigned char expect[kObjSize];
    StockObject(expect);
    const std::uint32_t readsBefore = ts::ScopeReadsForTest();
    Construct();
    QCHECK(ts::ScopeReadsForTest() - readsBefore == 1U);
    QCHECK(g_stockCalls == 1);
    Construct();  // a second one: counted, and the line is still written once
    QCHECK(std::memcmp(g_obj, expect, kObjSize) == 0);  // byte for byte the game's
    for (unsigned long s : kSteps) QCHECK(Read(s) == 0x20);
    QCHECK(im::CurrentStats().objects == 0 && im::CurrentStats().stock == 2);
    const std::vector<Line> stock = Lines("combat_instmodel_stock");
    QCHECK(stock.size() == 1U && Lines("combat_instmodel_steps").empty());
    if (stock.size() == 1U) QCHECK(Has(stock[0].text, "\"scope\":\"other\""));
    End1();
  }
  Begin1();  // no game verified yet: the game's object, and no read of any level
  Build();
  ts::SetGameForTest(&NoModelGame);
  QCHECK(im::InstallAt(&Lookup, g_pins, true).installed);
  Construct();
  QCHECK(ts::ScopeReadsForTest() == 0U && g_stockCalls == 1 && Read(0x98) == 0x20);
  const std::vector<Line> stock = Lines("combat_instmodel_stock");
  QCHECK(stock.size() == 1U && Has(stock[0].text, "\"scope\":\"unknown\""));
  End1();
}

// A null object is the game's to handle: the scope is not read for it and the steps are not touched.
void ANullObjectGoesStraightToTheGame() {
  Begin1();
  Build();
  SetWhere(Where::kCombatMap);
  QCHECK(im::InstallAt(&Lookup, g_pins, true).installed);
  SlotFn()(nullptr, nullptr);
  QCHECK(g_stockCalls == 1 && ts::ScopeReadsForTest() == 0U && im::CurrentStats().objects == 0 && im::CurrentStats().stock == 1);
  const std::vector<Line> stock = Lines("combat_instmodel_stock");
  QCHECK(stock.size() == 1U && Has(stock[0].text, "\"scope\":\"unknown\""));
  End1();
}

// One line per constructed object is too many: the first 16 are written, then one Warning that the cap was reached, then silence; the counts go on.
void TheStepLinesAreCappedAtSixteen() {
  Begin1();
  Build();
  SetWhere(Where::kCombatMap);
  QCHECK(im::InstallAt(&Lookup, g_pins, true).installed);
  for (int i = 0; i < 40; ++i) Construct();
  QCHECK(Lines("combat_instmodel_steps").size() == 16U);
  const std::vector<Line> cap = Lines("combat_instmodel_log_cap");
  QCHECK(cap.size() == 1U && cap[0].level == sentinel::LogLevel::kInfo && Has(cap[0].text, "\"cap\":16"));
  QCHECK(im::CurrentStats().objects == 40 && im::CurrentStats().stepsSet == 320);
  g_stockStepIndex = 0;  // and a step that does not hold 0x20 in each of the next 40: the Warnings stop at 16 too
  for (int i = 0; i < 40; ++i) Construct();
  QCHECK(Lines("combat_instmodel_mismatch").size() == 16U && im::CurrentStats().mismatch == 40);
  End1();
}

// The resources of a combat map are built while its gamespace is still loading (in the in-flight table only, not yet in the two tables): that is a
// combat match, and an Echo Arena level that is loading is not, whatever else is loaded.
void AMapThatIsStillLoadingIsACombatMatchAndAnArenaLevelThatIsLoadingIsNot() {
  for (const Where where : {Where::kCombatMapLoading, Where::kCombatMapLoadingAfterCombatLobby, Where::kCombatMapAndAFreshFiber}) {
    Begin1();
    Build();
    SetWhere(where);
    QCHECK(im::InstallAt(&Lookup, g_pins, true).installed);
    Construct();
    for (unsigned long s : kSteps) QCHECK(Read(s) == 1);
    QCHECK(im::CurrentStats().objects == 1 && im::CurrentStats().stock == 0);
    End1();
  }
  for (const Where where : {Where::kArenaLoadingAfterCombat, Where::kArenaAndCombatLoading, Where::kLobbyLoadingAfterCombat}) {
    Begin1();
    Build();
    SetWhere(where);
    QCHECK(im::InstallAt(&Lookup, g_pins, true).installed);
    Construct();
    for (unsigned long s : kSteps) QCHECK(Read(s) == 0x20);
    QCHECK(im::CurrentStats().objects == 0 && im::CurrentStats().stock == 1);
    End1();
  }
}

// Each change of the loaded level writes one counts line with what the level that ended cost (see meshstep_test).
void ALevelChangeWritesTheCountsOfTheLevelThatEnded() {
  Begin1();
  Build();
  SetWhere(Where::kCombatMap);
  QCHECK(im::InstallAt(&Lookup, g_pins, true).installed);
  Construct();
  Construct();
  QCHECK(Lines("combat_instmodel_counts").empty());
  SetWhere(Where::kArena);
  Construct();
  std::vector<Line> counts = Lines("combat_instmodel_counts");
  QCHECK(counts.size() == 1U);
  if (counts.size() == 1U) {
    QCHECK(counts[0].level == sentinel::LogLevel::kInfo);
    QCHECK(Golden(counts[0].text) == CountsText("combat_instmodel_counts", LevelHex("mpl_combat_gauss"), "mpl_combat_gauss", "combat", 0, 2));
  }
  Construct();
  Construct();
  SetWhere(Where::kNothingLoaded);
  Construct();
  counts = Lines("combat_instmodel_counts");
  QCHECK(counts.size() == 2U);
  if (counts.size() == 2U) QCHECK(Golden(counts[1].text) == CountsText("combat_instmodel_counts", LevelHex("mpl_arena_a"), "mpl_arena_a", "other", 3, 0));
  End1();
}

void AnInstallRefusalLeavesTheSlotAndTheConstructorAlone() {
  for (const unsigned long long vaddr : {0x19078dcULL, 0x1907958ULL}) {  // each pinned word, alone
    Begin1();
    Build();
    g_image.Put<std::uint32_t>(vaddr, 0x52800034);
    const nevr_quest::combat::ModuleResult r = im::InstallAt(&Lookup, g_pins, true);
    QCHECK(!r.installed && std::strcmp(r.status, "pinned_word_mismatch") == 0);
    QCHECK(!Hooked() && *g_image.Slot(g_pins.slot) == reinterpret_cast<void*>(&FakeStockCtor));
    const std::vector<Line> pin = Lines("combat_instmodel_pin");
    QCHECK(pin.size() == 1U && pin[0].level == sentinel::LogLevel::kWarn);
    const std::vector<Line> install = Lines("combat_instmodel_install");
    QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kError && Has(install[0].text, "\"reason\":\"pinned_word_mismatch\""));
    End1();
  }
  {  // a slot that holds another function (another hook, or another build)
    Begin1();
    Build(/*foreignSlot=*/true);
    const nevr_quest::combat::ModuleResult r = im::InstallAt(&Lookup, g_pins, true);
    QCHECK(!r.installed && std::strcmp(r.status, "original_mismatch") == 0);
    QCHECK(!Hooked() && *g_image.Slot(g_pins.slot) == reinterpret_cast<void*>(&ForeignCtor));
    const std::vector<Line> install = Lines("combat_instmodel_install");
    QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kError);
    End1();
  }
  {  // the build is not the pinned one; libr15 is not loaded
    Begin1();
    g_pins = im::kPins;
    g_image.Build({{im::kSymbol, g_pins.slot, false, false, g_pins.ctor}}, "0000000000000000000000000000000000000000");
    QCHECK(std::strcmp(im::InstallAt(&Lookup, g_pins, true).status, "build_id_mismatch") == 0);
    End1();
    Begin1();
    QCHECK(std::strcmp(im::InstallAt(&Lookup, g_pins, true).status, "module_not_loaded") == 0);
    End1();
  }
}

void TheSwitchAndASecondStepInstallNothing() {
  {
    Begin1();
    Build();
    const nevr_quest::combat::ModuleResult r = im::InstallAt(&Lookup, g_pins, false);
    QCHECK(!r.installed && r.off && std::strcmp(r.status, "off") == 0 && !Hooked());
    const std::vector<Line> install = Lines("combat_instmodel_install");
    QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kInfo);
    if (install.size() == 1U) {
      QCHECK(Golden(install[0].text) ==
             "\"event\":\"combat_instmodel_install\",\"result\":\"off\",\"reason\":\"off_config\",\"status\":\"off\","
             "\"slot_vaddr\":\"0x00000000036c5c68\","
             "\"consequence\":\"combat.instmodel is false: instanced-model resources are the game's, growth step 32\"}");
    }
    End1();
  }
  {
    Begin1();
    Build();
    QCHECK(im::InstallAt(&Lookup, g_pins, true).installed);
    const std::vector<Line> install = Lines("combat_instmodel_install");
    QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kInfo);
    if (install.size() == 1U) {
      QCHECK(Golden(install[0].text) ==
             "\"event\":\"combat_instmodel_install\",\"result\":\"installed\",\"reason\":\"ok\",\"status\":\"installed\","
             "\"slot_vaddr\":\"0x00000000036c5c68\","
             "\"consequence\":\"instanced-model resources built in a combat match have the table growth step 1; arena keeps the game's object\"}");
    }
    const nevr_quest::combat::ModuleResult again = im::InstallAt(&Lookup, g_pins, true);
    QCHECK(!again.installed && std::strcmp(again.status, "already_ran") == 0 && Hooked());
    End1();
  }
}

void TheKillFileIsOnlyAWarning() {
  Begin1();
  Build();
  const std::string dir = MakeLegacyDir("instmodel.off");
  ts::SetLegacyDirForTest(dir.c_str());
  QCHECK(im::InstallAt(&Lookup, g_pins, true).installed && Hooked());
  const std::vector<Line> w = Lines("combat_instmodel_legacy_switch");
  QCHECK(w.size() == 1U);
  if (w.size() == 1U) {
    QCHECK(w[0].level == sentinel::LogLevel::kWarn);
    QCHECK(Has(w[0].text, "\"file\":\"instmodel.off\"") && Has(w[0].text, "\"replaced_by\":\"combat.instmodel: false\""));
  }
  RemoveLegacyDir(dir, "instmodel.off");
  End1();
  Begin1();
  Build();
  QCHECK(im::InstallAt(&Lookup, g_pins, true).installed && Lines("combat_instmodel_legacy_switch").empty());
  End1();
}

}  // namespace

int main() {
  ThePinsAreTheReadelfAndDisassemblyPins();
  ACombatMatchSetsTheEightStepsAndNothingElse();
  AStepThatDoesNotHoldTheStockValueIsLeftAlone();
  NoStepHoldingTheStockValueChangesNothing();
  EveryOtherScopeLeavesTheGamesObjectAlone();
  AMapThatIsStillLoadingIsACombatMatchAndAnArenaLevelThatIsLoadingIsNot();
  ALevelChangeWritesTheCountsOfTheLevelThatEnded();
  ANullObjectGoesStraightToTheGame();
  TheStepLinesAreCappedAtSixteen();
  AnInstallRefusalLeavesTheSlotAndTheConstructorAlone();
  TheSwitchAndASecondStepInstallNothing();
  TheKillFileIsOnlyAWarning();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "instmodel_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::puts("instmodel_test: all checks passed");
  return 0;
}
