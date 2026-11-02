// NRadEngine::SComponentNodeUtils::ComponentHandle<NRadEngine::CModelCS>(NRadEngine::SScriptThread const&, NRadEngine::CComponentHandle<NRadEngine::CModelCS>&, NRadEngine::SScriptActorData, NRadEngine::CSymbol64)
// Quest libr15.so VA 0x273a944; ReVault binary 576.
// ReVault raw parent 100325382, reconstruction node 100600784; grade M: all 87 instructions (0x273a944-0x273aa9c) checked against the listing, 0 placeholders (every callee has a ReVault name); M because the game-space pointer at +0x80 is a CncaGameSpace to GameSpaceMT but a CncaGameSpaceMT to Actor and Find (held as void* and cast at each use), and the layouts of SScriptActorData, SScriptThread and CComponentHandle are raw offsets.
// @0x273a944 — NRadEngine::SComponentNodeUtils::ComponentHandle<NRadEngine::CModelCS>(NRadEngine::SScriptThread const&, NRadEngine::CComponentHandle<NRadEngine::CModelCS>&, NRadEngine::SScriptActorData, NRadEngine::CSymbol64)
/* @confidence: M */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

// Reads or writes a T at a byte offset. ReVault has no layout for SScriptThread, SScriptActorData,
// CncaGameSpaceMT or CComponentHandle, so each field is accessed where the listing accesses it.
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

// The game-space pointer: four loads, thread[+0x00] -> +0x30 -> +0x00 -> +0x80. The listing repeats
// them at 0x273a99c, 0x273a9d0 and 0x273aa34.
void* GameSpaceOf(const void* thread) {
    const void* const script = Load<const void*>(thread, 0x00);
    const void* const script_next = Load<const void*>(script, 0x30);
    const void* const script_last = Load<const void*>(script_next, 0x00);
    return Load<void*>(script_last, 0x80);
}

// The tail the two lookups share (0x273aa28..0x273aa30): table = *(*(space + 0x370) + 0x80), then the
// entry at ((uint32_t)actor << 3) & 0x7fff8 bytes. Both callers store the result in word 2 of the actor data.
std::uint64_t ActorNameOf(const void* space, std::uint64_t actor_bits) {
    const void* const table = Load<const void*>(Load<const void*>(space, 0x370), 0x80);
    const std::uint32_t scaled = static_cast<std::uint32_t>(actor_bits) << 3;
    return Load<std::uint64_t>(table, static_cast<std::size_t>(scaled & 0x7fff8u));
}

}  // namespace

namespace NRadEngine {

class CncaGameSpace;
class CncaGameSpaceMT;
class CComponentSystem;
struct SScriptThread;

// SetT takes the nested CModelCS::MT*, so CModelCS carries a forward-declared MT.
class CModelCS {
public:
    struct MT;
};

// One 64-bit word each; ReVault has no layout for CSymbol64 or SActor.
struct CSymbol64 {
    std::uint64_t bits;
};
static_assert(sizeof(CSymbol64) == 0x08);

struct SActor {
    std::uint64_t bits;
};
static_assert(sizeof(SActor) == 0x08);

// A 32-bit pool handle: SetT reads it as the low half of x2, Find stores it with `str w0`.
using SCompactPoolHandle = std::uint32_t;

// SScriptActorData is passed as a pointer to the caller's copy of three quadwords, which this function
// rewrites in place. Word 0 (+0x00) is the key GameSpaceMT takes, word 1 (+0x08) the SActor Find takes,
// word 2 (+0x10) the CSymbol64 Actor takes. Size 0x18 as in the merged tree (std::array<std::uint64_t, 3>).
struct SScriptActorData {
    alignas(8) std::byte raw[0x18];
};
static_assert(sizeof(SScriptActorData) == 0x18);

// The 16-byte local the function builds at sp+0x00 (the next local starts at sp+0x10) and passes to Find
// as `this`. Component and packed handle sit where clocal_component_handle_cr15_net_customization_cs_find.cpp
// puts them; Find stores a CComponentSystem* at +0x00 and the handle at +0x08.
template <class T>
class CLocalComponentHandle {
public:
    // CLocalComponentHandle<CModelCS>::Find(CncaGameSpaceMT*, SActor, CSymbol64), 0x17327c4 (thunk 0x1038780).
    void Find(CncaGameSpaceMT* game_space, SActor actor, CSymbol64 symbol);

    CComponentSystem* component;       // +0x00
    SCompactPoolHandle packed_handle;  // +0x08
    std::uint32_t reserved;            // +0x0c: zeroed with the rest at 0x273a95c/0x273a964, never read here
};
static_assert(sizeof(CLocalComponentHandle<CModelCS>) == 0x10);
static_assert(offsetof(CLocalComponentHandle<CModelCS>, packed_handle) == 0x08);

// CComponentHandle<T> is 0x50 bytes (static_assert in cr15_net_damageable_heal_node_enter.cpp). This
// function touches only the u32 at +0x24, whose bit 1 it clears before SetT.
template <class T>
class CComponentHandle {
public:
    // CComponentHandle<CModelCS>::SetT<CModelCS>(CModelCS::MT*, SCompactPoolHandle), 0x1313a80 (thunk 0x104a960).
    template <class U>
    void SetT(typename U::MT* component, SCompactPoolHandle handle);

