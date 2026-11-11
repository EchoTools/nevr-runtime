// Host test of fxtrail (src/quest/combat/fxtrail.cpp, #2178): the real handler through the thunk, installed by the real GotHook into the JUMP_SLOT
// of a libr15 image built in memory, and called the way the game calls it: through the slot. The game side is a model: a fake
// CParticleEffectCS laid out the way EffectTime reads it (index table at +0xc8, data array at +0xf8, 0x18 bytes an entry, the instance at +0x10)
// and a stand-in for EffectTime itself. The level the guard is scoped by is the real combat level answer (combat_scope.h), refreshed through the tick's
// callback against a model of the game's gamespace tables (combat_game_model.h, combat_scope_world.h).
//
// What it pins (the q15t plugin's fxtrail_test assertions, on the new fixture): an effect with no instance answers FLT_MAX with the game's function
// called once with the same arguments, an effect with an instance answers the game's value, the lookup's stride and index mask, the ended line at
// 1, 2, 4 ..., a failed read answers the game's value, a slot that does not hold EffectTime is refused and left alone with the pin named, every
// code word flipped in turn refuses the install, and an unreadable word does too. New with the move: the guard acts only while a combat map is
// loaded (an arena match, every lobby, a look that never ran and a combat map with an arena level also loaded read nothing and answer the game's
// value, with no read of the game's memory and no level scan, counted), a true answer older than kStaleAfterMs is not believed, the switch, the legacy
// switch file (a Warning, never a switch), the missing image and build ID, the stats lines, and a scope that cannot change (the install is refused).
//
// Built and run by `just test-quest-combat`.

#include "quest/combat/fxtrail.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "quest/combat/combat_level.h"
#include "quest/combat/combat_scope.h"
#include "quest/combat/combat_tick.h"
#include "quest/combat/legacy_switch.h"
#include "quest/combat/safe_read.h"
#include "quest/tests/combat_libr15_image.h"
#include "quest/tests/combat_log_capture.h"
#include "quest/tests/combat_pin_check.h"
#include "quest/tests/combat_pins.h"
#include "quest/tests/combat_scope_world.h"
#include "quest/tests/test_check.h"

