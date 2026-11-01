// @0x1dd4c68 — CR15PlayerNavCS::FinalTransform(SCompactPoolHandle) const
// Quest libr15.so, binary 576; raw parent 100283650; reconstruction node 100600326.
/* @confidence: H */

#include <cstddef>
#include <cstdint>

namespace q15t::r15::reconstruction {

struct CTransfQS {
    float value[12];
    void Append(const CTransfQS& other);
};

void CR15EchoUnitCS_BaseTransform(float* result, const void* system,
                                  std::uint32_t handle);

CTransfQS CR15PlayerNavCS_FinalTransform(const void* self,
                                         std::uint32_t handle) {
    CTransfQS result{};
    result.value[7] = 1.0f;
    result.value[8] = 1.0f;
    result.value[9] = 1.0f;

    const auto* const system_bytes = static_cast<const std::uint8_t*>(self);
    const auto* const handles =
        *reinterpret_cast<const std::uint8_t* const*>(system_bytes + 0xc8);
    const auto dense_index = *reinterpret_cast<const std::uint16_t*>(
        handles + static_cast<std::size_t>(handle & 0xffff) * 4);
    const auto* const nav_data =
        *reinterpret_cast<const std::uint8_t* const*>(system_bytes + 0xf8);
    const auto* const entry =
        nav_data + static_cast<std::size_t>(dense_index) * 0x898;

    const auto* const current_transform =
        reinterpret_cast<const CTransfQS*>(entry + 0x440);
    for (std::size_t index = 0; index < 7; ++index) {
        result.value[index] = current_transform->value[index];
    }
    result.Append(*reinterpret_cast<const CTransfQS*>(entry + 0x4d0));

    CTransfQS base_transform{};
    CR15EchoUnitCS_BaseTransform(base_transform.value,
        *reinterpret_cast<void* const*>(entry + 0x28),
        *reinterpret_cast<const std::uint32_t*>(entry + 0x30));
    result.Append(base_transform);
    return result;
}

} // namespace q15t::r15::reconstruction
