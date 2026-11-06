// Host test of the shader-set ConstructInstance module (src/quest/combat/shaderstep.cpp, #2177): the real handler through the real thunk,
// installed by the real GotHook into the ABS64 vtable word of a libr15 image built in memory, and called the way the game calls the factory's
// virtual: through the word. The game's function is a stand-in that returns the object it was given with the stock step 0x20 at +0x98, as the
// stock store leaves it.
//
// What it pins: in a combat match the step of the object the game built reads 1 (read back) and no neighbouring word changes; a word that is not
// 0x20 is left alone and counted; a null or unreadable object is no write; in an Echo Arena level, in a lobby arena players use, in the combat
// lobby, in a transition out of combat, with nothing loaded and with no game known the object is exactly the game's (and the scope is read once
// per construction); each refusal (a foreign word, the build, the switch, a second step) leaves the word alone; the plugin's kill file is a
// Warning; the golden lines.
//
// Built and run by `just test-quest-combat`.

#include "quest/combat/shaderstep.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "quest/tests/table_step_fixture.h"

namespace {

namespace ss = nevr_quest::combat::shaderstep;
namespace ts = nevr_quest::combat::table_step;
using namespace table_step_test;

constexpr std::size_t kObjBytes = 0x100;  // covers the step at +0x98 and the words around it
constexpr std::size_t kStepOff = 0x98;

int g_stockCalls = 0;
void* g_stockSelf = nullptr;
void* g_stockObj = nullptr;
const void* g_stockInit = nullptr;
std::uint64_t g_stockStep = 0x20;  // what the stock store writes (a test changes it to mimic a different build)

// The stock ConstructInstance: the object it is given, with the stock step.
extern "C" __attribute__((noinline)) void* FakeConstruct(void* self, void* obj, const void* init) {
  ++g_stockCalls;
  g_stockSelf = self;
  g_stockObj = obj;
  g_stockInit = init;
  auto* o = static_cast<std::uint8_t*>(obj);
  std::memset(o, 0xab, kObjBytes);
  std::memcpy(o + kStepOff, &g_stockStep, sizeof g_stockStep);
  return obj;
}
using ConstructFn = void* (*)(void*, void*, const void*);

std::uint64_t ReadWord(const std::uint8_t* b, std::size_t off) {
  std::uint64_t v;
  std::memcpy(&v, b + off, sizeof v);
  return v;
}
void WriteWord(std::uint8_t* b, std::size_t off, std::uint64_t v) { std::memcpy(b + off, &v, sizeof v); }

// A function the game does not own, in an executable mapping: a word that holds it is another hook's (or another build's), not ConstructInstance.
extern "C" __attribute__((noinline)) void* ForeignConstruct(void*, void* obj, const void*) { return obj; }

ss::Pins g_pins = ss::kPins;

void Build(bool foreignSlot = false) {
  g_pins = ss::kPins;
  g_image.Build({{ss::kSymbol, g_pins.slot, false, false, g_pins.fn, /*abs64=*/true}});
  g_pins.fn = reinterpret_cast<std::uintptr_t>(&FakeConstruct) - g_image.Base();  // base + fn is the stand-in
  *g_image.Slot(g_pins.slot) = foreignSlot ? reinterpret_cast<void*>(&ForeignConstruct) : reinterpret_cast<void*>(&FakeConstruct);
}
ConstructFn SlotFn() { return reinterpret_cast<ConstructFn>(*g_image.Slot(g_pins.slot)); }
bool Hooked() { return *g_image.Slot(g_pins.slot) == ss::Thunk::EntryAddress(); }

void Begin1() {
  Begin();
  ss::ResetForTest();
  ss::SetImageLookupForTest(&Lookup);
  ss::SetPinsForTest(&g_pins);
  g_stockCalls = 0;
  g_stockStep = 0x20;
  g_stockSelf = g_stockObj = nullptr;
  g_stockInit = nullptr;
}
void End1() {
  ss::ResetForTest();
  ss::SetImageLookupForTest(nullptr);
  ss::SetPinsForTest(nullptr);
  End();
}

void ThePinsAreTheReadelfAndDisassemblyPins() {
  QCHECK(ss::kPins.fn == 0x1906f4c && ss::kPins.slot == 0x365ddc8);
  QCHECK(std::strcmp(ss::kSymbol, "_ZN10NRadEngine8CFactoryINS_19CGShaderSetResourceEE17ConstructInstanceEPvPNS_13CPresenceInitE") == 0);
  QCHECK(ts::kShaderSetStepOffset == 0x98 && ts::kShaderSetStockStep == 0x20 && ts::kShaderSetStep == 1);
}

// The step correction on its own, on a local buffer laid out as the game's object: it writes 1 only where the game wrote 0x20, and leaves every
// other word alone.
void TheStepCorrectionWritesOneOnlyWhereTheGameWroteTheStockStep() {
  {  // the game wrote 0x20: the correction writes 1 and reads it back
    alignas(8) std::uint8_t obj[kObjBytes];
    std::memset(obj, 0xab, sizeof obj);
    WriteWord(obj, kStepOff, 0x20);
    WriteWord(obj, kStepOff - 8, 0x1122334455667788ull);  // a neighbour word: must stay
    WriteWord(obj, kStepOff + 8, 0x99aabbccddeeff00ull);  // the capacity word after the step: must stay
    QCHECK(ss::ApplyShaderSetStep(obj) == ss::StepFix::kSet);
    QCHECK(ReadWord(obj, kStepOff) == 1);
    QCHECK(ReadWord(obj, kStepOff - 8) == 0x1122334455667788ull && ReadWord(obj, kStepOff + 8) == 0x99aabbccddeeff00ull);
  }
  {  // any other word is not the game's store: nothing is written, and the word read is reported
    alignas(8) std::uint8_t obj[kObjBytes];
    std::memset(obj, 0, sizeof obj);
    WriteWord(obj, kStepOff, 0x40);
    std::uint64_t found = 0;
    QCHECK(ss::ApplyShaderSetStep(obj, &found) == ss::StepFix::kNotStock && found == 0x40);
    QCHECK(ReadWord(obj, kStepOff) == 0x40);
  }
  QCHECK(ss::ApplyShaderSetStep(nullptr) == ss::StepFix::kUnreadable);                           // a null object is unreadable
  QCHECK(ss::ApplyShaderSetStep(reinterpret_cast<void*>(0x10)) == ss::StepFix::kUnreadable);     // so is an address nothing maps
}

// In a combat match the object the game built has step 1, and the handler returns the game's object.
void ACombatMatchSetsTheStepOfTheObjectTheGameBuilt() {
  Begin1();
  Build();
  SetWhere(Where::kCombatMap);
  const nevr_quest::combat::ModuleResult r = ss::InstallAt(&Lookup, g_pins, true);
  QCHECK(r.installed && std::strcmp(r.status, "installed") == 0 && Hooked());
  alignas(8) std::uint8_t obj[kObjBytes] = {};
  int self = 0;
  const int init = 0;
  void* const built = SlotFn()(&self, obj, &init);
  QCHECK(built == obj);                                                          // the game's result, unchanged
  QCHECK(g_stockCalls == 1 && g_stockSelf == &self && g_stockObj == obj && g_stockInit == &init);  // the game's function, once, with its arguments
  QCHECK(ReadWord(obj, kStepOff) == 1);
  QCHECK(ReadWord(obj, kStepOff - 8) == 0xabababababababab && ReadWord(obj, kStepOff + 8) == 0xabababababababab);
  QCHECK(ss::CurrentStats().constructed == 1 && ss::CurrentStats().stepped == 1 && ss::CurrentStats().notStock == 0);
  const std::vector<Line> first = Lines("combat_shaderstep_first");
  QCHECK(first.size() == 1U && first[0].level == sentinel::LogLevel::kInfo && Lines("combat_shaderstep_stock").empty());
  if (first.size() == 1U) {
    QCHECK(Has(first[0].text, "\"step\":1") && Has(first[0].text, "\"n\":1"));
    QCHECK(Has(first[0].text, "the first shader set built in a combat match has the table growth step 1"));
  }
  alignas(8) std::uint8_t second[kObjBytes] = {};
  SlotFn()(&self, second, &init);
  QCHECK(ss::CurrentStats().stepped == 2 && Lines("combat_shaderstep_first").size() == 1U);  // the line is written once
  End1();
}

// A word that is not the game's 0x20 is left alone, counted, and warned about for the first five objects.
void AStepThatIsNotTheStockValueIsLeftAloneAndCounted() {
  Begin1();
  Build();
  SetWhere(Where::kCombatMap);
  QCHECK(ss::InstallAt(&Lookup, g_pins, true).installed);
  g_stockStep = 0x40;
  alignas(8) std::uint8_t obj[kObjBytes] = {};
  for (int i = 0; i < 8; ++i) SlotFn()(nullptr, obj, nullptr);
  QCHECK(ReadWord(obj, kStepOff) == 0x40);
  QCHECK(ss::CurrentStats().stepped == 0 && ss::CurrentStats().notStock == 8);
  const std::vector<Line> warn = Lines("combat_shaderstep_not_stock");
  QCHECK(warn.size() == 5U);
  if (!warn.empty()) {
    QCHECK(warn[0].level == sentinel::LogLevel::kWarn);
    QCHECK(Golden(warn[0].text) ==
           "\"event\":\"combat_shaderstep_not_stock\",\"found\":\"0x0000000000000040\",\"expected\":\"0x20\","
           "\"msg\":\"the step word is not the game's 0x20, so it is left as the game made it\"}");
  }
  End1();
}

// Arena shares the APK. Everywhere but a combat match the object is exactly the game's, and the cost was one read of the loaded levels.
void EveryOtherScopeLeavesTheGamesObjectAlone() {
  const Where cases[] = {Where::kArena, Where::kLobbyArenaUses, Where::kCombatLobby, Where::kCombatMapDuringArena, Where::kNothingLoaded};
  for (const Where where : cases) {
    Begin1();
    Build();
    SetWhere(where);
    QCHECK(ss::InstallAt(&Lookup, g_pins, true).installed);
    alignas(8) std::uint8_t obj[kObjBytes] = {};
    const std::uint32_t readsBefore = ts::ScopeReadsForTest();
    void* const built = SlotFn()(nullptr, obj, nullptr);
    QCHECK(ts::ScopeReadsForTest() - readsBefore == 1U);
    QCHECK(built == obj && g_stockCalls == 1);
    QCHECK(ReadWord(obj, kStepOff) == 0x20);  // the game's step
    alignas(8) std::uint8_t ref[kObjBytes] = {};
    FakeConstruct(nullptr, ref, nullptr);  // what the game's function makes alone
    g_stockCalls = 1;
    QCHECK(std::memcmp(obj, ref, kObjBytes) == 0);  // byte for byte the game's, all of it
    SlotFn()(nullptr, obj, nullptr);          // a second one: counted, and the line is still written once
    QCHECK(ss::CurrentStats().constructed == 0 && ss::CurrentStats().stock == 2);
    const std::vector<Line> stock = Lines("combat_shaderstep_stock");
    QCHECK(stock.size() == 1U && Lines("combat_shaderstep_first").empty());
    if (stock.size() == 1U) QCHECK(Has(stock[0].text, "\"scope\":\"other\""));
    End1();
  }
  Begin1();  // no game verified yet: the game's object, and no read of any level
  Build();
  ts::SetGameForTest(&NoModelGame);
  QCHECK(ss::InstallAt(&Lookup, g_pins, true).installed);
  alignas(8) std::uint8_t obj[kObjBytes] = {};
  SlotFn()(nullptr, obj, nullptr);
  QCHECK(ts::ScopeReadsForTest() == 0U && ReadWord(obj, kStepOff) == 0x20);
  alignas(8) std::uint8_t ref[kObjBytes] = {};
  FakeConstruct(nullptr, ref, nullptr);
  QCHECK(std::memcmp(obj, ref, kObjBytes) == 0);  // byte for byte the game's, with no game known too
  const std::vector<Line> stock = Lines("combat_shaderstep_stock");
  QCHECK(stock.size() == 1U && Has(stock[0].text, "\"scope\":\"unknown\""));
  End1();
}

// The resources of a combat map are built while its gamespace is still loading (in the in-flight table only, not yet in the two tables): that is a
// combat match, and an Echo Arena level that is loading is not, whatever else is loaded.
void AMapThatIsStillLoadingIsACombatMatchAndAnArenaLevelThatIsLoadingIsNot() {
  for (const Where where : {Where::kCombatMapLoading, Where::kCombatMapLoadingAfterCombatLobby}) {
    Begin1();
    Build();
    SetWhere(where);
    QCHECK(ss::InstallAt(&Lookup, g_pins, true).installed);
    alignas(8) std::uint8_t obj[kObjBytes] = {};
    SlotFn()(nullptr, obj, nullptr);
    QCHECK(ReadWord(obj, kStepOff) == 1 && ss::CurrentStats().stepped == 1 && ss::CurrentStats().stock == 0);
    End1();
  }
  for (const Where where : {Where::kArenaLoadingAfterCombat, Where::kArenaAndCombatLoading, Where::kLobbyLoadingAfterCombat}) {
    Begin1();
    Build();
    SetWhere(where);
    QCHECK(ss::InstallAt(&Lookup, g_pins, true).installed);
    alignas(8) std::uint8_t obj[kObjBytes] = {};
    SlotFn()(nullptr, obj, nullptr);
    QCHECK(ReadWord(obj, kStepOff) == 0x20 && ss::CurrentStats().stepped == 0 && ss::CurrentStats().stock == 1);
    End1();
  }
}

// Each change of the loaded level writes one counts line with what the level that ended cost (see meshstep_test).
void ALevelChangeWritesTheCountsOfTheLevelThatEnded() {
  Begin1();
  Build();
  SetWhere(Where::kCombatMap);
  QCHECK(ss::InstallAt(&Lookup, g_pins, true).installed);
  alignas(8) std::uint8_t o[kObjBytes] = {};
  SlotFn()(nullptr, o, nullptr);
  SlotFn()(nullptr, o, nullptr);
  QCHECK(Lines("combat_shaderstep_counts").empty());
  SetWhere(Where::kArena);
  SlotFn()(nullptr, o, nullptr);
  std::vector<Line> counts = Lines("combat_shaderstep_counts");
  QCHECK(counts.size() == 1U);
  if (counts.size() == 1U) {
    QCHECK(counts[0].level == sentinel::LogLevel::kInfo);
    QCHECK(Golden(counts[0].text) == CountsText("combat_shaderstep_counts", LevelHex("mpl_combat_gauss"), "mpl_combat_gauss", "combat", 0, 2));
  }
  SlotFn()(nullptr, o, nullptr);
  SlotFn()(nullptr, o, nullptr);
  SetWhere(Where::kNothingLoaded);
  SlotFn()(nullptr, o, nullptr);
  counts = Lines("combat_shaderstep_counts");
  QCHECK(counts.size() == 2U);
  if (counts.size() == 2U) QCHECK(Golden(counts[1].text) == CountsText("combat_shaderstep_counts", LevelHex("mpl_arena_a"), "mpl_arena_a", "other", 3, 0));
  End1();
}

void AnInstallRefusalLeavesTheWordAlone() {
  {  // a word that holds another function (another hook, or another build)
    Begin1();
    Build(/*foreignSlot=*/true);
    const nevr_quest::combat::ModuleResult r = ss::InstallAt(&Lookup, g_pins, true);
    QCHECK(!r.installed && std::strcmp(r.status, "original_mismatch") == 0);
    QCHECK(!Hooked() && *g_image.Slot(g_pins.slot) == reinterpret_cast<void*>(&ForeignConstruct));
    const std::vector<Line> install = Lines("combat_shaderstep_install");
    QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kError);
    End1();
  }
  {  // the build is not the pinned one; libr15 is not loaded
    Begin1();
    g_pins = ss::kPins;
    g_image.Build({{ss::kSymbol, g_pins.slot, false, false, g_pins.fn, true}}, "0000000000000000000000000000000000000000");
    QCHECK(std::strcmp(ss::InstallAt(&Lookup, g_pins, true).status, "build_id_mismatch") == 0);
    End1();
    Begin1();
    QCHECK(std::strcmp(ss::InstallAt(&Lookup, g_pins, true).status, "module_not_loaded") == 0);
    End1();
  }
  {  // the symbol has no word in the file at the pinned address (a pin off by one word): the manager refuses it
    Begin1();
    g_pins = ss::kPins;
    g_image.Build({{ss::kSymbol, g_pins.slot + 8, false, false, g_pins.fn, true}});
    g_pins.fn = reinterpret_cast<std::uintptr_t>(&FakeConstruct) - g_image.Base();
    *g_image.Slot(g_pins.slot + 8) = reinterpret_cast<void*>(&FakeConstruct);
    const nevr_quest::combat::ModuleResult r = ss::InstallAt(&Lookup, g_pins, true);
    QCHECK(!r.installed && std::strcmp(r.status, "slot_offset_mismatch") == 0);
    End1();
  }
}