namespace {

namespace fx = nevr_quest::combat::fxtrail;
namespace scope = nevr_quest::combat::scope;
namespace tick = nevr_quest::combat::tick;
namespace pinned = sentinel::pinned;
namespace world = combat_test::scope_world;
using combat_test::Has;
using combat_test::Image;
using combat_test::Line;
using combat_test::Lines;
using nevr_quest::combat::ModuleResult;
using nevr_quest::combat::pinned::Word;
// The tables themselves, not the module's own `kWordPins` (which a bug could shorten): the test puts every pinned word in the image and flips every one.
const nevr_quest::combat::pinned::Pins kAllWords = {fx::kWords, fx::kWordCount, nullptr, 0};

using EffectTimeFn = float (*)(const void*, std::uint64_t);

// ---- the fake engine ------------------------------------------------------------------------------------------------------------

alignas(16) std::uintptr_t g_cs[0x120 / 8];  // +0xc8 index table, +0xf8 data
alignas(16) std::uint16_t g_index[8];
struct Data {
  std::uint8_t b[0x18];
};
alignas(16) Data g_data[0x120];

int g_realCalls = 0;
const void* g_realCs = nullptr;
std::uint64_t g_realHandle = 0;
constexpr float kRealTime = 0.5f;

extern "C" __attribute__((noinline)) float RealEffectTime(const void* cs, std::uint64_t handle) {
  ++g_realCalls;
  g_realCs = cs;
  g_realHandle = handle;
  return kRealTime;
}
extern "C" __attribute__((noinline)) float OtherFn(const void*, std::uint64_t) { return 9.0f; }

void SetInstance(int dataSlot, std::uintptr_t instance) { std::memcpy(g_data[dataSlot].b + 0x10, &instance, sizeof(instance)); }

// Handle index 2: the u16 at byte offset 4 * 2 of the index table maps to data[3]; every other data entry gets `others`, data[3] gets `mine`.
void World(std::uintptr_t mine, std::uintptr_t others) {
  std::memset(g_cs, 0, sizeof(g_cs));
  std::memset(g_index, 0, sizeof(g_index));
  std::memset(g_data, 0, sizeof(g_data));
  g_cs[0xc8 / 8] = reinterpret_cast<std::uintptr_t>(g_index);
  g_cs[0xf8 / 8] = reinterpret_cast<std::uintptr_t>(g_data);
  g_index[4] = 3;  // the u16 at byte 4 * 2
  for (int i = 0; i < 0x120; ++i) SetInstance(i, others);
  SetInstance(3, mine);
  g_realCalls = 0;
}

constexpr std::uint64_t kHandle = 2ULL | (7ULL << 16) | (0xabcdULL << 32);  // index 2, a generation and junk above bit 16

// ---- the libr15 image ----------------------------------------------------------------------------------------------------------

Image g_image;
fx::Pins g_pins = fx::kPins;
bool Lookup(const char* module, sentinel::ElfImage* out) {
  if (!g_image.Present() || std::strcmp(module, pinned::kLibR15) != 0) return false;
  *out = g_image.Elf();
  return true;
}

// A libr15 holding the pinned words and the slot, which holds `slotWord` (the stand-in when 0).
void Build(std::uintptr_t slotWord = 0) {
  g_pins = fx::kPins;
  g_image.Build({{fx::kEffectTimeSymbol, g_pins.slot, false, false, g_pins.fn}, world::TickRow()});
  world::PutTickSlot(g_image);
  // base + fn is the stand-in's address: the real function is not in the image, so the pin holds the stand-in's offset.
  g_pins.fn = reinterpret_cast<std::uintptr_t>(&RealEffectTime) - g_image.Base();
  combat_test::PutPins(g_image, &kAllWords, 1);
  *g_image.Slot(g_pins.slot) = slotWord != 0 ? reinterpret_cast<void*>(slotWord) : reinterpret_cast<void*>(&RealEffectTime);
}
EffectTimeFn Slot() { return reinterpret_cast<EffectTimeFn>(*g_image.Slot(g_pins.slot)); }
bool Hooked() { return *g_image.Slot(g_pins.slot) == fx::EffectTimeThunk::EntryAddress(); }
bool SlotIsTheGames() { return *g_image.Slot(g_pins.slot) == reinterpret_cast<void*>(&RealEffectTime); }

// A fresh world: the module forgotten, a libr15 holding the pinned words, the slot and the tick's slot, and the capture started. `scopeLive`: the combat tick
// is installed on the image and the scope row has registered the refresh (what the shipped table does before this module).
std::atomic<std::uint64_t> g_reads{0};  // every SafeRead the module makes (safe_read.h's test counter)
void Begin(bool scopeLive = true) {
  combat_test::StartCapture();
  fx::ResetForTest();
  world::Reset();
  Build();
  g_reads = 0;
  nevr_quest::combat::SetReadCounterForTest(&g_reads);
  if (scopeLive) {
    QCHECK(world::StartScope(&Lookup));
    combat_test::Captured().clear();
  }
}
void End() {
  fx::ResetForTest();
  world::Reset();
  g_image.Drop();
  nevr_quest::combat::SetReadCounterForTest(nullptr);
  combat_test::StopCapture();
}

ModuleResult Install(bool on = true, const char* legacyDir = nullptr) {
  nevr_quest::combat::legacy::SetDirectoryForTest(legacyDir);  // where the plugin's fxtrail.off would sit (nullptr: production, which finds none on the host)
  const ModuleResult r = fx::InstallAt(&Lookup, g_pins, on);
  nevr_quest::combat::legacy::SetDirectoryForTest(nullptr);
  return r;
}

// Installs on a fresh world and loads the named levels.
void InstalledIn(std::initializer_list<const char*> levels) {
  Begin();
  QCHECK(Install().installed);
  world::Levels(levels);
  g_reads = 0;
}

// ---- the tests ------------------------------------------------------------------------------------------------------------------

void ThePinsAreTheReadelfAndDisassemblyPins() {
  QCHECK(fx::kEffectTimeSlotVaddr == 0x36e7b98 && fx::kEffectTimeVaddr == 0x16647e4);
  QCHECK(std::strcmp(fx::kEffectTimeSymbol, "_ZNK10NRadEngine17CParticleEffectCS10EffectTimeENS_18SCompactPoolHandleE") == 0);
  QCHECK(fx::kOffIndex == 0xc8 && fx::kOffData == 0xf8 && fx::kDataStride == 0x18 && fx::kOffInstance == 0x10);
  static const Word kWant[11] = {{"", 0x16647e4, 0xf9406408}, {"", 0x16647e8, 0x92403c29}, {"", 0x16647ec, 0xd37ef529}, {"", 0x16647f0, 0x5280030a},
                                 {"", 0x16647f4, 0x78696908}, {"", 0x16647f8, 0xf9407c09}, {"", 0x1664800, 0x9b0a2508}, {"", 0x1664804, 0xf9400908},
                                 {"", 0x1664808, 0xb4000068}, {"", 0x166480c, 0xbd434100}, {"", 0x1664810, 0xd65f03c0}};
  QCHECK(fx::kWordCount == 11);
  for (std::size_t i = 0; i < 11; ++i) QCHECK(fx::kWords[i].vaddr == kWant[i].vaddr && fx::kWords[i].word == kWant[i].word && fx::kWords[i].what[0] != '\0');
  QCHECK(combat_test::Fingerprint(fx::kWords, fx::kWordCount) == combat_test::Fingerprint(kWant, 11));
}

void TheThunkTakesTheSlotAndTheLineSaysSo() {
  Begin();
  const ModuleResult r = Install();
  QCHECK(r.installed && std::strcmp(r.status, "installed") == 0 && !r.off);
  QCHECK(Hooked() && tick::Count() == 1);  // the scope row registered the refresh with the tick, once; the module adds no callback
  const std::vector<Line> install = Lines("combat_fxtrail_install");
  QCHECK(install.size() == 1U);
  if (install.size() == 1U) {
    QCHECK(install[0].level == sentinel::LogLevel::kInfo);
    QCHECK(combat_test::Golden(install[0].text) ==
           "\"event\":\"combat_fxtrail_install\",\"result\":\"installed\",\"reason\":\"ok\",\"hook\":\"ok\",\"scope\":\"combat_map\","
           "\"consequence\":\"a bullet whose trail effect has no instance ends its trail, so it can be freed, while a combat map is loaded\"}");
  }
  End();
}

// 1. An effect with no instance, in a combat map: the game's function runs once with the same arguments and the answer is FLT_MAX.
void AnEffectWithNoInstanceEndsItsTrailInACombatMap() {
  InstalledIn({"mpl_combat_dyson"});
  World(0, 0x1234);
  const float t = Slot()(g_cs, kHandle);
  QCHECK(t == FLT_MAX);
  QCHECK(g_realCalls == 1 && g_realCs == g_cs && g_realHandle == kHandle);
  QCHECK(fx::CurrentStats().ended == 1 && fx::CurrentStats().inCombat == 1 && fx::CurrentStats().calls == 1);
  End();
}

// 2. An effect with an instance: the game's value, untouched. Every other data entry is empty, so a wrong stride or index mask is seen.
void AnEffectWithAnInstanceKeepsTheGamesValue() {
  InstalledIn({"mpl_combat_gauss"});
  World(0x1234, 0);
  const float t = Slot()(g_cs, kHandle);
  QCHECK(t == kRealTime);
  QCHECK(g_realCalls == 1 && fx::CurrentStats().ended == 0 && fx::CurrentStats().inCombat == 1);
  {  // the data index is a u16 (a handle whose entry is past 255 is found): handle index 3 -> data[0x103], whose instance is set; data[3] has none
    World(0, 0);
    g_index[6] = 0x103;
    SetInstance(0x103, 0x77);
    QCHECK(Slot()(g_cs, 3ULL | (7ULL << 16)) == kRealTime && fx::CurrentStats().ended == 0);
  }
  End();
}

// 3. The call path counts; the ended line is written at 1, 2, 4 ... only.
void TheEndedLineIsWrittenAtPowersOfTwoOnly() {
  InstalledIn({"mpl_combat_fission"});
  World(0, 0x1234);
  for (int i = 0; i < 5; ++i) Slot()(g_cs, kHandle);
  QCHECK(fx::CurrentStats().ended == 5);
  const std::vector<Line> ended = Lines("combat_fxtrail_ended");
  QCHECK(ended.size() == 3U);
  if (ended.size() == 3U) {
    QCHECK(ended[0].level == sentinel::LogLevel::kInfo);
    QCHECK(Has(ended[0].text, "\"n\":1,") && Has(ended[1].text, "\"n\":2,") && Has(ended[2].text, "\"n\":4,"));
    QCHECK(combat_test::Golden(ended[0].text) ==
           "\"event\":\"combat_fxtrail_ended\",\"n\":1,\"consequence\":\"an effect with no instance answered FLT_MAX, so its bullet's trail "
           "slot is finished and the bullet can be freed\"}");
  }
  End();
}

// 4. Any read that fails: the game's value, never FLT_MAX.
void AFailedReadKeepsTheGamesValue() {
  InstalledIn({"mpl_combat_dyson"});
  World(0, 0);
  QCHECK(Slot()(reinterpret_cast<const void*>(0x20), kHandle) == kRealTime);  // cs itself unreadable
  QCHECK(Slot()(nullptr, kHandle) == kRealTime);
  World(0, 0);
  g_cs[0xc8 / 8] = 0x30;  // index table unreadable
  QCHECK(Slot()(g_cs, kHandle) == kRealTime);
  World(0, 0);
  g_cs[0xf8 / 8] = 0x30;  // data array unreadable
  QCHECK(Slot()(g_cs, kHandle) == kRealTime);
  QCHECK(fx::CurrentStats().ended == 0);  // a failed read is not an ended trail
  End();
}

// The scope: only a combat map. Each of these answers the game's value for an effect that has no instance, counts the call as not in a combat map, writes
// no ended line, and (arena pays nothing, by count) reads none of the game's memory and scans no level.
void AnArenaMatchAndEveryOtherLevelKeepTheGamesValueAndReadNothing() {
  struct Case {
    const char* what;
    std::initializer_list<const char*> levels;
  };
  const Case cases[] = {{"an Echo Arena level", {"mpl_arena_a"}},
                        {"the lowspec arena level", {"mpl_arena_a_lowspec"}},
                        {"the arena's other level", {"mpl_arena_b"}},
                        {"the tutorial", {"mpl_tutorial_a"}},
                        {"the lobby arena players use", {"mpl_lobby_b2"}},
                        {"the arena social lobby", {"mpl_lobby_b_social"}},
                        {"the combat lobby (a map is needed)", {"mpl_lobby_b_combat"}},
                        {"a level nobody names", {"mpl_somewhere_else"}},
                        {"nothing loaded", {}},
                        {"a combat map with an arena level still loaded (arena wins)", {"mpl_combat_dyson", "mpl_arena_a"}}};
  for (const Case& c : cases) {
    InstalledIn(c.levels);
    World(0, 0x1234);  // the effect has no instance: in a combat map this would be FLT_MAX
    const std::uint32_t scans = nevr_quest::combat::level::ScanCalls();
    const float t = Slot()(g_cs, kHandle);
    if (t != kRealTime) std::fprintf(stderr, "fxtrail scope: %s answered %g\n", c.what, static_cast<double>(t));
    QCHECK(t == kRealTime && g_realCalls == 1);
    QCHECK(fx::CurrentStats().inCombat == 0 && fx::CurrentStats().ended == 0 && fx::CurrentStats().calls == 1);
    QCHECK(Lines("combat_fxtrail_ended").empty());
    QCHECK(g_reads.load() == 0 && nevr_quest::combat::level::ScanCalls() == scans);  // nothing of the game's was read, no level was asked for
    End();
  }
  {  // reads nothing: the effect memory is unreadable and the call still answers the game's value without a counted read
    InstalledIn({"mpl_arena_a"});
    QCHECK(Slot()(reinterpret_cast<const void*>(0x20), kHandle) == kRealTime && fx::CurrentStats().inCombat == 0 && g_reads.load() == 0);
    End();
  }
}

// Before the scope's first look nothing is ended (the answer starts false); the scope has not looked, and the module says nothing about it.
void ABeforeTheFirstLookNothingIsEnded() {
  Begin();
  QCHECK(Install().installed);
  g_reads = 0;  // the install's own pin reads are not the call's
  World(0, 0x1234);
  QCHECK(Slot()(g_cs, kHandle) == kRealTime && fx::CurrentStats().ended == 0 && scope::Looks() == 0 && g_reads.load() == 0);
  End();
}

// A true answer older than kStaleAfterMs is not believed: the tick stopped, so the guard answers the game's value and reads nothing, and the first look
// after the tick is back makes it act again.
void ATrueAnswerFromAStoppedTickIsNotBelieved() {
  InstalledIn({"mpl_combat_dyson"});
  World(0, 0x1234);
  QCHECK(Slot()(g_cs, kHandle) == FLT_MAX);  // fresh: acts
  const std::uint64_t reads = g_reads.load();
  world::TheTickStops();
  QCHECK(scope::SeenCombatMap() && !scope::LookIsFresh());  // the word still says combat; its look is too old
  QCHECK(Slot()(g_cs, kHandle) == kRealTime && fx::CurrentStats().ended == 1 && fx::CurrentStats().inCombat == 1);
  QCHECK(g_reads.load() == reads);  // the stale call read nothing
  world::Levels({"mpl_combat_dyson"});
  QCHECK(Slot()(g_cs, kHandle) == FLT_MAX && fx::CurrentStats().ended == 2);
  End();
}

// The look follows the level: a combat map, then an arena level, then a combat map again.
void TheGuardFollowsTheLoadedLevelAtTheNextLook() {
  InstalledIn({"mpl_lobby_b_combat"});
  World(0, 0x1234);
  QCHECK(Slot()(g_cs, kHandle) == kRealTime);  // the combat lobby is not a map
  world::Levels({"mpl_lobby_b_combat", "mpl_combat_gauss"});  // the map loads: the lobby is still loaded
  QCHECK(Slot()(g_cs, kHandle) == FLT_MAX);
  world::Levels({"mpl_combat_gauss", "mpl_arena_b"});  // an arena level loads while the map is still there: arena wins
  QCHECK(Slot()(g_cs, kHandle) == kRealTime);
  world::Levels({"mpl_combat_gauss"});
  QCHECK(Slot()(g_cs, kHandle) == FLT_MAX);
  QCHECK(fx::CurrentStats().ended == 2 && scope::Looks() == 4);  // one look per level change: the first load and the three moves
  // Between looks the answer is the last look's: the level changes in the game, nothing has looked yet.
  world::LevelsWithoutALook({"mpl_arena_a"});
  QCHECK(Slot()(g_cs, kHandle) == FLT_MAX && scope::Looks() == 4);
  End();
}

// 5. A slot that does not hold EffectTime is refused and left alone, with the pin named; so is a word that points at other code.
void ASlotThatDoesNotHoldEffectTimeIsRefusedAndLeftAlone() {
  Begin();
  Build(0x4242420);
  ModuleResult r = Install();
  QCHECK(!r.installed && std::strcmp(r.status, "original_mismatch") == 0);
  QCHECK(*g_image.Slot(g_pins.slot) == reinterpret_cast<void*>(0x4242420));
  std::vector<Line> install = Lines("combat_fxtrail_install");
  QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kError && Has(install[0].text, "\"result\":\"refused\"") &&
         Has(install[0].text, "\"reason\":\"slot_refused\"") && Has(install[0].text, "\"hook\":\"original_mismatch\""));
  QCHECK(Has(install.empty() ? "" : install[0].text, "bullets whose trail effect has no instance are never freed"));
  End();
  Begin();
  Build(reinterpret_cast<std::uintptr_t>(&OtherFn));  // real executable code, but not EffectTime: no acceptance by plausibility
  r = Install();
  QCHECK(!r.installed && std::strcmp(r.status, "original_mismatch") == 0);
  QCHECK(*g_image.Slot(g_pins.slot) == reinterpret_cast<void*>(&OtherFn));
  QCHECK(Slot()(g_cs, kHandle) == 9.0f);  // the other function still answers: the slot was left alone
  End();
}

