// NRadEngine::NRadGame::CR15NetVarCS::Int
// Quest libr15.so VA 0x22bdc40; ReVault binary 576.
// ReVault raw parent 100304955, reconstruction node 100600802; grade M: all 87 instructions (0x22bdc40-0x22bdd98, ret at 0x22bdd98) read and each branch checked; 0 placeholders; the ldar/ldaxr/stlxr lock sequence is written as a compare-exchange loop, so it is not byte-exact.
// @0x22bdc40 — NRadEngine::NRadGame::CR15NetVarCS::Int
/* @confidence: M */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

struct CSymbol64 {
    std::uint64_t bits;
};
static_assert(sizeof(CSymbol64) == 0x08);

// A component handle, 0x50 bytes. Offsets +0x00..+0x3f are not recovered; the first word is the presence test.
template <class T>
struct CComponentHandle {
    std::byte reserved[0x40];
    T* component;                  // +0x40: the cached component
    std::uint64_t compact_handle;  // +0x48: the handle; its low 16 bits are the table index
    void RefreshCache() const;     // NRadEngine::CComponentHandle<T>::RefreshCache() const, defined elsewhere
};

}  // namespace NRadEngine

namespace NRadEngine::NRadGame {

// Layout not recovered; the game object is read through raw offsets.
class CR15NetGameMT;

// Layout not recovered beyond the vtable slot this function reaches (slot 58, +0x1d0: IsComponentValid).
class CR15NetVarCS {
public:
    // NRadEngine::NRadGame::CR15NetVarCS::Int(CR15NetGameMT const&, NRadEngine::CSymbol64) @ 0x22bdc40.
    // Static: the function takes two argument registers, x0 (the game) and x1 (the symbol), and no this.
    static std::uint32_t Int(const CR15NetGameMT& game, CSymbol64 symbol);
};

static_assert(sizeof(CComponentHandle<CR15NetVarCS>) == 0x50);
static_assert(offsetof(CComponentHandle<CR15NetVarCS>, component) == 0x40);
static_assert(offsetof(CComponentHandle<CR15NetVarCS>, compact_handle) == 0x48);

}  // namespace NRadEngine::NRadGame