void TheSwitchAndASecondStepInstallNothing() {
  {
    Begin1();
    Build();
    const nevr_quest::combat::ModuleResult r = ss::InstallAt(&Lookup, g_pins, false);
    QCHECK(!r.installed && std::strcmp(r.status, "off") == 0 && !Hooked());
    const std::vector<Line> install = Lines("combat_shaderstep_install");
    QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kInfo);
    if (install.size() == 1U) {
      QCHECK(Golden(install[0].text) ==
             "\"event\":\"combat_shaderstep_install\",\"result\":\"off\",\"reason\":\"off_config\",\"status\":\"off\","
             "\"slot_vaddr\":\"0x000000000365ddc8\","
             "\"consequence\":\"combat.shaderstep is false: shader sets are the game's, growth step 32\"}");
    }
    End1();
  }
  {
    Begin1();
    Build();
    QCHECK(ss::InstallAt(&Lookup, g_pins, true).installed);
    const std::vector<Line> install = Lines("combat_shaderstep_install");
    QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kInfo);
    if (install.size() == 1U) {
      QCHECK(Golden(install[0].text) ==
             "\"event\":\"combat_shaderstep_install\",\"result\":\"installed\",\"reason\":\"ok\",\"status\":\"installed\","
             "\"slot_vaddr\":\"0x000000000365ddc8\","
             "\"consequence\":\"shader sets built by ConstructInstance in a combat match have the table growth step 1; arena keeps the game's object\"}");
    }
    const nevr_quest::combat::ModuleResult again = ss::InstallAt(&Lookup, g_pins, true);
    QCHECK(!again.installed && std::strcmp(again.status, "already_ran") == 0 && Hooked());
    End1();
  }
}

