// @0x14d6da8 — ResolveActorAlias(CncaGame*, SActorGUID&)
// Quest libr15.so, binary 576; raw parent 100231577; reconstruction node 100600330.
/* @confidence: H */

#include <cstdint>

#include "cnca_game.hpp"

namespace q15t::r15::reconstruction {

std::uint64_t ResolveActorAlias(CncaGame* game, SActorGuid* actor) {
    if (game == nullptr) {
        return 0;
    }

    void* game_space = game->GameSpace(actor->words[0]);
    if (!CActorAliasCS::Resolve(&game_space, &actor->words[1])) {
        return 0;
    }

    actor->words[0] = game_space == nullptr
        ? UINT64_MAX
        : *reinterpret_cast<const std::uint64_t*>(
              static_cast<const std::uint8_t*>(game_space) + 0x58);
    return 1;
}

} // namespace q15t::r15::reconstruction
