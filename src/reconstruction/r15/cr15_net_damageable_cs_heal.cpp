// @0x2237e74 — NRadEngine::NRadGame::CR15NetDamageableCS::Heal
/* @confidence: M */
// Quest libr15.so VA 0x2237e74 (RVA 0x2137e74).
// ReVault binary 576, raw parent 100302776, reconstruction node 100600280; tier: reconstructed; grade: M, assessed by revault-codex.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {
struct SPeer {
    static constexpr std::uint64_t kAllPeers = 0xffffffffffffffffULL;
};
struct CncaGameSpace {
    static void SendEventGlobal(void* game_space, std::uint64_t event_type,
                                const void* payload, std::uint32_t size,
                                std::uint64_t peer, std::uint32_t flags);
};

namespace NRadGame {

struct SCompactPoolHandle {
    std::uint32_t value;
};
struct SR15NetUserId {
    std::uint32_t value;
    static constexpr std::uint32_t kInvalid = 0xffffffffu;
    constexpr operator std::uint32_t() const { return value; }
};
struct SR15NetDamageableDealDamage {
    std::uint64_t session_id;
    std::uint64_t target;
    std::uint64_t health_and_shield;
};

class CR15NetGame {
public:
    std::int32_t UserId(std::uint64_t session_id, std::uint64_t net_user_id);
    template<class Event>
    void Broadcast(const Event& event, std::uint64_t arg0,
                   std::uint64_t arg1, std::uint64_t peer,
                   std::uint64_t sequence);
};

class CR15NetDamageableCS {
public:
    float HealModifier(SCompactPoolHandle handle);
    void Heal(SCompactPoolHandle handle, float amount,
              SR15NetUserId source_user);
};

void CR15NetDamageableCS::Heal(
    SCompactPoolHandle handle, float amount, SR15NetUserId source_user)
{
    auto* const self = reinterpret_cast<std::uintptr_t*>(this);
    const std::uint32_t compact_handle = handle.value;
    const std::size_t map_offset =
        static_cast<std::size_t>(compact_handle & 0xffffu) * 4;
    const auto* const pool_map =
        reinterpret_cast<const std::byte*>(self[0x1a]);
    const std::uint16_t pool_index =
        *reinterpret_cast<const std::uint16_t*>(pool_map + map_offset);
    auto* const pool = reinterpret_cast<std::byte*>(self[0x20]);
    auto* const entity = pool + static_cast<std::size_t>(pool_index) * 0xb8;

    // The ARM64 result is returned in s0 as a float. The Ghidra integer type
    // on HealModifier is incorrect; its body initializes s0 to 1.0f.
    const float heal_modifier = HealModifier(handle);
    auto* const archetypes =
        *reinterpret_cast<std::byte**>(self[0x18]);
    const std::uint16_t archetype_index =
        *reinterpret_cast<const std::uint16_t*>(entity);
    auto* const archetype =
        archetypes + static_cast<std::size_t>(archetype_index) * 0x38;

    float remaining = heal_modifier * amount;
    float healed = 0.0f;
    float* const health = reinterpret_cast<float*>(entity + 0x20);
    const float max_health = *reinterpret_cast<const float*>(archetype + 0x24);

    if (*health < max_health) {
        const float health_needed = max_health - *health;
        healed = health_needed;
        if (health_needed - remaining >= 0.0f)
            healed = remaining;
        remaining -= healed;

        float new_health = *health + healed;
        if (new_health - max_health >= 0.0f)
            new_health = max_health;
        *health = new_health;
    }

    const float max_shield = *reinterpret_cast<const float*>(archetype + 0x20);
    if (remaining > 0.0f && max_shield > 0.0f) {
        float* const shield = reinterpret_cast<float*>(entity + 0x1c);
        float shield_heal = max_shield - *shield;
        if (shield_heal - remaining >= 0.0f)
            shield_heal = remaining;
        float new_shield = *shield + shield_heal;
        healed += shield_heal;
        if (new_shield - max_shield >= 0.0f)
            new_shield = max_shield;
        *shield = new_shield;
    }

    const std::uint16_t current_pool_index =
        *reinterpret_cast<const std::uint16_t*>(pool_map + map_offset);
    auto* const current_entity =
        pool + static_cast<std::size_t>(current_pool_index) * 0xb8;
    auto* const game = reinterpret_cast<CR15NetGame*>(self[0x17]);
    auto* const game_space = reinterpret_cast<std::byte*>(self[0x10]);
    const std::uint64_t session_id =
        *reinterpret_cast<const std::uint64_t*>(game_space + 0x58);
    auto* const user_ids = reinterpret_cast<std::byte*>(self[0x21]);
    const std::uint64_t net_user_id =
        *reinterpret_cast<const std::uint64_t*>(
            user_ids + static_cast<std::size_t>(current_pool_index) * 0x10);
    const std::int32_t entity_user = game->UserId(session_id, net_user_id);

    if (*reinterpret_cast<const std::int16_t*>(current_entity + 0x14) == -1 &&
        entity_user != SR15NetUserId::kInvalid) {
        *reinterpret_cast<std::uint32_t*>(
            reinterpret_cast<std::byte*>(this) + 0x15c) = 1;
    } else {
        // The peer key is masked to 16 bits for table indexing. The
        // resolved peer qword is loaded and stored without another mask.
        std::uint64_t target_handle =
            static_cast<std::uint64_t>(compact_handle) | 0x100000000ULL;
        auto* const peer_component =
            reinterpret_cast<std::byte*>(self[0x29]);
        if (peer_component != nullptr) {
            const std::uint32_t peer_key =
                *reinterpret_cast<const std::uint32_t*>(current_entity + 0x10);
            auto** const peer_vtable =
                *reinterpret_cast<void***>(peer_component);
            using IsPeerValid = int (*)(void*, std::uint32_t);
            const auto is_valid = reinterpret_cast<IsPeerValid>(
                peer_vtable[0x1d0 / sizeof(void*)]);
            if (is_valid(peer_component, peer_key) != 0) {
                if ((~peer_key & 0xffffu) == 0) {
                    target_handle = ~std::uint64_t{0};
                } else {
                    const std::uint16_t peer_slot =
                        *reinterpret_cast<const std::uint16_t*>(
                            *reinterpret_cast<std::byte**>(
                                peer_component + 0xc8) +
                            static_cast<std::size_t>(peer_key & 0xffffu) * 4);
                    const auto* const peer_data =
                        *reinterpret_cast<const std::byte**>(
                            peer_component + 0xf8);
                    target_handle =
                        *reinterpret_cast<const std::uint64_t*>(
                            peer_data + static_cast<std::size_t>(peer_slot) *
                                            0x18 + 8);
                }
            }
        }

        SR15NetDamageableDealDamage event{session_id, target_handle, 0};

        std::uint64_t health_and_shield;
        std::memcpy(&health_and_shield, current_entity + 0x1c,
                    sizeof(health_and_shield));
        event.health_and_shield =
            ((health_and_shield & 0xffffffffULL) << 32) |
            (health_and_shield >> 32);
        game->Broadcast<SR15NetDamageableDealDamage>(
            event, 0, 0, SPeer::kAllPeers, 0xffffffffffffffffULL);
    }

    auto** const vtable = *reinterpret_cast<void***>(this);
    using GetEntityUserId =
        std::uint64_t (*)(CR15NetDamageableCS*, std::uint32_t);
    const auto get_entity_user_id = reinterpret_cast<GetEntityUserId>(
        vtable[0x1e8 / sizeof(void*)]);
    const std::uint64_t source_entity =
        get_entity_user_id(this, compact_handle);
    const std::int32_t source_user_id =
        game->UserId(session_id, source_entity);
    if (source_user != SR15NetUserId::kInvalid &&
        source_user_id != SR15NetUserId::kInvalid) {
        struct HealEvent {
            std::int32_t healed_user;
            std::int32_t source_user;
            float healed_amount;
            float reserved;
        } event{source_user_id, static_cast<std::int32_t>(source_user.value),
                healed, 0.0f};
        CncaGameSpace::SendEventGlobal(
            game_space, 0x9fe9183334399f50ULL, &event, sizeof(event),
            0xffffffffffffffffULL, 1);
    }
}

} // namespace NRadGame
} // namespace NRadEngine
