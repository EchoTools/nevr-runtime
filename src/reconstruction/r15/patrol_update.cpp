// SR15PatrolSM::Update(SR15NetAIContext&, SR15NetAgent&)
// Quest libr15.so: ReVault VA 0x13c096c / ELF RVA 0x12c096c, binary 576.
// PC `Update` twin not located; PC constructors initialize the same state-machine vtable.
// ReVault raw parent 100224992; reconstruction node 100600169; tier: reconstructed; grade: M, assessed by revault-codex.
// The random-distance stack alias in Ghidra is unresolved; no 8 m bound is asserted here.
#include <cstddef>
#include <cstdint>
namespace q15t::r15::reconstruction {
struct Ops {
    virtual bool bot_is_moving(std::uint64_t bot, std::uint32_t handle);
    virtual void* bot_ray(std::uint64_t agent);
    virtual Transform bot_transform(std::uint64_t bot, std::uint32_t handle);
    virtual Vector3 random_unit_direction();
    virtual bool static_raycast(void* ray, Transform const& from, Vector3 direction,
                                Transform& hit_transform, Vector3& hit_position,
                                float& distance_limit);
    virtual Transform transform_on_ray_miss(Transform const&, Vector3 const&, float random_fraction);
    virtual bool game_space_deferred_queue_empty();
    virtual void queue_move(std::uint64_t bot, std::uint32_t handle, Transform const&);
    virtual void move_now(std::uint64_t bot, std::uint32_t handle, Transform const&);
};
struct PatrolState { std::uint16_t pad; std::uint16_t state; std::byte opaque[0x28]; };
void Update(PatrolState* state, std::uint64_t agent, std::uint64_t bot,
            std::uint32_t handle, Ops& ops) {
    if (state->state == 1) {
        if (!ops.bot_is_moving(bot, handle)) state->state = 0;
    } else if (state->state == 0) {
        const auto from = ops.bot_transform(bot, handle);
        const auto direction = ops.random_unit_direction();
        auto target = from; Vector3 hit{}; float limit{};
        // Raw initializes this stack distance value to 0 before StaticRaycast; q15t's
        // separate G9b patch changes that operand to 8 m and changes the nonzero branch.
        limit = 0.0f;
        const bool raycast_result = ops.static_raycast(ops.bot_ray(agent), from, direction, target, hit, limit);
        // The raw zero-result branch copies StaticRaycast's output transform; nonzero
        // computes the randomized target. Do not infer hit/miss semantics from the flag.
        if (raycast_result)
            target = ops.transform_on_ray_miss(from, direction, random_fraction_from_thread_state());
        if (ops.game_space_deferred_queue_empty()) ops.queue_move(bot, handle, target);
        else ops.move_now(bot, handle, target);
        state->state = 1;
    }
}
} // namespace q15t::r15::reconstruction
