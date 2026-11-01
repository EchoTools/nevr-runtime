// CR15NetBoosterCS::AllocateExclusive()
// Quest libr15.so: ReVault VA 0x2210dd4 / ELF RVA 0x2110dd4, binary 576.
// PC twin not located; the PC search hit is an annotation inside AttachToPlayerModel.
// ReVault raw parent 100302427; reconstruction node 100600168; tier: reconstructed; grade: M, assessed by revault-codex.
// Component layouts and several helper semantics are intentionally represented by Ops.
#include <cstddef>
#include <cstdint>
namespace q15t::r15::reconstruction {
struct BoosterCS { std::byte opaque[0x148]; };
struct ComponentRef { void* object; std::uint32_t handle; bool present; };
struct AllocatedActor { std::uint64_t id; std::uint16_t generation; };
struct Ops {
    virtual std::uint16_t slot_count(BoosterCS*);
    virtual std::byte* slots(BoosterCS*); // per-slot stride 0x68
    virtual bool game_is_lowspec(BoosterCS*);
    virtual std::uint64_t actor_symbol(BoosterCS*);
    virtual std::uint64_t append_lowspec_suffix(std::uint64_t);
    virtual std::uint64_t get_space_thread();
    virtual AllocatedActor allocate_pooled_actor(std::uint64_t space_thread, std::uint64_t symbol);
    virtual void store_actor(std::byte* slot, AllocatedActor actor);
    virtual void attach();
    virtual std::uint64_t game_space_for_thread(std::uint64_t);
    virtual std::byte* net_game(BoosterCS*);
    virtual ComponentRef find_physics(std::uint64_t game_space, std::byte* slot, int sentinel);
    virtual bool physics_supports(void* physics, std::uint32_t handle);
    virtual std::uint32_t physics_body_exclusion_flags(void* physics);
    virtual std::uint32_t actor_user_id(std::byte* net_game, std::uint64_t actor_id);
    virtual void set_physics_body_exclusion_flags(void*, std::uint32_t slot, std::uint32_t flags);
    virtual ComponentRef find_model_swapper(std::uint64_t game_space, std::byte* slot, int sentinel);
    virtual bool model_swapper_supports(void*, std::uint32_t slot);
    virtual void register_pre_model_swap_callback(void*, std::uint32_t slot);
    virtual void register_success_model_swap_callback(void*, std::uint32_t slot);
    virtual ComponentRef find_team(std::byte* team_context, std::byte* slot, int sentinel);
    virtual bool team_supports(void*, std::uint32_t slot);
    virtual ComponentRef find_other_team(std::byte* slot);
    virtual bool deferred_queue_empty(void* team);
    virtual void queue_team_assignment(void*, std::uint32_t slot, std::uint32_t team);
    virtual void assign_team_now(void*, std::uint32_t slot, std::uint32_t team);
    virtual std::uint32_t resolved_team_value(void*, std::byte* slot, std::uint32_t handle);
    virtual bool cgs_initialized();
    virtual bool customization_component_valid(std::byte* slot);
    virtual void apply_customization_and_loadout(std::byte* slot, AllocatedActor actor);
    virtual void send_global_component_event(std::uint64_t game_space, std::uint64_t event,
                                             AllocatedActor actor);
};
void AllocateExclusive(BoosterCS* self, Ops& ops) {
    auto* object = reinterpret_cast<std::byte*>(self);
    auto* records = ops.slots(self);
    const auto count = ops.slot_count(self);
    const auto thread = ops.get_space_thread();
    const auto game_space = ops.game_space_for_thread(thread);
    auto* netgame = ops.net_game(self);
    for (std::uint16_t i=0; i<count; ++i) {
        auto* slot = records + static_cast<std::size_t>(i) * 0x68;
        auto* handles = slot + 0x58;
        if (*reinterpret_cast<std::int64_t*>(handles) != -1 &&
            *reinterpret_cast<std::int16_t*>(handles + 8) != -1) continue;
        auto symbol = ops.actor_symbol(self);
        if (ops.game_is_lowspec(self)) symbol = ops.append_lowspec_suffix(symbol);
        const auto actor = ops.allocate_pooled_actor(thread, symbol);
        ops.store_actor(slot, actor); // writes the id at +0x58 and generation at +0x60
        ops.attach();
        const auto actor_id = ops.actor_user_id(netgame, actor.id);
        const auto physics = ops.find_physics(game_space, handles, -1);
        if (physics.present && ops.physics_supports(physics.object, physics.handle)) {
            auto flags = ops.physics_body_exclusion_flags(physics.object);
            std::uint32_t local_id = UINT32_MAX;
            if (*reinterpret_cast<std::uint16_t*>(netgame + 0xde) != 0 &&
                *reinterpret_cast<std::uint16_t*>(netgame + 0xdc) != 0) {
                const auto index = *reinterpret_cast<std::uint16_t*>(netgame + 0x178) & 0x1f;
                local_id = index | (static_cast<std::uint32_t>(
                    *reinterpret_cast<std::uint16_t*>(netgame + 0x13a + index * 4)) << 16);
            }
            auto adjusted = flags | 0x4000;
            if (((flags >> 31) & 1) != static_cast<unsigned>(local_id != actor_id))
                adjusted = flags & 0xffffbfff;
            ops.set_physics_body_exclusion_flags(physics.object, physics.handle, adjusted);
        }
        const auto model = ops.find_model_swapper(game_space, slot + 0x18, -1);
        if (model.present && ops.model_swapper_supports(model.object, model.handle)) {
            ops.register_pre_model_swap_callback(model.object, model.handle);
            ops.register_success_model_swap_callback(model.object, model.handle);
        }
        const auto team = ops.find_team(object + 0x80, slot + 0x48, -1);
        if (team.present && ops.team_supports(team.object, team.handle)) {
            const auto other_team = ops.find_other_team(slot);
            if (other_team.present && ops.team_supports(other_team.object, other_team.handle)) {
                const auto team_id = ops.resolved_team_value(other_team.object, slot, other_team.handle);
                if (ops.deferred_queue_empty(team.object)) ops.queue_team_assignment(team.object, team.handle, team_id);
                else ops.assign_team_now(team.object, team.handle, team_id);
            }
        }
        if (ops.cgs_initialized() && ops.customization_component_valid(slot))
            ops.apply_customization_and_loadout(slot, actor);
        ops.send_global_component_event(game_space, 0x39d06385ba83a2ULL, actor);
    }
    *reinterpret_cast<std::uint32_t*>(object + 0x138) = 0;
}
} // namespace q15t::r15::reconstruction
