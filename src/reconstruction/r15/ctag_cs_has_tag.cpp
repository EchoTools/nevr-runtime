// @0x1823760 — CTagCS::HasTag(CncaGameMT*, SActorGUID const&, CSymbol64)
// Quest libr15.so, binary 576; raw parent 100250902; reconstruction node 100600322.
/* @confidence: H */

#include <cstddef>
#include <cstdint>

#include "cnca_game.hpp"

namespace q15t::r15::reconstruction {

struct CTagCS {};

bool CTagCS_HasTag(void* game_mt, const std::uint64_t* actor_guid,
                   std::uint64_t tag_symbol) {
    void* game_space = nullptr;
    if (game_mt != nullptr) {
        game_space = static_cast<CncaGame*>(game_mt)->GameSpaceMT(
            actor_guid[0]);
    }

    LocalComponentHandle handle{};
    handle.packed_handle = 0xffff;
    FindTagComponent(&handle, game_space, actor_guid[1], UINT64_MAX);
    if (handle.component == nullptr) {
        return false;
    }

    auto** const vtable = *reinterpret_cast<void***>(handle.component);
    using IsComponentValid = bool (*)(void*, std::uint32_t);
    const auto is_valid = reinterpret_cast<IsComponentValid>(
        vtable[0x1d0 / sizeof(void*)]);
    if (!is_valid(handle.component, handle.packed_handle)) {
        return false;
    }

    using HasTag = bool (*)(void*, std::uint32_t, std::uint64_t);
    const auto has_tag = reinterpret_cast<HasTag>(vtable[0x300 / sizeof(void*)]);
    return has_tag(handle.component, handle.packed_handle, tag_symbol);
}

} // namespace q15t::r15::reconstruction
