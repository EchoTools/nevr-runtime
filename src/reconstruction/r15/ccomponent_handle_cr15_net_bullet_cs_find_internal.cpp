// NRadEngine::CComponentHandle<NRadEngine::NRadGame::CR15NetBulletCS>::FindInternal
// Quest libr15.so VA 0x23e1270; ReVault binary 576.
// ReVault raw parent 100312162, reconstruction node 100600803; grade M: all 102 instructions (0x23e1270-0x23e1404: the ret at 0x23e13f0 and the SetT call after it) read and mapped; 0 placeholders, every callee has a ReVault name (NRadEngine::Sort<512ull,...> 0x130ec20, NRadEngine::ResolveActorAlias 0x14d6da4, CComponentHandle<CR15NetBulletCS>::SetT 0x23e1168, FindInternal itself, vslot 0x1b8 Component, vslot 0x1d0 IsComponentValid); the two Sort closures are empty stand-in structs, the system key is the file constant at 0x3283f18 rather than a named global, and the table entry is raw bytes; syntax-checked.
// @0x23e1270 — NRadEngine::CComponentHandle<NRadEngine::NRadGame::CR15NetBulletCS>::FindInternal
/* @confidence: M */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

class CComponentSystem;
class CncaGameSpaceMT;

struct CSymbol64 {
    std::uint64_t bits;
};
static_assert(sizeof(CSymbol64) == 0x08);

struct SActor {
    std::uint64_t bits;
};
static_assert(sizeof(SActor) == 0x08);

// Component() returns a compact pool handle in w0; IsComponentValid() takes one in a 64-bit register and reads its low 32 bits.
using SCompactPoolHandle = std::uint64_t;

namespace NRadGame {
// CR15NetBulletCS derives from CComponentSystem; SetT takes its nested CR15NetBulletCS::MT* (revault_function of 0x23e1168).
class CR15NetBulletCS {
public:
    class MT;
};
}  // namespace NRadGame

template <class Value>
class CTable;

template <class Value, class Key>
struct CSimpleKey;

// One 0x40-byte entry of CTable<CSimpleKey<CTable<CComponentSystem*>, CSymbol64>>, reached through raw offsets:
// +0x00 the key (CSymbol64, compared as a signed 64-bit integer), +0x08 the CTable<CComponentSystem*> whose first word
// (entry + 0x08) is the CComponentSystem* array and whose element count is the word at entry + 0x38.
template <>
struct CSimpleKey<CTable<CComponentSystem*>, CSymbol64> {
    alignas(8) std::byte raw[0x40];
};
static_assert(sizeof(CSimpleKey<CTable<CComponentSystem*>, CSymbol64>) == 0x40);

using SystemKey = CSimpleKey<CTable<CComponentSystem*>, CSymbol64>;

// NRadEngine::Sort<512ull, CSimpleKey<CTable<CComponentSystem*>, CSymbol64>*, ...> at 0x130ec20 (thunk 0x1043860); defined in
// sort_512_csimplekey_ctable_ccomponentsystem_csymbol64.cpp with this template signature.
template <unsigned long long Block, class Iterator, class Less, class Swap>
void Sort(Iterator data, unsigned long long count, const Less& less, const Swap& swap);

// Stand-ins for the two empty closure types Sort receives; a closure has no name. The decompile names them
// CTable<CSimpleKey<...>>::Sort<CSimpleKey<...>>()::{lambda(CSimpleKey const&, CSimpleKey const&)#1} (x2, sp+0x10) and
// Sort<512ull, ...>()::{lambda(CSimpleKey*, unsigned long long, unsigned long long)#1} (x3, sp+0x18). Sort reads neither; FindInternal
// only passes their addresses. They are in NRadEngine, not an unnamed namespace, so the Sort instantiation keeps external linkage.
struct CTableSortLess {};
struct SortSwap {};

// NRadEngine::ResolveActorAlias(CncaGameSpaceMT*&, SActor&) at 0x14d6da4 (thunk 0x104a520): a tail branch to
// CActorAliasCS::Resolve at 0x1461050. The caller tests w0 and reads back the rewritten space and actor.
bool ResolveActorAlias(CncaGameSpaceMT*& game_space, SActor& actor);

// A CComponentHandle<T> is 0x50 bytes reached through raw offsets (the size fire_bullet_local.cpp asserts). The bytes this
// function writes: +0x00, +0x08, +0x10, +0x16 (8 bytes, unaligned), +0x30, +0x38, +0x40 and +0x48 are cleared or set to -1 on a
// miss; +0x20 is the uint32 pool handle (int16 -1 on a miss); +0x24 is the uint32 flags word (bit 1 set after an alias).
template <class T>
class CComponentHandle {
public:
    // NRadEngine::CComponentHandle<NRadEngine::NRadGame::CR15NetBulletCS>::FindInternal(CncaGameSpaceMT*, SActor, CSymbol64)
    // at 0x23e1270 (thunk 0xfd6240); defined below.
    void FindInternal(CncaGameSpaceMT* game_space, SActor actor, CSymbol64 symbol);