// 6. The plugin's kill file: a Warning that names the key; the module is installed all the same.
void TheLegacySwitchFileIsOnlyToldAbout() {
  char tmpl[] = "/tmp/fxtrail-legacy-XXXXXX";
  QCHECK(mkdtemp(tmpl) != nullptr);
  const std::string dir = tmpl;
  const std::string file = dir + "/fxtrail.off";
  Begin();
  ModuleResult r = Install(true, dir.c_str());  // no file: nothing said
  QCHECK(r.installed && Lines("combat_fxtrail_legacy_switch").empty());
  End();
  std::FILE* f = std::fopen(file.c_str(), "w");
  QCHECK(f != nullptr);
  if (f != nullptr) std::fclose(f);
  Begin();
  r = Install(true, dir.c_str());
  QCHECK(r.installed && Hooked());  // the file turns nothing off
  const std::vector<Line> legacy = Lines("combat_fxtrail_legacy_switch");
  QCHECK(legacy.size() == 1U && legacy[0].level == sentinel::LogLevel::kWarn);
  if (legacy.size() == 1U) {
    QCHECK(Has(legacy[0].text, "\"key\":\"combat.fxtrail\"") && Has(legacy[0].text, "\"file\":\"fxtrail.off\""));
  }
  End();
  Begin();
  r = Install(false, dir.c_str());  // off by the key, with the file there: still told, still off
  QCHECK(!r.installed && r.off && Lines("combat_fxtrail_legacy_switch").size() == 1U);
  End();
  unlink(file.c_str());
  rmdir(dir.c_str());
}

