// @0x1461050 — CActorAliasCS::Resolve(CncaGameSpace*&, SActor&)
// Quest libr15.so, binary 576; raw parent 100228186; reconstruction node 100600341.
/* @confidence: M */

#include <cstddef>
#include <cstdint>

#include "cnca_game.hpp"

namespace q15t::r15::reconstruction {
namespace {

int CompareActorIds(const std::uint8_t* left, const std::uint8_t* right) {
    for (std::size_t offset = 0; offset < 6; offset += 2) {
        const auto left_word =
            *reinterpret_cast<const std::uint16_t*>(left + offset);
        const auto right_word =
            *reinterpret_cast<const std::uint16_t*>(right + offset);
        if (left_word < right_word) {
            return -1;
        }
        if (left_word > right_word) {
            return 1;
        }
    }

    const auto left_tail =
        *reinterpret_cast<const std::int64_t*>(left + 8);
    const auto right_tail =
        *reinterpret_cast<const std::int64_t*>(right + 8);
    if (left_tail < right_tail) {
        return -1;
    }
    if (left_tail > right_tail) {
        return 1;
    }
    return 0;
}

} // namespace

bool CActorAliasCS::Resolve(void** game_space, std::uint64_t* actor) {
    if (*game_space == nullptr) {
        return false;
    }

    auto* const game_space_bytes = static_cast<std::uint8_t*>(*game_space);
    auto* const alias_system =
        *reinterpret_cast<std::uint8_t**>(game_space_bytes + 0xc48);
    if (alias_system == nullptr) {
        return false;
    }

    auto** const vtable = *reinterpret_cast<void***>(alias_system);
    using ResolveAlias = int (*)(void*, std::uint64_t, std::uint64_t*);
    const auto resolve_alias =
        reinterpret_cast<ResolveAlias>(vtable[0x2b0 / sizeof(void*)]);

    std::uint64_t alias_id[2] = {0x0000ffffffffffffULL, UINT64_MAX};
    if (resolve_alias(alias_system, *actor, alias_id) == 0) {
        return false;
    }

    const auto count =
        *reinterpret_cast<const std::uint64_t*>(alias_system + 0x118);
    const auto* const entries =
        *reinterpret_cast<const std::uint8_t* const*>(alias_system + 0x110);
    std::uint64_t low = 0;
    std::uint64_t high = count;
    const std::uint8_t* entry = nullptr;
    while (low < high) {
        const std::uint64_t middle = low + (high - low) / 2;
        const auto* const candidate = entries + middle * 0x18;
        const int order = CompareActorIds(
            reinterpret_cast<const std::uint8_t*>(alias_id), candidate);
        if (order < 0) {
            high = middle;
        } else if (order > 0) {
            low = middle + 1;
        } else {
            entry = candidate;
            break;
        }
    }

    // A successful alias lookup is expected to have an entry in this table.
    if (entry == nullptr) {
        return false;
    }

    const auto component_index =
        *reinterpret_cast<const std::uint16_t*>(entry + 0x10);
    auto* const component_base =
        *reinterpret_cast<std::uint8_t**>(alias_system + 0xf8);
    auto* const component =
        component_base + static_cast<std::size_t>(component_index) * 0x18;
    auto* const game = static_cast<CncaGameSpace*>(*game_space);
    *game_space = game->GameSpace(
        *reinterpret_cast<const std::uint64_t*>(component + 8));
    *actor = *reinterpret_cast<const std::uint64_t*>(component + 0x10);
    return true;
}

} // namespace q15t::r15::reconstruction
