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

#include "nevr/symbol_hash.h"
#include "quest/tests/test_check.h"

namespace {

namespace lv = nevr_quest::combat::level;
using EchoVR::CSymbol64Hash;

constexpr std::size_t kTable0 = 0x1f48, kCount0 = 0x1f78, kTable1 = 0x1f88, kCount1 = 0x1fb8;
constexpr std::size_t kGamespaceOwner = 0x8, kGamespaceLevel = 0x58;

struct Gamespace {
  std::uintptr_t words[0x60 / 8 + 1] = {};
};
struct Game {
  std::uintptr_t words[0x2000 / 8] = {};
  std::uintptr_t table0[8][2] = {};
  std::uintptr_t table1[8][2] = {};
  Gamespace spaces[8];
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

void EveryLevelIdIsTheCSymbol64OfItsNameAndOfItsClass() {
  const char* lobbies[] = {"mpl_lobby_b", "mpl_lobby_b2", "mpl_lobby_b2_lowspec", "mpl_lobby_b_combat", "mpl_lobby_b_arena", "mpl_lobby_b_social"};
  const char* combats[] = {"mpl_combat_dyson", "mpl_combat_combustion", "mpl_combat_fission", "mpl_combat_gauss"};
  const char* arenas[] = {"mpl_arena_a", "mpl_arena_a_lowspec", "mpl_arena_b", "mpl_tutorial_a"};
  std::uint64_t level = 0;
  for (const char* name : lobbies) {
    QCHECK(lv::IsLobbyLevel(CSymbol64Hash(name)) && !lv::IsCombatMap(CSymbol64Hash(name)) && !lv::IsEchoArenaLevel(CSymbol64Hash(name)));
    ClearGame();
    PutLevel(0, 0, CSymbol64Hash(name));
    QCHECK(lv::Classify(GameAddress(), true, &level) == lv::Kind::kCombatOrLobby && level == CSymbol64Hash(name));
    QCHECK(lv::Classify(GameAddress(), false, &level) == lv::Kind::kNone);  // the lobby rule off: not a level of the rule
    QCHECK(!lv::CombatMapLoaded(GameAddress(), &level) && level == 0);       // a lobby is not a combat map
  }
  for (const char* name : combats) {
    QCHECK(lv::IsCombatMap(CSymbol64Hash(name)) && !lv::IsLobbyLevel(CSymbol64Hash(name)) && !lv::IsEchoArenaLevel(CSymbol64Hash(name)));
    ClearGame();
    PutLevel(0, 0, CSymbol64Hash(name));
    QCHECK(lv::Classify(GameAddress(), false, &level) == lv::Kind::kCombatOrLobby && level == CSymbol64Hash(name));
    QCHECK(lv::Classify(GameAddress(), true, &level) == lv::Kind::kCombatOrLobby);
    QCHECK(lv::CombatMapLoaded(GameAddress(), &level) && level == CSymbol64Hash(name));
  }
  for (const char* name : arenas) {
    QCHECK(lv::IsEchoArenaLevel(CSymbol64Hash(name)) && !lv::IsCombatMap(CSymbol64Hash(name)) && !lv::IsLobbyLevel(CSymbol64Hash(name)));
    ClearGame();
    PutLevel(0, 0, CSymbol64Hash(name));
    QCHECK(lv::Classify(GameAddress(), true, &level) == lv::Kind::kEchoArena && level == CSymbol64Hash(name));
    QCHECK(lv::Classify(GameAddress(), false, &level) == lv::Kind::kNone);  // decides only under the lobby rule
    QCHECK(!lv::CombatMapLoaded(GameAddress(), &level));                    // an arena level is never a combat map
  }
}

void AnEchoArenaLevelWinsOverALobbyLevelAndTheGamespaceMustBeTheGames() {
  std::uint64_t level = 0;
  ClearGame();
  PutLevel(0, 0, CSymbol64Hash("mpl_lobby_b_social"));
  PutLevel(1, 0, CSymbol64Hash("mpl_arena_a"));  // in the second table
  QCHECK(lv::Classify(GameAddress(), true, &level) == lv::Kind::kEchoArena);
  QCHECK(lv::Classify(GameAddress(), false, &level) == lv::Kind::kNone);
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
}

}  // namespace

int main() {
  EveryLevelIdIsTheCSymbol64OfItsNameAndOfItsClass();
  AnEchoArenaLevelWinsOverALobbyLevelAndTheGamespaceMustBeTheGames();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "combat_level_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::puts("combat_level_test: all checks passed");
  return 0;
}
