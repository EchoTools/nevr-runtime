// CR15NetBoosterCS::AllocateExclusive
// Quest libr15.so VA 0x2210dd4; ReVault binary 576.
// ReVault raw parent 100302427, reconstruction node 100600468; grade M: control flow checked branch by branch against the 404 instructions; 21 thunk targets named; vtable slots +0x1d0, +0x1e0, +0x1f0, +0x300 and six globals read by address are unnamed or not in the file.
// @0x2210dd4 — CR15NetBoosterCS::AllocateExclusive
/* @confidence: M */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine::NRadGame {

class CR15NetBoosterCS {
public:
    // Raw layout used here, none of it recovered as fields: +0x80 game space (CncaGameSpace*),
    // +0xb8 netgame, +0xd0 handle table (u32 entries: low half a record index, high half a generation),
    // +0xe0 u16 handle index per slot, +0xfc u16 slot count, +0x100 slot array, +0x138 u32.
    // Slot (0x68 bytes): +0x18 cached component object and +0x20 its handle (u32, low half read as an index),
    // +0x48 cached component object and +0x50 its handle, +0x58 actor id and +0x60 actor generation.
    void AllocateExclusive();
};

}  // namespace NRadEngine::NRadGame

// Callees, each under the ReVault name of the function its thunk reaches. The target VA and the thunk
// are in each comment. ReVault's callee list carries no class qualifier. Some names are shared by
// several instances; those are told apart by target VA.

// Two-word return value (x0, x1): an actor id and its generation.
struct AllocatedActor {
    std::uint64_t id;
    std::uint64_t generation;
};

// Out-parameter of the Find thunks: a 0x50-byte component-handle result. Layout not recovered; used offsets are
// 0x00 (first word; the caller's presence test is non-zero), 0x40 (component object), 0x48 (handle, u32).
struct FoundRef {
    alignas(8) std::byte raw[0x50];
};

// NRadEngine::CncaGameSpace::SendComponentEventGlobal @0x0190735c (thunk 0x0104a800).
void SendComponentEventGlobal(std::byte* game_space, std::uint64_t event, std::uint64_t id,
                              std::uint64_t generation, std::uint64_t flag);
// Append @0x012ccb34 (thunk 0x01031ea0).
std::uint64_t Append(std::uint64_t symbol, const char* suffix, int arg2, int arg3);
// GetST @0x018ff2fc (thunk 0x00fe2f50); called with the value at booster+0x80.
std::byte* GetST(std::byte* receiver);
// AllocatePooledActor @0x019037c4 (thunk 0x00feb620).
AllocatedActor AllocatePooledActor(std::byte* thread, std::uint64_t symbol);
// Attach @0x02212888 (thunk 0x00ffc750).
void Attach(std::byte* booster, std::uint32_t handle);
// GameSpaceMT @0x018f8f24 (thunk 0x00ffef20).
std::byte* GameSpaceMT(std::byte* thread_space, std::uint64_t actor_id);
// Find @0x013c3d70 (thunk 0x01015c90); the instance whose result feeds BodyTypeExclusionFlags.
void FindPhysics(FoundRef* out, std::byte* game_space, const std::byte* actor_handle, int sentinel);
// RefreshCache @0x013367cc (thunk 0x0103f840); on the FindPhysics result.
void RefreshCachePhysics(FoundRef* found);
// UserId @0x0135a9b8 (thunk 0x01044c30).
std::uint32_t UserId(std::byte* netgame, std::uint64_t actor_id);
// BodyTypeExclusionFlags @0x01778ea4 (thunk 0x01001380).
std::uint32_t BodyTypeExclusionFlags(std::byte* physics);
// SetBodyTypeExclusionFlags @0x0177e028 (thunk 0x00fd45c0).
void SetBodyTypeExclusionFlags(std::byte* physics, std::uint32_t handle, std::uint32_t flags);
// Find @0x02231d1c (thunk 0x00fd4260); the instance whose result feeds the model-swap callbacks.
void FindModelSwapper(FoundRef* out, std::byte* game_space, const std::byte* actor_handle, int sentinel);
// RefreshCache @0x023e4518 (thunk 0x00fe7fb0); on the FindModelSwapper result.
void RefreshCacheModelSwapper(FoundRef* found);
// RegisterPreModelSwapCallback @0x01734eb0 (thunk 0x00ff1dc0). The last two arguments are the slot index
// (x4) and 3 (x5); the callee's parameter names are not recovered.
void RegisterPreModelSwapCallback(std::byte* model_swapper, std::uint32_t handle, const void* callback,
                                  std::byte* booster, std::uint16_t index, std::uint32_t mode);
