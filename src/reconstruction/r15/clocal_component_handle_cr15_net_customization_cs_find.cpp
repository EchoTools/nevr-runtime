// NRadEngine::CLocalComponentHandle<NRadEngine::NRadGame::CR15NetCustomizationCS>::Find
// Quest libr15.so VA 0x13cd414; ReVault binary 576.
// ReVault raw parent 100225272, reconstruction node 100600681; grade M: all 86 instructions (0x13cd414-0x13cd568) checked against the asm; 1 placeholder stays: FUN_0130ec20 (VA 0x130ec20), the Sort<512ull,...> instantiation at 0x130ec20, not reconstructed in this file; syntax-checked.
// @0x13cd414 — NRadEngine::CLocalComponentHandle<NRadEngine::NRadGame::CR15NetCustomizationCS>::Find
/* @confidence: M */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

namespace NRadGame {
struct CR15NetCustomizationCS;
}  // namespace NRadGame

// Raw layout used here: the CSimpleKey entry table at +0x460, its count at +0x490, its flags at +0x498 (bit 0: unsorted).
class CncaGameSpaceMT;
class CComponentSystem;

// SCompactPoolHandle arrives in a 64-bit register; the slot reads its low 32 bits.
using SCompactPoolHandle = std::uint64_t;

struct SActor {
    std::uint64_t bits;
};
static_assert(sizeof(SActor) == 0x08);

struct CSymbol64 {
    std::uint64_t bits;
};
static_assert(sizeof(CSymbol64) == 0x08);

// One 0x40-byte entry of CTable<CSimpleKey<CTable<CComponentSystem*>, CSymbol64>>: the key at +0x00, the
// CComponentSystem* array at +0x08 and the array count at +0x38. Reached through raw offsets.
struct SystemEntry {
    alignas(8) std::byte raw[0x40];
};
static_assert(sizeof(SystemEntry) == 0x40);

// NRadEngine::ResolveActorAlias(CncaGameSpaceMT*&, SActor&). The caller tests the result as a bool.
bool ResolveActorAlias(CncaGameSpaceMT*& game_space, SActor& actor);

template <class T>
class CLocalComponentHandle {
public:
    void Find(CncaGameSpaceMT* game_space, SActor actor, CSymbol64 symbol);

    CComponentSystem* component;  // +0x00
    std::uint32_t packed_handle;  // +0x08
};
static_assert(offsetof(CLocalComponentHandle<NRadGame::CR15NetCustomizationCS>, packed_handle) == 0x08);

}  // namespace NRadEngine

// Placeholder for the Sort<512ull,...> instantiation at 0x130ec20 (Ghidra FUN_0130ec20), reached from 0x13cd468
// through thunk_FUN_0130ec20 at 0x1043860. Parameters follow the registers the call sets.
extern void FUN_0130ec20(NRadEngine::SystemEntry* keys, std::uint64_t count, const void* compare, void* swap);

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

}  // namespace

