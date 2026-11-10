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
#include <cstring>
#include <string>
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
    QCHECK(Has(change[0].text, "a combat level is loaded"));
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
    QCHECK(Has(change[2].text, want) && Has(change[2].text, "pass every call through"));
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
  if (install.size() == 1U) QCHECK(install[0].level == sentinel::LogLevel::kInfo && Has(install[0].text, "\"result\":\"registered\",\"reason\":\"ok\""));
  End();
}

void ATickThatIsNotInstalledRegistersNothing() {
  Begin(/*tickInstalled=*/false);
  const nevr_quest::combat::ModuleResult r = scope::InstallNormal();
  QCHECK(!r.installed && std::strcmp(r.status, "tick_not_installed") == 0 && !scope::Live() && tick::Count() == 0);
  const std::vector<Line> install = Lines("combat_scope_install");
  QCHECK(install.size() == 1U);
  if (install.size() == 1U) QCHECK(install[0].level == sentinel::LogLevel::kError && Has(install[0].text, "\"reason\":\"tick_not_installed\""));
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

}  // namespace

int main() {
  // The process starts with no combat level: the answer's own initial value, before any reset the tests make.
  QCHECK(!scope::InCombatOrLobby() && !scope::InCombatMap() && scope::Looks() == 0 && !scope::Live());
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
  TheRowRegistersTheRefreshOnceAndSaysSo();
  LiveFollowsTheTickAndTheTickInstallIsHonest();
  ATickThatIsNotInstalledRegistersNothing();
  AFullTickTableRefusesAndTheAnswerStaysFalse();
  if (quest_test::Failures() == 0) std::printf("combat_scope_test: all checks passed\n");
  return quest_test::Failures() == 0 ? 0 : 1;
}