// 7. combat.fxtrail: false installs nothing and says so.
void TheSwitchOffInstallsNothingAndSaysSo() {
  Begin();
  const ModuleResult r = Install(false);
  QCHECK(!r.installed && r.off && std::strcmp(r.status, "off_config") == 0);
  QCHECK(SlotIsTheGames() && tick::Count() == 1 && g_reads.load() == 0);  // the scope row's refresh is the only callback; the off module read nothing
  const std::vector<Line> install = Lines("combat_fxtrail_install");
  QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kInfo);
  if (install.size() == 1U) {
    QCHECK(combat_test::Golden(install[0].text) ==
           "\"event\":\"combat_fxtrail_install\",\"result\":\"off\",\"reason\":\"off_config\",\"consequence\":\"combat.fxtrail is false: the "
           "game's own EffectTime answers, and bullets with a trail are never freed\"}");
  }
  End();
}

// The image: not loaded, no build ID, another build.
void AMissingOrOtherImageInstallsNothing() {
  Begin();
  QCHECK(std::strcmp(fx::InstallAt(nullptr, g_pins, true).status, "module_not_loaded") == 0);
  g_image.Drop();
  QCHECK(std::strcmp(Install().status, "module_not_loaded") == 0);
  Build();
  g_image.Build({}, pinned::kLibR15StockBuildId);
  QCHECK(std::strcmp(Install().status, "build_id_mismatch") == 0);
  g_image.Build({}, nullptr, /*note=*/false);
  QCHECK(std::strcmp(Install().status, "no_build_id") == 0);
  const std::vector<Line> lines = Lines("combat_fxtrail_install");
  QCHECK(lines.size() == 4U);
  for (const Line& l : lines) QCHECK(l.level == sentinel::LogLevel::kError && Has(l.text, "\"result\":\"refused\""));
  End();
}

