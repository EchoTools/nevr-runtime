// @0x138e71c — NRadEngine::NRadGame::CR15NetGame::PartyCount(uint16_t) const
// ReVault binary 576; reconstruction node 100600206; confidence M.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "cr15_net_game.hpp"

namespace NRadEngine::NRadGame {

namespace {

template <typename T>
T read_at(const void* base, std::size_t offset) noexcept {
    T value;
    std::memcpy(&value, static_cast<const std::byte*>(base) + offset, sizeof(value));
    return value;
}

using ServiceMethod = std::uint64_t (*)(const void*);

std::uint64_t call_service_method(const void* service, std::size_t byte_offset) noexcept {
    const auto* vtable = read_at<const std::uintptr_t*>(service, 0);
    const auto method = reinterpret_cast<ServiceMethod>(vtable[byte_offset / sizeof(void*)]);
    return method(service);
}

extern "C" std::int64_t cns_lobby_entrant_int(
    const void* lobby_subobject,
    std::uint64_t entrant_index,
    const char* key,
    std::int64_t fallback) noexcept
    __asm__("_ZNK10NRadEngine8CNSLobby10EntrantIntEyPKcx");

constexpr char kPartyIdKey[] = "partyid";
constexpr std::size_t kLobbyPointerOffset = 0x40;
constexpr std::size_t kStateCountOffset = 0xde;
constexpr std::size_t kPlayerCountOffset = 0xe2;
constexpr std::size_t kPlayerArrayOffset = 0x178;
constexpr std::size_t kPlayerStride = 0x250;
constexpr std::size_t kServicePointerOffset = 0x647c8;
constexpr std::size_t kPlayerPartyIdOffset = 0;
constexpr std::size_t kServicePartyIdMethod = 0xd0;
constexpr std::size_t kServicePartyCountMethod = 0xd8;

const void* lobby_subobject(const void* game) noexcept {
    const auto* lobby = read_at<const std::byte*>(game, kLobbyPointerOffset);
    return lobby + 0x18;
}

std::uint64_t entrant_party_id(
    const void* game,
    std::uint16_t player_index) noexcept {
    const auto player_id = read_at<std::uint16_t>(
        game,
        kPlayerArrayOffset + static_cast<std::size_t>(player_index) * kPlayerStride +
            kPlayerPartyIdOffset);
    return static_cast<std::uint64_t>(player_id & 0x1f);
}

}  // namespace

std::uint64_t CR15NetGame::PartyCount(std::uint16_t index) const {
    const auto state_count = read_at<std::uint16_t>(this, kStateCountOffset);
    const auto* service = read_at<const void*>(this, kServicePointerOffset);

    if (index < state_count) {
        if (service != nullptr) {
            return call_service_method(service, kServicePartyCountMethod);
        }
        return state_count;
    }

    const auto* lobby = lobby_subobject(this);
    const auto party_id = cns_lobby_entrant_int(
        lobby,
        entrant_party_id(this, index),
        kPartyIdKey,
        0);

    if (party_id == 0) {
        return 1;
    }

    const auto player_count = read_at<std::uint16_t>(this, kPlayerCountOffset);
    if (player_count == 0) {
        return 0;
    }

    std::uint32_t matching_players = 0;
    for (std::uint64_t player_index = 0; player_index < player_count; ++player_index) {
        std::int64_t candidate_party_id = 0;
        if (player_index < state_count) {
            if (service != nullptr) {
                candidate_party_id = static_cast<std::int64_t>(
                    call_service_method(service, kServicePartyIdMethod));
            }
        } else {
            candidate_party_id = cns_lobby_entrant_int(
                lobby,
                entrant_party_id(this, static_cast<std::uint16_t>(player_index)),
                kPartyIdKey,
                0);
        }

        if (candidate_party_id == party_id) {
            ++matching_players;
        }
    }

    return matching_players;
}

}  // namespace NRadEngine::NRadGame
