// NRadEngine::SBroadcasterData::CleanupPeers
// Quest libr15.so VA 0x25ef9b0; ReVault binary 576.
// ReVault raw parent 100318060, reconstruction node 100600273; grade M: the Quest raw supplies the authoritative fields and branches, and the PC code confirms the shared broadcaster behavior.
// @0x25ef9b0 — NRadEngine::SBroadcasterData::CleanupPeers
// @confidence: M — Quest raw supplies the authoritative fields and branches; PC code confirms the shared broadcaster behavior.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace NRadEngine {
namespace CMemory {
std::int64_t Compare(const void* lhs, const void* rhs, std::uint64_t count);
}
namespace CSysNet::SAddress {
extern const std::byte kInvalid[0x80];
}
namespace CTimer {
float DeltaSecondsF(const void* timer);
std::uint64_t MilliSeconds();
std::int64_t DeltaMicroSeconds(const void* timer);
}
namespace CBroadcaster {
void Disconnect(void* self, std::uint64_t peer);
}
namespace NWriteLog {
void WriteLog(int level, int channel, const char* format, ...);
}
namespace SBroadcasterData {
void DestroyPeer(void* self, std::uint64_t index);
void* DefragmentIncoming(void* self, void* peer);

namespace {
template <typename T>
T load(const std::byte* base, std::size_t offset) {
    T value{};
    std::memcpy(&value, base + offset, sizeof(value));
    return value;
}

template <typename T>
void store(std::byte* base, std::size_t offset, T value) {
    std::memcpy(base + offset, &value, sizeof(value));
}

std::uint32_t reduce_sequence(std::uint32_t value) {
    const auto folded = (value / 0x3ffU + ((value - value / 0x3ffU) >> 1U)) >> 9U;
    return value - folded * 0x3ffU;
}
}

void CleanupPeers(void* self) {
    auto* broadcaster = static_cast<std::byte*>(self);
    auto index = load<std::uint32_t>(broadcaster, 0x554);
    while (index != UINT32_MAX) {
        const auto capacity = load<std::uint32_t>(broadcaster, 0x560);
        const auto bucket = capacity == 0 ? 0U : index / capacity;
        auto** chunks = reinterpret_cast<std::byte**>(
            load<std::byte*>(broadcaster, 0x518));
        const auto slot = capacity == 0 ? 0U : index - bucket * capacity;
        auto* peer = chunks[bucket] + static_cast<std::size_t>(slot) * 0x948;
        const auto next = load<std::uint32_t>(peer, 0x08);
        const auto peer_id = load<std::uint64_t>(peer, 0x198);
        const auto* address = peer + 0x18;

        if (CMemory::Compare(address, CSysNet::SAddress::kInvalid, 0x80) == 0) {
            DestroyPeer(self, index);
            index = next;
            continue;
        }

        auto flags = load<std::uint32_t>(peer, 0x244);
        if ((flags & 0x10010000U) == 0x00010000U || (flags & 7U) == 0U) {
            DestroyPeer(self, index);
            index = next;
            continue;
        }

        const auto elapsed = load<std::uint64_t>(broadcaster, 0x478) -
                             load<std::uint64_t>(peer, 0x228);
        if ((flags & 0x10U) != 0U) {
            if ((flags & 0x8000U) == 0U) {
                if (elapsed >= load<std::uint64_t>(broadcaster, 0x4a0)) {
                    DestroyPeer(self, index);
                    index = next;
                    continue;
                }
            } else {
                DestroyPeer(self, index);
                index = next;
                continue;
            }
        }

        if ((flags & 0x10U) == 0U) {
            if ((flags & 3U) != 0U && elapsed >= load<std::uint64_t>(broadcaster, 0x498)) {
                DestroyPeer(self, index);
                index = next;
                continue;
            }
        }

        auto* timer = load<std::byte*>(peer, 0x270);
        if (static_cast<float>(load<std::uint64_t>(broadcaster, 0x490)) * 1.0e-6F <=
            load<float>(timer, 0x304)) {
            NWriteLog::WriteLog(2, 0, "%s: Disconnect Peer %llu %s (decode recovery timeout)",
                reinterpret_cast<const char*>(broadcaster + 0x380),
                static_cast<unsigned long long>(peer_id),
                reinterpret_cast<const char*>(peer + 0x1a0));
            CBroadcaster::Disconnect(load<void*>(broadcaster, 0x08), peer_id);
            index = next;
            continue;
        }

        if (elapsed >= load<std::uint64_t>(broadcaster, 0x488)) {
            DestroyPeer(self, index);
            index = next;
            continue;
        }

        const auto delta_seconds = CTimer::DeltaSecondsF(timer + 0x220);
        store(timer, 0x230, CTimer::MilliSeconds());
        const auto timer_flags = load<std::uint32_t>(timer, 0x34c);
        if ((timer_flags & 4U) == 0U) {
            const float target = 1.0F - load<float>(timer, 0x2f8);
            float estimate;
            if (std::fabs(delta_seconds) <= 4.7683716e-7F) {
                estimate = load<float>(timer, 0x2fc);
                store(timer, 0x300, 0.0F);
            } else {
                const float decay = 1.0F / std::exp(delta_seconds);
                const float error = load<float>(timer, 0x2fc) - target;
                const float slope = (load<float>(timer, 0x300) + error) * delta_seconds;
                estimate = target + decay * (slope + error);
                store(timer, 0x300, (load<float>(timer, 0x300) - slope) * decay);
            }
            store(timer, 0x2fc, estimate);
            if (estimate < 0.5F) store(timer, 0x34c, timer_flags | 2U);
        } else {
            store(timer, 0x304, load<float>(timer, 0x304) + delta_seconds);
        }

        DefragmentIncoming(self, peer + 0x18);
        flags = load<std::uint32_t>(peer, 0x244);
        if ((~flags & 0x07fe0000U) != 0U) {
            const float interval = load<float>(peer, 0x248);
            const float sample = static_cast<float>(elapsed) * 0.001F;
            if (interval < sample) {
                const float coefficient = load<float>(broadcaster, 0x458);
                store(peer, 0x248, interval * (1.0F - coefficient) + sample * coefficient);
            }

            if ((flags & 0x08000000U) == 0U) {
                const auto timer_now = CTimer::DeltaMicroSeconds(broadcaster + 0x260);
                if (timer_now - load<std::uint64_t>(peer, 0x250) < 0x3d091U) {
                    const auto sent = reduce_sequence((flags >> 0x11U) & 0x3ffU);
                    const auto received = reduce_sequence(
                        static_cast<std::uint32_t>((load<std::uint64_t>(peer, 0x238) >> 0x1fU) & 0x3ffU));
                    const auto distance = sent - received;
                    if ((sent < received || distance == 0U)
                            ? (((sent & 0xffffU) < (received & 0xffffU)) &&
                               (received - sent <= distance + 0x3ffU))
                            : ((sent ^ 0x3ffU) + received < distance)) {
                        if ((load<std::uint8_t>(broadcaster, 0x449) & 8U) != 0U) {
                            char message[512]{};
                            std::snprintf(message, sizeof(message),
                                "Ping message expired (was message index %llu, currently on index %llu)",
                                static_cast<unsigned long long>((flags >> 0x11U) & 0x3ffU),
                                static_cast<unsigned long long>(
                                    (load<std::uint64_t>(peer, 0x238) >> 0x1fU) & 0x3ffU));
                            using LogCallback = void (*)(void*, void*, const char*);
                            const auto callback = load<LogCallback>(broadcaster, 0x250);
                            if (callback == nullptr) {
                                NWriteLog::WriteLog(2, 0, "%s", message);
                            } else {
                                callback(load<void*>(broadcaster, 0x238), broadcaster + 0x240, message);
                            }
                        }
                        store(peer, 0x244, flags | 0x07fe0000U);
                    }
                } else {
                    if ((load<std::uint8_t>(broadcaster, 0x449) & 8U) != 0U) {
                        char message[512]{};
                        std::snprintf(message, sizeof(message),
                            "Ping message expired (was message index %llu, currently on index %llu)",
                            static_cast<unsigned long long>((flags >> 0x11U) & 0x3ffU),
                            static_cast<unsigned long long>(
                                (load<std::uint64_t>(peer, 0x238) >> 0x1fU) & 0x3ffU));
                        using LogCallback = void (*)(void*, void*, const char*);
                        const auto callback = load<LogCallback>(broadcaster, 0x250);
                        if (callback == nullptr) {
                            NWriteLog::WriteLog(2, 0, "%s", message);
                        } else {
                            callback(load<void*>(broadcaster, 0x238), broadcaster + 0x240, message);
                        }
                    }
                    store(peer, 0x244, flags | 0x07fe0000U);
                }
            }
        }
        index = next;
    }
}
}  // namespace SBroadcasterData
}  // namespace NRadEngine
