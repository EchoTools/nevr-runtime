// NRadEngine::SComponentNodeUtils::Actor
// Quest libr15.so VA 0x2159714; ReVault binary 576.
// ReVault raw parent 100299922, reconstruction node 100600805; grade M: all 136 instructions (0x2159714-0x2159930, ret at 0x2159928) checked against the listing; placeholders: DAT_02d4ceb8 (VA 0x2d4ceb8) and CComponentSystem slots 0-54 (Slot00..Slot54, uncalled here); SScriptThread, CncaGameSpaceMT and the script record at (*thread)+0x30 are raw offsets.
// @0x2159714 — NRadEngine::SComponentNodeUtils::Actor
/* @confidence: M */

#include <cstddef>
#include <cstdint>
#include <cstring>

// DAT_02d4ceb8: the read-only 8-byte symbol constant at VA 0x2d4ceb8 (file bytes b6 12 76 29 48 3e c3 c8, that is
// 0xc8c33e48297612b6; the decompilation prints it as -0x373cc1b7d689ed4a). ReVault gives it no name; it is the symbol "Self" (entry {0xc8c33e48297612b6ULL, "Self"} in src/runtime/log/symcache_data.cpp on origin/main). The listing forms the
// address as adrp 0x2d4c000, add #0xebb, and #-8 (0x2159760-0x2159764) and loads the qword (0x2159768).
extern const std::uint64_t DAT_02d4ceb8;

namespace NRadEngine {

class CncaGameSpaceMT;
class CScriptCS;
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

// A 32-bit pool handle (as in the merged tree): SetT takes the low half of x2, CScriptCS::Actor takes w1.
struct SCompactPoolHandle {
    std::uint32_t index;
};
static_assert(sizeof(SCompactPoolHandle) == 4);

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

// CScriptCS::Actor writes three quadwords through x8: word 0 (+0x00) the game-space key, word 1 (+0x08) the SActor,
// word 2 (+0x10) the CSymbol64. ReVault does not state the return type; the triple is the SScriptActorData that
// SComponentNodeUtils::ComponentHandle takes (size 0x18 as in the merged tree).
struct SScriptActorData {
    alignas(8) std::byte raw[0x18];
};
static_assert(sizeof(SScriptActorData) == 0x18);

class CScriptCS : public CComponentSystem {
public:
    // SetT takes the nested CScriptCS::MT*; ReVault has no layout for it, so it is forward-declared here.
    struct MT;

    // CScriptCS::Actor(SCompactPoolHandle, CSymbol64) const, 0x178f0dc (thunk 0x100c770).
    SScriptActorData Actor(SCompactPoolHandle handle, CSymbol64 symbol) const;
};

// CComponentHandle<T> is 0x50 bytes (static_assert in the merged cr15_net_damageable_heal_node_enter.cpp). The
// function builds one on its stack and reaches its fields at raw offsets.
template <class T>
class CComponentHandle {
public:
    // CComponentHandle<CScriptCS>::SetT<CScriptCS>(CScriptCS::MT*, SCompactPoolHandle), 0x17f2d60 (thunk 0x1023c50).
    template <class U>
    void SetT(typename U::MT* component, SCompactPoolHandle handle);

    // CComponentHandle<CScriptCS>::RefreshCache() const, 0x1728bfc (thunk 0x103fa10).
    void RefreshCache() const;

    alignas(8) std::byte raw[0x50];
};
static_assert(sizeof(CComponentHandle<CScriptCS>) == 0x50);

class CncaGameSpace {
public:
    // CncaGameSpace::GameSpaceMT(CSymbol64) const, 0x1906704 (thunk 0xff5650).
    CncaGameSpaceMT* GameSpaceMT(CSymbol64 key) const;

    // CncaGameSpace::ActorMT(CSymbol64, CncaGameSpaceMT*&, SActor&) const, 0x1904354 (thunk 0x1033950). The caller
    // tests w0 with cbz/cbnz, so the result is an int.
    int ActorMT(CSymbol64 key, CncaGameSpaceMT*& out_space, SActor& out_actor) const;