// 8. The pinned words: the table is pinned, every word flipped in turn (and an unreadable one) refuses the install before the slot is touched.
void EveryPinnedWordGatesTheInstall() {
  Begin();
  combat_test::EveryPinIsRefused(
      "combat_fxtrail_install", kAllWords, g_image,
      [] {
        combat_test::StartCapture();
        fx::ResetForTest();
        world::Reset();
        Build();
        QCHECK(world::StartScope(&Lookup));
        combat_test::Captured().clear();
      },
      [] { return Install(); }, [] { return SlotIsTheGames(); });
  End();
}

// A scope that cannot change refuses the install: the guard would never see a combat map, so no hook is put in (the same refusal as the other readers of the
// answer). Either the scope row never registered its refresh, or the tick is not installed.
void AScopeThatCannotChangeRefusesTheInstall() {
  Begin(/*scopeLive=*/false);  // the tick is not installed, the scope row did not register
  ModuleResult r = Install();
  QCHECK(!r.installed && !r.off && std::strcmp(r.status, "scope_not_live") == 0 && SlotIsTheGames());
  std::vector<Line> install = Lines("combat_fxtrail_install");
  QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kError && Has(install[0].text, "\"reason\":\"scope_not_live\""));
  QCHECK(g_reads.load() == 0 && tick::Count() == 0);
  End();
  Begin(/*scopeLive=*/false);  // the tick is installed, but no row registered the refresh
  QCHECK(tick::InstallAt(&Lookup));
  r = Install();
  QCHECK(!r.installed && std::strcmp(r.status, "scope_not_live") == 0 && SlotIsTheGames() && !scope::Live());
  End();
  Begin(/*scopeLive=*/false);  // the tick table has no room, so the scope row cannot register
  QCHECK(tick::InstallAt(&Lookup));
  for (std::size_t i = tick::Count(); i < tick::kMaxCallbacks; ++i) QCHECK(tick::Add([](std::uintptr_t) noexcept {}));
  QCHECK(!scope::InstallNormal().installed);
  r = Install();
  QCHECK(!r.installed && std::strcmp(r.status, "scope_not_live") == 0 && SlotIsTheGames());
  End();
  Begin(/*scopeLive=*/false);  // off by the key wins over a scope that cannot change: nothing is asked of the scope
  r = Install(false);
  QCHECK(!r.installed && r.off && std::strcmp(r.status, "off_config") == 0);
  End();
}