// RegisterSuccessModelSwapCallback @0x01734f00 (thunk 0x0104aa10); same arguments.
void RegisterSuccessModelSwapCallback(std::byte* model_swapper, std::uint32_t handle, const void* callback,
                                      std::byte* booster, std::uint16_t index, std::uint32_t mode);
// Find @0x018265f0 (thunk 0x0104ae80); the instance for CTeamCS.
void FindTeam(FoundRef* out, std::byte* context, const std::byte* actor_handle, int sentinel);
// RefreshCache @0x013c4d38 (thunk 0x00ffa0d0); on the FindTeam result.
void RefreshCacheTeam(FoundRef* found);
// CreateContext @0x02212f64 (thunk 0x01016b30).
void CreateContext(std::byte* component, std::uint32_t handle, std::uint32_t mode);
// ApplyOverrides @0x02213074 (thunk 0x0101b020).
void ApplyOverrides(std::byte* component, std::uint32_t handle, std::uint32_t mode);
// ApplyItemFromLoadout @0x0135a6dc (thunk 0x00fdd050).
void ApplyItemFromLoadout(std::byte* netgame, std::uint64_t id, std::uint64_t generation,
                          std::uint64_t arg3, std::uint64_t arg4);
// QueueFunctor<NRadEngine::CDeferredMethodQueue::SFunctor2<void, NRadEngine::CTeamCS,
// NRadEngine::SCompactPoolHandle, unsigned int>> @0x01e08e9c (thunk 0x00fde7e0).
std::byte* QueueFunctor(std::byte* queue, const void* invoke);

// Data read by address. Each value is loaded at run time, or is a relocated pointer whose value is not in the file.
extern std::uint64_t g_actor_symbol_base;                // *(u64*)0x0038af868 (DAT_038af868)
extern const void* const g_team_set_invoke;              // *(u64*)0x03816cb8
extern const void* const g_pre_model_swap_callback;      // *(u64*)0x0380b780
extern const void* const g_success_model_swap_callback;  // *(u64*)0x03820c58
extern const std::uint32_t* const g_cgs_initialized;     // *(u64*)0x03829008 points at the CGS flag (u32)
extern const std::uint32_t* const g_invalid_user_id;     // *(u64*)0x0381a350 points at the invalid user id (u32)

