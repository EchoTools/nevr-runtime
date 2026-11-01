// NRadEngine::CMemory::Copy
// Quest libr15.so VA 0x12cb908; ReVault binary 576.
// ReVault raw parent 100219677, reconstruction node 100600562; grade H: the whole body is one tail jump to memcpy through GOT 0x37fb258 (thunk 0x10476e0), no branches, arguments forwarded unchanged.
// @0x12cb908 — NRadEngine::CMemory::Copy
/* @confidence: H */
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine::CMemory {

void Copy(void* destination, const void* source, std::uint64_t count);

}  // namespace NRadEngine::CMemory

void NRadEngine::CMemory::Copy(void* destination, const void* source, std::uint64_t count) {
    // 0x12cb908: b 0x010476e0 (tail jump to memcpy; import row slot 0x37fb258).
    std::memcpy(destination, source, static_cast<std::size_t>(count));
}