namespace {

// Reads a T at byte offset `offset` of a region whose layout is not recovered.
template <class T>
T Load(const std::byte* base, std::size_t offset) {
    T value{};
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

}  // namespace

namespace NRadEngine::NRadGame {

std::uint32_t CR15NetVarCS::Int(const CR15NetGameMT& game, CSymbol64 symbol) {
    const auto* const game_bytes = reinterpret_cast<const std::byte*>(&game);

    // 0x22bdc5c-0x22bdc88: the handle at game+0x2e00 is copied to a local: 0x48 bytes (+0x00..+0x47), then the
    // u32 at game+0x2e48 as the compact handle, its high word stored as zero.
    CComponentHandle<CR15NetVarCS> handle{};
    std::memcpy(&handle, game_bytes + 0x2e00, 0x48);
    handle.compact_handle = Load<std::uint32_t>(game_bytes, 0x2e48);

    // 0x22bdc8c: RefreshCache on the copy; 0x22bdc90-0x22bdc98: its component, null returns 0.
    handle.RefreshCache();
    CR15NetVarCS* component = handle.component;
    if (component == nullptr) {
        return 0;
    }
    // 0x22bdc9c-0x22bdca0: the copy's first word, the presence test; zero returns 0.
    if (Load<std::uint64_t>(handle.reserved, 0x00) == 0) {
        return 0;
    }

    // 0x22bdca4-0x22bdcbc: RefreshCache; then vtable slot 0x1d0 of the component, IsComponentValid
    // (bool CR15NetVarCS::IsComponentValid(SCompactPoolHandle) const, vslot[58]), with the 32-bit handle.
    // A false result returns 0.
    handle.RefreshCache();
    void* const* const vtable = *reinterpret_cast<void* const* const*>(component);
    using IsComponentValidFn = bool (*)(const CR15NetVarCS*, std::uint32_t);
    const auto is_component_valid = reinterpret_cast<IsComponentValidFn>(vtable[0x1d0 / sizeof(void*)]);
    if (!is_component_valid(component, static_cast<std::uint32_t>(handle.compact_handle))) {
        return 0;
    }

    // 0x22bdcc4-0x22bdccc: RefreshCache, then the component reloaded from the copy's +0x40.
    handle.RefreshCache();
    component = handle.component;
    // 0x22bdcd0-0x22bdcd4: RefreshCache again.
    handle.RefreshCache();
    // 0x22bdcd8: the low 16 bits of the handle, the index.
    const auto index = static_cast<std::uint16_t>(handle.compact_handle);

    // 0x22bdcdc-0x22bdcf8: the u16 at component+0xd0 + index*4 picks a 0x30-byte entry of the table at
    // component+0x100; the entry's +0x08 is the lock, a pointer to a u32.
    auto* const component_bytes = reinterpret_cast<std::byte*>(component);
    const std::byte* const index_table = Load<const std::byte*>(component_bytes, 0xd0);
    const auto record_index = Load<std::uint16_t>(index_table, static_cast<std::size_t>(index) * 4);
    std::byte* const entry =
        Load<std::byte*>(component_bytes, 0x100) + static_cast<std::size_t>(record_index) * 0x30;
    std::uint32_t* const lock = Load<std::uint32_t*>(entry, 0x08);

    // 0x22bdcfc-0x22bdd18 and 0x22bdd00: take the lock. Spin while it reads non-zero (ldar); then the exclusive
    // pair (ldaxr, stlxr 1), where a non-zero read clears the monitor (clrex) and goes back to the spin. Written as
    // a compare-exchange 0 -> 1: a spurious failure goes back through the spin read instead of straight to the
    // exclusive load, which changes only the order of an extra load.
    for (;;) {
        while (__atomic_load_n(lock, __ATOMIC_ACQUIRE) != 0) {
        }
        std::uint32_t expected = 0;
        if (__atomic_compare_exchange_n(lock, &expected, 1u, true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            break;
        }
    }

    // 0x22bdd1c-0x22bdd2c: the entry's container at +0x10 and its count at +0x38; a zero count leaves the result 0.
    std::uint32_t result = 0;
    const std::byte* const container = Load<const std::byte*>(entry, 0x10);
    const std::uint64_t count = Load<std::uint64_t>(container, 0x38);
    if (count != 0) {
        // 0x22bdd30-0x22bdd4c: the records at container+0x08, 0x120 bytes each, sorted by the signed u64 key at
        // +0x00. Binary search over [lo, hi): a key above the symbol moves hi to mid, a key below moves lo past mid.
        const std::byte* const records = Load<const std::byte*>(container, 0x08);
        const auto key = static_cast<std::int64_t>(symbol.bits);
        std::uint64_t lo = 0;
        std::uint64_t hi = count;
        while (lo < hi) {
            const std::uint64_t mid = lo + ((hi - lo) >> 1);
            const std::byte* const row = records + mid * 0x120;
            const auto row_key = static_cast<std::int64_t>(Load<std::uint64_t>(row, 0x00));
            if (row_key > key) {
                hi = mid;
            } else if (row_key < key) {
                lo = mid + 1;
            } else {
                // 0x22bdd7c-0x22bdd84: equal keys; the u32 at +0x118 is the result.
                result = Load<std::uint32_t>(row, 0x118);
                break;
            }
        }
    }

    // 0x22bdd88: release the lock (stlr wzr); 0x22bdd8c: return the 32-bit result in w0.
    __atomic_store_n(lock, 0u, __ATOMIC_RELEASE);
    return result;
}

}  // namespace NRadEngine::NRadGame
