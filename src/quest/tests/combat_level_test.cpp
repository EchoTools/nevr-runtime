// Host test for the shared "what level is loaded" answer (#2172, src/quest/combat/combat_level.cpp): the combat modules ask it whether a combat
// map, an Echo Arena level or the social lobby is loaded, so arena (same APK as combat) is told apart from combat by the level, not by the APK.
// The game is a model: CR15Game's two gamespace tables and a gamespace's level, laid out where the module reads them (the offsets are spelled in
// this file, not taken from the header), read by process_vm_readv as in production.
//
// Built and run by `just test-quest-combat`.

#include "quest/combat/combat_level.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

#include "nevr/symbol_hash.h"
#include "quest/tests/test_check.h"

namespace {

namespace lv = nevr_quest::combat::level;
using EchoVR::CSymbol64Hash;

constexpr std::size_t kTable0 = 0x1f48, kCount0 = 0x1f78, kTable1 = 0x1f88, kCount1 = 0x1fb8;
constexpr std::size_t kGamespaceOwner = 0x8, kGamespaceLevel = 0x58;
constexpr std::size_t kFiberRows = 0x1ed8, kFiberCount = 0x1f08, kFiberGamespace = 0x30;  // spelled here, not taken from the header

struct Gamespace {
  std::uintptr_t words[0x60 / 8 + 1] = {};
};
struct Game {
  std::uintptr_t words[0x2000 / 8] = {};
  std::uintptr_t table0[8][2] = {};
  std::uintptr_t table1[8][2] = {};
  Gamespace spaces[8];
  std::uintptr_t fiberRows[4] = {};
  std::uintptr_t fibers[4][8] = {};
  Gamespace flight[4];
};
Game g_game;
std::uintptr_t GameAddress() { return reinterpret_cast<std::uintptr_t>(g_game.words); }
void ClearGame() { g_game = Game{}; }

void PutLevel(int table, int row, std::uint64_t level, std::uintptr_t owner = 0) {
  Gamespace& gs = g_game.spaces[table * 4 + row];
  gs.words[kGamespaceOwner / 8] = owner != 0 ? owner : GameAddress();
  gs.words[kGamespaceLevel / 8] = static_cast<std::uintptr_t>(level);
  std::uintptr_t(*rows)[2] = table == 0 ? g_game.table0 : g_game.table1;
  rows[row][0] = 0x5555;
  rows[row][1] = reinterpret_cast<std::uintptr_t>(&gs);
  g_game.words[(table == 0 ? kTable0 : kTable1) / 8] = reinterpret_cast<std::uintptr_t>(rows);
  std::uintptr_t& count = g_game.words[(table == 0 ? kCount0 : kCount1) / 8];
  if (static_cast<std::uintptr_t>(row) + 1 > count) count = static_cast<std::uintptr_t>(row) + 1;
}

// A gamespace still loading: its init fiber (gamespace at fiber + 0x30) is in the table at game + 0x1ed8, and it is in neither of the two tables.
void PutInFlight(int row, std::uint64_t level, bool withGamespace = true) {
  Gamespace& gs = g_game.flight[row];
  gs.words[kGamespaceOwner / 8] = GameAddress();
  gs.words[kGamespaceLevel / 8] = static_cast<std::uintptr_t>(level);
  g_game.fibers[row][kFiberGamespace / 8] = withGamespace ? reinterpret_cast<std::uintptr_t>(&gs) : 0;
  g_game.fiberRows[row] = reinterpret_cast<std::uintptr_t>(g_game.fibers[row]);
  g_game.words[kFiberRows / 8] = reinterpret_cast<std::uintptr_t>(g_game.fiberRows);
  std::uintptr_t& count = g_game.words[kFiberCount / 8];
  if (static_cast<std::uintptr_t>(row) + 1 > count) count = static_cast<std::uintptr_t>(row) + 1;
}

void EveryLevelIdIsTheCSymbol64OfItsNameAndOfItsClass() {
  const char* otherLobbies[] = {"mpl_lobby_b", "mpl_lobby_b2", "mpl_lobby_b2_lowspec", "mpl_lobby_b_arena", "mpl_lobby_b_social"};
  const char* combats[] = {"mpl_combat_dyson", "mpl_combat_combustion", "mpl_combat_fission", "mpl_combat_gauss"};
  const char* arenas[] = {"mpl_arena_a", "mpl_arena_a_lowspec", "mpl_arena_b", "mpl_tutorial_a"};
  std::uint64_t level = 0;
  // The lobbies arena players use are no class: not combat, not arena, whatever the lobby rule.
  for (const char* name : otherLobbies) {
    const std::uint64_t id = CSymbol64Hash(name);
    QCHECK(!lv::IsCombatLobby(id) && !lv::IsCombatMap(id) && !lv::IsEchoArenaLevel(id));
    ClearGame();
    PutLevel(0, 0, id);
    QCHECK(lv::Classify(GameAddress(), true, &level) == lv::Kind::kNone && level == 0);
    QCHECK(lv::Classify(GameAddress(), false, &level) == lv::Kind::kNone);
    QCHECK(!lv::CombatMapLoaded(GameAddress(), &level));
    const lv::Loaded loaded = lv::Scan(GameAddress());
    QCHECK(!loaded.combatMap && !loaded.combatLobby && !loaded.echoArena);
  }
  {  // the combat lobby: a class of its own, counted only with the lobby rule, never a combat map
    const std::uint64_t id = CSymbol64Hash("mpl_lobby_b_combat");
    QCHECK(lv::IsCombatLobby(id) && !lv::IsCombatMap(id) && !lv::IsEchoArenaLevel(id));
    ClearGame();
    PutLevel(0, 0, id);
    QCHECK(lv::Classify(GameAddress(), true, &level) == lv::Kind::kCombatOrLobby && level == id);
    QCHECK(lv::Classify(GameAddress(), false, &level) == lv::Kind::kNone);
    QCHECK(!lv::CombatMapLoaded(GameAddress(), &level) && level == 0);
    const lv::Loaded loaded = lv::Scan(GameAddress());
    QCHECK(loaded.combatLobby && loaded.combatLobbyLevel == id && !loaded.combatMap && !loaded.echoArena);
  }
  for (const char* name : combats) {
    const std::uint64_t id = CSymbol64Hash(name);
    QCHECK(lv::IsCombatMap(id) && !lv::IsCombatLobby(id) && !lv::IsEchoArenaLevel(id));
    ClearGame();
    PutLevel(0, 0, id);
    QCHECK(lv::Classify(GameAddress(), false, &level) == lv::Kind::kCombatOrLobby && level == id);
    QCHECK(lv::Classify(GameAddress(), true, &level) == lv::Kind::kCombatOrLobby);
    QCHECK(lv::CombatMapLoaded(GameAddress(), &level) && level == id);
  }
  for (const char* name : arenas) {
    const std::uint64_t id = CSymbol64Hash(name);
    QCHECK(lv::IsEchoArenaLevel(id) && !lv::IsCombatMap(id) && !lv::IsCombatLobby(id));
    ClearGame();
    PutLevel(0, 0, id);
    // an arena level is an arena level whether or not the lobby rule is on
    QCHECK(lv::Classify(GameAddress(), true, &level) == lv::Kind::kEchoArena && level == id);
    QCHECK(lv::Classify(GameAddress(), false, &level) == lv::Kind::kEchoArena && level == id);
    QCHECK(!lv::CombatMapLoaded(GameAddress(), &level));  // never a combat map
  }
}

// Every class at once: a transition holds several levels, and Scan reports each class with its first level.
void ScanReportsEveryLoadedClassAtOnce() {
  ClearGame();
  PutLevel(0, 0, CSymbol64Hash("mpl_combat_gauss"));
  PutLevel(0, 1, CSymbol64Hash("mpl_lobby_b_combat"));
  PutLevel(1, 0, CSymbol64Hash("mpl_arena_a"));
  const lv::Loaded loaded = lv::Scan(GameAddress());
  QCHECK(loaded.combatMap && loaded.combatMapLevel == CSymbol64Hash("mpl_combat_gauss"));
  QCHECK(loaded.combatLobby && loaded.combatLobbyLevel == CSymbol64Hash("mpl_lobby_b_combat"));
  QCHECK(loaded.echoArena && loaded.echoArenaLevel == CSymbol64Hash("mpl_arena_a"));
  std::uint64_t level = 0;
  QCHECK(lv::Classify(GameAddress(), true, &level) == lv::Kind::kEchoArena);  // the arena level wins over both
  QCHECK(lv::CombatMapLoaded(GameAddress(), &level) && level == CSymbol64Hash("mpl_combat_gauss"));
  ClearGame();
  const lv::Loaded none = lv::Scan(GameAddress());
  QCHECK(!none.combatMap && !none.combatLobby && !none.echoArena);
}

void AnEchoArenaLevelWinsAndTheGamespaceMustBeTheGames() {
  std::uint64_t level = 0;
  ClearGame();
  PutLevel(0, 0, CSymbol64Hash("mpl_combat_dyson"));
  PutLevel(1, 0, CSymbol64Hash("mpl_arena_a"));  // in the second table: a combat map still loaded during a transition
  QCHECK(lv::Classify(GameAddress(), true, &level) == lv::Kind::kEchoArena);
  QCHECK(lv::Classify(GameAddress(), false, &level) == lv::Kind::kEchoArena);
  ClearGame();  // a gamespace some other game owns is not this game's level
  PutLevel(0, 0, CSymbol64Hash("mpl_combat_dyson"), /*owner=*/0xdead0000);
  QCHECK(lv::Classify(GameAddress(), true, &level) == lv::Kind::kNone && level == 0);
  QCHECK(!lv::CombatMapLoaded(GameAddress(), &level));
  ClearGame();
  PutLevel(0, 0, CSymbol64Hash("mpl_somewhere_else"));
  QCHECK(lv::Classify(GameAddress(), true, &level) == lv::Kind::kNone);
  ClearGame();
  QCHECK(lv::Classify(GameAddress(), true, &level) == lv::Kind::kNone);
  PutLevel(0, 0, CSymbol64Hash("mpl_combat_dyson"));
  g_game.words[kCount0 / 8] = 65;  // more than the 64 the module trusts
  QCHECK(lv::Classify(GameAddress(), true, &level) == lv::Kind::kNone);
  QCHECK(!lv::CombatMapLoaded(GameAddress(), &level));
  // An unreadable game is no level (fail closed): the read never faults.
  QCHECK(lv::Classify(0x1000, true, &level) == lv::Kind::kNone);
  QCHECK(lv::Classify(0, true, &level) == lv::Kind::kNone);
  QCHECK(!lv::CombatMapLoaded(0, &level));
  const lv::Loaded unreadable = lv::Scan(0);
  QCHECK(!unreadable.combatMap && !unreadable.combatLobby && !unreadable.echoArena);
}

// The gamespaces still loading (#2177): a level is stored in its gamespace before the level's resources are built, and the gamespace enters the two
// tables only after, so a caller that acts while resources are built asks `inFlight`. Without it, every answer is what it always was.
void AMapThatIsOnlyInTheFiberTableIsSeenOnlyWhenAsked() {
  std::uint64_t level = 0;
  for (const char* lobby : {"mpl_lobby_b", "mpl_lobby_b_combat"}) {
    ClearGame();
    PutLevel(0, 0, CSymbol64Hash(lobby));
    PutInFlight(0, CSymbol64Hash("mpl_combat_gauss"));
    // asked: the combat map that is loading is a combat map, with the lobby rule on or off
    QCHECK(lv::Classify(GameAddress(), false, &level, true) == lv::Kind::kCombatOrLobby && level == CSymbol64Hash("mpl_combat_gauss"));
    QCHECK(lv::Classify(GameAddress(), true, &level, true) == lv::Kind::kCombatOrLobby && level == CSymbol64Hash("mpl_combat_gauss"));
    const lv::Loaded loaded = lv::Scan(GameAddress(), true);
    QCHECK(loaded.combatMap && loaded.combatMapLevel == CSymbol64Hash("mpl_combat_gauss") && !loaded.echoArena);
    // not asked (refresh_rate, pfx): unchanged, the lobby alone
    QCHECK(lv::Classify(GameAddress(), false, &level) == lv::Kind::kNone && level == 0);
    QCHECK(!lv::CombatMapLoaded(GameAddress(), &level));
    const lv::Loaded plain = lv::Scan(GameAddress());
    QCHECK(!plain.combatMap && plain.combatLobby == (std::string(lobby) == "mpl_lobby_b_combat"));
  }
  ClearGame();  // nothing loaded, a combat map loading: the same
  PutInFlight(0, CSymbol64Hash("mpl_combat_fission"));
  QCHECK(lv::Classify(GameAddress(), false, &level, true) == lv::Kind::kCombatOrLobby && level == CSymbol64Hash("mpl_combat_fission"));
  QCHECK(lv::Classify(GameAddress(), false, &level) == lv::Kind::kNone);
}

void AnEchoArenaLevelThatIsLoadingWinsAsOneThatIsLoadedDoes() {
  std::uint64_t level = 0;
  ClearGame();  // arena loading while a combat map is still loaded: arena
  PutLevel(0, 0, CSymbol64Hash("mpl_combat_dyson"));
  PutInFlight(0, CSymbol64Hash("mpl_arena_a"));
  QCHECK(lv::Classify(GameAddress(), false, &level, true) == lv::Kind::kEchoArena && level == CSymbol64Hash("mpl_arena_a"));
  QCHECK(lv::Classify(GameAddress(), false, &level) == lv::Kind::kCombatOrLobby);  // not asked: what it always was
  ClearGame();  // both loading, in either order of the rows
  PutInFlight(0, CSymbol64Hash("mpl_combat_gauss"));
  PutInFlight(1, CSymbol64Hash("mpl_tutorial_a"));
  QCHECK(lv::Classify(GameAddress(), true, &level, true) == lv::Kind::kEchoArena);
  ClearGame();
  PutInFlight(0, CSymbol64Hash("mpl_arena_b"));
  PutInFlight(1, CSymbol64Hash("mpl_combat_gauss"));
  QCHECK(lv::Classify(GameAddress(), true, &level, true) == lv::Kind::kEchoArena);
  ClearGame();  // arena loaded, combat map loading
  PutLevel(1, 0, CSymbol64Hash("mpl_arena_a"));
  PutInFlight(0, CSymbol64Hash("mpl_combat_gauss"));
  QCHECK(lv::Classify(GameAddress(), false, &level, true) == lv::Kind::kEchoArena);
}

void TheFiberTableIsReadWithTheSameCareAsTheOthers() {
  std::uint64_t level = 0;
  ClearGame();  // a fiber with no gamespace yet, and a null fiber row, are skipped
  PutInFlight(0, CSymbol64Hash("mpl_combat_gauss"), /*withGamespace=*/false);
  QCHECK(lv::Classify(GameAddress(), false, &level, true) == lv::Kind::kNone);
  g_game.fiberRows[1] = 0;
  g_game.words[kFiberCount / 8] = 2;
  QCHECK(lv::Classify(GameAddress(), false, &level, true) == lv::Kind::kNone);
  ClearGame();  // more than the 64 the module trusts
  PutInFlight(0, CSymbol64Hash("mpl_combat_gauss"));
  g_game.words[kFiberCount / 8] = 65;
  QCHECK(lv::Classify(GameAddress(), false, &level, true) == lv::Kind::kNone);
  ClearGame();  // no table
  PutInFlight(0, CSymbol64Hash("mpl_combat_gauss"));
  g_game.words[kFiberRows / 8] = 0;
  QCHECK(lv::Classify(GameAddress(), false, &level, true) == lv::Kind::kNone);
  ClearGame();  // a row that points nowhere
  PutInFlight(0, CSymbol64Hash("mpl_combat_gauss"));
  g_game.fiberRows[0] = 0x1000;
  QCHECK(lv::Classify(GameAddress(), false, &level, true) == lv::Kind::kNone);
  // an unreadable game is no level, as before
  QCHECK(lv::Classify(0x1000, false, &level, true) == lv::Kind::kNone);
  QCHECK(lv::Classify(0, false, &level, true) == lv::Kind::kNone);
}

void EveryKnownLevelHasItsNameAndOthersHaveNone() {
  for (const char* name : {"mpl_combat_dyson", "mpl_combat_combustion", "mpl_combat_fission", "mpl_combat_gauss", "mpl_lobby_b_combat", "mpl_arena_a",
                           "mpl_arena_a_lowspec", "mpl_arena_b", "mpl_tutorial_a", "mpl_lobby_b", "mpl_lobby_b2", "mpl_lobby_b2_lowspec",
                           "mpl_lobby_b_arena", "mpl_lobby_b_social"}) {
    const char* const got = lv::LevelName(CSymbol64Hash(name));
    QCHECK(got != nullptr && std::string(got) == name);
  }
  QCHECK(lv::LevelName(0) == nullptr && lv::LevelName(CSymbol64Hash("mpl_somewhere_else")) == nullptr);
}

}  // namespace

int main() {
  EveryLevelIdIsTheCSymbol64OfItsNameAndOfItsClass();
  ScanReportsEveryLoadedClassAtOnce();
  AnEchoArenaLevelWinsAndTheGamespaceMustBeTheGames();
  AMapThatIsOnlyInTheFiberTableIsSeenOnlyWhenAsked();
  AnEchoArenaLevelThatIsLoadingWinsAsOneThatIsLoadedDoes();
  TheFiberTableIsReadWithTheSameCareAsTheOthers();
  EveryKnownLevelHasItsNameAndOthersHaveNone();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "combat_level_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::puts("combat_level_test: all checks passed");
  return 0;
}