    // NRadEngine::CComponentHandle<CR15NetBulletCS>::SetT<CR15NetBulletCS>(CR15NetBulletCS::MT*, SCompactPoolHandle) at 0x23e1168
    // (thunk 0x1000450): x0 this, x1 the system, w2 the handle.
    template <class U>
    void SetT(typename U::MT* system, SCompactPoolHandle handle);

    alignas(8) std::byte raw[0x50];
};
static_assert(sizeof(CComponentHandle<NRadGame::CR15NetBulletCS>) == 0x50);

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

// The function pointer in vtable slot `byte_offset` of `object` (the vptr is the first word of the object).
template <class Fn>
Fn VirtualSlot(const NRadEngine::CComponentSystem* object, std::size_t byte_offset) {
    const auto* const vtable = Load<const std::byte*>(reinterpret_cast<const std::byte*>(object), 0);
    Fn fn;
    std::memcpy(&fn, vtable + byte_offset, sizeof(fn));
    return fn;
}

// vslot 0x1b8: Component(SActor, CSymbol64) const; vslot 0x1d0: IsComponentValid(SCompactPoolHandle) const
// (revault_vtable of NRadEngine::CComponentSystem).
using ComponentFn = std::uint32_t (*)(const NRadEngine::CComponentSystem*, NRadEngine::SActor, NRadEngine::CSymbol64);
using IsComponentValidFn = bool (*)(const NRadEngine::CComponentSystem*, NRadEngine::SCompactPoolHandle);
constexpr std::size_t kComponentSlot = 0x1b8;
constexpr std::size_t kIsComponentValidSlot = 0x1d0;

// CncaGameSpaceMT: the sorted system-entry table, its count and its flags word (bit 0: unsorted).
constexpr std::size_t kSpaceTable = 0x460;
constexpr std::size_t kSpaceCount = 0x490;
constexpr std::size_t kSpaceFlags = 0x498;

// System entry: the CComponentSystem* array and its count.
constexpr std::size_t kEntrySystems = 0x08;
constexpr std::size_t kEntrySystemCount = 0x38;

// The key searched for: the u64 at VA 0x3283f18 (file bytes b2 c1 07 0e de 7c 03 48), compared as a signed integer.
constexpr std::int64_t kSystemKey = 0x48037cde0e07c1b2LL;

}  // namespace

