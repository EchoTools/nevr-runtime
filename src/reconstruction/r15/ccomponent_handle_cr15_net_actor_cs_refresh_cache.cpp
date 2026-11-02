// NRadEngine::CComponentHandle<NRadEngine::NRadGame::CR15NetActorCS>::RefreshCache
// Quest libr15.so VA 0x23dfbf4; ReVault binary 576.
// ReVault raw parent 100312148, reconstruction node 100600793; grade M: all 142 instructions (0x23dfbf4-0x23dfe28) read and mapped; no placeholders, every callee has a ReVault name: RefreshActorAndComponentHandle, CncaGame::CurrentActorGUID, CncaGame::GameSpace, NRadEngine::ResolveActorAlias, Sort<512ull,...>, vslot 0x1b8 Component, vslot 0x1d0 IsComponentValid; the two Sort closures are empty stand-ins for lambda types that cannot be named, x21's high half (the caller's) is passed as 0, and the LL/SC lock is a compare-exchange.
// @0x23dfbf4 — NRadEngine::CComponentHandle<NRadEngine::NRadGame::CR15NetActorCS>::RefreshCache
/* @confidence: M */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

struct CSymbol64 {
    std::uint64_t bits;
};
static_assert(sizeof(CSymbol64) == 0x08);

struct SActor {
    std::uint64_t bits;
};
static_assert(sizeof(SActor) == 0x08);

// The 16-byte actor GUID. The libr15.so symbol of CncaGame::CurrentActorGUID is
// _ZNK10NRadEngine8CncaGame16CurrentActorGUIDERKNS_10SActorGUIDE, so the type is NRadEngine::SActorGUID (the Ghidra decompile
// shows SActorGUID* without the namespace; the dynsym name decides). The symbol is passed to GameSpace, the actor to
// ResolveActorAlias and Component.
struct SActorGUID {
    CSymbol64 symbol;  // +0x00
    SActor actor;      // +0x08
};
static_assert(sizeof(SActorGUID) == 0x10);

// Component() and IsComponentValid() take a compact pool handle in a 64-bit register; the slots read its low 32 bits.
using SCompactPoolHandle = std::uint64_t;

class CComponentSystem;
class CncaGameSpace;

namespace NRadGame {
class CR15NetActorCS;
}  // namespace NRadGame

// One 16-byte entry of CTable<CSimpleKey<CComponentSystem*, CSymbol64>>: the key at +0x00, the system at +0x08.
template <class Value, class Key>
struct CSimpleKey {
    Key key;
    Value value;
};
static_assert(sizeof(CSimpleKey<CComponentSystem*, CSymbol64>) == 0x10);

// NRadEngine::Sort<512ull, ...> at 0x130d850 (thunk 0xff0b30): x0 = the table, x1 = the count, x2 = sp+0x20 (the compare
// closure), x3 = sp+0x28 (the swap closure).
template <unsigned long long Block, class Iterator, class Less, class Swap>
void Sort(Iterator data, unsigned long long count, const Less& less, const Swap& swap);

// Empty closures: both are passed by address and never written here. The decompile names them
// NRadEngine::CTable<CSimpleKey<CComponentSystem*, CSymbol64>>::Sort()::_lambda(...)_1_ and
// Sort<512ull,...>()::_lambda(...)_1_, which C++ cannot spell. They are in NRadEngine, not an unnamed namespace, so the
// Sort instantiation has external linkage and its definition can live in another file.
struct CSimpleKeyLess {};
struct CSimpleKeySwap {};

}  // namespace NRadEngine