    // CncaGameSpace::PooledActorMT(CSymbol64, CncaGameSpaceMT*&, SActor&) const, 0x19043f0 (thunk 0x101c1c0).
    int PooledActorMT(CSymbol64 key, CncaGameSpaceMT*& out_space, SActor& out_actor) const;
};

namespace SComponentNodeUtils {

// SComponentNodeUtils::Actor(SScriptThread const&, CncaGameSpaceMT const&, CSymbol64, CncaGameSpaceMT*&, SActor&,
// unsigned int&), 0x2159714 (thunk 0x1015330). The merged callers test w0 with cbz, so the result is an int.
int Actor(const SScriptThread& thread, const CncaGameSpaceMT& space, CSymbol64 key, CncaGameSpaceMT*& out_space,
          SActor& out_actor, std::uint32_t& out_flags);

}  // namespace SComponentNodeUtils

}  // namespace NRadEngine

namespace {

// Reads or writes a T at a byte offset. ReVault has no layout for SScriptThread, the script record, CncaGameSpaceMT or
// CComponentHandle, so each field is accessed where the listing accesses it.
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

}  // namespace

namespace NRadEngine::SComponentNodeUtils {

// @0x2159714 — NRadEngine::SComponentNodeUtils::Actor
/* @confidence: M */
int Actor(const SScriptThread& thread, const CncaGameSpaceMT& space, CSymbol64 key, CncaGameSpaceMT*& out_space,
          SActor& out_actor, std::uint32_t& out_flags) {
    constexpr std::uint64_t kNone = ~std::uint64_t{0};  // the -1 the listing compares with (cmn #1)

    // 0x2159730..0x2159754: record = (*thread)[+0x30], the script's own record (x10). Its first quadword is the
    // CScriptCS*, its halfwords at +0x08/+0x0a/+0x0c/+0x0e (w28, w27, w26, w9) are the four 16-bit words of the
    // script's own SActor, its quadword at +0x10 is a CSymbol64.
    const void* const record = Load<const void*>(Load<const void*>(&thread, 0x00), 0x30);
    const std::uint16_t own_0 = Load<std::uint16_t>(record, 0x08);
    const std::uint16_t own_1 = Load<std::uint16_t>(record, 0x0a);
    const std::uint16_t own_2 = Load<std::uint16_t>(record, 0x0c);
    const std::uint16_t own_3 = Load<std::uint16_t>(record, 0x0e);

    // 0x2159744..0x2159774: a key of -1, or the constant DAT_02d4ceb8, means the script's own actor.
    if (key.bits == kNone || key.bits == DAT_02d4ceb8) {
        // 0x2159778..0x21597a8: space = record[0][+0x80], the actor = the four words, flags = (word 1 != 0xffff).
        // ReVault lists a callee FUN_020597c8 for the branch at 0x21597a8; that instruction is `b 0x215990c`, a jump to
        // the epilogue, not a call.
        out_space = Load<CncaGameSpaceMT*>(Load<const void*>(record, 0x00), 0x80);
        Store<std::uint16_t>(&out_actor, 0x00, own_0);
        Store<std::uint16_t>(&out_actor, 0x02, own_1);
        Store<std::uint16_t>(&out_actor, 0x04, own_2);
        Store<std::uint16_t>(&out_actor, 0x06, own_3);
        out_flags = own_1 != 0xffffu ? 1u : 0u;
        return 1;
    }

    // 0x21597ac..0x21597cc: the CComponentHandle<CScriptCS> local (sp+0x20, 0x50 bytes) takes its initial values;
    // +0x40 (the cached CScriptCS*) is filled by SetT.
    CComponentHandle<CScriptCS> script;
    Store<std::uint16_t>(&script, 0x1e, 0);
    Store<std::uint64_t>(&script, 0x10, kNone);
    Store<std::uint64_t>(&script, 0x00, 0);
    Store<std::uint64_t>(&script, 0x08, kNone);
    Store<std::uint64_t>(&script, 0x16, kNone);
    Store<std::uint64_t>(&script, 0x20, 0xffffu);
    Store<std::uint64_t>(&script, 0x28, kNone);
    Store<std::uint64_t>(&script, 0x30, kNone);
    Store<std::uint64_t>(&script, 0x38, kNone);
    Store<std::uint64_t>(&script, 0x48, 0xffffu);

    // 0x21597d0..0x21597f4: CScriptCS* x25 = record[0]; its vtable slot +0x1b8 is Component(SActor, CSymbol64) const
    // (slot 55; the CScriptCS override at 0x17b6274 calls slot 54 and masks to 32 bits, CComponentSystem::Component is
    // 0x14e74d8), called with the record's quadwords at +0x08 and +0x10. The result is a 32-bit pool handle.
    CScriptCS* const script_system = Load<CScriptCS*>(record, 0x00);
    const SActor own_actor{Load<std::uint64_t>(record, 0x08)};
    const CSymbol64 own_symbol{Load<std::uint64_t>(record, 0x10)};
    const SCompactPoolHandle component_handle{script_system->Component(own_actor, own_symbol)};

    // 0x21597f8..0x2159818: clear the u32 at +0x24, SetT, RefreshCache, read the cached CScriptCS* at +0x40, RefreshCache.
    Store<std::uint32_t>(&script, 0x24, 0);
    script.SetT<CScriptCS>(reinterpret_cast<CScriptCS::MT*>(script_system), component_handle);
    script.RefreshCache();
    CScriptCS* const component = Load<CScriptCS*>(&script, 0x40);
    script.RefreshCache();

    // 0x215981c..0x215982c: CScriptCS::Actor(handle index at +0x48, key) fills the three-quadword result at sp+0x08.
    const SCompactPoolHandle handle_index{Load<std::uint32_t>(&script, 0x48)};
    const SScriptActorData data = component->Actor(handle_index, key);
    const std::uint64_t data_key = Load<std::uint64_t>(&data, 0x00);    // [sp+0x08]
    const std::uint64_t data_actor = Load<std::uint64_t>(&data, 0x08);  // [sp+0x10]
    const std::uint64_t data_name = Load<std::uint64_t>(&data, 0x10);   // [sp+0x18]

    // 0x2159830..0x2159838: a name of -1 skips straight to the lookup by the caller's key (0x21598b4).
    bool pooled_by_name = false;
    if (data_name != kNone) {
        // 0x2159840..0x2159850: the stored key and actor are both usable.
        if (data_key != kNone && static_cast<std::int16_t>(data_actor) != -1) {
            // 0x2159854..0x2159884: the game-space pointer is re-read as thread[0][+0x30][0][+0x80], then GameSpaceMT on
            // the stored key. The actor is copied whole; flags = (the high half of its low 32 bits is not all ones).
            const void* const game_space =
                Load<const void*>(Load<const void*>(Load<const void*>(Load<const void*>(&thread, 0x00), 0x30), 0x00), 0x80);
            out_space = static_cast<const CncaGameSpace*>(game_space)->GameSpaceMT(CSymbol64{data_key});
            out_actor.bits = data_actor;
            out_flags = (~static_cast<std::uint32_t>(data_actor) & 0xffff0000u) != 0 ? 1u : 0u;
            return 1;
        }

        // 0x2159888..0x21598b0: ActorMT then PooledActorMT on the stored name; ActorMT success returns with flags 0.
        const CncaGameSpace* const game_spaces = reinterpret_cast<const CncaGameSpace*>(&space);
        if (game_spaces->ActorMT(CSymbol64{data_name}, out_space, out_actor) != 0) {  // 0x2159898 cbnz -> 0x21598cc
            out_flags = 0;
            return 1;
        }
        pooled_by_name = game_spaces->PooledActorMT(CSymbol64{data_name}, out_space, out_actor) != 0;  // 0x21598ac
    }

    if (!pooled_by_name) {
        // 0x21598b4..0x21598e8: the same two lookups on the caller's key.
        const CncaGameSpace* const game_spaces = reinterpret_cast<const CncaGameSpace*>(&space);
        if (game_spaces->ActorMT(key, out_space, out_actor) != 0) {  // 0x21598c8 cbz -> 0x21598d4, else 0x21598cc
            out_flags = 0;
            return 1;
        }
        if (game_spaces->PooledActorMT(key, out_space, out_actor) == 0) {  // 0x21598e8 cbz -> 0x215992c
            out_flags = 0;  // 0x215992c..0x2159908: w0 stays 0 from PooledActorMT
            return 0;
        }
    }

    // 0x21598ec..0x2159900: when the found actor's word 0 equals the script's own word 0, words 1 and 2 are the script's.
    if (Load<std::uint16_t>(&out_actor, 0x00) == own_0) {
        Store<std::uint16_t>(&out_actor, 0x02, own_1);
        Store<std::uint16_t>(&out_actor, 0x04, own_2);
    }
    out_flags = 1;  // 0x2159900: mov w8, #1, stored at 0x2159908
    return 1;       // 0x2159904: mov w0, #1
}

}  // namespace NRadEngine::SComponentNodeUtils