void TheKillFileIsOnlyAWarning() {
  Begin1();
  Build();
  const std::string dir = MakeLegacyDir("shaderstep.off");
  ts::SetLegacyDirForTest(dir.c_str());
  QCHECK(ss::InstallAt(&Lookup, g_pins, true).installed && Hooked());
  const std::vector<Line> w = Lines("combat_shaderstep_legacy_switch");
  QCHECK(w.size() == 1U);
  if (w.size() == 1U) {
    QCHECK(w[0].level == sentinel::LogLevel::kWarn);
    QCHECK(Has(w[0].text, "\"file\":\"shaderstep.off\"") && Has(w[0].text, "\"replaced_by\":\"combat.shaderstep: false\""));
  }
  RemoveLegacyDir(dir, "shaderstep.off");
  End1();
  Begin1();
  Build();
  QCHECK(ss::InstallAt(&Lookup, g_pins, true).installed && Lines("combat_shaderstep_legacy_switch").empty());
  End1();
}

}  // namespace

int main() {
  ThePinsAreTheReadelfAndDisassemblyPins();
  TheStepCorrectionWritesOneOnlyWhereTheGameWroteTheStockStep();
  ACombatMatchSetsTheStepOfTheObjectTheGameBuilt();
  AStepThatIsNotTheStockValueIsLeftAloneAndCounted();
  EveryOtherScopeLeavesTheGamesObjectAlone();
  AMapThatIsStillLoadingIsACombatMatchAndAnArenaLevelThatIsLoadingIsNot();
  ALevelChangeWritesTheCountsOfTheLevelThatEnded();
  AnInstallRefusalLeavesTheWordAlone();
  TheSwitchAndASecondStepInstallNothing();
  TheKillFileIsOnlyAWarning();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "shaderstep_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::puts("shaderstep_test: all checks passed");
  return 0;
}