// A second install: the slot already holds the hook, nothing is installed again.
void ASecondInstallDoesNothing() {
  Begin();
  QCHECK(Install().installed);
  const ModuleResult again = Install();
  QCHECK(!again.installed && Hooked());
  End();
}

// The stats: a Debug line every kStatsEvery calls made in a combat map, the Info line at the end of the process only when the guard is installed.
void TheStatsAreWrittenEveryKStatsEveryCallsAndAtTheEnd() {
  InstalledIn({"mpl_combat_dyson"});
  World(0x1234, 0);  // an instance: the game's value; only the calls are counted
  for (unsigned i = 0; i < fx::kStatsEvery - 1; ++i) Slot()(g_cs, kHandle);
  QCHECK(Lines("combat_fxtrail_stats").empty());
  Slot()(g_cs, kHandle);
  std::vector<Line> stats = Lines("combat_fxtrail_stats");
  QCHECK(stats.size() == 1U && stats[0].level == sentinel::LogLevel::kDebug && Has(stats[0].text, "\"final\":0") &&
         Has(stats[0].text, "\"calls\":16384,\"in_combat\":16384,\"ended\":0,\"final\":0"));
  fx::NoteEnd();
  stats = Lines("combat_fxtrail_stats");
  QCHECK(stats.size() == 2U && stats[1].level == sentinel::LogLevel::kInfo && Has(stats[1].text, "\"final\":1"));
  End();
  Begin();  // not installed: no final line
  fx::NoteEnd();
  QCHECK(Lines("combat_fxtrail_stats").empty());
  End();
}

}  // namespace