namespace {

constexpr std::size_t kSlotSize = 0x68;
constexpr std::size_t kCustomizationRecordSize = 0x1c0;
constexpr std::size_t kTeamRecordSize = 12;
// The 16 bytes at VA 0x0329e1f0, copied into the team functor at +0x08.
constexpr std::uint64_t kTeamSetMember[2] = {0x300, 1};
// Event sent for every processed slot: *(u64*)0x032cf010.
constexpr std::uint64_t kComponentEvent = 0x39d06385ba83c7a2ULL;
// Arguments 3 and 4 of ApplyItemFromLoadout: *(u64*)0x02ca5270 and *(u64*)0x02caea38.
constexpr std::uint64_t kLoadoutArg3 = 0x2fc28196587d528eULL;
constexpr std::uint64_t kLoadoutArg4 = 0x2fd580904d605298ULL;
// Suffix of a low-spec symbol: the string at VA 0x02ca1bf6.
constexpr char kLowspecSuffix[] = "_lowspec";

std::byte* LoadPtr(const std::byte* base, std::size_t offset) {
    std::byte* value = nullptr;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

std::uint16_t LoadU16(const std::byte* base, std::size_t offset) {
    std::uint16_t value = 0;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

std::uint32_t LoadU32(const std::byte* base, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

std::uint64_t LoadU64(const std::byte* base, std::size_t offset) {
    std::uint64_t value = 0;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

std::int32_t LoadI32(const std::byte* base, std::size_t offset) {
    std::int32_t value = 0;
    std::memcpy(&value, base + offset, sizeof value);
    return value;
}

void StoreU16(std::byte* base, std::size_t offset, std::uint16_t value) {
    std::memcpy(base + offset, &value, sizeof value);
}

void StoreU32(std::byte* base, std::size_t offset, std::uint32_t value) {
    std::memcpy(base + offset, &value, sizeof value);
}

void StoreU64(std::byte* base, std::size_t offset, std::uint64_t value) {
    std::memcpy(base + offset, &value, sizeof value);
}

void StorePtr(std::byte* base, std::size_t offset, const std::byte* value) {
    std::memcpy(base + offset, &value, sizeof value);
}

// Reads the function pointer at vtable slot `slot` of `object` (the vptr is its first word). ReVault has no
// name for the slots this file calls; each call site casts the pointer to the signature its disassembly shows.
template <typename Fn>
Fn VirtualSlot(const std::byte* object, std::size_t slot) {
    const std::byte* const vtable = LoadPtr(object, 0);
    Fn fn = nullptr;
    std::memcpy(&fn, vtable + slot, sizeof fn);
    return fn;
}

// Vtable slot +0x1d0 of a component. The codex calls it Supports; the name is not recovered.
std::uint32_t SlotSupports(std::byte* component, std::uint32_t handle) {
    using Fn = std::uint32_t (*)(std::byte*, std::uint32_t);
    return VirtualSlot<Fn>(component, 0x1d0)(component, handle);
}

// Vtable slots +0x1e0 and +0x1f0 of the booster. No names recovered; each returns an actor pair in x0 and x1.
AllocatedActor BoosterSlot1e0(std::byte* booster, std::uint32_t handle) {
    using Fn = AllocatedActor (*)(std::byte*, std::uint32_t);
    return VirtualSlot<Fn>(booster, 0x1e0)(booster, handle);
}

AllocatedActor BoosterSlot1f0(std::byte* booster, std::uint32_t handle) {
    using Fn = AllocatedActor (*)(std::byte*, std::uint32_t);
    return VirtualSlot<Fn>(booster, 0x1f0)(booster, handle);
}

// The empty state the caller writes into a FoundRef before each Find (raw stores, in the order they are made).
void ResetFoundRef(FoundRef& found) {
    std::byte* const raw = found.raw;
    StoreU64(raw, 0x00, 0);
    StoreU64(raw, 0x08, ~std::uint64_t{0});
    StoreU64(raw, 0x10, ~std::uint64_t{0});
    StoreU64(raw, 0x16, ~std::uint64_t{0});
    StoreU16(raw, 0x1e, 0);
    StoreU64(raw, 0x20, 0xffff);
    StoreU64(raw, 0x28, ~std::uint64_t{0});
    StoreU64(raw, 0x30, ~std::uint64_t{0});
    StoreU64(raw, 0x38, ~std::uint64_t{0});
    StoreU16(raw, 0x48, 0xffff);
    StoreU16(raw, 0x4a, 0);
    StoreU32(raw, 0x4c, 0);
}

std::byte* FoundObject(const FoundRef& found) { return LoadPtr(found.raw, 0x40); }

std::uint32_t FoundHandle(const FoundRef& found) { return LoadU32(found.raw, 0x48); }

std::uint64_t FoundWord0(const FoundRef& found) { return LoadU64(found.raw, 0x00); }

// Customization record of a component: the index's table entry (low half) selects one of the 0x1c0-byte records.
std::byte* CustomizationRecord(std::byte* component, std::uint16_t index) {
    const std::uint16_t record = LoadU16(LoadPtr(component, 0xd0), static_cast<std::size_t>(index) * 4);
    return LoadPtr(component, 0x100) + static_cast<std::size_t>(record) * kCustomizationRecordSize;
}

}  // namespace

namespace NRadEngine::NRadGame {

void CR15NetBoosterCS::AllocateExclusive() {
    auto* const self = reinterpret_cast<std::byte*>(this);
    std::uint16_t count = LoadU16(self, 0xfc);
    for (std::uint16_t i = 0; i < count; ++i) {
        std::byte* const slot = LoadPtr(self, 0x100) + static_cast<std::size_t>(i) * kSlotSize;
        const std::uint16_t index = LoadU16(LoadPtr(self, 0xe0), static_cast<std::size_t>(i) * 2);
        const std::uint32_t entry = LoadU32(LoadPtr(self, 0xd0), static_cast<std::size_t>(index) * 4);
        // Packed handle: the index in the low half, the table entry's generation (its high half) above it.
        const std::uint32_t handle = index | (entry & 0xffff0000u);

        // A filled slot is skipped: its actor id is set and its generation is not -1.
        if (LoadU64(slot, 0x58) != ~std::uint64_t{0} &&
            static_cast<std::int16_t>(LoadU16(slot, 0x60)) != -1) {
            continue;
        }

        // Low-spec builds (*(i32*)(*(G+8) + 0x7ad8) < 0) allocate under the suffixed symbol.
        std::uint64_t symbol = g_actor_symbol_base;
        if (LoadI32(LoadPtr(LoadPtr(self, 0x80), 8), 0x7ad8) < 0) {
            symbol = Append(symbol, kLowspecSuffix, 1, 1);
        }
        std::byte* const thread = GetST(LoadPtr(self, 0x80));
        const AllocatedActor actor = AllocatePooledActor(thread, symbol);
        StoreU64(slot, 0x58, actor.id);
        StoreU64(slot, 0x60, actor.generation);
        Attach(self, handle);

        std::byte* const thread_mt = GetST(LoadPtr(self, 0x80));
        std::byte* const game_space = GameSpaceMT(LoadPtr(thread_mt, 8), LoadU64(slot, 0x58));

        // Physics: bit 14 of the exclusion flags follows whether the local user differs from the actor's user.
        FoundRef physics{};
        ResetFoundRef(physics);
        FindPhysics(&physics, game_space, slot + 0x58, -1);
        const AllocatedActor net_actor = BoosterSlot1f0(self, handle);
        const std::uint32_t actor_user = UserId(LoadPtr(self, 0xb8), net_actor.id);
        RefreshCachePhysics(&physics);
        std::byte* physics_obj = FoundObject(physics);
        RefreshCachePhysics(&physics);
        const std::uint32_t flags = BodyTypeExclusionFlags(physics_obj);

        std::uint32_t local_user = *g_invalid_user_id;
        std::byte* const netgame = LoadPtr(self, 0xb8);
        if (LoadU16(netgame, 0xde) != 0 && LoadU16(netgame, 0xdc) != 0) {
            const std::size_t local_index = LoadU16(netgame, 0x178) & 0x1fu;
            local_user = static_cast<std::uint32_t>(local_index) |
                         (static_cast<std::uint32_t>(LoadU16(netgame, 0x13a + local_index * 4)) << 16);
        }
        const std::uint32_t sign_bit = flags >> 31;
        const std::uint32_t differs = local_user != actor_user ? 1u : 0u;
        const std::uint32_t adjusted = sign_bit == differs ? (flags | 0x4000u) : (flags & 0xffffbfffu);
        RefreshCachePhysics(&physics);
        physics_obj = FoundObject(physics);
        RefreshCachePhysics(&physics);
        SetBodyTypeExclusionFlags(physics_obj, FoundHandle(physics), adjusted);

        // Model swapper: both callbacks are registered when the component supports the handle.
        FoundRef model{};
        ResetFoundRef(model);
        FindModelSwapper(&model, game_space, slot + 0x58, -1);
        RefreshCacheModelSwapper(&model);
        std::byte* model_obj = FoundObject(model);
        if (model_obj != nullptr && FoundWord0(model) != 0) {
            RefreshCacheModelSwapper(&model);
            if (SlotSupports(model_obj, FoundHandle(model)) != 0) {
                RefreshCacheModelSwapper(&model);
                model_obj = FoundObject(model);
                RefreshCacheModelSwapper(&model);
                RegisterPreModelSwapCallback(model_obj, FoundHandle(model), g_pre_model_swap_callback, self,
                                             index, 3);
                RefreshCacheModelSwapper(&model);
                model_obj = FoundObject(model);
                RefreshCacheModelSwapper(&model);
                RegisterSuccessModelSwapCallback(model_obj, FoundHandle(model), g_success_model_swap_callback,
                                                 self, index, 3);
            }
        }

        // Team: SetTeam now when the game space does not defer (u32 at +0x368), otherwise queue it.
        FoundRef team{};
        ResetFoundRef(team);
        FindTeam(&team, LoadPtr(self, 0x80), slot + 0x58, -1);
        RefreshCacheTeam(&team);
        std::byte* team_obj = FoundObject(team);
        if (team_obj != nullptr && FoundWord0(team) != 0) {
            RefreshCacheTeam(&team);
            if (SlotSupports(team_obj, FoundHandle(team)) != 0) {
                std::byte* const cached = LoadPtr(slot, 0x18);
                if (cached != nullptr && SlotSupports(cached, LoadU32(slot, 0x20)) != 0) {
                    RefreshCacheTeam(&team);
                    team_obj = FoundObject(team);
                    RefreshCacheTeam(&team);
                    const std::uint32_t team_handle = FoundHandle(team);
                    // Team id: the u32 at +4 of the 12-byte record that the cached handle's table entry selects.
                    const std::uint16_t cached_record =
                        LoadU16(LoadPtr(cached, 0xd0), static_cast<std::size_t>(LoadU16(slot, 0x20)) * 4);
                    const std::uint32_t team_id =
                        LoadU32(LoadPtr(cached, 0x100) + static_cast<std::size_t>(cached_record) * kTeamRecordSize,
                                4);
                    std::byte* const team_space = LoadPtr(team_obj, 0x80);
                    if (LoadU32(team_space, 0x368) != 0) {
                        VirtualSlot<void (*)(std::byte*, std::uint32_t, std::uint32_t)>(team_obj, 0x300)(
                            team_obj, team_handle, team_id);
                    } else {
                        std::byte* const functor = QueueFunctor(team_space + 0x170, g_team_set_invoke);
                        StorePtr(functor, 0x00, team_obj);
                        std::memcpy(functor + 0x08, kTeamSetMember, sizeof kTeamSetMember);
                        StoreU32(functor, 0x18, team_handle);
                        StoreU32(functor, 0x1c, team_id);
                    }
                }
            }
        }

        // Customization: only when CGS is initialized and the component supports the slot's handle.
        if (*g_cgs_initialized != 0) {
            std::byte* const cust = LoadPtr(slot, 0x48);
            if (cust != nullptr && SlotSupports(cust, LoadU32(slot, 0x50)) != 0) {
                const std::uint32_t cust_handle = LoadU32(slot, 0x50);
                std::byte actor_ref[16];
                std::memcpy(actor_ref, slot + 0x58, sizeof actor_ref);
                std::memcpy(CustomizationRecord(cust, LoadU16(slot, 0x50)) + 0x1b0, actor_ref, sizeof actor_ref);
                CreateContext(cust, cust_handle, 3);
                ApplyOverrides(cust, cust_handle, 3);
                StoreU32(CustomizationRecord(cust, LoadU16(slot, 0x50)), 0x38, 1);
                ApplyItemFromLoadout(LoadPtr(self, 0xb8), net_actor.id, net_actor.generation, kLoadoutArg3,
                                     kLoadoutArg4);
            }
        }

        // Component event for the slot's actor: the id and the generation come from two reads of booster slot +0x1e0.
        const AllocatedActor first = BoosterSlot1e0(self, handle);
        const AllocatedActor second = BoosterSlot1e0(self, handle);
        SendComponentEventGlobal(LoadPtr(self, 0x80), kComponentEvent, first.id, second.generation, 1);
        count = LoadU16(self, 0xfc);
    }
    StoreU32(self, 0x138, 0);
}

}  // namespace NRadEngine::NRadGame
