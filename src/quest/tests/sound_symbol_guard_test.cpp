// Host test of the sound-symbol guard (src/quest/combat/sound_symbol_guard.cpp, #1737): the rule (runtime/combat/sound_symbol_guard_rules.h), and
// the real handler through the real caller-aware thunk, installed by the real GotHook into Play3DSound's JUMP_SLOT of a libr15 image built in memory
// whose slot holds base + the pinned function, called through the slot from three call sites of the test (two stand in for PlaySound's and the
// bullet site's: their return addresses are learnt once and given to the install as the two sites).
//
// What it pins: the rule's table (0 and 0x1406b30a drop, 1, 0xc7b3d4fb and others do not); in a combat level a post from either site whose symbol
// drops returns 0 and never reaches Play3DSound, counted by class, and the same post from any other caller reaches it; any other post reaches it with
// its four arguments unchanged and its answer is returned; outside a combat level (an arena level, no level) and on a stale answer every post reaches
// it and nothing is counted, read or logged; the install refuses a scope that cannot change, a foreign slot word, every pinned word and record bent,
// and a missing or other image, with the slot untouched; the switch; the drop lines (16 at Debug, then one notice, then a count at most every
// 10 s) and the end total.
//
// Built and run by `just test-quest-combat sound_symbol_guard`.
#include "quest/combat/sound_symbol_guard.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "quest/combat/combat_level.h"
#include "quest/combat/combat_scope.h"
#include "quest/combat/combat_tick.h"
#include "quest/combat/combat_tick_targets.h"
#include "quest/sentinel/hook_install.h"
#include "quest/tests/combat_game_model.h"
#include "quest/tests/combat_libr15_image.h"
#include "quest/tests/combat_log_capture.h"
#include "quest/tests/combat_pin_check.h"
#include "quest/tests/combat_pins.h"
#include "quest/tests/test_check.h"
#include "runtime/combat/sound_symbol_guard_rules.h"