template <>
void NRadEngine::CComponentHandle<NRadEngine::NRadGame::CR15NetBulletCS>::FindInternal(
    NRadEngine::CncaGameSpaceMT* game_space, NRadEngine::SActor actor, NRadEngine::CSymbol64 symbol) {
    auto* const self = reinterpret_cast<std::byte*>(this);  // x19

    // 0x23e1288: stp x1, x2, [sp]. Both are passed by reference to ResolveActorAlias and read back from the frame.
    NRadEngine::CncaGameSpaceMT* local_space = game_space;  // sp+0x00
    NRadEngine::SActor local_actor = actor;                 // sp+0x08

    if (game_space != nullptr) {  // 0x23e128c cbz x1 -> 0x23e13b0
        auto* const space = reinterpret_cast<std::byte*>(game_space);  // x21
        std::uint64_t count = Load<std::uint64_t>(space, kSpaceCount);  // 0x23e1294

        if (count != 0) {  // 0x23e129c cbz x1 -> 0x23e1380
            // 0x23e12ac-0x23e12b4: bit 0 of the flags byte means the table is unsorted; sort it, then clear the bit.
            if ((Load<std::uint8_t>(space, kSpaceFlags) & 1U) != 0) {
                NRadEngine::CTableSortLess less;  // sp+0x10 (x2)
                NRadEngine::SortSwap swap;        // sp+0x18 (x3)
                NRadEngine::Sort<512ULL>(Load<NRadEngine::SystemKey*>(space, kSpaceTable), count, less, swap);  // 0x23e12c4
                Store<std::uint32_t>(space, kSpaceFlags, Load<std::uint32_t>(space, kSpaceFlags) & 0xfffffffeU);  // 0x23e12c8-0x23e12d4
                count = Load<std::uint64_t>(space, kSpaceCount);  // 0x23e12cc: the count is read again
            }

            if (count != 0) {  // 0x23e12d8 cbz x1 -> 0x23e1380
                // 0x23e12dc-0x23e1314: binary search of the entries by key, signed compares (b.gt, b.lt).
                const std::byte* const table = Load<const std::byte*>(space, kSpaceTable);  // 0x23e12dc
                std::uint64_t lo = 0;
                std::uint64_t hi = count;
                while (lo < hi) {
                    const std::uint64_t mid = lo + ((hi - lo) >> 1);  // 0x23e12fc-0x23e1300
                    const std::byte* const entry = table + mid * sizeof(NRadEngine::SystemKey);  // 0x23e1304-0x23e1308, 0x23e1318
                    const auto entry_key = static_cast<std::int64_t>(Load<std::uint64_t>(entry, 0));
                    if (entry_key > kSystemKey) {  // 0x23e1310 b.gt 0x23e12f0
                        hi = mid;
                        continue;
                    }
                    if (entry_key < kSystemKey) {  // 0x23e1314 b.lt 0x23e12e8
                        lo = mid + 1;
                        continue;
                    }

                    // 0x23e131c-0x23e137c: each system of the entry in order, until IsComponentValid accepts one.
                    if (Load<std::uint64_t>(entry, kEntrySystemCount) != 0) {  // 0x23e1320-0x23e1324
                        std::uint64_t index = 0;  // x24
                        do {
                            auto* const component_system =
                                Load<NRadEngine::CComponentSystem* const*>(entry, kEntrySystems)[index];  // 0x23e1330-0x23e133c

                            // 0x23e1340-0x23e134c: Component(actor, symbol) through vslot 0x1b8; the result is the pool handle.
                            const std::uint32_t packed_handle = VirtualSlot<ComponentFn>(component_system, kComponentSlot)(
                                component_system, local_actor, symbol);
                            Store<std::uint32_t>(self, 0x20, packed_handle);  // 0x23e1350 str w0, [x19, #0x20]

                            // 0x23e1358: bfxil x21, x0, #0, #32 keeps the high half of the game space pointer (x21) and puts
                            // the handle in the low half.
                            const NRadEngine::SCompactPoolHandle handle_arg =
                                (reinterpret_cast<std::uintptr_t>(game_space) & 0xffffffff00000000ULL) | packed_handle;

                            // 0x23e135c-0x23e136c: IsComponentValid(handle) through vslot 0x1d0; nonzero goes to 0x23e13f4.
                            if (VirtualSlot<IsComponentValidFn>(component_system, kIsComponentValidSlot)(component_system,
                                                                                                         handle_arg)) {
                                // 0x23e13f4-0x23e1400: SetT(this, system, *(uint32_t*)(this + 0x20)); then the epilogue.
                                this->SetT<NRadEngine::NRadGame::CR15NetBulletCS>(
                                    reinterpret_cast<NRadEngine::NRadGame::CR15NetBulletCS::MT*>(component_system),
                                    Load<std::uint32_t>(self, 0x20));
                                return;
                            }

                            // 0x23e1370-0x23e137c: the entry's system count is read again for every pass.
                            ++index;
                        } while (index < Load<std::uint64_t>(entry, kEntrySystemCount));
                    }
                    break;  // 0x23e1324 cbz x9 -> 0x23e1380, or the loop ran out: resolve the alias
                }
            }
        }

        // 0x23e1380-0x23e13ac: ResolveActorAlias rewrites the space and the actor; on a match flag bit 1 is set and the
        // search runs again on this handle with the rewritten pair.
        if (NRadEngine::ResolveActorAlias(local_space, local_actor)) {  // 0x23e1388 bl; cbz w0 -> 0x23e13b0
            Store<std::uint32_t>(self, 0x24, Load<std::uint32_t>(self, 0x24) | 2U);  // 0x23e1390-0x23e13a0
            FindInternal(local_space, local_actor, symbol);  // 0x23e13a4-0x23e13a8: ldp x1, x2, [sp]; bl (thunk 0xfd6240)
            return;
        }
    }

    // 0x23e13b0-0x23e13d8: no space, or no match: the handle is cleared in the order the stores run.
    Store<std::uint64_t>(self, 0x40, 0);              // str xzr, [x19, #0x40]
    Store<std::uint16_t>(self, 0x48, 0xffff);         // strh w8, [x19, #0x48]
    Store<std::uint16_t>(self, 0x20, 0xffff);         // strh w8, [x19, #0x20]
    Store<std::uint64_t>(self, 0x30, ~0ULL);          // str q0, [x19, #0x30]: all-ones, 16 bytes
    Store<std::uint64_t>(self, 0x38, ~0ULL);
    Store<std::uint64_t>(self, 0x00, 0);              // stp xzr, x9, [x19]
    Store<std::uint64_t>(self, 0x08, ~0ULL);
    Store<std::uint64_t>(self, 0x10, ~0ULL);          // str x9, [x19, #0x10]
    Store<std::uint64_t>(self, 0x16, ~0ULL);          // stur x9, [x19, #0x16]
    Store<std::uint32_t>(self, 0x24, 0);              // str wzr, [x19, #0x24]
}
