// A model of CR15Game's two gamespace tables for the combat host tests: the rows of {sym, gamespace*} and, in each gamespace, the game at +0x8
// and the level at +0x58, laid out where combat_level.cpp reads them (the offsets are spelled here, not taken from the header, so a changed offset
// in the module fails a test). The module reads the model with process_vm_readv, as it reads the game.
#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>

#include "nevr/symbol_hash.h"

namespace combat_test {

class GameModel {
 public:
  void Clear() { game_ = Game{}; }
  std::uintptr_t Address() const { return reinterpret_cast<std::uintptr_t>(game_.words); }
  // A gamespace in table 0 or 1, row 0..3, holding `level`.
  void PutLevel(int table, int row, std::uint64_t level) {
    Gamespace& gs = game_.spaces[table * 4 + row];
    gs.words[kOwner / 8] = Address();
    gs.words[kLevel / 8] = static_cast<std::uintptr_t>(level);
    std::uintptr_t(*rows)[2] = table == 0 ? game_.table0 : game_.table1;
    rows[row][0] = 0x5555;
    rows[row][1] = reinterpret_cast<std::uintptr_t>(&gs);
    game_.words[(table == 0 ? kTable0 : kTable1) / 8] = reinterpret_cast<std::uintptr_t>(rows);
    std::uintptr_t& count = game_.words[(table == 0 ? kCount0 : kCount1) / 8];
    if (static_cast<std::uintptr_t>(row) + 1 > count) count = static_cast<std::uintptr_t>(row) + 1;
  }
  // Only these levels are loaded (table 0, row 0 and on).
  void Only(std::initializer_list<const char*> names) {
    Clear();
    int row = 0;
    for (const char* name : names) PutLevel(0, row++, EchoVR::CSymbol64Hash(name));
  }

 private:
  static constexpr std::size_t kTable0 = 0x1f48, kCount0 = 0x1f78, kTable1 = 0x1f88, kCount1 = 0x1fb8;
  static constexpr std::size_t kOwner = 0x8, kLevel = 0x58;
  struct Gamespace {
    std::uintptr_t words[0x60 / 8 + 1] = {};
  };
  struct Game {
    std::uintptr_t words[0x2000 / 8] = {};
    std::uintptr_t table0[8][2] = {};
    std::uintptr_t table1[8][2] = {};
    Gamespace spaces[8];
  };
  Game game_;
};

}  // namespace combat_test