template <>
void NRadEngine::CLocalComponentHandle<NRadEngine::NRadGame::CR15NetCustomizationCS>::Find(
    NRadEngine::CncaGameSpaceMT* game_space, NRadEngine::SActor actor, NRadEngine::CSymbol64 symbol) {
    using ComponentSlot = std::uint32_t (*)(const NRadEngine::CComponentSystem*, NRadEngine::SActor,
                                            NRadEngine::CSymbol64);  // vslot 0x1b8: Component(SActor, CSymbol64) const
    using IsValidSlot = bool (*)(const NRadEngine::CComponentSystem*,
                                 NRadEngine::SCompactPoolHandle);  // vslot 0x1d0: IsComponentValid(SCompactPoolHandle) const
    // The key searched for: a read-only u64 at VA 0x2ca5ed8 (file bytes 7e 40 c4 0a 81 34 d5 2d).
    constexpr std::int64_t kSystemKey = static_cast<std::int64_t>(0x2dd534810ac4407eULL);

    if (game_space == nullptr) {  // 0x13cd430 cbz x1 -> 0x13cd550
        component = nullptr;
        return;
    }
    auto* const space = reinterpret_cast<std::byte*>(game_space);
    std::uint64_t count = Load<std::uint64_t>(space, 0x490);  // 0x13cd438

    // 0x13cd458-0x13cd478: bit 0 of the flags word sorts the table first and is then cleared.
    if (count != 0 && (Load<std::uint8_t>(space, 0x498) & 1U) != 0) {
        std::byte compare_object;  // sp+0x18: first lambda slot, one byte (an empty object); not written here
        std::byte swap_object;     // sp+0x28: second lambda slot, one byte; not written here
        FUN_0130ec20(reinterpret_cast<NRadEngine::SystemEntry*>(Load<std::byte*>(space, 0x460)), count,
                     &compare_object, &swap_object);
        Store<std::uint32_t>(space, 0x498, Load<std::uint32_t>(space, 0x498) & ~1U);
        count = Load<std::uint64_t>(space, 0x490);
    }

    // 0x13cd480-0x13cd52c: binary search of the entries by key, signed compares (b.gt, b.ge).
    if (count != 0) {
        const std::byte* const table = Load<const std::byte*>(space, 0x460);
        std::uint64_t lo = 0;
        std::uint64_t hi = count;
        while (lo < hi) {
            const std::uint64_t mid = lo + (hi - lo) / 2;  // 0x13cd498-0x13cd49c
            const std::byte* const entry = table + mid * sizeof(NRadEngine::SystemEntry);
            const auto key = static_cast<std::int64_t>(Load<std::uint64_t>(entry, 0x00));
            if (key > kSystemKey) {  // 0x13cd4ac
                hi = mid;
                continue;
            }
            if (key < kSystemKey) {  // 0x13cd4b0
                lo = mid + 1;
                continue;
            }

            // 0x13cd4c0-0x13cd554: each system of the entry in order, until IsComponentValid accepts one.
            const std::uint64_t system_count = Load<std::uint64_t>(entry, 0x38);
            NRadEngine::CComponentSystem* const* const systems =
                Load<NRadEngine::CComponentSystem* const*>(entry, 0x08);
            for (std::uint64_t index = 0; index < system_count; ++index) {
                NRadEngine::CComponentSystem* const component_system = systems[index];
                component = component_system;  // 0x13cd4e4 str x0, [x20]
                void* const* const vtable = *reinterpret_cast<void* const* const*>(component_system);
                const auto component_slot = reinterpret_cast<ComponentSlot>(vtable[0x1b8 / sizeof(void*)]);
                packed_handle = component_slot(component_system, actor, symbol);  // 0x13cd4f4 blr x8
                // 0x13cd500: bfxil keeps the space pointer's high 32 bits and puts the handle in the low 32.
                const NRadEngine::SCompactPoolHandle handle_arg =
                    (reinterpret_cast<std::uintptr_t>(game_space) & 0xffffffff00000000ULL) | packed_handle;
                const auto is_valid = reinterpret_cast<IsValidSlot>(vtable[0x1d0 / sizeof(void*)]);
                if (is_valid(component_system, handle_arg)) {  // 0x13cd514 blr x9; cbnz w0 -> return
                    return;
                }
            }
            break;
        }
    }

    // 0x13cd52c-0x13cd54c: ResolveActorAlias rewrites the space and the actor; on a match the search runs again on this.
    NRadEngine::CncaGameSpaceMT* resolved_space = game_space;
    NRadEngine::SActor resolved_actor = actor;
    if (NRadEngine::ResolveActorAlias(resolved_space, resolved_actor)) {  // 0x13cd534 bl; cbz w0 -> 0x13cd550
        Find(resolved_space, resolved_actor, symbol);                       // 0x13cd548 bl (thunk to Find)
        return;
    }
    component = nullptr;  // 0x13cd550 str xzr, [x20]
}