namespace {

namespace ssg = nevr_quest::combat::sound_symbol_guard;
namespace rules = nevr_combat::sound_symbol_guard;
namespace scope = nevr_quest::combat::scope;
namespace tick = nevr_quest::combat::tick;
namespace pinned = sentinel::pinned;
using combat_test::Has;
using combat_test::Image;
using combat_test::Line;
using combat_test::Lines;
using nevr_quest::combat::ModuleResult;

using Play3DFn = std::int32_t (*)(void*, std::uint64_t, std::uint32_t, float);

// ---- the stand-in for Play3DSound ----------------------------------------------------------------------------------------------
struct Call {
  void* player;
  std::uint64_t symbol;
  std::uint32_t emitter;
  float volume;
};
std::vector<Call> g_calls;
bool g_record = true;  // false: the cost loop's calls are not recorded
__attribute__((noinline)) std::int32_t FakePlay3D(void* player, std::uint64_t symbol, std::uint32_t emitter, float volume) {
  if (g_record) g_calls.push_back({player, symbol, emitter, volume});
  return 0x77;
}
std::int32_t OtherPlay3D(void*, std::uint64_t, std::uint32_t, float) { return 0; }

// ---- the libr15 image ----------------------------------------------------------------------------------------------------------
Image g_image;
combat_test::GameModel g_model;
ssg::Pins g_pins = ssg::kPins;
bool Lookup(const char* module, sentinel::ElfImage* out) {
  if (!g_image.Present() || std::strcmp(module, pinned::kLibR15) != 0) return false;
  *out = g_image.Elf();
  return true;
}
void FakeTickFrame(void*) {}
// A libr15 whose Play3DSound slot holds base + the pinned function (the stand-in behind it, or `slotWord`), with every pinned word and record,
// and the combat tick's slot.
void Build(void* slotWord = nullptr) {
  namespace tt = nevr_quest::combat::tick::targets;
  g_image.Build({{ssg::targets::kPlay3DSymbol, ssg::kPins.slot, false, false, ssg::kPins.fn}, {tt::kSymbol, tt::kSlotVaddr, false, false, tt::kStockVaddr}});
  *g_image.Slot(tt::kSlotVaddr) = reinterpret_cast<void*>(&FakeTickFrame);
  g_image.Real(ssg::kPins.fn, reinterpret_cast<void*>(&FakePlay3D));
  const nevr_quest::combat::pinned::Pins lists[] = {ssg::targets::kWordPins, ssg::targets::kDropPins};
  combat_test::PutPins(g_image, lists, 2);
  *g_image.Slot(ssg::kPins.slot) = slotWord != nullptr ? slotWord : reinterpret_cast<void*>(g_image.Base() + ssg::kPins.fn);
}
Play3DFn Slot() { return reinterpret_cast<Play3DFn>(*g_image.Slot(ssg::kPins.slot)); }
bool Hooked() { return *g_image.Slot(ssg::kPins.slot) == ssg::Play3DThunk::EntryAddress(); }
bool SlotIsTheGames() { return *g_image.Slot(ssg::kPins.slot) == reinterpret_cast<void*>(g_image.Base() + ssg::kPins.fn); }

// The three call sites: each calls through the slot, as the game's `bl Play3DSound@plt` does.
void* const kPlayer = reinterpret_cast<void*>(0x4400);
__attribute__((noinline)) std::int32_t PostFromPlaySound(std::uint64_t symbol, std::uint32_t emitter = 3, float volume = 0.5f) {
  volatile std::int32_t r = Slot()(kPlayer, symbol, emitter, volume);
  return r;
}
__attribute__((noinline)) std::int32_t PostFromBulletSite(std::uint64_t symbol, std::uint32_t emitter = 4, float volume = 0.75f) {
  volatile std::int32_t r = Slot()(kPlayer, symbol, emitter, volume);
  return r;
}
__attribute__((noinline)) std::int32_t PostFromOther(std::uint64_t symbol, std::uint32_t emitter = 5, float volume = 1.0f) {
  volatile std::int32_t r = Slot()(kPlayer, symbol, emitter, volume);
  return r;
}

std::uint64_t g_nowMs = 1000;     // the combat level answer's clock
std::uint64_t g_budgetMs = 1000;  // the drop lines' clock
std::uint64_t Clock() { return g_budgetMs; }
std::int64_t ScopeClock() noexcept { return static_cast<std::int64_t>(g_nowMs); }

// `scopeLive`: the combat tick is installed on the image and the `scope` row has registered the refresh (what the shipped table does first).
void Begin(bool scopeLive = true) {
  combat_test::StartCapture();
  ssg::ResetForTest();
  tick::ResetForTest();
  scope::ResetForTest();
  g_calls.clear();
  g_nowMs = 1000;
  g_budgetMs = 1000;
  g_model.Clear();
  scope::SetClockForTest(&ScopeClock);
  ssg::SetClockForTest(&Clock);
  Build();
  if (scopeLive) {
    QCHECK(tick::InstallAt(&Lookup));
    QCHECK(scope::InstallNormal().installed && scope::Live());
    combat_test::Captured().clear();
  }
}
void End() {
  ssg::ResetForTest();
  scope::ResetForTest();
  tick::ResetForTest();
  g_image.Drop();
  combat_test::StopCapture();
}
// The game goes to these levels; the tick's callback runs once.
void Levels(std::initializer_list<const char*> names) {
  g_model.Only(names);
  g_nowMs += scope::kLookEveryMs;
  scope::Refresh(g_model.Address());
}
void EnterCombat() { Levels({"mpl_combat_dyson"}); }
ModuleResult Install() { return ssg::InstallAt(&Lookup, g_pins); }

// The two sites' return addresses, learnt once: each post's return address less the image base, from the guard itself.
std::uint64_t g_playRet = 0, g_bulletRet = 0, g_otherRet = 0;
void LearnTheSites() {
  Begin();
  g_pins = ssg::kPins;
  QCHECK(Install().installed);
  EnterCombat();
  PostFromPlaySound(0x1234);
  g_playRet = ssg::LastCallerForTest();
  PostFromBulletSite(0x1234);
  g_bulletRet = ssg::LastCallerForTest();
  PostFromOther(0x1234);
  g_otherRet = ssg::LastCallerForTest();
  QCHECK(g_playRet != 0 && g_bulletRet != 0 && g_otherRet != 0 && g_playRet != g_bulletRet && g_otherRet != g_playRet && g_otherRet != g_bulletRet);
  End();
  g_pins = {ssg::kPins.slot, ssg::kPins.fn, g_playRet, g_bulletRet};
}

// ---- the rule ------------------------------------------------------------------------------------------------------------------
void TheRuleDropsZeroAndTheUnregisteredDefaultOnly() {
  QCHECK(rules::Drops(0) && rules::Classify(0) == rules::Drop::kZero);
  // 0x1406b30a, the bullet site's default, is in no stock bank (tools/cmd/wwise-events over the two stock packages: 34 banks, 1295 events;
  // docs/quest/combat/sound_symbol_guard.md, What it hooks).
  QCHECK(rules::Drops(0x1406b30aU) && rules::Classify(0x1406b30aU) == rules::Drop::kUnregistered);
  QCHECK(rules::kUnregisteredCount == 1 && rules::kUnregistered[0] == 0x1406b30aU);
  for (std::size_t i = 0; i < rules::kUnregisteredCount; ++i) QCHECK(rules::Classify(rules::kUnregistered[i]) == rules::Drop::kUnregistered);
  // the list's entry is the .rodata word the default's writer copies (the targets' last kWords entry)
  QCHECK(ssg::targets::kWords[ssg::targets::kWordCount - 1].vaddr == 0x31c8330 && ssg::targets::kWords[ssg::targets::kWordCount - 1].word == rules::kUnregistered[0]);
  // 1 and 0xc7b3d4fb are not events either, but neither is a symbol a site posts (sound_symbol_guard.md, What it hooks): not listed, so not
  // dropped. Nor is any other value.
  const std::uint32_t keep[] = {1U, 0xc7b3d4fbU, 0x1406b30bU, 0x1406b309U, 0xffffffffU, 0x5b8cc538U, 0x80000000U, 0x0000b30aU};
  for (std::uint32_t s : keep) QCHECK(!rules::Drops(s) && rules::Classify(s) == rules::Drop::kNone);
  QCHECK(std::strcmp(rules::DropName(rules::Drop::kZero), "zero") == 0 && std::strcmp(rules::DropName(rules::Drop::kUnregistered), "unregistered") == 0 &&
         std::strcmp(rules::DropName(rules::Drop::kNone), "none") == 0);
}

// The production pins are the targets' pins, and the two return addresses are the pinned "return address" words.
void ThePinsAreTheTargetsPins() {
  QCHECK(ssg::kPins.slot == 0x3701090 && ssg::kPins.fn == 0x134b468);
  QCHECK(ssg::kPins.slot == ssg::targets::kPins.play3DSlot && ssg::kPins.fn == ssg::targets::kPins.play3DFn);
  bool play = false, bullet = false;
  for (const auto& w : ssg::targets::kDropWords) play = play || (w.vaddr == ssg::kPins.playSoundReturn && Has(w.what, "PlaySound: the return address"));
  for (const auto& w : ssg::targets::kWords) bullet = bullet || (w.vaddr == ssg::kPins.bulletSiteReturn && Has(w.what, "bullet site: the return address"));
  QCHECK(play && bullet && ssg::kPins.playSoundReturn == 0x170698c && ssg::kPins.bulletSiteReturn == 0x21230d4);
  const std::uintptr_t base = 0x7000000000;
  QCHECK(ssg::CallerOf(base + 0x170698c, base, ssg::kPins) == ssg::Caller::kPlaySound);
  QCHECK(ssg::CallerOf(base + 0x21230d4, base, ssg::kPins) == ssg::Caller::kBulletSite);
  QCHECK(ssg::CallerOf(base + 0x24bd5b0, base, ssg::kPins) == ssg::Caller::kOther);  // TriggerSingle's return: its emitter carries the destroy bit
  QCHECK(ssg::CallerOf(0x170698c, base, ssg::kPins) == ssg::Caller::kOther);         // the ELF address without the base is not the site
  QCHECK(std::strcmp(ssg::CallerName(ssg::Caller::kPlaySound), "play_sound") == 0 && std::strcmp(ssg::CallerName(ssg::Caller::kBulletSite), "bullet_site") == 0 &&
         std::strcmp(ssg::CallerName(ssg::Caller::kOther), "other") == 0);
}

// ---- the hook ------------------------------------------------------------------------------------------------------------------
void TheThunkTakesTheSlotAndTheLineSaysSo() {
  Begin();
  const ModuleResult r = Install();
  QCHECK(r.installed && std::strcmp(r.status, "installed") == 0 && !r.off && Hooked());
  QCHECK(tick::Count() == 1);  // the scope row registered the refresh, the module did not add one
  const std::vector<Line> lines = Lines("combat_sound_symbol_guard_install");
  QCHECK(lines.size() == 1U);
  if (lines.size() == 1U) {
    char slot[19];
    QCHECK(lines[0].level == sentinel::LogLevel::kInfo);
    QCHECK(combat_test::Golden(lines[0].text) ==
           std::string("\"event\":\"combat_sound_symbol_guard_install\",\"result\":\"installed\",\"reason\":\"ok\",\"slot\":\"") +
               sentinel::HexString(slot, ssg::kPins.slot) +
               "\",\"msg\":\"in a combat level, a post from PlaySound or the bullet site whose symbol is 0 or unregistered is not made; every other post "
               "reaches Play3DSound unchanged\"}");
  }
  End();
}

void ADropAtEitherSiteReturnsZeroAndNeverReachesTheGame() {
  Begin();
  QCHECK(Install().installed);
  EnterCombat();
  QCHECK(PostFromPlaySound(0) == 0);
  QCHECK(PostFromBulletSite(0x1406b30a) == 0);
  QCHECK(PostFromBulletSite(0) == 0);
  QCHECK(PostFromPlaySound(0xffffffff00000000ULL) == 0);  // the CSoundSymbol is the low 32 bits: 0
  QCHECK(g_calls.empty());
  const ssg::Counts c = ssg::CurrentCounts();
  QCHECK(c.calls == 4 && c.droppedZero == 3 && c.droppedUnregistered == 1 && c.keptOtherCaller == 0);
  const std::vector<Line> drops = Lines("combat_sound_symbol_guard_drop");
  QCHECK(drops.size() == 4U);
  if (drops.size() == 4U) {
    QCHECK(drops[0].level == sentinel::LogLevel::kDebug);
    QCHECK(combat_test::Golden(drops[0].text) ==
           "\"event\":\"combat_sound_symbol_guard_drop\",\"symbol\":\"0x00000000\",\"class\":\"zero\",\"caller\":\"play_sound\",\"n\":1,\"msg\":\"a sound post whose "
           "symbol no bank defines was not made: Play3DSound would fail it in PostEvent and hand the error to OutputError\"}");
    QCHECK(Has(drops[1].text, "\"symbol\":\"0x1406b30a\",\"class\":\"unregistered\",\"caller\":\"bullet_site\",\"n\":1,"));
    QCHECK(Has(drops[2].text, "\"class\":\"zero\",\"caller\":\"bullet_site\",\"n\":2,"));
    QCHECK(Has(drops[3].text, "\"class\":\"zero\",\"caller\":\"play_sound\",\"n\":3,"));
  }
  End();
}

void AnyOtherPostReachesTheGameUnchanged() {
  Begin();
  QCHECK(Install().installed);
  Levels({"mpl_lobby_b_combat"});  // the combat lobby is in scope too
  QCHECK(PostFromPlaySound(0xdead00005b8cc538ULL, 9, 0.25f) == 0x77);
  QCHECK(PostFromBulletSite(1, 11, 2.0f) == 0x77);
  QCHECK(PostFromBulletSite(0xc7b3d4fb, 12, 0.0f) == 0x77);
  QCHECK(g_calls.size() == 3U);
  if (g_calls.size() == 3U) {
    QCHECK(g_calls[0].player == kPlayer && g_calls[0].symbol == 0xdead00005b8cc538ULL && g_calls[0].emitter == 9 && g_calls[0].volume == 0.25f);
    QCHECK(g_calls[1].symbol == 1 && g_calls[1].emitter == 11 && g_calls[1].volume == 2.0f);
    QCHECK(g_calls[2].symbol == 0xc7b3d4fb && g_calls[2].emitter == 12);
  }
  const ssg::Counts c = ssg::CurrentCounts();
  QCHECK(c.calls == 3 && c.droppedZero == 0 && c.droppedUnregistered == 0 && c.keptOtherCaller == 0);
  QCHECK(Lines("combat_sound_symbol_guard_drop").empty());
  End();
}

// A caller that is neither site (TriggerSingle's emitter carries the destroy bit): the game's own failed post, counted, never dropped.
void AnotherCallerIsNeverDropped() {
  Begin();
  QCHECK(Install().installed);
  EnterCombat();
  QCHECK(PostFromOther(0, 5, 1.0f) == 0x77 && PostFromOther(0x1406b30a) == 0x77);
  QCHECK(g_calls.size() == 2U && g_calls[0].symbol == 0 && g_calls[0].emitter == 5 && g_calls[1].symbol == 0x1406b30a);
  const ssg::Counts c = ssg::CurrentCounts();
  QCHECK(c.calls == 2 && c.keptOtherCaller == 2 && c.droppedZero == 0 && c.droppedUnregistered == 0);
  QCHECK(Lines("combat_sound_symbol_guard_drop").empty());
  End();
}

// Outside a combat level, and on a stale answer: every post reaches the game, nothing counted, no level read, no line.
void OutsideACombatLevelEveryPostPassesUntouched() {
  Begin();
  QCHECK(Install().installed);
  const std::uint64_t scans0 = nevr_quest::combat::level::ScanCalls();
  QCHECK(PostFromPlaySound(0) == 0x77);  // no level loaded yet
  QCHECK(nevr_quest::combat::level::ScanCalls() == scans0);
  Levels({"mpl_arena_a"});
  QCHECK(!scope::SeenCombatOrLobby());
  const std::uint64_t scans1 = nevr_quest::combat::level::ScanCalls();
  QCHECK(PostFromPlaySound(0) == 0x77 && PostFromBulletSite(0x1406b30a) == 0x77);
  QCHECK(nevr_quest::combat::level::ScanCalls() == scans1);
  Levels({"mpl_combat_dyson", "mpl_arena_a"});  // an arena level wins over a combat one still loaded
  QCHECK(PostFromBulletSite(0) == 0x77);
  QCHECK(g_calls.size() == 4U);
  ssg::Counts c = ssg::CurrentCounts();
  QCHECK(c.calls == 0 && c.droppedZero == 0 && c.droppedUnregistered == 0 && c.keptOtherCaller == 0 && ssg::LastCallerForTest() == 0);
  // the tick stops in a combat level: a true answer older than kStaleAfterMs is not believed
  EnterCombat();
  QCHECK(PostFromPlaySound(0) == 0);
  g_nowMs += static_cast<std::uint64_t>(scope::kStaleAfterMs) + 1;
  QCHECK(scope::SeenCombatOrLobby() && !scope::LookIsFresh());
  QCHECK(PostFromPlaySound(0) == 0x77);
  c = ssg::CurrentCounts();
  QCHECK(c.calls == 1 && c.droppedZero == 1 && g_calls.size() == 5U);
  QCHECK(Lines("combat_sound_symbol_guard_drop").size() == 1U);
  End();
}

void RefusesAScopeThatCannotChange() {
  Begin(/*scopeLive=*/false);
  const ModuleResult r = Install();
  QCHECK(!r.installed && std::strcmp(r.status, "scope_not_live") == 0 && SlotIsTheGames());
  const std::vector<Line> lines = Lines("combat_sound_symbol_guard_install");
  QCHECK(lines.size() == 1U && lines[0].level == sentinel::LogLevel::kError && Has(lines[0].text, "\"reason\":\"scope_not_live\""));
  End();
}

void RefusesAForeignOriginal() {
  Begin();
  Build(reinterpret_cast<void*>(&OtherPlay3D));
  const ModuleResult r = Install();
  QCHECK(!r.installed && std::strcmp(r.status, "original_mismatch") == 0);
  QCHECK(*g_image.Slot(ssg::kPins.slot) == reinterpret_cast<void*>(&OtherPlay3D));
  const std::vector<Line> lines = Lines("combat_sound_symbol_guard_install");
  QCHECK(lines.size() == 1U && lines[0].level == sentinel::LogLevel::kError && Has(lines[0].text, "\"reason\":\"original_mismatch\""));
  End();
}

void RefusesEveryChangedWordAndRecord() {
  const auto prepare = [] {
    combat_test::StartCapture();
    ssg::ResetForTest();
    tick::ResetForTest();
    scope::ResetForTest();
    Build();
    QCHECK(tick::InstallAt(&Lookup) && scope::InstallNormal().installed);
    combat_test::Captured().clear();
  };
  Begin();
  combat_test::EveryPinIsRefused("combat_sound_symbol_guard_install", ssg::targets::kWordPins, g_image, prepare, [] { return Install(); },
                                 [] { return SlotIsTheGames(); });
  combat_test::EveryPinIsRefused("combat_sound_symbol_guard_install", ssg::targets::kDropPins, g_image, prepare, [] { return Install(); },
                                 [] { return SlotIsTheGames(); });
  // the two .rodata records
  prepare();
  const nevr_quest::combat::pinned::Pins quadsOnly[] = {{nullptr, 0, ssg::targets::kDropQuads, sizeof(ssg::targets::kDropQuads) / sizeof(ssg::targets::kDropQuads[0])}};
  const std::size_t bent = combat_test::ForEachBentPin(g_image, quadsOnly, 1, [](const char* what) {
    combat_test::Captured().clear();
    const ModuleResult r = Install();
    QCHECK(!r.installed && std::strcmp(r.status, "pinned_word_mismatch") == 0 && SlotIsTheGames());
    const std::vector<Line> lines = Lines("combat_sound_symbol_guard_install");
    // the record's name holds quotes, which the line escapes: its unquoted head names it
    const std::string head = std::string(what).substr(0, std::string(what).find('"'));
    QCHECK(lines.size() == 1U && Has(lines[0].text, std::string("\"pin\":\"") + head));
  });
  QCHECK(bent == 2U);
  End();
}

void TheSwitchOffInstallsNothingAndSaysSo() {
  Begin();
  ssg::SetInstallForTest(&Lookup, &g_pins);
  const ModuleResult r = ssg::Install(false);
  QCHECK(!r.installed && r.off && std::strcmp(r.status, "off") == 0 && SlotIsTheGames());
  const std::vector<Line> lines = Lines("combat_sound_symbol_guard_install");
  QCHECK(lines.size() == 1U && lines[0].level == sentinel::LogLevel::kInfo && Has(lines[0].text, "\"result\":\"off\",\"reason\":\"off_config\""));
  const ModuleResult on = ssg::Install(true);  // the step's production path, on the test image
  QCHECK(on.installed && Hooked());
  End();
}

void AMissingOrOtherImageInstallsNothing() {
  Begin();
  QCHECK(std::strcmp(ssg::InstallAt(nullptr, g_pins).status, "module_not_loaded") == 0);
  g_image.Drop();
  QCHECK(std::strcmp(Install().status, "module_not_loaded") == 0);
  g_image.Build({}, pinned::kLibR15StockBuildId);
  QCHECK(std::strcmp(Install().status, "build_id_mismatch") == 0);
  g_image.Build({}, nullptr, /*note=*/false);
  QCHECK(std::strcmp(Install().status, "no_build_id") == 0);
  const std::vector<Line> lines = Lines("combat_sound_symbol_guard_install");
  QCHECK(lines.size() == 4U);
  for (const Line& l : lines) QCHECK(l.level == sentinel::LogLevel::kError && Has(l.text, "\"result\":\"refused\""));
  End();
}

// 16 drop lines at Debug, then one notice, then a count line at most every 10 s while drops continue; the end total counts every class.
void TheDropLinesAreBudgetedAndTotalled() {
  Begin();
  QCHECK(Install().installed);
  EnterCombat();
  for (int i = 0; i < 16; ++i) PostFromPlaySound(0);
  QCHECK(Lines("combat_sound_symbol_guard_drop").size() == 16U && Lines("combat_sound_symbol_guard_drops").empty());
  PostFromBulletSite(0x1406b30a);  // the 17th: the notice
  std::vector<Line> drops = Lines("combat_sound_symbol_guard_drops");
  QCHECK(drops.size() == 1U && drops[0].level == sentinel::LogLevel::kDebug && Has(drops[0].text, "\"n\":17,\"since_last\":0"));
  for (int i = 0; i < 5; ++i) PostFromPlaySound(0);
  g_budgetMs += 9999;
  PostFromPlaySound(0);
  QCHECK(Lines("combat_sound_symbol_guard_drops").size() == 1U);  // under 10 s since the notice: counted only
  g_budgetMs += 1;
  PostFromPlaySound(0);  // 10 s: the count of the 7 since the notice
  drops = Lines("combat_sound_symbol_guard_drops");
  QCHECK(drops.size() == 2U && Has(drops[1].text, "\"n\":24,\"since_last\":7"));
  if (drops.size() == 2U) QCHECK(drops[1].level == sentinel::LogLevel::kDebug);
  QCHECK(Lines("combat_sound_symbol_guard_drop").size() == 16U);
  PostFromOther(0);
  PostFromPlaySound(0x42);
  ssg::NoteEnd();
  const std::vector<Line> total = Lines("combat_sound_symbol_guard_total");
  QCHECK(total.size() == 1U && total[0].level == sentinel::LogLevel::kInfo &&
         Has(total[0].text, "\"dropped_zero\":23,\"dropped_unregistered\":1,\"kept_other_caller\":1,\"calls\":26"));
  End();
  Begin();  // not installed: no total
  ssg::NoteEnd();
  QCHECK(Lines("combat_sound_symbol_guard_total").empty());
  End();
}

// What the guard costs a post on the host: through the hook outside a combat level (the arena path) against the game's function called directly.
void TheArenaPathCostIsMeasured() {
  Begin();
  QCHECK(Install().installed);
  Levels({"mpl_arena_a"});
  constexpr int kCalls = 2000000;
  const auto best = [&](Play3DFn f) {
    double b = 1e18;
    for (int rep = 0; rep < 7; ++rep) {
      const auto start = std::chrono::steady_clock::now();
      volatile std::int32_t sink = 0;
      for (int i = 0; i < kCalls; ++i) sink = sink + f(kPlayer, 0, 1, 1.0f);
      const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / kCalls;
      if (ns < b) b = ns;
    }
    return b;
  };
  g_record = false;
  const double ref = best(&FakePlay3D);
  const std::uint64_t scans0 = nevr_quest::combat::level::ScanCalls();
  const double arena = best(Slot());
  g_record = true;
  QCHECK(nevr_quest::combat::level::ScanCalls() == scans0 && ssg::CurrentCounts().calls == 0 && g_calls.empty());
  std::printf("sound_symbol_guard_test: cost on the host: the game's function %.1f ns, through the hook outside a combat level %.1f ns (+%.1f ns); "
              "%d calls, best of 7\n",
              ref, arena, arena - ref, kCalls);
  End();
}

}  // namespace

int main() {
  TheRuleDropsZeroAndTheUnregisteredDefaultOnly();
  ThePinsAreTheTargetsPins();
  LearnTheSites();
  TheThunkTakesTheSlotAndTheLineSaysSo();
  ADropAtEitherSiteReturnsZeroAndNeverReachesTheGame();
  AnyOtherPostReachesTheGameUnchanged();
  AnotherCallerIsNeverDropped();
  OutsideACombatLevelEveryPostPassesUntouched();
  RefusesAScopeThatCannotChange();
  RefusesAForeignOriginal();
  RefusesEveryChangedWordAndRecord();
  TheSwitchOffInstallsNothingAndSaysSo();
  AMissingOrOtherImageInstallsNothing();
  TheDropLinesAreBudgetedAndTotalled();
  TheArenaPathCostIsMeasured();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "sound_symbol_guard_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::puts("sound_symbol_guard_test: all checks passed");
  return 0;
}
