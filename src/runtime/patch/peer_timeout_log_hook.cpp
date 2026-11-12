#include "runtime/patch/peer_timeout_log_hook.h"

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "nevr/pc/nevr_common.h"  // N97: the one ValidatePrologue
#include "runtime/lifecycle/cli.h"  // g_isServer
#include "core/logging.h"
#include "runtime/compat/peer_timeout_log.h"
#include "runtime/hook/addresses.h"
#include "runtime/hook/patching.h"

namespace nevr_peer_timeout_hook {

DestroyPeerFn OriginalDestroyPeer = nullptr;
EchoVR::BroadcasterSendFunc* OriginalBroadcasterSend = nullptr;

namespace {

// The peer record's layout as the game's DestroyPeer reads it (the disassembly; see DestroyPeer's reconstruction).
constexpr std::size_t kBroadcasterChunksOffset = 0x518;   // chunk table
constexpr std::size_t kBroadcasterCapacityOffset = 0x560;  // entries per chunk
constexpr std::size_t kBroadcasterNowOffset = 0x478;       // the broadcaster's clock, microseconds
constexpr std::size_t kPeerLastReceiveOffset = 0x228;      // the game's last-received stamp, microseconds
constexpr std::size_t kPeerAddressOffset = 0x1a0;          // the peer's address text
constexpr std::size_t kPeerRecordSize = 0x948;

std::mutex g_mutex;
nevr_peer_timeout_log::SendTracker g_sendTracker;

std::uint64_t NowMicros() {
    using std::chrono::duration_cast;
    using std::chrono::microseconds;
    using std::chrono::steady_clock;
    return static_cast<std::uint64_t>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

// Refuses a hook whose bytes differ from the build's; writes the expected and actual bytes (N120, Rule 6).
bool PrologueMatches(const char* name, std::uintptr_t va, const std::uint8_t* want, std::size_t count) {
    const auto* site = reinterpret_cast<const BYTE*>(va);
    if (nevr::ValidatePrologue(site, want, count)) return true;
    constexpr char kHex[] = "0123456789abcdef";
    constexpr unsigned kNibbleBits = 4;
    constexpr unsigned kNibbleMask = 0xF;
    std::string expected;
    std::string actual;
    for (std::size_t i = 0; i < count; ++i) {
        expected += kHex[(want[i] >> kNibbleBits) & kNibbleMask];
        expected += kHex[want[i] & kNibbleMask];
        actual += kHex[(site[i] >> kNibbleBits) & kNibbleMask];
        actual += kHex[site[i] & kNibbleMask];
    }
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.GAMESERVER] peer timeout log hook refused name=%s va=0x%llX expected=%s actual=%s", name,
        static_cast<unsigned long long>(va), expected.c_str(), actual.c_str());
    return false;
}

}  // namespace

bool Install(std::uintptr_t gameBase, bool isServer) {
    if (isServer) {
        const std::uintptr_t va = gameBase + nevr_patch_addresses::DESTROY_PEER;
        if (!PrologueMatches("DestroyPeer", va, kDestroyPeerPrologue.data(), kDestroyPeerPrologue.size())) return false;
        OriginalDestroyPeer = reinterpret_cast<DestroyPeerFn>(va);
        if (!PatchDetour(&OriginalDestroyPeer, reinterpret_cast<PVOID>(DestroyPeerHook), "PeerTimeoutDestroyPeer")) {
            Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] peer timeout log hook not installed name=DestroyPeer va=0x%llX",
                static_cast<unsigned long long>(va));
            return false;
        }
        Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] peer timeout log installed name=DestroyPeer va=0x%llX",
            static_cast<unsigned long long>(va));
        return true;
    }
    const std::uintptr_t va = gameBase + nevr_patch_addresses::BROADCASTER_SEND;
    if (!PrologueMatches("BroadcasterSend", va, kBroadcasterSendPrologue.data(), kBroadcasterSendPrologue.size())) {
        return false;
    }
    OriginalBroadcasterSend = reinterpret_cast<EchoVR::BroadcasterSendFunc*>(va);
    if (!PatchDetour(&OriginalBroadcasterSend, reinterpret_cast<PVOID>(BroadcasterSendHook), "PeerTimeoutBroadcasterSend")) {
        Log(EchoVR::LogLevel::Warning, "[NEVR.WS] peer timeout log hook not installed name=BroadcasterSend va=0x%llX",
            static_cast<unsigned long long>(va));
        return false;
    }
    Log(EchoVR::LogLevel::Info, "[NEVR.WS] peer timeout log installed name=BroadcasterSend va=0x%llX",
        static_cast<unsigned long long>(va));
    return true;
}

