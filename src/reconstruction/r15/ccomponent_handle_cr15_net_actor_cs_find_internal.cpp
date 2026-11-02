// NRadEngine::CComponentHandle<NRadEngine::NRadGame::CR15NetActorCS>::FindInternal
// Quest libr15.so VA 0x23e1910; ReVault binary 576.
// ReVault raw parent 100312166, reconstruction node 100600800; grade M: all 102 instructions (0x23e1910-0x23e1aa4) checked against the asm; 0 placeholders; the key loaded from VA 0x2ca5e80 is an unnamed literal, the handle is raw offsets, and the two Sort closure types are namespace stand-ins with no source names; syntax-checked.
// @0x23e1910 — NRadEngine::CComponentHandle<NRadEngine::NRadGame::CR15NetActorCS>::FindInternal
/* @confidence: M */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

class CComponentSystem;
class CncaGameSpaceMT;

// SCompactPoolHandle arrives in a 64-bit register; the slots read its low 32 bits (as in the merged
// clocal_component_handle_cr15_net_customization_cs_find.cpp).
using SCompactPoolHandle = std::uint64_t;

struct SActor {
    std::uint64_t bits;
};
static_assert(sizeof(SActor) == 0x08);

struct CSymbol64 {
    std::uint64_t bits;
};
static_assert(sizeof(CSymbol64) == 0x08);

template <class T>
class CTable;

template <class Value, class Key>
struct CSimpleKey;

// One 0x40-byte entry of CTable<CSimpleKey<CTable<CComponentSystem*>, CSymbol64>> (the stride is `lsl x11, x10, #6`):
// the key at +0x00, the CComponentSystem* array at +0x08 and the array count at +0x38. Reached through raw offsets.
template <>
struct CSimpleKey<CTable<CComponentSystem*>, CSymbol64> {
    alignas(8) std::byte raw[0x40];
};
static_assert(sizeof(CSimpleKey<CTable<CComponentSystem*>, CSymbol64>) == 0x40);

// The two empty closure objects the call to Sort receives by address (sp+0x10 and sp+0x18). Closure types have no
// source name; these stand-ins sit in a named namespace so the Sort instantiation keeps external linkage.
// CTableSortLessClosure: CTable<CSimpleKey<...>>::Sort<CSimpleKey<...>>()::{lambda(CSimpleKey const&, CSimpleKey const&)#1}.
struct CTableSortLessClosure {};
// SortSwapClosure: the {lambda(CSimpleKey*, unsigned long long, unsigned long long)#1} inside the Sort<512ull, ...> overload.
struct SortSwapClosure {};

namespace NRadGame {
class CR15NetActorCS {
public:
    class MT;
};
}  // namespace NRadGame

// NRadEngine::ResolveActorAlias(CncaGameSpaceMT*&, SActor&), VA 0x14d6da4 (thunk 0x104a520). The caller tests w0.
bool ResolveActorAlias(CncaGameSpaceMT*& game_space, SActor& actor);

// NRadEngine::Sort<512ull, CSimpleKey<CTable<CComponentSystem*>, CSymbol64>*, CTableSortLessClosure, SortSwapClosure>,
// VA 0x130ec20 (thunk 0x1043860). Declaration only; the body is sort_512_csimplekey_ctable_ccomponentsystem_csymbol64.cpp.
template <unsigned long long Block, class Iterator, class Less, class Swap>
void Sort(Iterator data, unsigned long long count, const Less& less, const Swap& swap);

// A CComponentHandle<T> object of 0x50 bytes (merged fire_bullet_local.cpp); its fields are reached through raw offsets.
template <class T>
class CComponentHandle {
public:
    void FindInternal(CncaGameSpaceMT* game_space, SActor actor, CSymbol64 symbol);

    // CComponentHandle<T>::SetT<T>(T::MT*, SCompactPoolHandle), VA 0x23dfa0c (thunk 0x104d8c0).
    template <class U>
    void SetT(typename U::MT* component, SCompactPoolHandle handle);