int main() {
  ThePinsAreTheReadelfAndDisassemblyPins();
  TheThunkTakesTheSlotAndTheLineSaysSo();
  AnEffectWithNoInstanceEndsItsTrailInACombatMap();
  AnEffectWithAnInstanceKeepsTheGamesValue();
  TheEndedLineIsWrittenAtPowersOfTwoOnly();
  AFailedReadKeepsTheGamesValue();
  AnArenaMatchAndEveryOtherLevelKeepTheGamesValueAndReadNothing();
  ABeforeTheFirstLookNothingIsEnded();
  ATrueAnswerFromAStoppedTickIsNotBelieved();
  TheGuardFollowsTheLoadedLevelAtTheNextLook();
  ASlotThatDoesNotHoldEffectTimeIsRefusedAndLeftAlone();
  TheLegacySwitchFileIsOnlyToldAbout();
  TheSwitchOffInstallsNothingAndSaysSo();
  AMissingOrOtherImageInstallsNothing();
  EveryPinnedWordGatesTheInstall();
  AScopeThatCannotChangeRefusesTheInstall();
  ASecondInstallDoesNothing();
  TheStatsAreWrittenEveryKStatsEveryCallsAndAtTheEnd();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "fxtrail_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::puts("fxtrail_test: all checks passed");
  return 0;
}
