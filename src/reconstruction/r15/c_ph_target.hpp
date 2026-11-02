#pragma once

#include <cstddef>
#include <cstdint>

namespace NRadEngine {

struct CPhBody;

struct CPhTarget {
    std::uint64_t opaque_00;
    std::uint32_t body_index;    // +0x08
    std::uint32_t zone_index;    // +0x0c
    std::int32_t generation;     // +0x10
    std::uint32_t opaque_14;

    void ValidateTag() const;
    CPhBody* ResolveBody() const;
};
static_assert(sizeof(CPhTarget) == 0x18);
static_assert(offsetof(CPhTarget, body_index) == 0x08);
static_assert(offsetof(CPhTarget, zone_index) == 0x0c);
static_assert(offsetof(CPhTarget, generation) == 0x10);

} // namespace NRadEngine
