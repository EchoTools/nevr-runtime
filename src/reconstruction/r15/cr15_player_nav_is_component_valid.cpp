// @0x1ddb3e0 — CR15PlayerNavCS::IsComponentValid(SCompactPoolHandle) const
// Quest libr15.so, binary 576; raw parent 100283892; reconstruction node 100600323.
/* @confidence: H */

#include <cstddef>
#include <cstdint>

namespace q15t::r15::reconstruction {

bool CR15PlayerNavCS_IsComponentValid(const void* self, std::uint32_t handle) {
    const auto* const bytes = static_cast<const std::uint8_t*>(self);
    const auto count = *reinterpret_cast<const std::uint16_t*>(bytes + 0xf0);
    const auto slot = static_cast<std::uint16_t>(handle);
    if (slot >= count) {
        return false;
    }

    const auto* const slots =
        *reinterpret_cast<const std::uint8_t* const*>(bytes + 0xc8);
    const auto* const entry = slots + static_cast<std::size_t>(slot) * 4;
    const auto generation = *reinterpret_cast<const std::uint16_t*>(entry + 2);
    if (generation != static_cast<std::uint16_t>(handle >> 16)) {
        return false;
    }

    const auto dense_index = *reinterpret_cast<const std::uint16_t*>(entry);
    const auto dense_count = *reinterpret_cast<const std::uint16_t*>(bytes + 0xf2);
    return dense_index < dense_count;
}

} // namespace q15t::r15::reconstruction
