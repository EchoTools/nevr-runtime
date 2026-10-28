#pragma once

// The level the game holds as its main level (#1094).
//
// The game context (the runtime's g_pGame, the CR15Game: `*(*(g_pGame + 0x8518) + 8)` is the same address) keeps
// the CSymbol64 of the main level in one qword, with the qword after it serialised alongside by the game's
// SaveLoad (CR15Game vslot 91, echovr.exe 0x1401064b0). The game looks its game space up by this symbol (the spawn
// point code does: CR15SpawnPointCS::CycleSpawnPoint 0x1408a52c0 through the accessor 0x140100d90), and a reset
// (0x1400f6130) stores -1. A live run showed the qword go from -1 to the lobby level's symbol while the state
// was "loading level" and keep it through "in game", and show it hold -1 through the menu levels, which live
// in other qwords of the same object. docs/reference/game-level.md has the addresses, the instructions that
// prove each offset and what the runs showed.
//
// Pure: std only, so the unit test links it on its own and the Quest sentinel can include it.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace EchoVR {

/// echovr.exe: the main level symbol in the game context.
inline constexpr std::size_t kPcMainLevelSymbolOffset = 0x8a48;

/// libr15.so: the same field (CR15Game::CurrentSpawnGameSpace 0x130065c loads it; SetCurrentSpawnGameSpace 0x12fbcf8
/// stores the game space's symbol, game space +0x58, into it).
inline constexpr std::size_t kQuestMainLevelSymbolOffset = 0x8a40;

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
