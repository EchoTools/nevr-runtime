// CR15NetGame::ServiceConnectionFailedCB(STcpMsg<STcpConnectionFailedEvent> const&)
// Quest libr15.so: VA 0x135ec04 (RVA 0x125ec04), binary 576.
// PC echovr.exe: no matching function located in the project-wide search.
// ReVault node 100223546 (raw); reconstructed grade M; object fields/helpers opaque.
#include <cstddef>
#include <cstdint>
namespace q15t::r15::reconstruction {
struct NetGame { std::byte opaque[0x2dc0]; };
struct Message { std::byte opaque[0x20]; };
struct Ops {
    virtual void log(const char*);
    virtual bool has_internet();
    virtual bool certificate_error(const Message*);
    virtual void login_offline();
    virtual void refresh_service_status();
    virtual void switch_to(int);
    virtual void cancel_lobby(void*, int);
    virtual void switch_lobby(void*);
};
// Service IDs are supplied by the binary's globals: login and matchmaking.
void ServiceConnectionFailedCB(NetGame* game, const Message* msg,
                               std::uint64_t service_id, std::uint64_t login_id,
                               std::uint64_t matchmaking_id, Ops& ops) {
    auto* b = reinterpret_cast<std::byte*>(game);
    auto& flags = *reinterpret_cast<std::uint64_t*>(b + 0x2da0);
    if (service_id == login_id) {
        ops.log("[NETGAME] Failed to connect to the login service");
        if ((flags & 4) == 0) {
            if ((flags & 0x44) == 0x40) {
                ops.login_offline(); ops.refresh_service_status(); return;
            }
            if ((flags & (1ull << 41)) == 0 && ops.has_internet()) {
                ops.switch_to(ops.certificate_error(msg) ? -96 : -95);
            } else ops.switch_to(-98);
        }
    } else if (service_id == matchmaking_id) {
        ops.log("[NETGAME] Failed to connect to the matchmaking service");
        if ((flags & 4) == 0) {
            if ((flags & (1ull << 41)) != 0 || !ops.has_internet()) ops.switch_to(-98);
            else {
                flags |= 0x400000;
                if (*reinterpret_cast<std::int32_t*>(b) == 9) {
                    void* lobby = *reinterpret_cast<void**>(b + 0x40);
                    auto state = *reinterpret_cast<std::int32_t*>(static_cast<std::byte*>(lobby) + 0x448);
                    if (state >= 4 && state <= 6) ops.cancel_lobby(lobby, 0xb);
                    else ops.switch_lobby(lobby);
                    ops.refresh_service_status(); return;
                }
                ops.switch_to(ops.certificate_error(msg) ? -96 : -95);
            }
        }
    }
    ops.refresh_service_status();
}
} // namespace q15t::r15::reconstruction
