// @0x1d76a80 — CR15EchoUnitCS::BaseTransform(SCompactPoolHandle) const
// Quest libr15.so, binary 576; raw parent 100281596; reconstruction node 100600325.
/* @confidence: H */

#include <cstddef>
#include <cstdint>

namespace q15t::r15::reconstruction {

void CR15EchoUnitCS_BaseTransform(float* result, const void* system,
                                  std::uint32_t handle) {
    const auto* const system_bytes =
        static_cast<const std::uint8_t*>(system);
    const auto* const handles =
        *reinterpret_cast<const std::uint8_t* const*>(system_bytes + 0xc8);
    const auto dense_index = *reinterpret_cast<const std::uint16_t*>(
        handles + static_cast<std::size_t>(handle & 0xffff) * 4);
    const auto* const transform_data =
        *reinterpret_cast<const std::uint8_t* const*>(system_bytes + 0xf8);
    const auto* const source =
        transform_data + static_cast<std::size_t>(dense_index) * 0x280;

    for (std::size_t word = 0; word < 6; ++word) {
        reinterpret_cast<std::uint64_t*>(result)[word] =
            *reinterpret_cast<const std::uint64_t*>(source + word * 8);
    }
}

} // namespace q15t::r15::reconstruction
