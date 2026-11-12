// Host test of the shared "is a combat level loaded" cache (#2186, src/quest/combat/combat_scope.cpp): the production refresh against a model of
// CR15Game's gamespace tables (combat_game_model.h), a fake clock, and the real combat tick installed into the JUMP_SLOT of a libr15 image built in
// memory.
//
// What it pins: the answer fails closed (false before the first look, with the game unreadable, when the tick's table is full or the tick is not
// installed, and when the last look is older than kStaleAfterMs: the tick stopped); InCombatMap and InCombatOrLobby differ exactly at the combat
// lobby; both are true for a combat map, false for every Echo Arena level and every lobby arena players use, and an arena level wins over a combat
// level still loaded; the game is looked at once per kLookEveryMs (not once per frame); a change of either answer is one Info line with its value, and
// no change is no line (an arena match writes nothing); the row registers the refresh once and only with the tick installed.
//
// Built and run by `just test-quest-combat`.

#include "quest/combat/combat_scope.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "nevr/symbol_hash.h"
#include "quest/combat/combat_tick.h"
#include "quest/combat/combat_tick_targets.h"
#include "quest/sentinel/hook_log.h"
#include "quest/tests/combat_game_model.h"
#include "quest/tests/combat_libr15_image.h"
#include "quest/tests/test_check.h"

