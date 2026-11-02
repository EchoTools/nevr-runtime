// NRadEngine::CComponentHandle<NRadEngine::CPhysicsCS>::RefreshCache
// Quest libr15.so VA 0x13367cc; ReVault binary 576.
// ReVault raw parent 100222770, reconstruction node 100600724; grade M: all 142 instructions (0x13367cc-0x1336a00) read and mapped; no placeholders; every callee has a ReVault name: RefreshActorAndComponentHandle, CncaGame::CurrentActorGUID(SActorGUID*), CncaGame::GameSpace, NRadEngine::ResolveActorAlias, Sort<512ull>, vslot 0x1b8 Component, vslot 0x1d0 IsComponentValid; the two Sort closures are empty stand-ins (the asm passes sp+0x20/sp+0x28 and never stores them), x21's high half is caller state and is not recovered, and the LL/SC lock is a compare-exchange; CurrentActorGUID returns the 16-byte GUID in x0:x1, which is stored over the GUID (0x133689c stp) and whose x0 goes to GameSpace; syntax-checked.
// @0x13367cc — NRadEngine::CComponentHandle<NRadEngine::CPhysicsCS>::RefreshCache
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

// Component() and IsComponentValid() take a compact pool handle in a 64-bit register; the slots read its low 32 bits.
using SCompactPoolHandle = std::uint64_t;

class CComponentSystem;
class CncaGameSpace;
struct CPhysicsCS;

// One 16-byte entry of CTable<CSimpleKey<CComponentSystem*, CSymbol64>>: the key at +0x00, the system at +0x08.
template <class Value, class Key>
struct CSimpleKey {
    Key key;
    Value value;
};
static_assert(sizeof(CSimpleKey<CComponentSystem*, CSymbol64>) == 0x10);

// NRadEngine::Sort<512ull, ...> at 0x130d850 (thunk 0xff0b30), declared as sort_512_csimplekey_ccomponentsystem_csymbol64.cpp declares it.
template <unsigned long long Block, class Iterator, class Less, class Swap>
void Sort(Iterator data, unsigned long long count, const Less& less, const Swap& swap);

// Empty closures: Sort reads neither (sort_512_csimplekey_ccomponentsystem_csymbol64.cpp). The decompile names them
// NRadEngine::CTable<CSimpleKey<CComponentSystem*, CSymbol64>>::Sort()::_lambda(...)_1_ and Sort<512ull,...>()::_lambda(...)_1_.
// They are in NRadEngine, not an unnamed namespace, so the Sort instantiation has external linkage and its definition
// can live in the other file.
struct CSimpleKeyLess {};
struct CSimpleKeySwap {};

}  // namespace NRadEngine

// The 16-byte actor GUID that CncaGame::CurrentActorGUID returns by value (x0:x1). ReVault shows the type
// without a namespace, so it is declared at global scope: symbol is passed to GameSpace, actor to ResolveActorAlias.
struct SActorGUID {
    NRadEngine::CSymbol64 symbol;  // +0x00
    NRadEngine::SActor actor;      // +0x08
};
static_assert(sizeof(SActorGUID) == 0x10);

namespace NRadEngine {

// NRadEngine::ResolveActorAlias(CncaGameSpace**, SActor*) at 0x100afb0; the signature local_teleport.cpp declares.
bool ResolveActorAlias(CncaGameSpace** game_space, SActor* actor);

class CncaGame {
public:
    // NRadEngine::CncaGame::CurrentActorGUID(SActorGUID*): thunk 0xfea210. The game is passed in x0 and the address of the
    // 16-byte GUID (sp+0x10) in x1; the 16-byte GUID comes back by value in x0:x1 ({symbol, actor}), and the caller stores
    // it over the GUID (0x133689c stp). ReVault shows a void result, which does not express the returned pair.
    SActorGUID CurrentActorGUID(SActorGUID* guid);
    // NRadEngine::CncaGame::GameSpace(CSymbol64) const: thunk 0x1031ef0; the signature begin_multiplayer.cpp declares.
    CncaGameSpace* GameSpace(CSymbol64 symbol) const;
};

template <class T>
class CComponentHandle {
public:
    // NRadEngine::CComponentHandle<NRadEngine::CPhysicsCS>::RefreshCache() const; defined below.
    void RefreshCache() const;
    // NRadEngine::CComponentHandle<NRadEngine::CPhysicsCS>::RefreshActorAndComponentHandle() const: thunk 0xfee1b0.
    // Ghidra shows an undefined4 result; RefreshCache does not read it.
    std::uint32_t RefreshActorAndComponentHandle() const;

