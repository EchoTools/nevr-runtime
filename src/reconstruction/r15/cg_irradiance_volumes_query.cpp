// CGIrradianceVolumes::Query
// Quest libr15.so VA 0x195cbfc; ReVault binary 576.
// ReVault raw parent 100257816, reconstruction node 100600453; grade H: every load and store of both paths matches the disassembly; the not-found bytes equal GetEmptyQuery (VA 0x195cd34).
// @0x195cbfc — CGIrradianceVolumes::Query
/* @confidence: H */
// H: every load and store of both paths matches the disassembly; the not-found bytes equal GetEmptyQuery (VA 0x195cd34).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace NRadEngine {

struct C3Vector;           // query position; passed by reference only
struct SGIrradianceQuery;  // result; written at the fixed offsets below

class CGIrradianceVolumes {
public:
    bool Query(const C3Vector& position, SGIrradianceQuery& query) const;

    // NRadEngine::CGIrradianceVolumes::QueryDistance, VA 0x195c834 (reached through the stub at 0xfd0ff0).
    void QueryDistance(const C3Vector& position, float& best_distance_sq,
                       float& best_radius, std::uint64_t& best_index) const;
};

// Image VA 0x386dec8: a pointer-sized global read at run time; the file has no bytes there.
extern std::uint64_t g_irradiance_default_base;
// Image VA 0x32709b8: u32 table indexed by a record's u32 at +0x48; its bound is not recovered.
extern const std::uint32_t g_irradiance_u32_table[];

}  // namespace NRadEngine

namespace {

constexpr std::uint32_t kFloatOneBits = 0x3f800000u;  // 1.0f
// 8 bytes at image VA 0x2c2f958 (01 00 00 00 ff ff ff ff): u32 1, then u32 0xffffffff.
constexpr std::uint64_t kNotFoundPairBits = 0xffffffff00000001u;

std::uint32_t LoadU32(const std::byte* base, std::size_t offset) {
    std::uint32_t value;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

std::uint64_t LoadU64(const std::byte* base, std::size_t offset) {
    std::uint64_t value;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

void StoreU32(std::byte* base, std::size_t offset, std::uint32_t value) {
    std::memcpy(base + offset, &value, sizeof value);
}

void StoreU64(std::byte* base, std::size_t offset, std::uint64_t value) {
    std::memcpy(base + offset, &value, sizeof value);
}

void CopyBytes(std::byte* dst, std::size_t dst_offset, const std::byte* src, std::size_t bytes) {
    std::memcpy(dst + dst_offset, src, bytes);
}

}  // namespace

namespace NRadEngine {

// @0x195cbfc
bool CGIrradianceVolumes::Query(const C3Vector& position, SGIrradianceQuery& query) const {
    auto* const self = reinterpret_cast<const std::byte*>(this);
    auto* const out = reinterpret_cast<std::byte*>(&query);

    // Stored before the call: distance = FLT_MAX, radius = 0, index = -1.
    float best_distance_sq = std::numeric_limits<float>::max();
    float best_radius = 0.0f;
    std::uint64_t best_index = ~std::uint64_t{0};
    QueryDistance(position, best_distance_sq, best_radius, best_index);

    if (best_index == ~std::uint64_t{0}) {
        // 0x195ccb8: no volume matched; default fill, the same bytes GetEmptyQuery writes.
        StoreU64(out, 0x0c, 0);
        StoreU64(out, 0x04, 0);
        StoreU64(out, 0x18, 0);
        StoreU64(out, 0x20, 0);
        StoreU64(out, 0x34, 0);
        StoreU64(out, 0x2c, 0);
        StoreU32(out, 0x00, kFloatOneBits);
        StoreU32(out, 0x14, kFloatOneBits);
        StoreU32(out, 0x28, kFloatOneBits);
        StoreU32(out, 0x3c, kFloatOneBits);
        const std::uint64_t default_base = g_irradiance_default_base;
        StoreU64(out, 0x58, default_base + 0x17c8);
        StoreU64(out, 0x60, default_base + 0x1a30);
        StoreU32(out, 0x40, 0);
        StoreU32(out, 0x44, 4);
        StoreU64(out, 0x48, kNotFoundPairBits);
        StoreU32(out, 0x50, 0xffffffffu);
        return false;
    }

    // 0x195cc3c: index names a volume; copy its 64-byte matrix from this->[0x160].
    const std::uint64_t index = best_index;
    const std::byte* const matrix =
        reinterpret_cast<const std::byte*>(LoadU64(self, 0x160)) + index * 0x40;
    const std::byte* const record_root = reinterpret_cast<const std::byte*>(LoadU64(self, 0));
    CopyBytes(out, 0x00, matrix, 0x10);
    const std::byte* const record =
        reinterpret_cast<const std::byte*>(LoadU64(record_root, 0)) + index * 0x78;
    CopyBytes(out, 0x10, matrix + 0x10, 0x10);
    CopyBytes(out, 0x20, matrix + 0x20, 0x10);
    CopyBytes(out, 0x30, matrix + 0x30, 0x10);
    const auto* const q_array = reinterpret_cast<const std::byte*>(LoadU64(self, 0x08));
    StoreU64(out, 0x58, LoadU64(q_array, index * 8));
    const std::uint32_t key = LoadU32(record, 0x48);
    const std::uint64_t d0_bits = LoadU64(record, 0x34);
    const std::uint64_t volume_array = LoadU64(self, 0x180);
    const std::uint32_t table_value = g_irradiance_u32_table[key];
    const std::uint32_t sum = LoadU32(self, 0x198) + static_cast<std::uint32_t>(index);
    StoreU64(out, 0x60, volume_array + index * 0xb0);
    StoreU32(out, 0x40, key);
    StoreU32(out, 0x44, table_value);
    StoreU64(out, 0x48, d0_bits);
    StoreU32(out, 0x50, sum);
    return true;
}

}  // namespace NRadEngine