void DestroyPeerHook(std::int64_t* self, std::uint64_t index, const char* reason) {
    if (reason != nullptr && std::strcmp(reason, kCommunicationTimeoutReason) == 0) {
        // The same arithmetic as the game's DestroyPeer: the peer's slot is index in the chunk table.
        auto* bytes = reinterpret_cast<std::uint8_t*>(self);
        const auto capacity = *reinterpret_cast<const std::uint32_t*>(bytes + kBroadcasterCapacityOffset);
        if (capacity == 0) {
            Log(EchoVR::LogLevel::Warning, "[NEVR.GAMESERVER] peer communication timeout not logged slot=%llu capacity=0",
                static_cast<unsigned long long>(index));
            OriginalDestroyPeer(self, index, reason);
            return;
        }
        const auto bucket = index / capacity;
        const auto slot = index % capacity;
        auto* chunk = *reinterpret_cast<std::uint8_t* const*>(
            *reinterpret_cast<std::uint8_t* const*>(bytes + kBroadcasterChunksOffset) + bucket * sizeof(void*));
        const auto* peer = chunk + slot * kPeerRecordSize;
        nevr_peer_timeout_log::CommTimeoutRecord record{};
        record.slot = index;
        record.address = reinterpret_cast<const char*>(peer + kPeerAddressOffset);
        record.last_receive_us = *reinterpret_cast<const std::uint64_t*>(peer + kPeerLastReceiveOffset);
        record.now_us = *reinterpret_cast<const std::uint64_t*>(bytes + kBroadcasterNowOffset);
        Log(EchoVR::LogLevel::Warning, "%s",
            nevr_peer_timeout_log::FormatCommunicationTimeout(record).c_str());
    }
    OriginalDestroyPeer(self, index, reason);
}

INT32 BroadcasterSendHook(EchoVR::Broadcaster* broadcaster, EchoVR::SymbolId messageId, INT32 mbThreadPriority,
                          VOID* item, UINT64 size, VOID* buffer, UINT64 bufferLen, EchoVR::Peer peer, UINT64 dest,
                          FLOAT priority, EchoVR::SymbolId unk) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_sendTracker.RecordSend(NowMicros());
    }
    return OriginalBroadcasterSend(broadcaster, messageId, mbThreadPriority, item, size, buffer, bufferLen, peer, dest,
                                   priority, unk);
}

void OnNetGameState(bool inGame) {
    if (g_isServer) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!inGame) {
        g_sendTracker.LeaveInGame();
        return;
    }
    g_sendTracker.EnterInGame(NowMicros());
    Log(EchoVR::LogLevel::Info, "%s", nevr_peer_timeout_log::FormatInGameSendState(g_sendTracker).c_str());
}

void CheckNoSend() {
    if (g_isServer) return;
    std::string line;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto silent = g_sendTracker.CheckNoSend(NowMicros());
        if (!silent.has_value()) return;
        line = nevr_peer_timeout_log::FormatNoSendWarning(g_sendTracker, *silent);
    }
    Log(EchoVR::LogLevel::Warning, "%s", line.c_str());
}

}  // namespace nevr_peer_timeout_hook
