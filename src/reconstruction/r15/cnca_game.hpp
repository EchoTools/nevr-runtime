#pragma once

#include <cstdint>

namespace q15t::r15::reconstruction {

struct SActorGuid {
    std::uint64_t words[2];
};

struct CncaGame {
    void* GameSpace(std::uint64_t symbol) const;
    void* LoadedGameSpace(std::uint64_t symbol) const;
    void* GameSpaceMT(std::uint64_t symbol) const;
};

struct CncaGameSpace {
    void* GameSpace(std::uint64_t symbol) const;
};

std::uint64_t ResolveActorAlias(CncaGame* game, SActorGuid* actor);

struct CActorAliasCS {
    static bool Resolve(void** game_space, std::uint64_t* actor);
};

struct LocalComponentHandle {
    void* component;
    std::uint32_t packed_handle;
    std::uint32_t flags;
};

void FindTagComponent(LocalComponentHandle* result, void* game_space,
                      std::uint64_t actor, std::uint64_t component_symbol);

} // namespace q15t::r15::reconstruction
