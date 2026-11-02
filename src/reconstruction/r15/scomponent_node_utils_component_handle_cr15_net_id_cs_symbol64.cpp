// NRadEngine::SComponentNodeUtils::ComponentHandle<NRadEngine::NRadGame::CR15NetIdCS>(NRadEngine::SScriptThread const&, NRadEngine::CComponentHandle<NRadEngine::NRadGame::CR15NetIdCS>&, NRadEngine::CSymbol64, NRadEngine::CSymbol64)
// Quest libr15.so VA 0x2533c4c; ReVault binary 576.
// ReVault raw parent 100316193, reconstruction node 100600798; grade M: all 91 instructions (0x2533c4c-0x2533db4) checked against the listing, every callee has a ReVault name and the vtable call is a member call on CComponentSystem (slots 0-54 are placeholder virtuals, slot 55 is Component); M because the game-space pointer at +0x80 is a CncaGameSpace to GameSpaceMT but a CncaGameSpaceMT to Actor and Find (held as void* and cast at each use), CScriptCS::Actor is typed void in ReVault so its SScriptActorData result is inferred from the three words, and the layouts of SScriptThread and CComponentHandle are raw offsets.
// @0x2533c4c — NRadEngine::SComponentNodeUtils::ComponentHandle<NRadEngine::NRadGame::CR15NetIdCS>(NRadEngine::SScriptThread const&, NRadEngine::CComponentHandle<NRadEngine::NRadGame::CR15NetIdCS>&, NRadEngine::CSymbol64, NRadEngine::CSymbol64)
/* @confidence: M */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

// Reads or writes a T at a byte offset. ReVault has no layout for SScriptThread, the script instance state
// or CComponentHandle, so each field is accessed where the listing accesses it.
template <typename T>
T Load(const void* base, std::size_t offset) {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(base) + offset, sizeof value);
    return value;
}

template <typename T>
void Store(void* base, std::size_t offset, T value) {
    std::memcpy(static_cast<std::byte*>(base) + offset, &value, sizeof value);
}

// The script state record: thread[+0x00] -> +0x30. The listing loads it at 0x2533ca4 (before the vtable call)
// and again at 0x2533d10 and 0x2533d4c.
const void* ScriptStateOf(const void* thread) {
    const void* const script = Load<const void*>(thread, 0x00);
    return Load<const void*>(script, 0x30);
}

// The game-space pointer: state[+0x00] -> +0x80, the system's game space. The listing repeats the chain at
// 0x2533d10..0x2533d24 and 0x2533d4c..0x2533d6c.
void* GameSpaceOf(const void* thread) {
    const void* const system = Load<const void*>(ScriptStateOf(thread), 0x00);
    return Load<void*>(system, 0x80);
}

}  // namespace

