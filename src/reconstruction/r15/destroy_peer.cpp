// NRadEngine::SBroadcasterData::DestroyPeer(index)
// Quest libr15.so VA 0x2612cdc; ReVault binary 576.
// ReVault raw parent 100318541, reconstruction node 100600271; grade M: the Quest raw establishes the peer lookup, state-dependent event, listener dispatch and record cleanup.
// @0x2612cdc — NRadEngine::SBroadcasterData::DestroyPeer(index)
// @confidence: M — Quest raw establishes the peer lookup, state-dependent event, listener dispatch, and record cleanup.
#include <cstddef>
#include <cstdint>

namespace NRadEngine {
namespace NWriteLog {
void WriteLog(int level, int channel, const char* format, ...);
}
namespace CMatSym { std::uint64_t Hash(const char* name); }
namespace SBroadcasterData {
void InvokeListeners(void* self, std::uint64_t event_type, std::uint64_t target,
                     std::uint64_t peer_id, const void* event, std::uint64_t flags);
void DestroyPeer(void* self, const std::byte* peer);

void DestroyPeer(void* self, std::uint64_t index) {
    const auto connect_failed_event_type = [] {
        static const auto id = CMatSym::Hash("SBroadcasterConnectFailedEvent");
        return id;
    };
    const auto disconnect_event_type = [] {
        static const auto id = CMatSym::Hash("SBroadcasterDisconnectEvent");
        return id;
    };
    auto* bytes = static_cast<std::byte*>(self);
    const auto capacity = *reinterpret_cast<std::uint32_t*>(bytes + 0x560);
    const auto bucket = capacity == 0 ? 0U : static_cast<std::uint32_t>(index / capacity);
    const auto slot = capacity == 0 ? 0U : static_cast<std::uint32_t>(index % capacity);
    auto** chunks = reinterpret_cast<std::byte**>(
        *reinterpret_cast<std::byte**>(bytes + 0x518));
    auto* peer = chunks[bucket] + static_cast<std::size_t>(slot) * 0x948;
    const auto peer_id = *reinterpret_cast<std::uint64_t*>(peer + 0x198);
    NWriteLog::WriteLog(2, 0,
        "%s: Destroy Peer %llu %s (%s), max send interval: %u, rcvd: %u",
        reinterpret_cast<const char*>(bytes + 0x380),
        static_cast<unsigned long long>(peer_id),
        reinterpret_cast<const char*>(peer + 0x1a0), "disconnected",
        *reinterpret_cast<std::uint32_t*>(peer + 0x230),
        *reinterpret_cast<std::uint32_t*>(peer + 0x234));

    const auto state = *reinterpret_cast<std::uint32_t*>(peer + 0x244);
    std::uint32_t event_storage = 0;
    if ((state & 3U) == 0) {
        if ((state & 0x10004U) == 4U) {
            InvokeListeners(self, disconnect_event_type(),
                            UINT64_MAX, peer_id, &event_storage, 1);
        }
    } else {
        InvokeListeners(self, connect_failed_event_type(),
                        UINT64_MAX, peer_id, &event_storage, 1);
    }
    DestroyPeer(self, peer);
}
}  // namespace SBroadcasterData
}  // namespace NRadEngine
