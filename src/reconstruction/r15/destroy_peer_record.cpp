// NRadEngine::SBroadcasterData::DestroyPeer(record)
// Quest libr15.so VA 0x25ea1bc; ReVault binary 576.
// ReVault raw parent 100318011, reconstruction node 100600272; grade M: queue offsets and copied peer fields follow Quest raw, and the allocator and hash-table operations are named imports.
// @0x25ea1bc — NRadEngine::SBroadcasterData::DestroyPeer(record)
// @confidence: M — queue offsets and copied peer fields follow Quest raw; allocator and hash-table operations are named imports.
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace NRadEngine {
namespace CMemBlock {
void Resize(void* block, std::uint64_t bytes, std::uint32_t copy_existing);
}
namespace CHashTable {
void Destroy(void* table, const std::uint64_t* key);
}
namespace SBroadcasterData {
namespace {
void append_peer_value(std::byte* self, std::size_t data_offset,
                       std::size_t capacity_offset, std::size_t minimum_offset,
                       std::size_t count_offset, std::uint64_t value) {
    auto& capacity = *reinterpret_cast<std::uint64_t*>(self + capacity_offset);
    auto& minimum = *reinterpret_cast<std::uint64_t*>(self + minimum_offset);
    auto& count = *reinterpret_cast<std::uint64_t*>(self + count_offset);
    if (capacity <= count) {
        const auto growth = std::max(capacity, minimum);
        CMemBlock::Resize(self + data_offset,
                          (growth + capacity) * sizeof(std::uint64_t), 1);
        capacity += growth;
    }
    auto* values = *reinterpret_cast<std::uint64_t**>(self + data_offset);
    values[count++] = value;
}
}

void DestroyPeer(void* self, const std::byte* peer) {
    auto* bytes = static_cast<std::byte*>(self);
    append_peer_value(bytes, 0x790, 0x7b8, 0x7b0, 0x7c0,
                      *reinterpret_cast<const std::uint64_t*>(peer + 0x240));
    append_peer_value(bytes, 0x790, 0x7b8, 0x7b0, 0x7c0,
                      *reinterpret_cast<const std::uint64_t*>(peer + 0x250));
    append_peer_value(bytes, 0x7c8, 0x7f0, 0x7e8, 0x7f8,
                      *reinterpret_cast<const std::uint64_t*>(peer + 0x248));
    append_peer_value(bytes, 0x7c8, 0x7f0, 0x7e8, 0x7f8,
                      *reinterpret_cast<const std::uint64_t*>(peer + 0x258));
    const auto key = *reinterpret_cast<const std::uint64_t*>(peer + 0x180);
    CHashTable::Destroy(bytes + 0x500, &key);
}
}  // namespace SBroadcasterData
}  // namespace NRadEngine