namespace NRadEngine {

struct CncaGameSpace;
struct CncaGameSpaceMT;
class CComponentSystem;
struct SScriptThread;

// One 64-bit word each; ReVault has no layout for CSymbol64 or SActor.
struct CSymbol64 {
    std::uint64_t bits;
};
static_assert(sizeof(CSymbol64) == 0x08);

struct SActor {
    std::uint64_t bits;
};
static_assert(sizeof(SActor) == 0x08);

// CComponentSystem vtable (0x3738298 prefix; CScriptCS shares slots 0-58). Slots 0-54 are not called here and keep
// placeholder names; slot 55 (+0x1b8) is Component (revault_vtable: CScriptCS index 55, CComponentSystem::Component 0x14e74d8).
class CComponentSystem {
public:
    virtual void Slot00();
    virtual void Slot01();
    virtual void Slot02();
    virtual void Slot03();
    virtual void Slot04();
    virtual void Slot05();
    virtual void Slot06();
    virtual void Slot07();
    virtual void Slot08();
    virtual void Slot09();
    virtual void Slot10();
    virtual void Slot11();
    virtual void Slot12();
    virtual void Slot13();
    virtual void Slot14();
    virtual void Slot15();
    virtual void Slot16();
    virtual void Slot17();
    virtual void Slot18();
    virtual void Slot19();
    virtual void Slot20();
    virtual void Slot21();
    virtual void Slot22();
    virtual void Slot23();
    virtual void Slot24();
    virtual void Slot25();
    virtual void Slot26();
    virtual void Slot27();
    virtual void Slot28();
    virtual void Slot29();
    virtual void Slot30();
    virtual void Slot31();
    virtual void Slot32();
    virtual void Slot33();
    virtual void Slot34();
    virtual void Slot35();
    virtual void Slot36();
    virtual void Slot37();
    virtual void Slot38();
    virtual void Slot39();
    virtual void Slot40();
    virtual void Slot41();
    virtual void Slot42();
    virtual void Slot43();
    virtual void Slot44();
    virtual void Slot45();
    virtual void Slot46();
    virtual void Slot47();
    virtual void Slot48();
    virtual void Slot49();
    virtual void Slot50();
    virtual void Slot51();
    virtual void Slot52();
    virtual void Slot53();
    virtual void Slot54();
    virtual std::uint32_t Component(SActor actor, CSymbol64 symbol) const;  // slot 55, +0x1b8
};

// A 32-bit pool handle as in the merged tree: Component returns its index in w0, SetT takes it in w2,
// CScriptCS::Actor in w1.
struct SCompactPoolHandle {
    std::uint32_t index;
};
static_assert(sizeof(SCompactPoolHandle) == 4);

// Three quadwords as in the merged tree and the CModelCS instantiation: word 0 (+0x00) is the key GameSpaceMT
// takes, word 1 (+0x08) the SActor Find takes, word 2 (+0x10) the CSymbol64 name. CScriptCS::Actor returns it
// through x8 (0x2533cf4 mov x8, sp), and its failure path (CScriptInstance::Actor, 0x26f7458) writes -1,
// 0x0000ffffffffffff, -1 into the same three words.
struct SScriptActorData {
    alignas(8) std::byte raw[0x18];
};
static_assert(sizeof(SScriptActorData) == 0x18);

// SetT takes the nested T::MT*, so each component type carries a forward-declared MT.
class CScriptCS {
public:
    struct MT;

    // CScriptCS::Actor(SCompactPoolHandle, CSymbol64) const, 0x178f0dc (thunk 0x100c770).
    SScriptActorData Actor(SCompactPoolHandle handle, CSymbol64 name) const;
};

namespace NRadGame {

class CR15NetIdCS {
public:
    struct MT;
};

}  // namespace NRadGame

// The 16-byte local the function builds at sp+0x68 and passes to Find as `this`. Find stores a
// CComponentSystem* at +0x00 and the handle at +0x08 (clocal_component_handle_cr15_net_customization_cs_find.cpp).
template <class T>
class CLocalComponentHandle {
public:
    // CLocalComponentHandle<CR15NetIdCS>::Find(CncaGameSpaceMT*, SActor, CSymbol64), 0x252e61c (thunk 0x1049770).
    void Find(CncaGameSpaceMT* game_space, SActor actor, CSymbol64 symbol);

    CComponentSystem* component;       // +0x00
    SCompactPoolHandle packed_handle;  // +0x08
    std::uint32_t reserved;            // +0x0c: zeroed at 0x2533c68/0x2533c6c, never read here
};
static_assert(sizeof(CLocalComponentHandle<NRadGame::CR15NetIdCS>) == 0x10);
static_assert(offsetof(CLocalComponentHandle<NRadGame::CR15NetIdCS>, packed_handle) == 0x08);

// CComponentHandle<T> is 0x50 bytes (static_assert in cr15_net_damageable_heal_node_enter.cpp); the component
// pointer sits at +0x40 and the pool handle at +0x48. Both the CScriptCS and the CR15NetIdCS instantiation
// appear here.
template <class T>
class CComponentHandle {
public:
    // CComponentHandle<CScriptCS>::SetT<CScriptCS>(CScriptCS::MT*, SCompactPoolHandle), 0x17f2d60 (thunk 0x1023c50);
    // CComponentHandle<CR15NetIdCS>::SetT<CR15NetIdCS>(CR15NetIdCS::MT*, SCompactPoolHandle), 0x2533db8 (thunk 0x1044a30).
    template <class U>
    void SetT(typename U::MT* component, SCompactPoolHandle handle);

