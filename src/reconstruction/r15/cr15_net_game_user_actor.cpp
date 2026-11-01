// @0x1351654 — CR15NetGame::UserActor
// Quest libr15.so, binary 576; raw parent 100223325; reconstruction node 100600321.
// PC echovr.exe: no matching body returned by project-wide search for CR15NetGame::UserActor (1,041 binaries).
/* @confidence: M */

#include <cstddef>
#include <cstdint>

namespace q15t::r15::reconstruction {

struct SActorGuid {
    std::uint64_t words[2];
};

struct CncaGame {
    static void ResolveActorAlias(CncaGame* game, SActorGuid* actor);
};

struct CR15NetGame {
    CncaGame* game;
    SActorGuid UserActor(std::uint16_t member_index) const;
};

SActorGuid CR15NetGame::UserActor(std::uint16_t member_index) const {
    const auto* const self = reinterpret_cast<const std::uint8_t*>(this);
    auto* const game = *reinterpret_cast<CncaGame* const*>(self + 0x8);
    SActorGuid actor = *reinterpret_cast<const SActorGuid*>(
        self + static_cast<std::size_t>(member_index) * 0x250 + 0x180);
    CncaGame::ResolveActorAlias(game, &actor);
    return actor;
}

} // namespace q15t::r15::reconstruction
