// @0x12cae74 — NRadEngine::CMemory::Compare
/* @confidence: H — the Quest body is a direct three-argument memcmp thunk. */
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine::CMemory {
std::int64_t Compare(const void* lhs, const void* rhs, std::uint64_t count) {
    return static_cast<std::int64_t>(std::memcmp(lhs, rhs, static_cast<std::size_t>(count)));
}
}  // namespace NRadEngine::CMemory
