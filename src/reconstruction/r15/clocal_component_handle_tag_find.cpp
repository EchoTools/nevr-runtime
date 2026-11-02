// @0x18c8d88 — CLocalComponentHandle<CTagCS>::Find
// Quest libr15.so, binary 576; raw parent 100254995; reconstruction node 100600331.
/* @confidence: M */

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "cnca_game.hpp"

namespace q15t::r15::reconstruction {

void FindTagComponent(LocalComponentHandle* result, void* game_space,
                      std::uint64_t actor, std::uint64_t component_symbol) {
    if (game_space == nullptr) {
        result->component = nullptr;
        return;
    }

    auto* const space_bytes = static_cast<std::uint8_t*>(game_space);
    auto count = *reinterpret_cast<std::uint64_t*>(space_bytes + 0x490);
    auto* entries = *reinterpret_cast<std::uint8_t**>(space_bytes + 0x460);
    if (count != 0 && (*reinterpret_cast<std::uint8_t*>(space_bytes + 0x498) & 1) != 0) {
        struct Entry {
            std::int64_t key;
            std::uint8_t remainder[56];
        };
        auto* const records = reinterpret_cast<Entry*>(entries);
        std::sort(records, records + count,
                  [](const Entry& lhs, const Entry& rhs) {
                      return lhs.key < rhs.key;
                  });
        *reinterpret_cast<std::uint32_t*>(space_bytes + 0x498) &= ~1u;
        count = *reinterpret_cast<std::uint64_t*>(space_bytes + 0x490);
        entries = *reinterpret_cast<std::uint8_t**>(space_bytes + 0x460);
        if (count == 0) {
            goto resolve_alias;
        }
    }

    {
        constexpr std::int64_t tag_system_symbol =
            -INT64_C(0x37172f4e577b1c13);
        std::uint64_t low = 0;
        std::uint64_t high = count;
        while (low < high) {
            const std::uint64_t mid = low + (high - low) / 2;
            auto* const entry = entries + mid * 0x40;
            const auto key = *reinterpret_cast<std::int64_t*>(entry);
            if (key > tag_system_symbol) {
                high = mid;
                continue;
            }
            if (key < tag_system_symbol) {
                low = mid + 1;
                continue;
            }

            const auto system_count =
                *reinterpret_cast<std::uint64_t*>(entry + 0x38);
            auto* const systems = *reinterpret_cast<void***>(entry + 0x8);
            for (std::uint64_t index = 0; index < system_count; ++index) {
                void* const component_system = systems[index];
                auto** const vtable = *reinterpret_cast<void***>(component_system);
                using GetHandle = std::uint32_t (*)(void*, std::uint64_t,
                                                    std::uint64_t);
                using IsValid = bool (*)(void*, std::uint32_t);
                const auto get_handle = reinterpret_cast<GetHandle>(
                    vtable[0x1b8 / sizeof(void*)]);
                const auto packed_handle =
                    get_handle(component_system, actor, component_symbol);
                result->component = component_system;
                result->packed_handle = packed_handle;
                const auto is_valid = reinterpret_cast<IsValid>(
                    vtable[0x1d0 / sizeof(void*)]);
                if (is_valid(component_system, packed_handle)) {
                    return;
                }
            }
            break;
        }
    }

resolve_alias:
    if (CActorAliasCS::Resolve(&game_space, &actor)) {
        FindTagComponent(result, game_space, actor, component_symbol);
        return;
    }
    result->component = nullptr;
}

} // namespace q15t::r15::reconstruction
