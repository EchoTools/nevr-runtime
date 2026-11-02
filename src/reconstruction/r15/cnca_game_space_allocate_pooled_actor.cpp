// NRadEngine::CncaGameSpace::AllocatePooledActor
// Quest libr15.so VA 0x19037c4; ReVault binary 576.
// ReVault raw parent 100255959, reconstruction node 100600591; grade H: all 77 instructions mapped; the one callee is the named AllocatePooledActorInternal; table and pool fields are raw offsets, commented in place.
// @0x19037c4 — NRadEngine::CncaGameSpace::AllocatePooledActor
/* @confidence: H */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

// The key arrives in x1 and is compared as a signed 64-bit value.
struct CSymbol64 {
    std::uint64_t bits;
};
static_assert(sizeof(CSymbol64) == 0x08);

// The packed actor value: the x1 half of the result, and the argument of AllocatePooledActorInternal.
struct SActor {
    std::uint64_t bits;
};
static_assert(sizeof(SActor) == 0x08);

// 16-byte result, returned in x0:x1.
struct SActorId {
    std::uint64_t low;
    std::uint64_t high;
};
static_assert(sizeof(SActorId) == 0x10);

class CncaGameSpace {
public:
    // VA 0x19037c4 (thunk 0xfeb620).
    SActorId AllocatePooledActor(CSymbol64 symbol);
    // VA 0x18fe1a4 (thunk 0xfeb390). Called on the space that held the symbol, with the packed SActor.
    void AllocatePooledActorInternal(SActor actor);
};

namespace {

template <class T>
T Load(const std::byte* base, std::size_t offset) {
    T value{};
    std::memcpy(&value, base + offset, sizeof(T));
    return value;
}

template <class T>
void Store(std::byte* base, std::size_t offset, T value) {
    std::memcpy(base + offset, &value, sizeof(T));
}

// Raw layout (no ReVault type recovered for any of it):
//   space +0x18   next space in the chain, searched after this one
//   space +0x58   u64 that is the low half of the result on a hit
//   space +0x370  symbol table of the space, or null
//   space +0x3e8  pool of 0x88-byte slot records, indexed by table index
//   table +0x00   sorted array of 16-byte entries: i64 key, u16 table index at +0x08
//   table +0x30   i64 entry count
//   table +0x160  i16 presence map indexed by table index; -1 = no pool slot for it in this space
//   slot  +0x08   pointer to u16 generations, indexed by free index
//   slot  +0x28   pointer to u16 free-index stack
//   slot  +0x30   i64 free-stack depth
//   slot  +0x40   pointer to u64 bitmap of allocated free indices (bit i = free index i in use)
//   slot  +0x80   u16 high-water free index
//   slot  +0x82   u16 capacity
SActorId PooledAllocate(std::byte* space, std::uint64_t key_bits) {
    // No space to search: {-1, 0xffffffffffff}.
    if (space == nullptr) {
        return SActorId{~std::uint64_t{0}, 0xffffffffffffULL};
    }
    const auto wanted = static_cast<std::int64_t>(key_bits);
    // Stays 0xffffffffffff unless a presence-map skip sets it to index | 0xffffffff0000.
    std::uint64_t skipped_high = 0xffffffffffffULL;

    for (std::byte* object = space; object != nullptr; object = Load<std::byte*>(object, 0x18)) {
        const std::byte* const table = Load<const std::byte*>(object, 0x370);
        if (table == nullptr || Load<std::uint64_t>(table, 0x30) == 0) {
            continue;
        }

        // Binary search, lo = 0, hi = count.
        const std::byte* const entries = Load<const std::byte*>(table, 0x00);
        std::uint64_t lo = 0;
        std::uint64_t hi = Load<std::uint64_t>(table, 0x30);
        bool hit = false;
        std::uint64_t hit_at = 0;
        while (lo < hi) {
            const std::uint64_t mid = lo + ((hi - lo) >> 1);
            const auto probe = static_cast<std::int64_t>(Load<std::uint64_t>(entries, mid * 0x10));
            if (probe == wanted) {
                hit = true;
                hit_at = mid;
                break;
            }
            if (probe > wanted) {
                hi = mid;
            } else {
                lo = mid + 1;
            }
        }
        if (!hit) {
            continue;
        }

        const auto table_index = Load<std::uint16_t>(entries, hit_at * 0x10 + 0x08);
        const std::byte* const presence = Load<const std::byte*>(table, 0x160);
        if (Load<std::int16_t>(presence, static_cast<std::size_t>(table_index) * 2) == -1) {
            skipped_high = static_cast<std::uint64_t>(table_index) | 0xffffffff0000ULL;
            continue;
        }

        // Allocate from this space's pool.
        std::byte* const slot = Load<std::byte*>(object, 0x3e8) + static_cast<std::size_t>(table_index) * 0x88;
        const auto capacity = static_cast<std::uint64_t>(Load<std::uint16_t>(slot, 0x82));
        const auto depth = Load<std::uint64_t>(slot, 0x30);
        // Fails when depth is 0 or above capacity: the unsigned difference is not below capacity.
        if (capacity <= capacity - depth) {
            return SActorId{~std::uint64_t{0}, 0xffffffffffffULL};
        }
        // depth is at least 1 from here on.
        const std::uint64_t low = Load<std::uint64_t>(object, 0x58);
        const auto free_index = Load<std::uint16_t>(Load<const std::byte*>(slot, 0x28), (depth - 1) * 2);
        Store<std::uint64_t>(slot, 0x30, depth - 1);
        const std::uint16_t high_water = Load<std::uint16_t>(slot, 0x80);
        Store<std::uint16_t>(slot, 0x80, high_water <= free_index ? free_index : high_water);
        std::byte* const bitmap = Load<std::byte*>(slot, 0x40);
        const std::size_t bitmap_offset = (static_cast<std::size_t>(free_index) >> 3) & 0x1ff8u;
        Store<std::uint64_t>(bitmap, bitmap_offset,
                             Load<std::uint64_t>(bitmap, bitmap_offset) |
                                 (std::uint64_t{1} << (free_index & 0x3fu)));
        const auto generation =
            Load<std::uint16_t>(Load<const std::byte*>(slot, 0x08), static_cast<std::size_t>(free_index) * 2);
        const std::uint64_t packed = static_cast<std::uint64_t>(table_index) |
                                     (static_cast<std::uint64_t>(free_index) << 16) |
                                     (static_cast<std::uint64_t>(generation) << 32);
        reinterpret_cast<CncaGameSpace*>(object)->AllocatePooledActorInternal(SActor{packed});
        return SActorId{low, packed};
    }
    return SActorId{~std::uint64_t{0}, skipped_high};
}

}  // namespace

SActorId CncaGameSpace::AllocatePooledActor(CSymbol64 symbol) {
    return PooledAllocate(reinterpret_cast<std::byte*>(this), symbol.bits);
}

}  // namespace NRadEngine
