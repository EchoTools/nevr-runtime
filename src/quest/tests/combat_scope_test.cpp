// Host test of the shared "is a combat level loaded" cache (#2186, src/quest/combat/combat_scope.cpp): the production refresh against a model of
// CR15Game's gamespace tables (combat_game_model.h), a fake clock, and the real combat tick's callback table.
//
// What it pins: the answer fails closed (false before the first look, with the game unreadable, and when the tick's table is full), it is true
// for a combat map and the combat lobby and false for every Echo Arena level and every lobby arena players use, an Echo Arena level wins over a
// combat level still loaded, the game is looked at once per kLookEveryMs (not once per frame), and a change of the answer is one Info line while
// no change is no line (an arena match writes nothing).
//
// Built and run by `just test-quest-combat`.

#include "quest/combat/combat_scope.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "quest/combat/combat_tick.h"
#include "quest/sentinel/hook_log.h"
#include "quest/tests/combat_game_model.h"
#include "quest/tests/test_check.h"

namespace {

namespace scope = nevr_quest::combat::scope;
namespace tick = nevr_quest::combat::tick;
using combat_test::GameModel;

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

void Begin() {
  g_lines.clear();
  g_nowMs = 1000;
  g_model.Clear();
  tick::ResetForTest();
  scope::ResetForTest();
  scope::SetClockForTest(&Clock);
  sentinel::SetLogSink(&Capture);
}
void End() {
  scope::ResetForTest();
  tick::ResetForTest();
  sentinel::SetLogSink(nullptr);
}

// One look at the model: the clock moves past the spacing and the refresh runs.
void Look() {
  g_nowMs += scope::kLookEveryMs;
  scope::Refresh(g_model.Address());
}
bool InCombatWith(std::initializer_list<const char*> levels) {
  g_model.Only(levels);
  Look();
  return scope::InCombat();
}

void FailsClosedUntilTheFirstLook() {
  Begin();
  QCHECK(!scope::InCombat());
  QCHECK(scope::Looks() == 0);
  g_model.Only({"mpl_combat_dyson"});
  QCHECK(!scope::InCombat());  // the game is a combat map, but nothing has looked yet
  Look();
  QCHECK(scope::InCombat() && scope::Looks() == 1);
  End();
}

void ACombatMapAndTheCombatLobbyAreCombatAndNothingElseIs() {
  Begin();
  for (const char* name : {"mpl_combat_dyson", "mpl_combat_combustion", "mpl_combat_fission", "mpl_combat_gauss", "mpl_lobby_b_combat"}) {
    QCHECK(InCombatWith({name}));
    QCHECK(!InCombatWith({}));  // back to nothing loaded
  }
  // Every Echo Arena level, and every lobby arena players use: not combat.
  for (const char* name : {"mpl_arena_a", "mpl_arena_a_lowspec", "mpl_arena_b", "mpl_tutorial_a", "mpl_lobby_b", "mpl_lobby_b2",
                           "mpl_lobby_b2_lowspec", "mpl_lobby_b_arena", "mpl_lobby_b_social"}) {
    QCHECK(!InCombatWith({name}));
  }
  End();
}

void AnEchoArenaLevelWinsOverACombatLevelStillLoaded() {
  Begin();
  QCHECK(InCombatWith({"mpl_combat_gauss"}));
  QCHECK(!InCombatWith({"mpl_combat_gauss", "mpl_arena_a"}));  // a transition from combat to arena: arena wins
  QCHECK(!InCombatWith({"mpl_arena_a", "mpl_lobby_b_combat"}));
  QCHECK(InCombatWith({"mpl_lobby_b", "mpl_combat_dyson"}));  // an arena-style lobby does not mask a combat map
  End();
}

void AnUnreadableGameIsNotCombat() {
  Begin();
  QCHECK(InCombatWith({"mpl_combat_dyson"}));
  g_nowMs += scope::kLookEveryMs;
  scope::Refresh(0);  // no game
  QCHECK(!scope::InCombat());
  g_model.Only({"mpl_combat_dyson"});
  g_nowMs += scope::kLookEveryMs;
  scope::Refresh(0x1000);  // an unmapped address
  QCHECK(!scope::InCombat());
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
  QCHECK(scope::Looks() == 2 && scope::InCombat());
  g_model.Only({"mpl_arena_b"});
  g_nowMs += scope::kLookEveryMs - 1;
  scope::Refresh(g_model.Address());
  QCHECK(scope::Looks() == 2 && scope::InCombat());  // not yet
  ++g_nowMs;
  scope::Refresh(g_model.Address());
  QCHECK(scope::Looks() == 3 && !scope::InCombat());
  End();
}

void AChangeIsOneInfoLineAndNoChangeIsNoLine() {
  Begin();
  QCHECK(!InCombatWith({"mpl_arena_a"}));
  QCHECK(!InCombatWith({"mpl_arena_b"}));
  QCHECK(!InCombatWith({}));
  QCHECK(Lines("combat_scope_change").empty());  // an arena match writes nothing
  QCHECK(InCombatWith({"mpl_combat_fission"}));
  QCHECK(InCombatWith({"mpl_combat_fission"}));
  std::vector<Line> change = Lines("combat_scope_change");
  QCHECK(change.size() == 1U);
  if (change.size() == 1U) {
    QCHECK(change[0].level == sentinel::LogLevel::kInfo);
    QCHECK(Has(change[0].text, "\"from\":0,\"to\":1") && Has(change[0].text, "\"level\":\"0x") && Has(change[0].text, "\"looks\":4"));
    QCHECK(Has(change[0].text, "a combat level is loaded"));
  }
  QCHECK(!InCombatWith({"mpl_arena_a"}));
  change = Lines("combat_scope_change");
  QCHECK(change.size() == 2U);
  if (change.size() == 2U) QCHECK(Has(change[1].text, "\"from\":1,\"to\":0") && Has(change[1].text, "pass every call through"));
  End();
}

void EnrollRegistersTheRefreshOnceAndSaysSo() {
  Begin();
  QCHECK(tick::Count() == 0);
  QCHECK(scope::Enroll());
  QCHECK(tick::Count() == 1);
  QCHECK(scope::Enroll() && scope::Enroll());
  QCHECK(tick::Count() == 1);  // two modules enrol, one callback runs
  const std::vector<Line> enroll = Lines("combat_scope_enroll");
  QCHECK(enroll.size() == 1U);
  if (enroll.size() == 1U) QCHECK(enroll[0].level == sentinel::LogLevel::kInfo && Has(enroll[0].text, "\"result\":\"registered\""));
  End();
}

void AFullTickTableRefusesAndTheAnswerStaysFalse() {
  Begin();
  for (std::size_t i = 0; i < tick::kMaxCallbacks; ++i) QCHECK(tick::Add([](std::uintptr_t) noexcept {}));
  QCHECK(!scope::Enroll());
  const std::vector<Line> enroll = Lines("combat_scope_enroll");
  QCHECK(enroll.size() == 1U);
  if (enroll.size() == 1U) QCHECK(enroll[0].level == sentinel::LogLevel::kError && Has(enroll[0].text, "\"result\":\"refused\""));
  QCHECK(!scope::InCombat());
  QCHECK(!scope::Enroll());  // asking again is asking again: still refused, not remembered as enrolled
  End();
}

}  // namespace

int main() {
  // The process starts with no combat level: the answer's own initial value, before any reset the tests make.
  QCHECK(!scope::InCombat() && scope::Looks() == 0);
  QCHECK(scope::kLookEveryMs == 250);  // four looks a second: what the header and the docs say
  FailsClosedUntilTheFirstLook();
  ACombatMapAndTheCombatLobbyAreCombatAndNothingElseIs();
  AnEchoArenaLevelWinsOverACombatLevelStillLoaded();
  AnUnreadableGameIsNotCombat();
  TheGameIsLookedAtOncePerSpacingNotOncePerFrame();
  AChangeIsOneInfoLineAndNoChangeIsNoLine();
  EnrollRegistersTheRefreshOnceAndSaysSo();
  AFullTickTableRefusesAndTheAnswerStaysFalse();
  if (quest_test::Failures() == 0) std::printf("combat_scope_test: all checks passed\n");
  return quest_test::Failures() == 0 ? 0 : 1;
}