    alignas(8) std::byte raw[0x50];
};
static_assert(sizeof(CComponentHandle<NRadGame::CR15NetActorCS>) == 0x50);

}  // namespace NRadEngine

namespace {

template <class T>
T Load(const std::byte* base, std::size_t offset) {
    T value;
    std::memcpy(&value, base + offset, sizeof(T));
    return value;
}

template <class T>
void Store(std::byte* base, std::size_t offset, T value) {
    std::memcpy(base + offset, &value, sizeof(T));
}

// The function pointer stored at byte_offset of the object's vtable.
template <class Fn>
Fn VirtualSlot(const void* object, std::size_t byte_offset) {
    const auto* const bytes = static_cast<const std::byte*>(object);
    const auto table = Load<std::uintptr_t>(bytes, 0);
    return reinterpret_cast<Fn>(Load<std::uintptr_t>(reinterpret_cast<const std::byte*>(table), byte_offset));
}

}  // namespace

// Layout used on the handle (this, x19): +0x20 u32 packed handle, +0x24 u32 flags (bit 1 set when the search went
// through an actor alias); the other fields are only written by the reset block at the end.
// Layout used on the game space (x21): the CSimpleKey table pointer at +0x460, its count at +0x490, its flags at +0x498
// (bit 0: unsorted).
template <>
void NRadEngine::CComponentHandle<NRadEngine::NRadGame::CR15NetActorCS>::FindInternal(
    NRadEngine::CncaGameSpaceMT* game_space, NRadEngine::SActor actor, NRadEngine::CSymbol64 symbol) {
    using SystemKey = NRadEngine::CSimpleKey<NRadEngine::CTable<NRadEngine::CComponentSystem*>, NRadEngine::CSymbol64>;
    using ComponentSlot = std::uint32_t (*)(NRadEngine::CComponentSystem*, NRadEngine::SActor,
                                            NRadEngine::CSymbol64);  // vslot 0x1b8: Component(SActor, CSymbol64) const
    using IsValidSlot = bool (*)(NRadEngine::CComponentSystem*,
                                 NRadEngine::SCompactPoolHandle);  // vslot 0x1d0: IsComponentValid(SCompactPoolHandle) const
    // The key searched for: the u64 at VA 0x2ca5e80, the address `adrp 0x2ca5000; add #0xe84; and #-8` forms
    // (file bytes da 73 37 90 03 9f b8 cf), compared as a signed 64-bit value.
    constexpr std::int64_t kSystemKey = static_cast<std::int64_t>(0xcfb89f03903773daULL);

    std::byte* const self = raw;                                // x19
    NRadEngine::CncaGameSpaceMT* resolved_space = game_space;  // sp+0x00
    NRadEngine::SActor resolved_actor = actor;                 // sp+0x08

    if (game_space != nullptr) {  // 0x23e192c cbz x1 -> 0x23e1a50
        auto* const space = reinterpret_cast<std::byte*>(game_space);  // x21
        std::uint64_t count = Load<std::uint64_t>(space, 0x490);       // 0x23e1934

        if (count != 0) {  // 0x23e193c cbz x1 -> 0x23e1a20
            // 0x23e194c-0x23e1978: bit 0 of the flags sorts the table first; the bit is then cleared (32-bit store).
            if ((Load<std::uint8_t>(space, 0x498) & 1U) != 0) {
                const NRadEngine::CTableSortLessClosure less{};  // sp+0x10
                const NRadEngine::SortSwapClosure swap{};        // sp+0x18
                NRadEngine::Sort<512>(Load<SystemKey*>(space, 0x460), count, less, swap);  // 0x23e1964 bl 0x1043860
                count = Load<std::uint64_t>(space, 0x490);                                 // 0x23e196c
                Store<std::uint32_t>(space, 0x498, Load<std::uint32_t>(space, 0x498) & 0xfffffffeU);
            }

            // 0x23e197c-0x23e1998: binary search of the entries by key, signed compares (b.gt, b.lt).
            const auto* const table = Load<const std::byte*>(space, 0x460);  // x8
            std::uint64_t lo = 0;                                            // x9
            std::uint64_t hi = count;                                        // x1
            while (lo < hi) {
                const std::uint64_t mid = lo + ((hi - lo) >> 1);  // 0x23e199c-0x23e19a0
                const std::byte* const entry = table + mid * sizeof(SystemKey);
                const auto entry_key = Load<std::int64_t>(entry, 0x00);  // 0x23e19a8
                if (entry_key > kSystemKey) {                            // 0x23e19b0 b.gt: hi = mid
                    hi = mid;
                    continue;
                }
                if (entry_key < kSystemKey) {  // 0x23e19b4 b.lt: lo = mid + 1
                    lo = mid + 1;
                    continue;
                }

                // 0x23e19b8-0x23e1a1c: each system of the entry in order, until IsComponentValid accepts one.
                if (Load<std::uint64_t>(entry, 0x38) != 0) {  // 0x23e19c0 ldr x9, [x23, #0x38]!; cbz
                    std::uint64_t index = 0;                   // x24
                    do {
                        NRadEngine::CComponentSystem* const component_system =
                            Load<NRadEngine::CComponentSystem* const*>(entry, 0x08)[index];  // 0x23e19d0-0x23e19dc
                        const auto component_slot = VirtualSlot<ComponentSlot>(component_system, 0x1b8);
                        const std::uint32_t handle = component_slot(component_system, actor, symbol);  // 0x23e19ec blr x8
                        Store<std::uint32_t>(self, 0x20, handle);  // 0x23e19f0 str w0, [x19, #0x20]
                        // 0x23e19f8: bfxil keeps the space pointer's high 32 bits and puts the handle in the low 32.
                        const NRadEngine::SCompactPoolHandle handle_arg =
                            (reinterpret_cast<std::uintptr_t>(game_space) & 0xffffffff00000000ULL) | handle;
                        const auto is_valid = VirtualSlot<IsValidSlot>(component_system, 0x1d0);
                        if (is_valid(component_system, handle_arg)) {  // 0x23e1a08 blr x8; 0x23e1a0c cbnz w0
                            // 0x23e1a94-0x23e1aa0: SetT<CR15NetActorCS>(this, system, [this+0x20]), then return.
                            SetT<NRadEngine::NRadGame::CR15NetActorCS>(
                                reinterpret_cast<NRadEngine::NRadGame::CR15NetActorCS::MT*>(component_system),
                                Load<std::uint32_t>(self, 0x20));
                            return;
                        }
                        ++index;  // 0x23e1a10-0x23e1a1c: the count at entry+0x38 is read again
                    } while (index < Load<std::uint64_t>(entry, 0x38));
                }
                break;
            }
        }

        // 0x23e1a20-0x23e1a4c: ResolveActorAlias rewrites the space and the actor in place; on a match the search
        // runs again with the flags word marked.
        if (NRadEngine::ResolveActorAlias(resolved_space, resolved_actor)) {  // 0x23e1a28 bl 0x104a520
            Store<std::uint32_t>(self, 0x24, Load<std::uint32_t>(self, 0x24) | 2U);
            FindInternal(resolved_space, resolved_actor, symbol);  // 0x23e1a48 bl 0xfe60f0 (thunk to this function)
            return;
        }
    }

    // 0x23e1a50-0x23e1a78: no game space, or no alias to follow: reset the handle.
    Store<std::uint64_t>(self, 0x40, 0);
    Store<std::uint16_t>(self, 0x48, 0xffff);
    Store<std::uint16_t>(self, 0x20, 0xffff);
    Store<std::uint64_t>(self, 0x30, ~0ULL);
    Store<std::uint64_t>(self, 0x38, ~0ULL);
    Store<std::uint64_t>(self, 0x00, 0);
    Store<std::uint64_t>(self, 0x08, ~0ULL);
    Store<std::uint64_t>(self, 0x10, ~0ULL);
    Store<std::uint64_t>(self, 0x16, ~0ULL);  // stur: overlaps +0x10..+0x17, same bytes
    Store<std::uint32_t>(self, 0x24, 0);
}
