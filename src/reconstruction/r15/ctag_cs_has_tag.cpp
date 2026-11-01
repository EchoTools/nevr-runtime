// @0x1823760 — CTagCS::HasTag(CncaGameMT*, SActorGUID const&, CSymbol64)
// Quest libr15.so, binary 576; raw parent 100250902; reconstruction node 100600322.
/* @confidence: H */

#include <cstddef>
#include <cstdint>

namespace q15t::r15::reconstruction {

struct CTagCS {};

struct CncaGame {
    static void* GameSpaceMT(CncaGame* game, std::uint64_t actor_key);
};

template <typename T>
struct CLocalComponentHandle {
    void* component;
    std::uint16_t slot;
    std::uint16_t generation;
    std::uint32_t flags;

    static void Find(CLocalComponentHandle* result, void* game_space,
                     std::uint64_t actor_key, std::uint64_t component_key);
};

bool CTagCS_HasTag(void* game_mt, const std::uint64_t* actor_guid,
                   std::uint64_t tag_symbol) {
    void* game_space = nullptr;
    if (game_mt != nullptr) {
        game_space = CncaGame::GameSpaceMT(
            static_cast<CncaGame*>(game_mt), actor_guid[0]);
    }

    CLocalComponentHandle<CTagCS> handle{};
    handle.slot = 0xffff;
    CLocalComponentHandle<CTagCS>::Find(&handle, game_space, actor_guid[1],
                                        UINT64_MAX);
    if (handle.component == nullptr) {
        return false;
    }

    auto** const vtable = *reinterpret_cast<void***>(handle.component);
    using IsComponentValid = bool (*)(void*, std::uint32_t);
    const auto is_valid = reinterpret_cast<IsComponentValid>(
        vtable[0x1d0 / sizeof(void*)]);
    const std::uint32_t packed_handle =
        static_cast<std::uint32_t>(handle.slot) |
        (static_cast<std::uint32_t>(handle.generation) << 16);
    if (!is_valid(handle.component, packed_handle)) {
        return false;
    }

    using HasTag = bool (*)(void*, std::uint32_t, std::uint64_t);
    const auto has_tag = reinterpret_cast<HasTag>(vtable[0x300 / sizeof(void*)]);
    return has_tag(handle.component, packed_handle, tag_symbol);
}

} // namespace q15t::r15::reconstruction
