// Host test of the level scope (src/quest/combat/level_scope.cpp, #2178): what the guards that act only in a combat level ask. The game is a model
// (combat_game_model.h): CR15Game's gamespace tables and each gamespace's level, read by process_vm_readv as in production. The scope is driven
// through its tick callback with a test clock.
//
// What it pins: both answers are false until the first look; a combat map is `InCombatMap` and `InCombatMapOrLobby`; the combat lobby is only the
// second; every lobby arena players use and every other level is neither; an Echo Arena level wins over a combat level still loaded during a
// transition; one look per half second whatever the frame rate; an unreadable game is no level; Start registers the tick once and says so when
// the table is full.
//
// Built and run by `just test-quest-combat`.

#include "quest/combat/level_scope.h"

#include <cstdint>
#include <cstdio>

#include "quest/combat/combat_tick.h"
#include "quest/tests/combat_game_model.h"
#include "quest/tests/test_check.h"

namespace {

namespace scope = nevr_quest::combat::scope;
namespace tick = nevr_quest::combat::tick;
namespace gm = combat_test::game_model;

void Begin() {
  scope::ResetForTest();
  tick::ResetForTest();
  gm::ClearGame();
  gm::Now() = 0.0;
  scope::SetClockForTest(&gm::Clock);
}

void BothAnswersAreFalseUntilTheFirstLook() {
  Begin();
  gm::PutLevel(0, 0, EchoVR::CSymbol64Hash("mpl_combat_dyson"));
  QCHECK(!scope::InCombatMap() && !scope::InCombatMapOrLobby() && scope::Looks() == 0);
  scope::Tick(gm::GameAddress());  // the clock is at 0: the schedule starts at 0, so this is the first look
  QCHECK(scope::InCombatMap() && scope::InCombatMapOrLobby() && scope::Looks() == 1);
  scope::ResetForTest();
  QCHECK(!scope::InCombatMap() && !scope::InCombatMapOrLobby() && scope::Looks() == 0);
}

void EachLevelClassIsToldApart() {
  struct Case {
    const char* name;
    bool map;
    bool mapOrLobby;
  };
  const Case cases[] = {{"mpl_combat_dyson", true, true},    {"mpl_combat_combustion", true, true}, {"mpl_combat_fission", true, true},
                        {"mpl_combat_gauss", true, true},    {"mpl_lobby_b_combat", false, true},   {"mpl_lobby_b", false, false},
                        {"mpl_lobby_b2", false, false},      {"mpl_lobby_b2_lowspec", false, false}, {"mpl_lobby_b_arena", false, false},
                        {"mpl_lobby_b_social", false, false}, {"mpl_arena_a", false, false},         {"mpl_arena_a_lowspec", false, false},
                        {"mpl_arena_b", false, false},       {"mpl_tutorial_a", false, false},      {"mpl_somewhere_else", false, false}};
  for (const Case& c : cases) {
    Begin();
    gm::SetLevels({c.name});
    if (scope::InCombatMap() != c.map || scope::InCombatMapOrLobby() != c.mapOrLobby) std::fprintf(stderr, "level scope: %s\n", c.name);
    QCHECK(scope::InCombatMap() == c.map);
    QCHECK(scope::InCombatMapOrLobby() == c.mapOrLobby);
  }
  Begin();
  gm::SetLevels({});
  QCHECK(!scope::InCombatMap() && !scope::InCombatMapOrLobby() && scope::Looks() == 1);
}

void AnEchoArenaLevelWinsOverACombatLevelStillLoaded() {
  for (const char* arena : {"mpl_arena_a", "mpl_arena_a_lowspec", "mpl_arena_b", "mpl_tutorial_a"}) {
    Begin();
    gm::SetLevels({"mpl_combat_gauss", "mpl_lobby_b_combat", arena});
    QCHECK(!scope::InCombatMap() && !scope::InCombatMapOrLobby());
  }
  Begin();
  gm::SetLevels({"mpl_combat_gauss", "mpl_lobby_b_combat"});  // a map and its lobby: both
  QCHECK(scope::InCombatMap() && scope::InCombatMapOrLobby());
}

void ItLooksOnceEveryHalfSecondWhateverTheFrameRate() {
  Begin();
  gm::SetLevels({"mpl_lobby_b2"});
  QCHECK(scope::Looks() == 1 && !scope::InCombatMap());
  gm::ClearGame();
  gm::PutLevel(0, 0, EchoVR::CSymbol64Hash("mpl_combat_dyson"));
  for (int frame = 0; frame < 40; ++frame) {  // 40 frames at 90 Hz: 0.44 s, short of the schedule
    gm::Now() += 1.0 / 90.0;
    scope::Tick(gm::GameAddress());
  }
  QCHECK(scope::Looks() == 1 && !scope::InCombatMap());  // no look yet: the answer is the last look's
  gm::Now() += 0.1;
  scope::Tick(gm::GameAddress());
  QCHECK(scope::Looks() == 2 && scope::InCombatMap());
  for (int frame = 0; frame < 100; ++frame) {  // 1.1 s: two more looks (at 0.5 s and at 1.0 s)
    gm::Now() += 1.0 / 90.0;
    scope::Tick(gm::GameAddress());
  }
  QCHECK(scope::Looks() == 4);
}

void AnUnreadableGameIsNoLevel() {
  Begin();
  gm::SetLevels({"mpl_combat_dyson"});
  QCHECK(scope::InCombatMap());
  gm::Now() += 1.0;
  scope::Tick(0x1000);  // a game the read cannot reach: the next look says no level
  QCHECK(!scope::InCombatMap() && !scope::InCombatMapOrLobby() && scope::Looks() == 2);
  gm::Now() += 1.0;
  scope::Tick(0);
  QCHECK(!scope::InCombatMap() && scope::Looks() == 3);
}

void StartRegistersTheTickOnceAndSaysWhenTheTableIsFull() {
  Begin();
  QCHECK(tick::Count() == 0);
  QCHECK(scope::Start() && tick::Count() == 1);
  QCHECK(scope::Start() && scope::Start() && tick::Count() == 1);  // once per process, whichever module asks
  Begin();
  for (std::size_t i = 0; i < tick::kMaxCallbacks; ++i) QCHECK(tick::Add([](std::uintptr_t) noexcept {}));
  QCHECK(!scope::Start() && tick::Count() == tick::kMaxCallbacks);
  QCHECK(!scope::Start());  // and it keeps saying so
}

}  // namespace

int main() {
  BothAnswersAreFalseUntilTheFirstLook();
  EachLevelClassIsToldApart();
  AnEchoArenaLevelWinsOverACombatLevelStillLoaded();
  ItLooksOnceEveryHalfSecondWhateverTheFrameRate();
  AnUnreadableGameIsNoLevel();
  StartRegistersTheTickOnceAndSaysWhenTheTableIsFull();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "level_scope_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::puts("level_scope_test: all checks passed");
  return 0;
}
