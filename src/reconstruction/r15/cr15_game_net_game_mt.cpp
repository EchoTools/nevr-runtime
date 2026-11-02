// NRadEngine::NRadGame::CR15Game::NetGameMT
// Quest libr15.so VA 0x13027d4; ReVault binary 576.
// ReVault raw parent 100221553, reconstruction node 100600560; grade H: one 8-byte load at this+0x8510 with no branches; the CR15NetGame* type comes from the merged callers, the decompile says undefined8.
// @0x13027d4 — NRadEngine::NRadGame::CR15Game::NetGameMT
/* @confidence: H */
#include <cstddef>
#include <cstring>

namespace NRadEngine::NRadGame {

class CR15NetGame;

class CR15Game {
  public:
    // Reads the 8-byte pointer at offset 0x8510 (mov w8,#0x8510; ldr x0,[x0,x8]).
    CR15NetGame* NetGameMT() const;

    // Storage for the bytes this method reads. The object is larger; its size is not in the decompilation.
    alignas(8) unsigned char accessed_[0x8510 + sizeof(CR15NetGame*)];
};

static_assert(offsetof(CR15Game, accessed_) == 0);
static_assert(sizeof(CR15Game::accessed_) == 0x8518);

CR15NetGame* CR15Game::NetGameMT() const {
    CR15NetGame* net_game = nullptr;
    std::memcpy(&net_game, &accessed_[0x8510], sizeof net_game);
    return net_game;
}

}  // namespace NRadEngine::NRadGame
