// CR15NetGame::ServiceConnectionFailedCB
// Quest libr15.so VA 0x135ec04; ReVault binary 576.
// ReVault raw parent 100223546, reconstruction node 100600457; grade H: every branch and all 13 call sites checked against the disassembly; callee names are ReVault thunk targets, struct access is raw offsets.
// @0x135ec04 — CR15NetGame::ServiceConnectionFailedCB
/* @confidence: H */

#include <cstddef>
#include <cstdint>
#include <cstring>

// Service ids the failure is matched against: 64-bit globals of libr15.so.
extern std::uint64_t g_login_service_id;        // @0x0386d208
extern std::uint64_t g_matchmaking_service_id;  // @0x0386d210

namespace NRadEngine {

namespace NWriteLog {
// NRadEngine::NWriteLog::WriteLog(ELogLevel, unsigned long long, char const*, ...)
// @0x01088c20; called through the stub at 0x0104b960.
void WriteLog(std::int32_t level, std::uint64_t category, const char* format, ...);
}  // namespace NWriteLog

// NRadEngine::CSysNet::HasInternet(); reads no receiver.
// @0x010989fc; called through the stub at 0x0101ce90.
class CSysNet {
public:
    static bool HasInternet();
};

// NRadEngine::CTcpBroadcaster::CertificateError(STcpPeer) const
// @0x026087f4; called through the stub at 0x00ffaa10.
class CTcpBroadcaster {
public:
    // The peer is STcpPeer, passed as its one 64-bit word.
    std::uint64_t CertificateError(std::uint64_t peer) const;
};

namespace NRadGame {

// NRadEngine::NRadGame::CR15NetClientLobby::Cancel(ELobbyStatus)
// @0x01345be8; called through the stub at 0x010436b0.
// NRadEngine::NRadGame::CR15NetClientLobby::SwitchTo(int)
// @0x0133e21c; called through the stub at 0x00ff52f0.
class CR15NetClientLobby {
public:
    void Cancel(std::int32_t status);
    void SwitchTo(std::int32_t status);
};

class CR15NetGame {
public:
    // The message is STcpMsg<STcpConnectionFailedEvent> const&, passed as its address.
    void ServiceConnectionFailedCB(const std::byte* msg);
    // NRadEngine::NRadGame::CR15NetGame::LogInOffline(); @0x0135ed54, stub 0x0103f6e0.
    void LogInOffline();
    // NRadEngine::NRadGame::CR15NetGame::RefreshServiceStatus(); @0x0135ee64, stub 0x00fe8d30.
    void RefreshServiceStatus();
    // NRadEngine::NRadGame::CR15NetGame::SwitchTo(EState); @0x0135b8b4, stub 0x00fe6330.
    void SwitchTo(std::int32_t state);
};

}  // namespace NRadGame
}  // namespace NRadEngine

namespace {

std::byte* LoadPtr(const std::byte* base, std::size_t offset) {
    std::byte* value = nullptr;
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

void StoreU64(std::byte* base, std::size_t offset, std::uint64_t value) {
    std::memcpy(base + offset, &value, sizeof value);
}

// Certificate branch of both services (0x135ed04): CertificateError on the pointer
// at this+0x28c0, with the first word of the message as the peer. -96 or -95.
std::int32_t CertificateSwitchState(const std::byte* self, const std::byte* msg) {
    const auto* const broadcaster =
        reinterpret_cast<const NRadEngine::CTcpBroadcaster*>(LoadPtr(self, 0x28c0));
    const std::uint64_t peer = LoadU64(msg, 0);
    // The caller branches on w0 (cbz w0), so only the low word decides.
    const bool certificate_error =
        static_cast<std::uint32_t>(broadcaster->CertificateError(peer)) != 0;
    return certificate_error ? -96 : -95;
}

}  // namespace

namespace NRadEngine::NRadGame {

void CR15NetGame::ServiceConnectionFailedCB(const std::byte* msg) {
    auto* const self = reinterpret_cast<std::byte*>(this);
    // msg+0x08 holds the event pointer; the service id is the event's first word.
    const std::uint64_t service = LoadU64(LoadPtr(msg, 0x08), 0);

    if (service == g_login_service_id) {
        NWriteLog::WriteLog(8, 0, "[NETGAME] Failed to connect to the login service");  // 0x135ec3c
        const std::uint64_t flags = LoadU64(LoadPtr(self, 0x2da0), 0);
        if ((flags & 0x4) == 0) {
            if ((flags & 0x44) == 0x40) {
                LogInOffline();  // 0x135ec60
                RefreshServiceStatus();  // 0x135ec70 (tail call)
                return;
            }
            if ((flags & (1ull << 41)) == 0 && CSysNet::HasInternet()) {  // 0x135ecf8, 0x135ecfc
                SwitchTo(CertificateSwitchState(self, msg));
            } else {
                SwitchTo(-98);  // 0x135ed1c
            }
        }
    } else if (service == g_matchmaking_service_id) {
        NWriteLog::WriteLog(8, 0, "[NETGAME] Failed to connect to the matchmaking service");  // 0x135ec94
        const std::uint64_t flags = LoadU64(LoadPtr(self, 0x2da0), 0);
        if ((flags & 0x4) == 0) {
            if ((flags & (1ull << 41)) != 0 || !CSysNet::HasInternet()) {  // 0x135eca4, 0x135eca8
                SwitchTo(-98);  // 0x135ed1c
            } else {
                std::byte* const flags_word = LoadPtr(self, 0x2da0);
                StoreU64(flags_word, 0, LoadU64(flags_word, 0) | 0x400000);  // 0x135ecb0
                if (LoadI32(self, 0x00) == 9) {  // 0x135ecc0
                    auto* const lobby = reinterpret_cast<CR15NetClientLobby*>(LoadPtr(self, 0x40));
                    const std::int32_t lobby_status =
                        LoadI32(reinterpret_cast<const std::byte*>(lobby), 0x448);
                    if (lobby_status >= 4 && lobby_status <= 6) {  // 0x135ecdc
                        lobby->Cancel(0xb);  // 0x135ece4
                    } else {
                        lobby->SwitchTo(0xb);  // 0x135ed40 (w1 still 0xb)
                    }
                    RefreshServiceStatus();  // 0x135ecf4 after Cancel, 0x135ed50 after SwitchTo (tail calls)
                    return;
                }
                SwitchTo(CertificateSwitchState(self, msg));  // 0x135ed04
            }
        }
    }
    RefreshServiceStatus();  // 0x135ed34 (tail call)
}

}  // namespace NRadEngine::NRadGame
