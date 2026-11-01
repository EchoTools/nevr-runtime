// CR15NetGame::ReturnToLobby
// Quest libr15.so VA 0x1391e58 (RVA 0x1381e58); PC echovr.exe: no matching function located in project-wide search.
// ReVault binary 576, raw parent 100224229, reconstruction node 100600175; tier: reconstructed; grade: M, assessed by revault-codex.
// @0x1391e58 — CR15NetGame::ReturnToLobby
/* @confidence: M */
#include <cstddef>
#include <cstdint>
#include <cstring>

struct ReturnLobbyOps {
    virtual void write_log(int level, const char* message);
    virtual void write_log_symbol(int level, const char* format, std::uint64_t symbol_text);
    virtual void leave_game(void* net_game);
    virtual void switch_to();
    virtual void begin_online_multiplayer();
    virtual bool lobby_allows_return(void* lobby);
    virtual bool party_lobby_unjoinable();
    virtual std::uint64_t lobby_id_low();
    virtual std::uint64_t lobby_id_high();
    virtual std::uint64_t symbol_string(std::int64_t symbol);
    virtual void find_lobby(void* client_lobby, const std::byte uuid[16]);
    virtual void find_social_lobby(void* client_lobby,
                                   const std::byte first_uuid[16],
                                   const std::byte second_uuid[16]);
    virtual std::uint64_t global_uuid_high();
};

void ReturnToLobby(void* self, ReturnLobbyOps& ops) {
    auto* game = static_cast<std::byte*>(self);
    auto* flags = *reinterpret_cast<std::uint64_t**>(game + 0x2da0);

    if (*reinterpret_cast<std::int64_t*>(game + 0x1f10) != 0) {
        *flags |= 0x40000000000ULL;
    } else {
        *flags &= ~0x40000000000ULL;
        ops.write_log(2, "[NETGAME] Return to lobby");

        auto* client_lobby = *reinterpret_cast<void**>(game + 0x40);
        if (!client_lobby) return;

        const auto current_flags = *flags;
        if (((current_flags >> 2) & 1) != 0) {
            ops.leave_game(self);
            ops.switch_to();
            return;
        }
        if ((current_flags & 0x44) == 0x40) {
            ops.begin_online_multiplayer();
            return;
        }

        auto* lobby_gate = *reinterpret_cast<void**>(game + 0x647c8);
        const bool may_return =
            lobby_gate == nullptr || ops.lobby_allows_return(lobby_gate) ||
            ops.party_lobby_unjoinable();

        if (may_return) {
            if (*reinterpret_cast<std::int32_t*>(game) < 6) {
                if ((*flags & 0x24) == 0x20) {
                    auto* parent = *reinterpret_cast<std::byte**>(game + 0x08);
                    std::int64_t spectate_symbol =
                        *reinterpret_cast<std::int64_t*>(parent + 0x8560);
                    if (spectate_symbol == -1)
                        spectate_symbol = -0x349f5b2181e3508dLL;

                    const auto text = ops.symbol_string(spectate_symbol);
                    ops.write_log_symbol(2, "[NETGAME] Spectate '%s'", text);

                    std::memcpy(client_lobby + 0x6a0, &spectate_symbol,
                                sizeof(spectate_symbol));
                    auto* client_game = *reinterpret_cast<std::byte**>(client_lobby + 0x08);
                    auto* profile_flags =
                        *reinterpret_cast<std::uint8_t**>(client_game + 0x2da0);
                    std::uint64_t spectate_id = UINT64_MAX;
                    if ((*profile_flags & 1) != 0) {
                        auto* profile = *reinterpret_cast<std::byte**>(client_game + 0x08);
                        spectate_id = *reinterpret_cast<std::uint64_t*>(profile + 0x948);
                    }
                    *reinterpret_cast<std::uint64_t*>(client_lobby + 0x6a8) = spectate_id;
                    *reinterpret_cast<std::uint32_t*>(client_lobby + 0x6d8) = 1;
                    *reinterpret_cast<std::uint16_t*>(client_lobby + 0x6b8) = 2;

                    std::byte uuid[16]{};
                    *reinterpret_cast<std::uint64_t*>(uuid) = UINT64_MAX;
                    *reinterpret_cast<std::uint64_t*>(uuid + 8) = ops.global_uuid_high();
                    ops.find_lobby(client_lobby, uuid);
                } else {
                    std::byte first_uuid[16]{};
                    std::byte second_uuid[16]{};
                    *reinterpret_cast<std::uint64_t*>(first_uuid) = UINT64_MAX;
                    *reinterpret_cast<std::uint64_t*>(first_uuid + 8) = ops.global_uuid_high();
                    *reinterpret_cast<std::uint64_t*>(second_uuid) = UINT64_MAX;
                    *reinterpret_cast<std::uint64_t*>(second_uuid + 8) = ops.global_uuid_high();
                    ops.find_social_lobby(client_lobby, first_uuid, second_uuid);
                }
            } else {
                const auto low = ops.lobby_id_low();
                const auto high = ops.lobby_id_high();
                *reinterpret_cast<std::uint64_t*>(game + 0x98) = low;
                *reinterpret_cast<std::uint64_t*>(game + 0xa0) = high;
                ops.leave_game(self);
                ops.switch_to();
            }
            *flags &= ~0x40000ULL;
        } else {
            *flags |= 0x40000ULL;
        }
    }
}
