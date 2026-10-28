#pragma once

// The level the game holds as its main level (#1094).
//
// The game object (the runtime's g_pGame, `*(CR15NetGame + 8)`) keeps the CSymbol64 of its main level in one
// qword and the level instance's handle id in the next. Every level load writes them: CncaGameSpace::LoadLevel
// stores the game space's symbol (game space +0x58) there when the handle slot is -1, the per-frame update of a
// game space (echovr.exe 0x1404efd00; Quest ExclusiveUpdate 0x18ef190) stores it when a pending handle arrives,
// the command-line `-level` / `-startlevel` argument seeds it at boot (echovr.exe 0x140504790), and the reset
// that precedes a level request and the game object's constructor write -1. docs/reference/game-level.md has
// the addresses, the instructions that prove each offset and what a run showed.
//
// Pure: std only, so the unit test links it on its own and the Quest sentinel can include it.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace EchoVR {

/// echovr.exe: the main level symbol in the game object (the handle id is the qword after it).
inline constexpr std::size_t kPcMainLevelSymbolOffset = 0x7678;

/// libr15.so: the same two qwords in the reverse order, so the symbol is the first (the handle id follows at +0x7678).
inline constexpr std::size_t kQuestMainLevelSymbolOffset = 0x7670;

/// What the game stores in both qwords while no level is the main level.
inline constexpr std::uint64_t kNoLevelInGame = ~std::uint64_t{0};

/// The main level's symbol read from the game object at `game` with the layout `symbolOffset` names, or 0 when
/// `game` is null or the game holds no level (-1). A CSymbol64 of a level name is never 0.
inline std::uint64_t ReadMainLevelSymbol(const void* game, std::size_t symbolOffset) {
  if (game == nullptr) return 0;
  std::uint64_t symbol = 0;
  std::memcpy(&symbol, static_cast<const unsigned char*>(game) + symbolOffset, sizeof(symbol));
  return symbol == kNoLevelInGame ? 0 : symbol;
}

}  // namespace EchoVR
