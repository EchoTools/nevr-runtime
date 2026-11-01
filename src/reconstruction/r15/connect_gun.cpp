// CR15NetGunCS::ConnectGun
// Quest libr15.so VA 0x2254e78; ReVault binary 576.
// ReVault raw parent 100303122, reconstruction node 100600264; grade M: Quest branches, fields and call ABIs match; the two-register actor return is inferred from x0/x1 use.
// @0x2254e78 — NRadEngine::NRadGame::CR15NetGunCS::ConnectGun
/* @confidence: M */

#include <cstddef>
#include <cstdint>

namespace NRadEngine {
struct CncaGameSpace {
    void SendComponentEventGlobal(std::uint64_t event_id,
                                  std::uint64_t low,
                                  std::uint64_t high,
                                  std::uint32_t flags);
};

namespace NRadGame {
struct SActorId {
    std::uint64_t low;
    std::uint64_t high;
};

struct CR15NetGame {
    SActorId UserActor(std::uint16_t slot);
};

struct CR15Game {
    static CR15NetGame* NetGameMT();
};

struct CTeamCS;

template<class T>
struct CComponentHandle;

template<>
struct CComponentHandle<CTeamCS> {
    static void Find(void* handle, CncaGameSpace* game_space,
                     const SActorId& actor, std::uint64_t mask);
};

struct CR15NetGunCS {
    void ReparentGun(std::uint32_t user_id, std::uint32_t gun_handle);
    void ConnectGun(std::uint64_t user_id, std::uint64_t compact_gun_handle);
};

namespace {
template<class T>
T load(const void* base, std::size_t offset) {
    return *reinterpret_cast<const T*>(
        static_cast<const std::byte*>(base) + offset);
}

template<class T>
void store(void* base, std::size_t offset, T value) {
    *reinterpret_cast<T*>(static_cast<std::byte*>(base) + offset) = value;
}

struct SActorParts {
    std::uint64_t low;
    std::uint64_t high;
};
static_assert(sizeof(SActorParts) == 0x10);
}

void CR15NetGunCS::ConnectGun(std::uint64_t user_id,
                              std::uint64_t compact_gun_handle) {
    auto* const self = reinterpret_cast<std::byte*>(this);
    auto* const gun_records = load<std::byte*>(self, 0xf8);
    auto* const gun_slot_map = load<std::uint16_t*>(self, 0x98);
    const std::uint16_t gun_slot_index =
        gun_slot_map[(compact_gun_handle & 0xffffu) * 2];
    auto* const gun_record = gun_records +
        static_cast<std::size_t>(gun_slot_index) * 0x168;
    auto* const actor_records =
        *load<std::byte**>(self, 0x88);
    const std::uint32_t user_low = static_cast<std::uint32_t>(user_id);
    store(gun_record, 0xb4, user_low);

    const std::uint16_t actor_record_index = load<std::uint16_t>(gun_record, 0xa0);
    auto* const net_game = CR15Game::NetGameMT();
    auto* const game_space = load<CncaGameSpace*>(self, 0x80);

    SActorId actor{0xffffffffffffffffULL, 0x0000ffffffffffffULL};
    const std::uint16_t user_slot = static_cast<std::uint16_t>(user_id);
    const std::uint16_t user_generation =
        static_cast<std::uint16_t>(user_id >> 16);
    if ((user_id & 0xfff0u) == 0 && user_generation != 0xffffu &&
        user_generation == load<std::uint16_t>(
            net_game, static_cast<std::size_t>(user_slot) * 4 + 0x13a) &&
        load<std::int16_t>(
            net_game, static_cast<std::size_t>(user_slot) * 4 + 0x138) != -1) {
        actor = net_game->UserActor(user_slot);
    }

    auto* const team_handle = gun_record + 0x40;
    CComponentHandle<CTeamCS>::Find(
        team_handle, game_space, actor, 0xffffffffffffffffULL);

    if (load<std::int32_t>(actor_records,
            static_cast<std::size_t>(actor_record_index) * 0xa0 + 0x9c) == 0) {
        return;
    }

    const auto packed_handle =
        static_cast<std::uint32_t>(compact_gun_handle);
    ReparentGun(user_low, packed_handle);
    store<std::uint32_t>(gun_records,
        static_cast<std::size_t>(gun_slot_index) * 0x168 + 0xd8, 0);

    using GetActor = SActorParts (*)(CR15NetGunCS*, std::uint64_t);
    const auto* const vtable = *reinterpret_cast<const std::uintptr_t* const*>(this);
    const auto get_actor = reinterpret_cast<GetActor>(vtable[0x1e0 / sizeof(void*)]);
    const SActorParts actor_low = get_actor(this, compact_gun_handle);
    const SActorParts actor_high = get_actor(this, compact_gun_handle);
    game_space->SendComponentEventGlobal(
        0x3ea2981ec20b2652ULL, actor_low.low, actor_high.high, 1);
}
} // namespace NRadGame
} // namespace NRadEngine
