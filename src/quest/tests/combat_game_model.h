// A model of the game's levels for a combat host test: CR15Game's two gamespace tables and each gamespace's level, laid out where
// combat_level.cpp reads them (the offsets are spelled here, not taken from the header). A test puts levels in with PutLevel, then runs the
// level scope's tick (scope::Tick(GameAddress())) to publish what the guards under test scope by.
#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>

#include "nevr/symbol_hash.h"
#include "quest/combat/level_scope.h"

namespace combat_test {

namespace game_model {

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

inline Game& Instance() {
  static Game game;
  return game;
}
inline std::uintptr_t GameAddress() { return reinterpret_cast<std::uintptr_t>(Instance().words); }
inline void ClearGame() { Instance() = Game{}; }

inline void PutLevel(int table, int row, std::uint64_t level) {
  Game& g = Instance();
  Gamespace& gs = g.spaces[table * 4 + row];
  gs.words[kGamespaceOwner / 8] = GameAddress();
  gs.words[kGamespaceLevel / 8] = static_cast<std::uintptr_t>(level);
  std::uintptr_t(*rows)[2] = table == 0 ? g.table0 : g.table1;
  rows[row][0] = 0x5555;
  rows[row][1] = reinterpret_cast<std::uintptr_t>(&gs);
  g.words[(table == 0 ? kTable0 : kTable1) / 8] = reinterpret_cast<std::uintptr_t>(rows);
  std::uintptr_t& count = g.words[(table == 0 ? kCount0 : kCount1) / 8];
  if (static_cast<std::uintptr_t>(row) + 1 > count) count = static_cast<std::uintptr_t>(row) + 1;
}

// The test clock the scope looks by: a look happens when it has moved past the last look's half second.
inline double& Now() {
  static double now = 0.0;
  return now;
}
inline double Clock() noexcept { return Now(); }

// Loads exactly the named levels (none: no gamespace) and makes the scope look at them now.
inline void SetLevels(std::initializer_list<const char*> names) {
  ClearGame();
  int row = 0;
  for (const char* name : names) PutLevel(0, row++, EchoVR::CSymbol64Hash(name));
  nevr_quest::combat::scope::SetClockForTest(&Clock);
  Now() += 1.0;  // past the last look's schedule
  nevr_quest::combat::scope::Tick(GameAddress());
}

}  // namespace game_model

}  // namespace combat_test