namespace NRadEngine {

// NRadEngine::ResolveActorAlias(CncaGameSpace*&, SActor&) (symbol _ZN10NRadEngine17ResolveActorAliasERPNS_13CncaGameSpaceERNS_6SActorE):
// thunk 0x100afb0 -> 0x14d6da0. It rewrites the space and the actor and returns whether it did.
bool ResolveActorAlias(CncaGameSpace*& game_space, SActor& actor);

class CncaGame {
public:
    // NRadEngine::CncaGame::CurrentActorGUID(NRadEngine::SActorGUID const&) const: thunk 0xfea210 -> 0x18f9ac4, which adds
    // 0x2138 to the game and tail-calls CurrentGUID (0x18ddf24). x1 = &guid; that function loads the 16 bytes at x1 into
    // x0:x1 (ldp), stores nothing through them and returns the possibly translated GUID by value in x0:x1, assigned by the
    // caller.
    SActorGUID CurrentActorGUID(const SActorGUID& guid) const;
    // NRadEngine::CncaGame::GameSpace(CSymbol64) const: thunk 0x1031ef0 -> 0x18d3414; the signature begin_multiplayer.cpp
    // declares.
    CncaGameSpace* GameSpace(CSymbol64 symbol) const;
};

template <class T>
class CComponentHandle {
public:
    // NRadEngine::CComponentHandle<NRadEngine::NRadGame::CR15NetActorCS>::RefreshCache() const; defined below.
    void RefreshCache() const;
    // NRadEngine::CComponentHandle<NRadEngine::NRadGame::CR15NetActorCS>::RefreshActorAndComponentHandle() const: thunk
    // 0x10216a0 -> 0x23dfe2c. Ghidra shows an undefined4 result; RefreshCache does not read it.
    std::uint32_t RefreshActorAndComponentHandle() const;

    // Raw layout, 0x50 bytes (the size fire_bullet_local.cpp asserts for this instantiation):
    // +0x00 CncaGame*; +0x08 SActorGUID.symbol; +0x10 int64 key (compared signed); +0x18 SActorGUID.actor;
    // +0x20 uint32 (and int16 at +0x20); +0x24 uint32 flags (bit 0: current actor GUID; bits 0-1: Component;
    // bit 1: IsComponentValid, and ResolveActorAlias after a miss); +0x30 CSymbol64; +0x38 int64 stamp (acquire/release);
    // +0x40 CComponentSystem* (the member is T* in the template); +0x48 SCompactPoolHandle as uint32 (0xffff as uint16 for none);
    // +0x4c int32 lock word (0 free, -1 held).
    // mutable: the method is const in ReVault's demangled name, yet it writes the stamp, the handle and the lock word.
    alignas(8) mutable std::byte raw[0x50];
};
static_assert(sizeof(CComponentHandle<NRadGame::CR15NetActorCS>) == 0x50);

}  // namespace NRadEngine

namespace {

template <class T>
T Load(const void* base, std::size_t offset) {
    T value{};
    std::memcpy(&value, static_cast<const std::byte*>(base) + offset, sizeof(T));
    return value;
}

template <class T>
void Store(void* base, std::size_t offset, T value) {
    std::memcpy(static_cast<std::byte*>(base) + offset, &value, sizeof(T));
}

// The function pointer in vtable slot `byte_offset` of `object` (the vptr is the first word of the object).
template <class Fn>
Fn VirtualSlot(const void* object, std::size_t byte_offset) {
    const auto* const vtable = Load<const std::byte*>(object, 0);
    Fn fn{};
    std::memcpy(&fn, vtable + byte_offset, sizeof fn);
    return fn;
}

// The stamp a handle caches: the int64 at game + 0x2130, game being the CncaGame* stored at the handle's +0x00.
std::int64_t GameStamp(const void* handle) {
    return Load<std::int64_t>(Load<const void*>(handle, 0x00), 0x2130);
}

using SystemKey = NRadEngine::CSimpleKey<NRadEngine::CComponentSystem*, NRadEngine::CSymbol64>;
// vslot 0x1b8: Component(SActor, CSymbol64) const; vslot 0x1d0: IsComponentValid(SCompactPoolHandle) const (revault_vtable of NRadEngine::CComponentSystem).
using ComponentFn = std::uint32_t (*)(const NRadEngine::CComponentSystem*, NRadEngine::SActor, NRadEngine::CSymbol64);
using IsComponentValidFn = bool (*)(const NRadEngine::CComponentSystem*, NRadEngine::SCompactPoolHandle);

}  // namespace