    // Raw layout, 0x50 bytes (the size the merged CComponentHandle<T> declarations assert):
    // +0x00 CncaGame*; +0x08 SActorGUID.symbol; +0x10 int64 key (compared signed); +0x18 SActorGUID.actor;
    // +0x20 uint32 (and int16 at +0x20); +0x24 uint32 flags (bit 0: current actor GUID; bits 0-1: Component;
    // bit 1: IsComponentValid, and ResolveActorAlias after a miss); +0x30 CSymbol64; +0x38 int64 stamp (acquire/release);
    // +0x40 CComponentSystem* (the member is T* in the template); +0x48 SCompactPoolHandle as uint32 (0xffff as uint16 for none);
    // +0x4c int32 lock word (0 free, -1 held).
    alignas(8) std::byte raw[0x50];
};
static_assert(sizeof(CComponentHandle<CPhysicsCS>) == 0x50);

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
void NRadEngine::CComponentHandle<NRadEngine::CPhysicsCS>::RefreshCache() const {
    // The demangled name says const, but the body writes the stamp, the handle and the lock word, which the object owns.
    std::byte* const base = reinterpret_cast<std::byte*>(const_cast<NRadEngine::CComponentHandle<NRadEngine::CPhysicsCS>*>(this));
    auto* const stamp = reinterpret_cast<std::int64_t*>(base + 0x38);
    auto* const lock = reinterpret_cast<std::int32_t*>(base + 0x4c);

    // Locals first: the labels below are jumped to from inside the search, so no initialization may sit between them.
    NRadEngine::CncaGameSpace* space = nullptr;          // sp+0x08 (local_68)
    SActorGUID guid{};                                   // sp+0x10 (local_60)
    NRadEngine::CSimpleKeyLess less_closure{};           // sp+0x20
    NRadEngine::CSimpleKeySwap swap_closure{};           // sp+0x28
    SystemKey* table = nullptr;
    NRadEngine::CComponentSystem* component = nullptr;   // plVar12 (x22)
    std::uint64_t count = 0;                             // uVar8 over the table
    std::uint64_t lo = 0;                                // uVar10
    std::uint64_t hi = 0;                                // uVar8 in the search
    std::uint64_t mid = 0;                               // uVar2
    std::int64_t key = 0;                                // lVar13
    std::int64_t entry_key = 0;                          // lVar11
    bool found = false;
    std::uint32_t index = 0;                             // uVar6
    std::uint32_t flags = 0;                             // uVar9
    // x21 is not written before 0x1336980. Its high half is the caller's and is not recovered; the slot reads only the low 32
    // bits of the argument, so the high half is held at 0 here.
    std::uint64_t x21 = 0;

    // 0x13367cc-0x13367e8 and 0x1336810-0x1336820: no game pointer resets the stamp (-1) and the handle, and returns; no lock.
    if (Load<const void*>(base, 0x00) == nullptr) {
        Store<std::int64_t>(base, 0x38, -1);
        Store<std::uint64_t>(base, 0x40, 0);
        Store<std::uint16_t>(base, 0x48, 0xffff);
        return;
    }
    // 0x13367ec-0x1336800: an acquire load of the stamp equal to the game's stamp needs no refresh; return with no lock.
    if (__atomic_load_n(stamp, __ATOMIC_ACQUIRE) == GameStamp(base)) {
        return;
    }
    // 0x1336804-0x133683c: take the lock word (0 free, -1 held). The LL/SC loop (ldar, ldaxr, stlxr, clrex) is a compare-exchange here.
    for (;;) {
        while (__atomic_load_n(lock, __ATOMIC_ACQUIRE) != 0) {
        }
        std::int32_t expected = 0;
        if (__atomic_compare_exchange_n(lock, &expected, -1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            break;
        }
    }
    // 0x1336840-0x1336854: the stamp checked again with the lock held; equal releases the lock and returns.
    if (__atomic_load_n(stamp, __ATOMIC_ACQUIRE) == GameStamp(base)) {
        goto LAB_013369e8;
    }
    // 0x1336858-0x1336870: a short of -1 at +0x18 or +0x20 refreshes the handle first.
    if (Load<std::int16_t>(base, 0x18) == -1 || Load<std::int16_t>(base, 0x20) == -1) {
        RefreshActorAndComponentHandle();
    }
    // 0x1336878-0x133689c: local_60 = { this+0x08, this+0x18 }; flags bit 0 replaces it with the current actor's GUID.
    guid.symbol = NRadEngine::CSymbol64{Load<std::uint64_t>(base, 0x08)};
    guid.actor = NRadEngine::SActor{Load<std::uint64_t>(base, 0x18)};
    if ((Load<std::uint8_t>(base, 0x24) & 1U) != 0) {
        // 0x133688c-0x133689c: x1 = &guid; the returned x0:x1 is stored over guid (stp), and its x0 is GameSpace's symbol.
        guid = Load<NRadEngine::CncaGame*>(base, 0x00)->CurrentActorGUID(&guid);
    }
    // 0x13368a0-0x13368b0: the game space for the symbol; none goes to LAB_013369d0.
    space = Load<NRadEngine::CncaGame*>(base, 0x00)->GameSpace(guid.symbol);
    if (space == nullptr) {
        goto LAB_013369d0;
    }

LAB_013368c4:
    // Every pass starts a fresh search: ResolveActorAlias jumps back here, and a miss must reach LAB_013369a8.
    found = false;
    lo = 0;
    hi = 0;
    mid = 0;
    // 0x13368c4-0x13368c8: the space's table count at +0x450; zero means nothing to search.
    count = Load<std::uint64_t>(space, 0x450);
    if (count == 0) {
        goto LAB_013369a8;
    }
    // 0x13368d0: the key is this+0x10.
    key = Load<std::int64_t>(base, 0x10);
    // 0x13368cc-0x13368f8: flags bit 0 of the space (+0x458) means unsorted: sort the table, then clear the bit.
    if ((Load<std::uint8_t>(space, 0x458) & 1U) != 0) {
        NRadEngine::Sort<512ull>(Load<SystemKey*>(space, 0x420), count, less_closure, swap_closure);
        Store<std::uint32_t>(space, 0x458, Load<std::uint32_t>(space, 0x458) & 0xfffffffeU);
        count = Load<std::uint64_t>(space, 0x450);
        if (count == 0) {
            goto LAB_013369a8;
        }
    }
    // 0x13368fc-0x133693c: binary search over [0, count) for the key; the value is the CComponentSystem* of the matching entry.
    table = Load<SystemKey*>(space, 0x420);
    lo = 0;
    hi = count;
    for (;;) {
        mid = lo + ((hi - lo) >> 1);
        entry_key = static_cast<std::int64_t>(table[mid].key.bits);
        if (entry_key > key) {
            hi = mid;
        } else if (entry_key < key) {
            lo = mid + 1;
        } else {
            found = true;
            break;
        }
        if (!(lo < hi)) {
            break;
        }
    }
    if (!found) {
        goto LAB_013369a8;
    }
    component = table[mid].value;
    if (component == nullptr) {
        goto LAB_013369a8;
    }
    // 0x1336948-0x1336958: this+0x40 = the system; this+0x48 = the uint32 at this+0x20; flags at +0x24 bits 0-1 call Component.
    Store<NRadEngine::CComponentSystem*>(base, 0x40, component);
    index = Load<std::uint32_t>(base, 0x20);
    flags = Load<std::uint32_t>(base, 0x24);
    Store<std::uint32_t>(base, 0x48, index);
    if ((flags & 3U) != 0) {
        // 0x133695c-0x1336978: Component(actor, symbol at +0x30) through vslot 0x1b8; the result replaces this+0x48.
        index = VirtualSlot<ComponentFn>(component, 0x1b8)(component, guid.actor, NRadEngine::CSymbol64{Load<std::uint64_t>(base, 0x30)});
        flags = Load<std::uint32_t>(base, 0x24);
        Store<std::uint32_t>(base, 0x48, index);
    }
    // 0x133697c-0x13369a4: flags bit 1 tests IsComponentValid with x21 (its low half is the handle); false goes to LAB_013369b8.
    if (((flags >> 1) & 1U) == 0) {
        goto LAB_013369dc;
    }
    x21 = (x21 & 0xffffffff00000000ULL) | index;
    if (!VirtualSlot<IsComponentValidFn>(component, 0x1d0)(component, x21)) {
        goto LAB_013369b8;
    }
    goto LAB_013369dc;

LAB_013369a8:
    // 0x13369a8-0x13369b4: no entry (or a null system): this+0x40 = 0, this+0x48 = 0xffff; flags bit 1 gates ResolveActorAlias.
    Store<std::uint64_t>(base, 0x40, 0);
    Store<std::uint16_t>(base, 0x48, 0xffff);
    if (((Load<std::uint8_t>(base, 0x24) >> 1) & 1U) == 0) {
        goto LAB_013369dc;
    }
LAB_013369b8:
    // 0x13369b8-0x13369d0: ResolveActorAlias rewrites the space and the actor; on a match, search again in the new space.
    if (!NRadEngine::ResolveActorAlias(&space, &guid.actor)) {
        goto LAB_013369dc;
    }
    if (space != nullptr) {
        goto LAB_013368c4;
    }
LAB_013369d0:
    // 0x13369d0-0x13369d4: no space: this+0x40 = 0, this+0x48 = 0xffff.
    Store<std::uint64_t>(base, 0x40, 0);
    Store<std::uint16_t>(base, 0x48, 0xffff);
LAB_013369dc:
    // 0x13369dc-0x13369e4: this+0x38 = the game's stamp (release store).
    __atomic_store_n(stamp, GameStamp(base), __ATOMIC_RELEASE);
LAB_013369e8:
    // 0x13369e8: release the lock word (stlr wzr).
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
}