namespace {

namespace scope = nevr_quest::combat::scope;
namespace tick = nevr_quest::combat::tick;
namespace tt = nevr_quest::combat::tick::targets;
using combat_test::GameModel;
using combat_test::Image;

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

std::int64_t g_nowMs = 1000;
std::int64_t Clock() noexcept { return g_nowMs; }
GameModel g_model;
Image g_image;
void FakeTickFrame(void*) {}
bool Lookup(const char* module, sentinel::ElfImage* out) {
  if (!g_image.Present() || std::strcmp(module, sentinel::pinned::kLibR15) != 0) return false;
  *out = g_image.Elf();
  return true;
}
// A libr15 whose tick slot holds the game's function (a stand-in), so the real tick can be installed on it.
void BuildImage() {
  g_image.Build({{tt::kSymbol, tt::kSlotVaddr, false, false, tt::kStockVaddr}});
  *g_image.Slot(tt::kSlotVaddr) = reinterpret_cast<void*>(&FakeTickFrame);  // the game's function: any address in an executable mapping
}

void Begin(bool tickInstalled = true) {
  g_lines.clear();
  g_nowMs = 1000;
  g_model.Clear();
  tick::ResetForTest();
  scope::ResetForTest();
  scope::SetClockForTest(&Clock);
  sentinel::SetLogSink(&Capture);
  BuildImage();
  if (tickInstalled) QCHECK(tick::InstallAt(&Lookup));
  g_lines.clear();
}
void End() {
  scope::ResetForTest();
  tick::ResetForTest();
  g_image.Drop();
  sentinel::SetLogSink(nullptr);
}

// One look at the model: the clock moves past the spacing and the refresh runs.
void Look() {
  g_nowMs += scope::kLookEveryMs;
  scope::Refresh(g_model.Address());
}
void LookAt(std::initializer_list<const char*> levels) {
  g_model.Only(levels);
  Look();
}

void FailsClosedUntilTheFirstLook() {
  Begin();
  QCHECK(!scope::InCombatOrLobby() && !scope::InCombatMap());
  QCHECK(scope::Looks() == 0);
  g_model.Only({"mpl_combat_dyson"});
  QCHECK(!scope::InCombatOrLobby() && !scope::InCombatMap());  // the game is a combat map, but nothing has looked yet
  Look();
  QCHECK(scope::InCombatOrLobby() && scope::InCombatMap() && scope::Looks() == 1);
  End();
}

// The two questions differ at the combat lobby and nowhere else.
void TheTwoAnswersDifferExactlyAtTheCombatLobby() {
  Begin();
  for (const char* name : {"mpl_combat_dyson", "mpl_combat_combustion", "mpl_combat_fission", "mpl_combat_gauss"}) {
    LookAt({name});
    QCHECK(scope::InCombatOrLobby() && scope::InCombatMap());
    LookAt({});
    QCHECK(!scope::InCombatOrLobby() && !scope::InCombatMap());
  }
  LookAt({"mpl_lobby_b_combat"});
  QCHECK(scope::InCombatOrLobby() && !scope::InCombatMap());
  // Every Echo Arena level, and every lobby arena players use: neither.
  for (const char* name : {"mpl_arena_a", "mpl_arena_a_lowspec", "mpl_arena_b", "mpl_tutorial_a", "mpl_lobby_b", "mpl_lobby_b2",
                           "mpl_lobby_b2_lowspec", "mpl_lobby_b_arena", "mpl_lobby_b_social"}) {
    LookAt({name});
    QCHECK(!scope::InCombatOrLobby() && !scope::InCombatMap());
  }
  End();
}

void AnEchoArenaLevelWinsOverACombatLevelStillLoaded() {
  Begin();
  LookAt({"mpl_combat_gauss"});
  QCHECK(scope::InCombatOrLobby() && scope::InCombatMap());
  LookAt({"mpl_combat_gauss", "mpl_arena_a"});  // a transition from combat to arena: arena wins
  QCHECK(!scope::InCombatOrLobby() && !scope::InCombatMap());
  LookAt({"mpl_arena_a", "mpl_lobby_b_combat"});
  QCHECK(!scope::InCombatOrLobby() && !scope::InCombatMap());
  LookAt({"mpl_lobby_b", "mpl_combat_dyson"});  // an arena-style lobby does not mask a combat map
  QCHECK(scope::InCombatOrLobby() && scope::InCombatMap());
  End();
}

void AnUnreadableGameIsNotCombat() {
  Begin();
  LookAt({"mpl_combat_dyson"});
  QCHECK(scope::InCombatOrLobby());
  g_nowMs += scope::kLookEveryMs;
  scope::Refresh(0);  // no game
  QCHECK(!scope::InCombatOrLobby() && !scope::InCombatMap());
  g_model.Only({"mpl_combat_dyson"});
  g_nowMs += scope::kLookEveryMs;
  scope::Refresh(0x1000);  // an unmapped address
  QCHECK(!scope::InCombatOrLobby() && !scope::InCombatMap());
  End();
}

// The game is looked at once per kLookEveryMs, however many frames run; a level change shows at the next look and not before.
void TheGameIsLookedAtOncePerSpacingNotOncePerFrame() {
  Begin();
  g_model.Only({"mpl_arena_a"});
  for (int frame = 0; frame < 1000; ++frame) scope::Refresh(g_model.Address());  // 1000 frames in the same millisecond
  QCHECK(scope::Looks() == 1);
  for (std::int64_t step = 0; step < scope::kLookEveryMs - 1; ++step) {  // one frame per ms until the spacing has almost passed
    ++g_nowMs;
    scope::Refresh(g_model.Address());
  }
  QCHECK(scope::Looks() == 1);
  g_model.Only({"mpl_combat_gauss"});
  ++g_nowMs;
  scope::Refresh(g_model.Address());  // exactly kLookEveryMs after the first look
  QCHECK(scope::Looks() == 2 && scope::InCombatOrLobby());
  g_model.Only({"mpl_arena_b"});
  g_nowMs += scope::kLookEveryMs - 1;
  scope::Refresh(g_model.Address());
  QCHECK(scope::Looks() == 2 && scope::InCombatOrLobby());  // not yet
  ++g_nowMs;
  scope::Refresh(g_model.Address());
  QCHECK(scope::Looks() == 3 && !scope::InCombatOrLobby());
  End();
}

// The tick stops calling the refresh (its object fails the vptr check, or the game thread stops): a true answer is believed for kStaleAfterMs
// after its look and not a millisecond longer; a refresh brings it back; an answer that was false is false whatever the time.
void ATrueAnswerOlderThanTheStalenessLimitIsFalse() {
  Begin();
  LookAt({"mpl_combat_dyson"});
  QCHECK(scope::InCombatOrLobby() && scope::InCombatMap());
  const std::int64_t looked = g_nowMs;
  g_nowMs = looked + scope::kStaleAfterMs;  // exactly at the limit: still believed
  QCHECK(scope::InCombatOrLobby() && scope::InCombatMap());
  g_nowMs = looked + scope::kStaleAfterMs + 1;  // a millisecond over
  QCHECK(!scope::InCombatOrLobby() && !scope::InCombatMap());
  g_nowMs = looked + 60 * 60 * 1000;  // an hour of no tick
  QCHECK(!scope::InCombatOrLobby() && !scope::InCombatMap());
  scope::Refresh(g_model.Address());  // the tick is back: the next look brings the answer back
  QCHECK(scope::InCombatOrLobby() && scope::InCombatMap());
  // the combat lobby is stale in the same way
  LookAt({"mpl_lobby_b_combat"});
  QCHECK(scope::InCombatOrLobby());
  g_nowMs += scope::kStaleAfterMs + 1;
  QCHECK(!scope::InCombatOrLobby());
  End();
}

// A stale answer is a freeze, not a false: the look that comes back finds the level the game is at, and a change is logged then.
void AFalseAnswerStaysFalseWhateverTheTime() {
  Begin();
  LookAt({"mpl_arena_a"});
  g_nowMs += 24 * 60 * 60 * 1000;
  QCHECK(!scope::InCombatOrLobby() && !scope::InCombatMap());
  End();
}

void AChangeIsOneInfoLineWithItsValueAndNoChangeIsNoLine() {
  Begin();
  LookAt({"mpl_arena_a"});
  LookAt({"mpl_arena_b"});
  LookAt({});
  QCHECK(Lines("combat_scope_change").empty());  // an arena match writes nothing
  LookAt({"mpl_combat_dyson"});
  LookAt({"mpl_combat_dyson"});
  std::vector<Line> change = Lines("combat_scope_change");
  QCHECK(change.size() == 1U);
  char want[96];
  if (change.size() == 1U) {
    QCHECK(change[0].level == sentinel::LogLevel::kInfo);
    std::snprintf(want, sizeof(want), "\"from\":0,\"to\":1,\"map\":1,\"level\":\"0x%016llx\",\"looks\":4",
                  static_cast<unsigned long long>(EchoVR::CSymbol64Hash("mpl_combat_dyson")));
    QCHECK(Has(change[0].text, want));
    QCHECK(Has(change[0].text, "\"msg\":\"a combat level is loaded: the combat modules that read the level answer act\""));
  }
  LookAt({"mpl_lobby_b_combat"});  // map -> lobby: InCombatOrLobby stays, InCombatMap goes: a change
  change = Lines("combat_scope_change");
  QCHECK(change.size() == 2U);
  if (change.size() == 2U) {
    std::snprintf(want, sizeof(want), "\"from\":1,\"to\":1,\"map\":0,\"level\":\"0x%016llx\"",
                  static_cast<unsigned long long>(EchoVR::CSymbol64Hash("mpl_lobby_b_combat")));
    QCHECK(Has(change[1].text, want));
  }
  LookAt({"mpl_arena_a"});
  change = Lines("combat_scope_change");
  QCHECK(change.size() == 3U);
  if (change.size() == 3U) {
    std::snprintf(want, sizeof(want), "\"from\":1,\"to\":0,\"map\":0,\"level\":\"0x%016llx\"",
                  static_cast<unsigned long long>(EchoVR::CSymbol64Hash("mpl_arena_a")));
    QCHECK(Has(change[2].text, want) &&
           Has(change[2].text, "\"msg\":\"no combat level is loaded: the combat modules that read the level answer pass every call through\""));
  }
  End();
}

// The change line names the level that decided the answer: the arena level when one is loaded with a combat level (arena wins), else the combat map, else
// the combat lobby; checked by value, so a line that carries a constant or the wrong class's level fails.
void TheChangeLineNamesTheLevelThatDecided() {
  Begin();
  const auto idOf = [](const char* name) {
    char want[48];
    std::snprintf(want, sizeof(want), "\"level\":\"0x%016llx\"", static_cast<unsigned long long>(EchoVR::CSymbol64Hash(name)));
    return std::string(want);
  };
  LookAt({"mpl_combat_gauss"});
  LookAt({"mpl_combat_gauss", "mpl_arena_a"});  // combat to arena, both loaded: the line names the arena level
  std::vector<Line> change = Lines("combat_scope_change");
  QCHECK(change.size() == 2U);
  if (change.size() == 2U) {
    QCHECK(Has(change[0].text, idOf("mpl_combat_gauss")) && Has(change[1].text, idOf("mpl_arena_a")));
    QCHECK(Has(change[1].text, "\"from\":1,\"to\":0"));
  }
  LookAt({"mpl_lobby_b_combat", "mpl_lobby_b"});  // the combat lobby with an arena lobby beside it: the combat lobby's level
  change = Lines("combat_scope_change");
  QCHECK(change.size() == 3U && (change.size() < 3U || Has(change[2].text, idOf("mpl_lobby_b_combat"))));
  LookAt({"mpl_lobby_b_combat", "mpl_combat_dyson"});  // the map and the lobby together: the map's level
  change = Lines("combat_scope_change");
  QCHECK(change.size() == 4U && (change.size() < 4U || (Has(change[3].text, idOf("mpl_combat_dyson")) && Has(change[3].text, "\"map\":1"))));
  End();
}

// The answer is live only while the tick is: after the row registered the refresh, a tick that goes away (its install is forgotten) makes Live() false, so
// a module that asks it refuses; and a tick install on an image that does not hold the slot fails and leaves Installed() false.
void LiveFollowsTheTickAndTheTickInstallIsHonest() {
  Begin();
  QCHECK(tick::Installed() && scope::InstallNormal().installed && scope::Live());
  tick::ResetForTest();
  QCHECK(!tick::Installed() && !scope::Live());
  End();
  Begin(/*tickInstalled=*/false);
  g_image.Drop();
  QCHECK(!tick::InstallAt(&Lookup) && !tick::Installed());  // no libr15: nothing was installed, and nothing says it was
  const nevr_quest::combat::ModuleResult r = scope::InstallNormal();
  QCHECK(!r.installed && std::strcmp(r.status, "tick_not_installed") == 0 && !scope::Live());
  End();
}

// When the tick comes back after a gap in which the readers saw a stale look, one Warning says so (nothing can write it while the tick is stopped): the gap, the
// limit and the level the answer was held for. Not at the limit exactly, not for a gap after an answer that was not combat (arena's readers were never
// affected), and once per gap, not once per look.
void AStaleGapIsOneWarningWhenTheTickIsBack() {
  Begin();
  const auto idOf = [](const char* name) {
    char want[48];
    std::snprintf(want, sizeof(want), "\"level\":\"0x%016llx\"", static_cast<unsigned long long>(EchoVR::CSymbol64Hash(name)));
    return std::string(want);
  };
  LookAt({"mpl_combat_dyson"});
  g_nowMs += scope::kStaleAfterMs;  // exactly the limit: the answer was still believed, so nothing to say
  scope::Refresh(g_model.Address());
  QCHECK(Lines("combat_scope_stale").empty());
  g_nowMs += scope::kStaleAfterMs + 1;  // a millisecond over
  g_model.Only({"mpl_arena_a"});          // and the game is elsewhere when the tick is back: the record names the level held
  scope::Refresh(g_model.Address());
  std::vector<Line> stale = Lines("combat_scope_stale");
  QCHECK(stale.size() == 1U);
  if (stale.size() == 1U) {
    QCHECK(stale[0].level == sentinel::LogLevel::kWarn);
    QCHECK(Has(stale[0].text, "\"gap_ms\":1001,\"stale_after_ms\":1000,") && Has(stale[0].text, idOf("mpl_combat_dyson")));
    QCHECK(Has(stale[0].text, "\"msg\":\"the combat tick stopped for longer than the level answer is believed: every combat module that reads it "
                              "passed every call through to the game in that gap\""));
  }
  Look();  // the next looks age from the one that said it: no second record
  Look();
  QCHECK(Lines("combat_scope_stale").size() == 1U);
  // a gap after an arena answer says nothing: no reader was acting on a combat level
  g_nowMs += 5 * scope::kStaleAfterMs;
  scope::Refresh(g_model.Address());
  QCHECK(Lines("combat_scope_stale").size() == 1U);
  // a second gap after combat again is a second record, naming its own level (the combat lobby)
  LookAt({"mpl_lobby_b_combat"});
  g_nowMs += 3000;
  scope::Refresh(g_model.Address());
  stale = Lines("combat_scope_stale");
  QCHECK(stale.size() == 2U && (stale.size() < 2U || (Has(stale[1].text, idOf("mpl_lobby_b_combat")) && Has(stale[1].text, "\"gap_ms\":3000"))));
  End();
}

// The real clock: milliseconds of CLOCK_MONOTONIC. The look spacing and the staleness limit are in that unit, so a conversion that drops the seconds, the
// factor or the nanosecond divisor changes how often the game is looked at and when an answer goes stale; the fake clock of the other tests cannot see it.
void TheRealClockIsInMilliseconds() {
  using Steady = std::chrono::steady_clock;  // CLOCK_MONOTONIC, the clock the module reads
  const auto ms = [](Steady::time_point from, Steady::time_point to) { return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count(); };
  Begin();
  scope::SetClockForTest(nullptr);
  g_model.Only({"mpl_combat_dyson"});
  // The window of the Refresh that made the last look (the one that raised Looks()): it was taken between `lookBefore` and `lookAfter`. Every assertion below is
  // made against the time that really passed since then, so a thread held off by a loaded machine, at any point, cannot fail one: only a bound that the
  // measured time proves is asserted (on an unloaded machine all of them are).
  Steady::time_point lookBefore{}, lookAfter{};
  const auto refresh = [&] {
    const auto before = Steady::now();
    const std::uint32_t looks = scope::Looks();
    scope::Refresh(g_model.Address());
    const auto after = Steady::now();
    if (scope::Looks() != looks) {
      lookBefore = before;
      lookAfter = after;
    }
    return std::make_pair(before, after);
  };
  // After a Refresh at [before, after] with the last look at [lookBefore, lookAfter] as it stood before it: the gap to that look is in [lo, hi] ms. Each
  // reading of the module's clock is floored to whole ms, so the spacing test can read one ms more than the real gap: "no look" is proved only below
  // kLookEveryMs - 1, and "a look" from kLookEveryMs on.
  const auto checkSpacing = [&](Steady::time_point prevBefore, Steady::time_point prevAfter, Steady::time_point before, Steady::time_point after, std::uint32_t looksWas) {
    const long lo = ms(prevAfter, before), hi = ms(prevBefore, after);
    if (hi < scope::kLookEveryMs - 1) QCHECK(scope::Looks() == looksWas);
    if (lo >= scope::kLookEveryMs) QCHECK(scope::Looks() == looksWas + 1);
  };
  // The answer is fresh when the last look is within kStaleAfterMs: asserted only when the time since the last look, measured after the call, is below it.
  const auto checkFresh = [&] {
    const bool answer = scope::InCombatOrLobby();
    if (ms(lookBefore, Steady::now()) < scope::kStaleAfterMs - 1) QCHECK(answer);
  };

  refresh();
  QCHECK(scope::Looks() == 1);
  checkFresh();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  Steady::time_point pb = lookBefore, pa = lookAfter;
  std::uint32_t was = scope::Looks();
  auto w = refresh();
  checkSpacing(pb, pa, w.first, w.second, was);  // about 100 ms: no look, unless the thread was held off past the spacing
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  pb = lookBefore;
  pa = lookAfter;
  was = scope::Looks();
  w = refresh();
  checkSpacing(pb, pa, w.first, w.second, was);  // about 300 ms since the first look, about 200 since the second if that one looked
  QCHECK(scope::Looks() >= 2);                   // by now the game has been looked at again
  checkFresh();
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  QCHECK(!scope::InCombatOrLobby() && !scope::InCombatMap());  // more than 1.1 s without a look: stale, and any stall only makes it staler
  pb = lookBefore;
  pa = lookAfter;
  was = scope::Looks();
  w = refresh();
  QCHECK(scope::Looks() == was + 1);  // more than 1 s since the last look: a look, whatever the load
  const std::vector<Line> stale = Lines("combat_scope_stale");
  QCHECK(!stale.empty());  // (a stall of more than a second earlier in the test would add a record before this one)
  if (!stale.empty()) {    // the gap the last record carries is the real gap to the look before it, in ms
    const std::size_t at = stale.back().text.find("\"gap_ms\":");
    const long gap = at == std::string::npos ? 0 : std::strtol(stale.back().text.c_str() + at + 9, nullptr, 10);
    QCHECK(gap >= ms(pa, w.first) - 1 && gap <= ms(pb, w.second) + 1);  // each reading is floored on its own
    QCHECK(gap >= 1099);
  }
  End();
}

void TheRowRegistersTheRefreshOnceAndSaysSo() {
  Begin();
  QCHECK(tick::Count() == 0 && !scope::Live());
  nevr_quest::combat::ModuleResult r = scope::InstallNormal();
  QCHECK(r.installed && std::strcmp(r.status, "installed") == 0 && scope::Live());
  QCHECK(tick::Count() == 1);
  r = scope::InstallNormal();
  QCHECK(r.installed && tick::Count() == 1);  // asked again: the same registration
  const std::vector<Line> install = Lines("combat_scope_install");
  QCHECK(install.size() == 1U);
  if (install.size() == 1U) {
    QCHECK(install[0].level == sentinel::LogLevel::kInfo && Has(install[0].text, "\"result\":\"registered\",\"reason\":\"ok\""));
    QCHECK(Has(install[0].text, "\"msg\":\"the combat level answer is refreshed from the game-thread tick, four times a second, for every combat module that reads it\""));
  }
  End();
}

void ATickThatIsNotInstalledRegistersNothing() {
  Begin(/*tickInstalled=*/false);
  const nevr_quest::combat::ModuleResult r = scope::InstallNormal();
  QCHECK(!r.installed && std::strcmp(r.status, "tick_not_installed") == 0 && !scope::Live() && tick::Count() == 0);
  const std::vector<Line> install = Lines("combat_scope_install");
  QCHECK(install.size() == 1U);
  if (install.size() == 1U) {
    QCHECK(install[0].level == sentinel::LogLevel::kError && Has(install[0].text, "\"reason\":\"tick_not_installed\""));
    QCHECK(Has(install[0].text, "\"msg\":\"the combat level answer cannot change, so the combat modules that read it are not installed and the game keeps its own behaviour\""));
  }
  QCHECK(!scope::InCombatOrLobby());
  End();
}

void AFullTickTableRefusesAndTheAnswerStaysFalse() {
  Begin();
  for (std::size_t i = 0; i < tick::kMaxCallbacks; ++i) QCHECK(tick::Add([](std::uintptr_t) noexcept {}));
  const nevr_quest::combat::ModuleResult r = scope::InstallNormal();
  QCHECK(!r.installed && std::strcmp(r.status, "tick_table_full") == 0 && !scope::Live());
  const std::vector<Line> install = Lines("combat_scope_install");
  QCHECK(install.size() == 1U);
  if (install.size() == 1U) QCHECK(install[0].level == sentinel::LogLevel::kError && Has(install[0].text, "\"result\":\"refused\""));
  QCHECK(!scope::InCombatOrLobby());
  QCHECK(!scope::InstallNormal().installed);  // asked again: still refused, not remembered as registered
  End();
}

std::uint64_t Id(const char* name) { return EchoVR::CSymbol64Hash(name); }

// The last look as one record (LastLook): its classes, the level that decided it (the change line's), the count of looks and whether it is fresh; and the
// arena question built on it (SeenEchoArena), which is true with no look, with a stale look and with an Echo Arena level, and false only for a fresh look
// that saw none.
void TheLastLookIsOneRecordAndTheArenaQuestionFailsClosed() {
  Begin();
  scope::Look look = scope::LastLook();
  QCHECK(look.bits == 0 && look.level == 0 && look.looks == 0 && !look.fresh);
  QCHECK(scope::SeenEchoArena());  // no look yet: arena is not ruled out
  LookAt({"mpl_combat_dyson"});
  look = scope::LastLook();
  QCHECK(look.bits == scope::kBitCombatMap && look.level == Id("mpl_combat_dyson") && look.looks == 1 && look.fresh);
  QCHECK(!scope::SeenEchoArena());
  LookAt({"mpl_combat_gauss", "mpl_arena_a"});  // a transition: arena wins, and the record names the arena level
  look = scope::LastLook();
  QCHECK(look.bits == (scope::kBitCombatMap | scope::kBitEchoArena) && look.level == Id("mpl_arena_a") && look.looks == 2 && look.fresh);
  QCHECK(scope::SeenEchoArena());
  for (const char* name : {"mpl_arena_a_lowspec", "mpl_arena_b", "mpl_tutorial_a"}) {
    LookAt({name});
    QCHECK(scope::SeenEchoArena() && scope::LastLook().level == Id(name) && scope::LastLook().bits == scope::kBitEchoArena);
  }
  LookAt({"mpl_lobby_b_combat", "mpl_lobby_b"});
  look = scope::LastLook();
  QCHECK(look.bits == scope::kBitCombatLobby && look.level == Id("mpl_lobby_b_combat") && !scope::SeenEchoArena());
  LookAt({"mpl_lobby_b_combat", "mpl_combat_fission"});
  look = scope::LastLook();
  QCHECK(look.bits == (scope::kBitCombatMap | scope::kBitCombatLobby) && look.level == Id("mpl_combat_fission"));
  LookAt({"mpl_lobby_b"});  // a lobby arena players use: no class, no level, and not arena
  look = scope::LastLook();
  QCHECK(look.bits == 0 && look.level == 0 && look.looks == 8 && look.fresh && !scope::SeenEchoArena());
  // staleness: believed at the limit, not a millisecond after; the record keeps its classes and level
  const std::int64_t looked = g_nowMs;
  g_nowMs = looked + scope::kStaleAfterMs;
  QCHECK(scope::LastLook().fresh && !scope::SeenEchoArena());
  g_nowMs = looked + scope::kStaleAfterMs + 1;
  look = scope::LastLook();
  QCHECK(!look.fresh && look.looks == 8 && look.bits == 0);
  QCHECK(scope::SeenEchoArena());  // the tick stopped: arena is not ruled out
  LookAt({"mpl_combat_dyson"});
  QCHECK(scope::LastLook().fresh && !scope::SeenEchoArena());
  // an unreadable game is no level, freshly looked
  g_nowMs += scope::kLookEveryMs;
  scope::Refresh(0);
  look = scope::LastLook();
  QCHECK(look.bits == 0 && look.level == 0 && look.fresh && !scope::SeenEchoArena());
  End();
}

// The questions on a record already taken (InCombatMap(look)), for a caller that also names the level of the look it decided on (dronekill's change
// line): the same rule as the live question, on one record.
void TheRecordQuestionsAnswerAsTheLiveOnes() {
  Begin();
  QCHECK(!scope::InCombatMap(scope::Look{}) && !scope::InCombatMap(scope::LastLook()));  // no look yet
  LookAt({"mpl_combat_dyson"});
  QCHECK(scope::InCombatMap(scope::LastLook()) && scope::InCombatMap());
  LookAt({"mpl_combat_gauss", "mpl_arena_a"});  // a transition: arena wins
  QCHECK(!scope::InCombatMap(scope::LastLook()) && !scope::InCombatMap());
  LookAt({"mpl_lobby_b_combat"});  // the combat lobby is not a combat map
  QCHECK(!scope::InCombatMap(scope::LastLook()) && !scope::InCombatMap());
  LookAt({"mpl_lobby_b_combat", "mpl_combat_fission"});
  QCHECK(scope::InCombatMap(scope::LastLook()) && scope::InCombatMap());
  // staleness: believed at the limit, not a millisecond after; the record keeps its classes
  const std::int64_t looked = g_nowMs;
  g_nowMs = looked + scope::kStaleAfterMs;
  QCHECK(scope::InCombatMap(scope::LastLook()));
  g_nowMs = looked + scope::kStaleAfterMs + 1;
  const scope::Look stale = scope::LastLook();
  QCHECK((stale.bits & scope::kBitCombatMap) != 0 && !stale.fresh && !scope::InCombatMap(stale) && !scope::InCombatMap());
  End();
}

// A record read while a look is being written is not handed out (its fields could be of two looks): the read gives up after kLookReadTries and answers
// "no look, not fresh", so SeenEchoArena is true. Once the write ends, the new look is read whole.
scope::Look g_midWrite;
bool g_midArena = false;
int g_seamCalls = 0;
void ReadMidWrite() {
  ++g_seamCalls;
  g_midWrite = scope::LastLook();
  g_midArena = scope::SeenEchoArena();
}
int g_lookedDuringRead = 0;
void LookOnceDuringARead() {
  if (g_lookedDuringRead++ != 0) return;
  Look();
}
void AHalfWrittenLookIsNeverHandedOut() {
  Begin();
  LookAt({"mpl_combat_dyson"});
  scope::SetLookSeamForTest(&ReadMidWrite);
  g_midWrite = scope::Look{};
  g_midWrite.looks = 99;
  LookAt({"mpl_arena_a"});
  QCHECK(g_seamCalls == 1);
  QCHECK(g_midWrite.looks == 0 && g_midWrite.bits == 0 && g_midWrite.level == 0 && !g_midWrite.fresh);
  QCHECK(g_midArena);
  scope::SetLookSeamForTest(nullptr);
  scope::Look look = scope::LastLook();
  QCHECK(look.bits == scope::kBitEchoArena && look.level == Id("mpl_arena_a") && look.looks == 2 && look.fresh);
  // A look written while a read is under way (between its classes and its level): the read starts again and hands out the new look whole, never the
  // old classes with the new level.
  g_model.Only({"mpl_combat_gauss"});
  scope::SetLookReadSeamForTest(&LookOnceDuringARead);
  look = scope::LastLook();
  scope::SetLookReadSeamForTest(nullptr);
  QCHECK(g_lookedDuringRead == 2);  // the first read met the look, the second read found the record whole
  QCHECK(look.bits == scope::kBitCombatMap && look.level == Id("mpl_combat_gauss") && look.looks == 3 && look.fresh);
  End();
}

// The moment a look's sequence count closes, every field of that look is written: a record read there is the new look whole (its classes, its level, its
// count and its stamp), never the new classes with the old level. The look comes two seconds after the one before it, so a stamp written after the close
// would read as stale there.
scope::Look g_atClose;
void ReadAtClose() { g_atClose = scope::LastLook(); }
void AClosedLookIsWholeTheMomentItCloses() {
  Begin();
  LookAt({"mpl_combat_dyson"});
  scope::SetLookClosedSeamForTest(&ReadAtClose);
  g_atClose = scope::Look{};
  g_nowMs += 2000;
  g_model.Only({"mpl_arena_a"});
  scope::Refresh(g_model.Address());
  scope::SetLookClosedSeamForTest(nullptr);
  QCHECK(g_atClose.bits == scope::kBitEchoArena);
  QCHECK(g_atClose.level == Id("mpl_arena_a"));
  QCHECK(g_atClose.looks == 2 && g_atClose.fresh);
  // the rule on a record already taken: the same answer as the call that takes its own
  QCHECK(scope::SeenEchoArena(g_atClose) && scope::SeenEchoArena(scope::Look{}));
  scope::Look combat;
  combat.bits = scope::kBitCombatMap;
  combat.looks = 1;
  combat.fresh = true;
  QCHECK(!scope::SeenEchoArena(combat));
  combat.fresh = false;
  QCHECK(scope::SeenEchoArena(combat));
  End();
}

}  // namespace

