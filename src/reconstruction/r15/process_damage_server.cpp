// CR15NetDamageableCS::ProcessDamageServer
// Quest libr15.so VA 0x2239c1c (RVA 0x2229c1c); PC echovr.exe: echovr.exe VA 0x140d29770 (RVA 0xd29770).
// ReVault binary 576, raw parent 100302795, reconstruction node 100600183; tier: reconstructed; grade: L, assessed by revault-codex.
// @0x2239c1c — CR15NetDamageableCS::ProcessDamageServer
/* @confidence: L — complex transform/message construction remains represented by explicit callbacks. */
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

struct DamageServerOps {
    virtual std::uint32_t user_id(void* net_game, std::uint64_t game_space_id,
                                  const void* actor_id_record);
    virtual std::uint64_t actor_for_user(void* net_game, std::uint32_t user_id);
    virtual std::uint64_t actor_id_for_handle(void* self, std::uint32_t handle);
    virtual void send_component_event_global(std::uint64_t game_space, std::uint64_t event,
                                              std::uint64_t value);
    virtual void send_event_global(std::uint64_t game_space, std::uint64_t event,
                                   const void* data, std::size_t size,
                                   std::uint64_t event_type);
    virtual void broadcast_player_killed(void* net_game, const void* data,
                                          std::uint64_t peer);
    virtual std::uint64_t all_peers();
    virtual std::uint64_t global_clock_microseconds();
    virtual void write_log(int level, const char* message);
    virtual bool sort_damage_types(void* table, std::uint64_t count);
    virtual std::uint64_t find_or_insert_damage_type(void* table, std::uint64_t count,
                                                      std::int64_t symbol);
    virtual void send_damage_threshold_event(void* self, std::uint32_t handle, std::uint64_t event);
    virtual void emit_kill_and_death_log(void* self, void* game_space, void* net_game,
                                         std::byte* player_row, std::byte* player_stats,
                                         std::uint32_t victim_user, std::uint64_t damage_type,
                                         std::uint64_t attacker, std::uint64_t peer);
    virtual void emit_health_shield_update(void* self, void* game_space,
                                            std::byte* player_row, std::uint32_t victim_user);
};

void ProcessDamageServer(void* self, std::uint32_t handle, float damage,
                         std::uint64_t damage_type, std::uint64_t attacker,
                         DamageServerOps& ops) {
    auto* component = static_cast<std::byte*>(self);
    auto* rows = *reinterpret_cast<std::byte**>(component + 0x100);
    auto* row_index = *reinterpret_cast<std::byte**>(component + 0xd0);
    const auto row_number = *reinterpret_cast<std::uint16_t*>(row_index + (handle & 0xffff) * 4);
    auto* row = rows + static_cast<std::size_t>(row_number) * 0xb8;
    // The state byte is a bit field: std::byte has no bitwise operators with int.
    auto* state = reinterpret_cast<std::uint8_t*>(row + 2);
    if ((*state & 4) != 0) return;

    auto* net_game = *reinterpret_cast<void**>(component + 0xb8);
    auto* game_space = *reinterpret_cast<std::byte**>(component + 0x80);
    auto* actor_ids = *reinterpret_cast<std::byte**>(component + 0x108);
    auto* player_stats = *reinterpret_cast<std::byte**>(component + 0xc0);
    const auto player_index = row_number;
    auto* stats = player_stats +
        static_cast<std::size_t>(*reinterpret_cast<std::uint16_t*>(row)) * 0x38;
    const auto game_space_id = *reinterpret_cast<std::uint64_t*>(game_space + 0x58);
    const auto* victim_actor_record = actor_ids + player_index * 0x10;
    const auto victim_user = ops.user_id(net_game, game_space_id, victim_actor_record);

    auto* health = reinterpret_cast<float*>(row + 0x1c);
    auto* shield = reinterpret_cast<float*>(row + 0x20);
    float remaining_damage = damage;
    bool consume_health = ((*state >> 1) & 1) != 0;

    if (*reinterpret_cast<std::int16_t*>(row + 0x14) == -1 &&
        victim_user != UINT32_MAX) {
        const float old_health = *health;
        float health_damage = 0.0f;
        if (old_health <= 0.0f) {
            remaining_damage = 0.0f;
        } else {
            float after_health = old_health - damage;
            *health = after_health;
            health_damage = damage;
            if (after_health <= 0.0f) {
                ops.send_damage_threshold_event(self, handle, 0x56e33d8cedc6a80ULL);
                *state |= 2;
                after_health = *health;
                health_damage = -after_health;
            }
            if (after_health <= 0.0f) after_health = 0.0f;
            const float cap = *reinterpret_cast<float*>(stats + 0x20);
            const float applied_health = std::min(after_health, cap);
            if (old_health + old_health <= damage) consume_health = true;
            remaining_damage = 0.0f;
            if (old_health + old_health <= damage) remaining_damage = health_damage;
            *health = applied_health;
            remaining_damage = old_health - applied_health;
        }

        if (consume_health) {
            const float before_shield = *shield;
            if (before_shield > 0.0f) {
                float after_shield = before_shield - remaining_damage;
                if (after_shield <= 0.0f) after_shield = 0.0f;
                const float shield_cap = *reinterpret_cast<float*>(stats + 0x24);
                after_shield = std::min(after_shield, shield_cap);
                remaining_damage = (before_shield - after_shield) + remaining_damage;
                *shield = after_shield;
                if (after_shield <= 0.0f) {
                    ops.send_damage_threshold_event(self, handle, 0xa4c9beeed373373eULL);
                    *state |= 4;
                    ops.emit_kill_and_death_log(self, game_space, net_game, row, stats,
                                                 victim_user, damage_type, attacker,
                                                 ops.all_peers());
                }
            }
        }

        ops.emit_health_shield_update(self, game_space, row, victim_user);
    } else {
        *shield -= damage;
        if (*shield <= 0.0f) {
            *state |= 4;
            const auto payload = ops.actor_id_for_handle(self, handle);
            ops.send_component_event_global(
                reinterpret_cast<std::uintptr_t>(game_space), 0xa4c9beeed373373eULL, payload);
        }
        if (*shield <= 0.0f) *shield = 0.0f;
        *shield = std::min(*shield, *reinterpret_cast<float*>(stats + 0x24));
    }

    auto* count = reinterpret_cast<std::uint64_t*>(row + 0xa0);
    auto* table = *reinterpret_cast<void**>(row + 0x70);
    if (*count > 0xf) {
        ops.write_log(4, "[CR15NetDamageableCS](ProcessDamageServer): Max damage types exceeded");
    } else {
        auto* sorted = reinterpret_cast<std::uint32_t*>(row + 0xa8);
        if (*count != 0 && ((*sorted & 1u) != 0)) {
            ops.sort_damage_types(table, *count);
            *sorted &= ~1u;
        }
        const auto index = ops.find_or_insert_damage_type(
            table, *count, static_cast<std::int64_t>(damage_type));
        // The raw CTable insert initializes a new entry's float to zero; add the
        // incoming damage to the selected or newly inserted key.
        auto* entry = static_cast<std::byte*>(table) + index * 0x10;
        *reinterpret_cast<float*>(entry + 8) += damage;
    }

    const auto event_payload = ops.actor_id_for_handle(self, handle);
    ops.send_component_event_global(
        reinterpret_cast<std::uintptr_t>(game_space), 0xd7c8d476f727e09aULL, event_payload);
    *reinterpret_cast<float*>(row + 0x24) =
        static_cast<float>(ops.global_clock_microseconds()) * 1.0e-6f;
    *state &= 0xfe;
}
