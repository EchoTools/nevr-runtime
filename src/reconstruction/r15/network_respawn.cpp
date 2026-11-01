// CR15NetDamageableCS::NetworkRespawn
// Quest libr15.so VA 0x2238eb4 (RVA 0x2228eb4); PC echovr.exe: no direct twin located.
// ReVault binary 576, raw parent 100302785, reconstruction node 100600180; tier: reconstructed; grade: M, assessed by revault-codex.
// @0x2238eb4 — CR15NetDamageableCS::NetworkRespawn
/* @confidence: M */
#include <cstddef>
#include <cstdint>

struct NetworkRespawnOps {
    virtual std::uint64_t find_respawn_component(void* self, std::uint64_t actor,
                                                 std::uint64_t component_symbol);
    virtual std::uint64_t user_actor(void* net_game, std::uint16_t slot);
    virtual void reset_to_full_shields_and_health(void* self, std::uint32_t handle);
    virtual std::uint64_t component_event_payload(void* self, std::uint32_t handle);
    virtual void send_component_event_global(std::uint64_t game_space, std::uint64_t event,
                                              std::uint64_t payload_a, std::uint64_t payload_b,
                                              int global);
    virtual std::uint64_t user_id(void* net_game, std::uint64_t game_space,
                                  std::uint64_t actor);
    virtual std::uint64_t user_id_actor(void* self, std::uint32_t handle);
    virtual std::uint64_t server_peer();
    virtual std::uint64_t all_peers();
    virtual void broadcast_respawn_complete(void* net_game, const std::byte message[16],
                                             std::uint64_t peer);
    virtual void broadcast_player_respawn(void* net_game, const void* source_message,
                                          std::uint64_t peer);
    virtual void write_log(int level, const char* message);
};

void NetworkRespawn(void* self, const void* message, NetworkRespawnOps& ops) {
    auto* component = static_cast<std::byte*>(self);
    auto* payload = static_cast<const std::byte*>(message);
    auto* game_space = *reinterpret_cast<std::byte**>(component + 0x80);
    auto* net_game = *reinterpret_cast<void**>(component + 0xb8);
    if (*reinterpret_cast<const std::uint64_t*>(payload) !=
        *reinterpret_cast<std::uint64_t*>(game_space + 0x58))
        return;

    const auto packed_user = *reinterpret_cast<const std::uint32_t*>(payload + 8);
    std::uint64_t actor = UINT64_C(0xffffffffffff);
    auto* users = static_cast<std::byte*>(net_game);
    const auto slot = static_cast<std::uint16_t>(packed_user);
    const auto generation = static_cast<std::uint16_t>(packed_user >> 16);
    if (generation != 0xffff && (packed_user & 0xfff0u) == 0 &&
        generation == *reinterpret_cast<std::uint16_t*>(users + 0x13a + slot * 4) &&
        *reinterpret_cast<std::int16_t*>(users + 0x138 + slot * 4) != -1) {
        actor = ops.user_actor(net_game, slot);
    }

    const auto handle = static_cast<std::uint32_t>(
        ops.find_respawn_component(self, actor, 0xc9dc08cfc2f5920fULL));
    auto* rows = *reinterpret_cast<std::byte**>(component + 0x100);
    auto* row_indices = *reinterpret_cast<std::byte**>(component + 0xd0);
    const auto row_index = *reinterpret_cast<std::uint16_t*>(row_indices + (handle & 0xffff) * 4);
    auto* row = rows + static_cast<std::size_t>(row_index) * 0xb8;
    // The state byte is a bit field: std::byte has no bitwise operators with int.
    auto* state = reinterpret_cast<std::uint8_t*>(row + 2);
    *reinterpret_cast<std::uint64_t*>(row + 0xa0) = 0;
    *reinterpret_cast<std::uint32_t*>(row + 0xa8) &= ~1u;
    if (((*state >> 2) & 1) == 0)
        ops.write_log(4, "[DAMAGEABLE] Player isn't currently dead, respawning anyway");

    ops.reset_to_full_shields_and_health(self, handle);
    const auto payload_a = ops.component_event_payload(self, handle);
    const auto payload_b = ops.component_event_payload(self, handle);
    ops.send_component_event_global(
        reinterpret_cast<std::uintptr_t>(game_space), 0xd7ded068e621f290ULL,
        payload_a, payload_b, 1);

    const auto previous_state = *state;
    *state = previous_state | 0x10;
    if (*reinterpret_cast<std::int32_t*>(component + 0x158) == 0) {
        *state = previous_state & 0xef;
        std::byte complete[16]{};
        *reinterpret_cast<std::uint64_t*>(complete) =
            *reinterpret_cast<std::uint64_t*>(game_space + 0x58);
        const auto actor_for_id = ops.user_id_actor(self, handle);
        *reinterpret_cast<std::uint32_t*>(complete + 8) = static_cast<std::uint32_t>(
            ops.user_id(net_game, *reinterpret_cast<std::uint64_t*>(game_space + 0x58),
                        actor_for_id));
        ops.broadcast_respawn_complete(net_game, complete, ops.server_peer());
    } else {
        ops.broadcast_player_respawn(net_game, message, ops.all_peers());
    }
    *reinterpret_cast<std::uint32_t*>(component + 0x15c) = 1;
}