int main() {
  // The process starts with no combat level: the answer's own initial value, before any reset the tests make. The unchecked views the arena path reads
  // are false too (a word that started with a combat bit would be believed by them: the staleness test is made only on the combat path).
  QCHECK(!scope::InCombatOrLobby() && !scope::InCombatMap() && scope::Looks() == 0 && !scope::Live());
  QCHECK(!scope::SeenCombatOrLobby() && !scope::SeenCombatMap());
  QCHECK(scope::SeenEchoArena() && scope::LastLook().looks == 0);  // and arena is not ruled out
  QCHECK(scope::kLookEveryMs == 250 && scope::kStaleAfterMs == 1000);  // four looks a second; a second without one is a stopped tick
  FailsClosedUntilTheFirstLook();
  TheTwoAnswersDifferExactlyAtTheCombatLobby();
  AnEchoArenaLevelWinsOverACombatLevelStillLoaded();
  AnUnreadableGameIsNotCombat();
  TheGameIsLookedAtOncePerSpacingNotOncePerFrame();
  ATrueAnswerOlderThanTheStalenessLimitIsFalse();
  AFalseAnswerStaysFalseWhateverTheTime();
  AChangeIsOneInfoLineWithItsValueAndNoChangeIsNoLine();
  TheChangeLineNamesTheLevelThatDecided();
  AStaleGapIsOneWarningWhenTheTickIsBack();
  TheRealClockIsInMilliseconds();
  TheRowRegistersTheRefreshOnceAndSaysSo();
  LiveFollowsTheTickAndTheTickInstallIsHonest();
  ATickThatIsNotInstalledRegistersNothing();
  AFullTickTableRefusesAndTheAnswerStaysFalse();
  TheLastLookIsOneRecordAndTheArenaQuestionFailsClosed();
  TheRecordQuestionsAnswerAsTheLiveOnes();
  AHalfWrittenLookIsNeverHandedOut();
  AClosedLookIsWholeTheMomentItCloses();
  if (quest_test::Failures() == 0) std::printf("combat_scope_test: all checks passed\n");
  return quest_test::Failures() == 0 ? 0 : 1;
}