    // CComponentHandle<CScriptCS>::RefreshCache() const, 0x1728bfc (thunk 0x103fa10).
    void RefreshCache() const;

    alignas(8) std::byte raw[0x50];
};
static_assert(sizeof(CComponentHandle<CScriptCS>) == 0x50);
static_assert(sizeof(CComponentHandle<NRadGame::CR15NetIdCS>) == 0x50);

struct CncaGameSpace {
    // CncaGameSpace::GameSpaceMT(CSymbol64) const, 0x1906704 (thunk 0xff5650).
    CncaGameSpaceMT* GameSpaceMT(CSymbol64 key) const;
};

namespace SComponentNodeUtils {

// SComponentNodeUtils::Actor(SScriptThread const&, CncaGameSpaceMT const&, CSymbol64, CncaGameSpaceMT*&,
// SActor&, unsigned int&), 0x2159714 (thunk 0x1015330). The caller tests w0 with cbz, so the result is an int.
int Actor(const SScriptThread& thread, const CncaGameSpaceMT& space, CSymbol64 key,
          CncaGameSpaceMT*& out_space, SActor& out_actor, std::uint32_t& out_flags);

template <class T>
void ComponentHandle(const SScriptThread& thread, CComponentHandle<T>& handle, CSymbol64 actor_name,
                     CSymbol64 component_name);

// 0x2533c4c..0x2533db4, the CR15NetIdCS instantiation of the script-node overload: the actor comes from the
// script component's own actor data (CScriptCS::Actor) instead of from a caller-supplied SScriptActorData.
template <>
void ComponentHandle<NRadGame::CR15NetIdCS>(const SScriptThread& thread,
                                            CComponentHandle<NRadGame::CR15NetIdCS>& handle,
                                            CSymbol64 actor_name, CSymbol64 component_name) {
    constexpr std::uint64_t kNone = ~std::uint64_t{0};  // the -1 the listing compares with (cmn #1)

    // 0x2533c68..0x2533c90: the locals take their initial values.
    // sp+0x68..0x77: the local component handle, component null, handle 0xffff, the other six bytes zero.
    CLocalComponentHandle<NRadGame::CR15NetIdCS> found{nullptr, SCompactPoolHandle{0xffffu}, 0u};
    // sp+0x78: the SActor Actor will fill, 0x0000ffffffffffff.
    SActor out_actor{0x0000ffffffffffffULL};
    CncaGameSpaceMT* out_space;   // sp+0x80: written by Actor, or by the GameSpaceMT path below
    std::uint32_t out_flags;      // sp+0x8c: written only by Actor
    // sp+0x18..0x67: a CComponentHandle<CScriptCS>, the inlined constructor's stores (offsets from sp+0x18):
    // +0x00 = 0, +0x08 = -1, +0x10 = -1, +0x16 = -1 (eight bytes), +0x1e = 0 (u16), +0x20 = 0xffff,
    // +0x28/+0x30/+0x38 = -1, +0x48 = 0xffff; +0x40 (the component pointer) is not written here.
    CComponentHandle<CScriptCS> script;
    Store<std::uint64_t>(&script, 0x00, 0);
    Store<std::uint64_t>(&script, 0x08, kNone);
    Store<std::uint64_t>(&script, 0x10, kNone);
    Store<std::uint64_t>(&script, 0x16, kNone);
    Store<std::uint16_t>(&script, 0x1e, 0);
    Store<std::uint64_t>(&script, 0x20, 0xffffu);
    Store<std::uint64_t>(&script, 0x28, kNone);
    Store<std::uint64_t>(&script, 0x30, kNone);
    Store<std::uint64_t>(&script, 0x38, kNone);
    Store<std::uint64_t>(&script, 0x48, 0xffffu);

    // 0x2533c94..0x2533cc4: the state record holds {CComponentSystem*, SActor, CSymbol64}; the virtual call at
    // vtable slot 0x1b8 (index 55, CComponentSystem::Component(SActor, CSymbol64) const, 0x14e74d8) returns the
    // pool handle of that actor's script component.
    const void* const state = ScriptStateOf(&thread);
    CComponentSystem* const script_system = Load<CComponentSystem*>(state, 0x00);
    const SCompactPoolHandle script_handle{script_system->Component(
        SActor{Load<std::uint64_t>(state, 0x08)},
        CSymbol64{Load<std::uint64_t>(state, 0x10)})};  // 0x2533cc4 blr x9; 0x2533cc8 and x2, x0, #0xffffffff

    // 0x2533cd4: str wzr, [sp, #0x3c] clears the high half of the 0xffff word at +0x20 (local_88 & 0xffffffff).
    Store<std::uint32_t>(&script, 0x24, 0u);
    // 0x2533cd8: SetT<CScriptCS> on the local with the system and the handle.
    script.SetT<CScriptCS>(reinterpret_cast<CScriptCS::MT*>(script_system), script_handle);

    // 0x2533ce0..0x2533cf0: RefreshCache, read the component (+0x40), RefreshCache again, read the handle (+0x48).
    script.RefreshCache();
    const CScriptCS* const script_component = Load<const CScriptCS*>(&script, 0x40);
    script.RefreshCache();

    // 0x2533cf4..0x2533d00: CScriptCS::Actor(handle, actor_name) fills the three words at sp+0x00.
    const SScriptActorData script_actor =
        script_component->Actor(SCompactPoolHandle{Load<std::uint32_t>(&script, 0x48)}, actor_name);

    if (Load<std::uint64_t>(&script_actor, 0x10) == kNone) {  // 0x2533d04..0x2533d0c: word 2 is -1 -> 0x2533d4c
        // 0x2533d4c..0x2533d74: no name in the actor data, so ask Actor with actor_name. Its result is tested with cbz.
        if (Actor(thread, *static_cast<const CncaGameSpaceMT*>(GameSpaceOf(&thread)), actor_name, out_space,
                  out_actor, out_flags) != 0) {
            // 0x2533d78..0x2533d84: Find(space, the SActor Actor found, component_name).
            found.Find(out_space, out_actor, component_name);
        }
    } else {
        // 0x2533d10..0x2533d48: the game-space pointer, replaced by GameSpaceMT(word 0) unless word 0 is -1. The
        // decompilation drops that key; the listing passes x1, loaded from word 0 at 0x2533d14.
        void* game_space = GameSpaceOf(&thread);
        const std::uint64_t key = Load<std::uint64_t>(&script_actor, 0x00);
        if (key != kNone) {
            game_space = static_cast<const CncaGameSpace*>(game_space)->GameSpaceMT(CSymbol64{key});  // 0x2533d30
        }
        out_space = static_cast<CncaGameSpaceMT*>(game_space);  // 0x2533d3c str x8, [sp, #0x80]
        // 0x2533d38, 0x2533d80..0x2533d84: Find(space, word 1, component_name).
        found.Find(out_space, SActor{Load<std::uint64_t>(&script_actor, 0x08)}, component_name);
    }

    // 0x2533d88..0x2533da0: clear bit 1 of the flags at handle+0x24, then SetT with the local's two fields.
    Store<std::uint32_t>(&handle, 0x24, Load<std::uint32_t>(&handle, 0x24) & 0xfffffffdu);
    handle.SetT<NRadGame::CR15NetIdCS>(reinterpret_cast<NRadGame::CR15NetIdCS::MT*>(found.component),
                                       found.packed_handle);
}

}  // namespace SComponentNodeUtils

}  // namespace NRadEngine