    alignas(8) std::byte raw[0x50];
};
static_assert(sizeof(CComponentHandle<CModelCS>) == 0x50);

class CncaGameSpace {
public:
    // CncaGameSpace::GameSpaceMT(CSymbol64) const, 0x1906704 (thunk 0xff5650).
    CncaGameSpaceMT* GameSpaceMT(CSymbol64 key) const;
};

namespace SComponentNodeUtils {

// SComponentNodeUtils::Actor(SScriptThread const&, CncaGameSpaceMT const&, CSymbol64, CncaGameSpaceMT*&,
// SActor&, unsigned int&), 0x2159714 (thunk 0x1015330). The caller tests w0 with cbz, so the result is an int.
int Actor(const SScriptThread& thread, const CncaGameSpaceMT& space, CSymbol64 key,
          CncaGameSpaceMT*& out_space, SActor& out_actor, std::uint32_t& out_flags);

template <class T>
void ComponentHandle(const SScriptThread& thread, CComponentHandle<T>& handle, SScriptActorData actor,
                     CSymbol64 name);

// 0x273a944..0x273aa9c, the CModelCS instantiation.
template <>
void ComponentHandle<CModelCS>(const SScriptThread& thread, CComponentHandle<CModelCS>& handle,
                               SScriptActorData actor, CSymbol64 name) {
    constexpr std::uint64_t kNone = ~std::uint64_t{0};  // the -1 the listing compares with (cmn #1)

    // 0x273a954..0x273a964: component null, handle 0xffff, the other six bytes zero.
    CLocalComponentHandle<CModelCS> found{nullptr, 0xffffu, 0u};

    // 0x273a97c..0x273a98c: word 0 is -1, or the low 16 bits of word 1 are 0xffff (ldrsh, cmn #1).
    if (Load<std::uint64_t>(&actor, 0x00) == kNone || Load<std::int16_t>(&actor, 0x08) == -1) {
        // 0x273a9c8..0x273a9f4: ask Actor. Its out SActor starts at 0x0000ffffffffffff (sp+0x10); the
        // space (sp+0x18) and the flags (sp+0x2c) are written only by Actor.
        SActor actor_out{0x0000ffffffffffffULL};
        CncaGameSpaceMT* actor_space;
        std::uint32_t actor_flags;
        if (Actor(thread, *static_cast<const CncaGameSpaceMT*>(GameSpaceOf(&thread)),
                  CSymbol64{Load<std::uint64_t>(&actor, 0x10)}, actor_space, actor_out, actor_flags) == 0) {
            goto set_handle;  // 0x273a9f8: cbz w0 -> 0x273aa70, Find is skipped
        }
        // 0x273a9fc..0x273aa18: word 0 = space[+0x58], word 1 = the SActor Actor found.
        Store<std::uint64_t>(&actor, 0x00, Load<std::uint64_t>(actor_space, 0x58));
        Store<std::uint64_t>(&actor, 0x08, actor_out.bits);
        if (Load<std::uint64_t>(&actor, 0x10) == kNone) {
            // 0x273aa1c..0x273aa30
            Store<std::uint64_t>(&actor, 0x10, ActorNameOf(actor_space, actor_out.bits));
        }
    } else if (Load<std::uint64_t>(&actor, 0x10) == kNone) {
        // 0x273a99c..0x273a9c4: GameSpaceMT on the game-space pointer with word 0 as the key. The
        // decompilation drops that second argument; the listing passes x1, loaded from word 0 at 0x273a96c.
        CncaGameSpaceMT* const space = static_cast<const CncaGameSpace*>(GameSpaceOf(&thread))
                                           ->GameSpaceMT(CSymbol64{Load<std::uint64_t>(&actor, 0x00)});
        if (space != nullptr) {
            // 0x273a9b4..0x273a9c4, then 0x273aa28..0x273aa30; the index is the low half of word 1.
            Store<std::uint64_t>(&actor, 0x10, ActorNameOf(space, Load<std::uint64_t>(&actor, 0x08)));
        }
    }

    {
        // 0x273aa34..0x273aa6c: the game-space pointer, replaced by GameSpaceMT(word 0) unless word 0 is -1,
        // then Find fills the local from it, word 1 and the name.
        void* game_space = GameSpaceOf(&thread);
        const std::uint64_t key = Load<std::uint64_t>(&actor, 0x00);
        if (key != kNone) {
            game_space = static_cast<const CncaGameSpace*>(game_space)->GameSpaceMT(CSymbol64{key});
        }
        found.Find(static_cast<CncaGameSpaceMT*>(game_space), SActor{Load<std::uint64_t>(&actor, 0x08)}, name);
    }

set_handle:
    // 0x273aa70..0x273aa88: clear bit 1 of the flags at handle+0x24, then SetT with the local's two fields.
    Store<std::uint32_t>(&handle, 0x24, Load<std::uint32_t>(&handle, 0x24) & 0xfffffffdu);
    handle.SetT<CModelCS>(reinterpret_cast<CModelCS::MT*>(found.component), found.packed_handle);
}

}  // namespace SComponentNodeUtils

}  // namespace NRadEngine