template <>
void NRadEngine::CComponentHandle<NRadEngine::NRadGame::CR15NetActorCS>::RefreshCache() const {
    std::byte* const base = raw;
    auto* const stamp = reinterpret_cast<std::int64_t*>(base + 0x38);
    auto* const lock = reinterpret_cast<std::int32_t*>(base + 0x4c);

    // 0x23dfc08-0x23dfc10 and 0x23dfc38-0x23dfc48: no game pointer resets the stamp (-1) and the handle, and returns; no lock.
    if (Load<const void*>(base, 0x00) == nullptr) {
        Store<std::int64_t>(base, 0x38, -1);
        Store<std::uint64_t>(base, 0x40, 0);
        Store<std::uint16_t>(base, 0x48, 0xffff);
        return;
    }
    // 0x23dfc14-0x23dfc28: an acquire load of the stamp equal to the game's stamp needs no refresh; return with no lock.
    const std::int64_t cached = __atomic_load_n(stamp, __ATOMIC_ACQUIRE);
    if (cached == GameStamp(base)) {
        return;
    }
    // 0x23dfc2c-0x23dfc64: take the lock word (0 free, -1 held). The LL/SC loop (ldar, ldaxr, stlxr, clrex) is a compare-exchange here.
    for (;;) {
        while (__atomic_load_n(lock, __ATOMIC_ACQUIRE) != 0) {
        }
        std::int32_t expected = 0;
        if (__atomic_compare_exchange_n(lock, &expected, -1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            break;
        }
    }
    // 0x23dfc68-0x23dfc7c: the stamp checked again with the lock held; equal releases the lock (0x23dfe10) and returns.
    const std::int64_t cached_locked = __atomic_load_n(stamp, __ATOMIC_ACQUIRE);
    if (cached_locked == GameStamp(base)) {
        __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
        return;
    }
    // 0x23dfc80-0x23dfc9c: a short of -1 at +0x18 or +0x20 refreshes the actor and the handle first.
    if (Load<std::int16_t>(base, 0x18) == -1 || Load<std::int16_t>(base, 0x20) == -1) {
        RefreshActorAndComponentHandle();
    }
    // 0x23dfca0-0x23dfcc4: sp+0x10 = { this+0x08, this+0x18 }; flags bit 0 replaces it with the GUID CurrentActorGUID
    // returns in x0:x1 (stp x0, x1 at 0x23dfcc4).
    NRadEngine::SActorGUID guid{NRadEngine::CSymbol64{Load<std::uint64_t>(base, 0x08)},
                                NRadEngine::SActor{Load<std::uint64_t>(base, 0x18)}};
    if ((Load<std::uint8_t>(base, 0x24) & 1U) != 0) {
        guid = Load<NRadEngine::CncaGame*>(base, 0x00)->CurrentActorGUID(guid);
    }
    // 0x23dfcc8-0x23dfcd8: the game space for the symbol (stored at sp+0x08, whose address ResolveActorAlias takes).
    NRadEngine::CncaGameSpace* space = Load<NRadEngine::CncaGame*>(base, 0x00)->GameSpace(guid.symbol);
    if (space == nullptr) {
        // 0x23dfdf8-0x23dfe00: no space: this+0x40 = 0, this+0x48 = 0xffff, then the stamp is stored.
        Store<std::uint64_t>(base, 0x40, 0);
        Store<std::uint16_t>(base, 0x48, 0xffff);
    } else {
        // 0x23dfcec: every pass starts here; ResolveActorAlias jumps back with the rewritten space.
        for (;;) {
            NRadEngine::CComponentSystem* component = nullptr;
            // 0x23dfcec-0x23dfcf0: the space's table count at +0x450; zero means nothing to search.
            std::uint64_t count = Load<std::uint64_t>(space, 0x450);
            if (count != 0) {
                // 0x23dfcf8: the key is this+0x10.
                const std::int64_t key = Load<std::int64_t>(base, 0x10);
                // 0x23dfcf4-0x23dfd20: flags bit 0 of the space (+0x458) means unsorted: sort the table, then clear the bit.
                if ((Load<std::uint8_t>(space, 0x458) & 1U) != 0) {
                    NRadEngine::CSimpleKeyLess less_closure{};  // sp+0x20
                    NRadEngine::CSimpleKeySwap swap_closure{};  // sp+0x28
                    NRadEngine::Sort<512ull>(Load<SystemKey*>(space, 0x420), count, less_closure, swap_closure);
                    Store<std::uint32_t>(space, 0x458, Load<std::uint32_t>(space, 0x458) & 0xfffffffeU);
                    count = Load<std::uint64_t>(space, 0x450);
                }
                // 0x23dfd24-0x23dfd6c: binary search over [0, count) for the key (signed compares); the value is the
                // CComponentSystem* of the matching entry, and a null one counts as a miss.
                if (count != 0) {
                    const SystemKey* const table = Load<const SystemKey*>(space, 0x420);
                    std::uint64_t lo = 0;
                    std::uint64_t hi = count;
                    while (lo < hi) {
                        const std::uint64_t mid = lo + ((hi - lo) >> 1);
                        const auto entry_key = static_cast<std::int64_t>(table[mid].key.bits);
                        if (entry_key > key) {
                            hi = mid;
                        } else if (entry_key < key) {
                            lo = mid + 1;
                        } else {
                            component = table[mid].value;
                            break;
                        }
                    }
                }
            }
            if (component != nullptr) {
                // 0x23dfd70-0x23dfd80: this+0x40 = the system; this+0x48 = the uint32 at this+0x20; flags at +0x24.
                std::uint32_t index = Load<std::uint32_t>(base, 0x20);
                std::uint32_t flags = Load<std::uint32_t>(base, 0x24);
                Store<NRadEngine::CComponentSystem*>(base, 0x40, component);
                Store<std::uint32_t>(base, 0x48, index);
                if ((flags & 3U) != 0) {
                    // 0x23dfd84-0x23dfda0: Component(actor, symbol at +0x30) through vslot 0x1b8; the result replaces this+0x48.
                    index = VirtualSlot<ComponentFn>(component, 0x1b8)(component, guid.actor,
                                                                       NRadEngine::CSymbol64{Load<std::uint64_t>(base, 0x30)});
                    flags = Load<std::uint32_t>(base, 0x24);
                    Store<std::uint32_t>(base, 0x48, index);
                }
                // 0x23dfda4: flags bit 1 clear stores the stamp.
                if ((flags & 2U) == 0) {
                    break;
                }
                // 0x23dfdac-0x23dfdc8: IsComponentValid(handle) through vslot 0x1d0. The handle is x21 with its low half
                // replaced by the index (0x23dfdb0-0x23dfdb4); x21's high half is the caller's and is not recovered, so it is 0.
                // True stores the stamp; false falls to ResolveActorAlias (0x23dfde0).
                const NRadEngine::SCompactPoolHandle handle = index;
                if (VirtualSlot<IsComponentValidFn>(component, 0x1d0)(component, handle)) {
                    break;
                }
            } else {
                // 0x23dfdd0-0x23dfddc: no entry (or a null system): this+0x40 = 0, this+0x48 = 0xffff; flags bit 1 gates
                // ResolveActorAlias.
                Store<std::uint64_t>(base, 0x40, 0);
                Store<std::uint16_t>(base, 0x48, 0xffff);
                if (((Load<std::uint8_t>(base, 0x24) >> 1) & 1U) == 0) {
                    break;
                }
            }
            // 0x23dfde0-0x23dfdf4: ResolveActorAlias rewrites the space (sp+0x08) and the actor (sp+0x18); no match stores the stamp.
            if (!NRadEngine::ResolveActorAlias(space, guid.actor)) {
                break;
            }
            if (space == nullptr) {
                // 0x23dfdf8-0x23dfe00: the alias left no space: this+0x40 = 0, this+0x48 = 0xffff.
                Store<std::uint64_t>(base, 0x40, 0);
                Store<std::uint16_t>(base, 0x48, 0xffff);
                break;
            }
        }
    }
    // 0x23dfe04-0x23dfe0c: this+0x38 = the game's stamp (release store).
    __atomic_store_n(stamp, GameStamp(base), __ATOMIC_RELEASE);
    // 0x23dfe10: release the lock word (stlr wzr).
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
}
