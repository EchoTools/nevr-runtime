// NRadEngine::CBroadcaster::Disconnect
// Quest libr15.so VA 0x25ebf68; ReVault binary 576.
// ReVault raw parent 100318027, reconstruction node 100600779; grade H: all 86 instructions checked (0x25ebf68 to the ret at 0x25ec074, then the guard-initialisation tail to the b at 0x25ec0bc; 344 bytes), every callee and the one global (SBroadcasterDisconnectEvent::TypeId()::id) named by ReVault or an import, its init guard compiler-generated, no placeholders.
// @0x25ebf68 — NRadEngine::CBroadcaster::Disconnect
/* @confidence: H */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

// No ReVault layout exists for CAllocator, SBroadcasterData or a peer record; they are read at raw byte offsets.
struct CAllocator;

struct CMemoryContext {
    // NRadEngine::CMemoryContext::CurrentPtr(), 0x12cae90 (thunk 0xffbc50).
    static CAllocator* CurrentPtr();
    // NRadEngine::CMemoryContext::SetCurrent(CAllocator*), 0x12cafac (thunk 0xfd3590).
    static void SetCurrent(CAllocator* allocator);
};

struct CMatSym {
    // NRadEngine::CMatSym::Hash(char const*), 0x13106f8 (thunk 0xfddeb0); ReVault's prototype returns ulong.
    static std::uint64_t Hash(const char* name);
};

struct SBroadcasterDisconnectEvent {
    // The ReVault global is NRadEngine::SBroadcasterDisconnectEvent::TypeId()::id: a function-local static of 8 bytes
    // (stored by `str x0` at 0x25ec0a4) with its init guard, both reached through GOT slots (id 0x3819150, guard
    // 0x3818798). The guard check at 0x25ec004-0x25ec00c, __cxa_guard_acquire at 0x25ec080 (import thunk 0xff6770)
    // and __cxa_guard_release at 0x25ec0b0 (import thunk 0x1051c60) are what this static local compiles to.
    static std::uint64_t TypeId() {
        static const std::uint64_t id = CMatSym::Hash("SBroadcasterDisconnectEvent");  // string at 0x2ca2b8e
        return id;
    }
};

// Layout of CBroadcaster is not in ReVault. Its first word (+0x00) is a pointer to the SBroadcasterData that
// SBroadcasterData::CleanupPeers (merged cleanup_peers.cpp) works on; the class carries no data members here.
// An SPeer, a CMatSym and a CSymbol64 are 8-byte values: they travel in one x register or one 8-byte stack slot.
class CBroadcaster {
public:
    // NRadEngine::CBroadcaster::Disconnect(SPeer), 0x25ebf68 (thunk 0x1018f00).
    void Disconnect(std::uint64_t peer);
    // NRadEngine::CBroadcaster::DestroyPeer(SPeer), 0x25ec0c0 (thunk 0x104f340).
    void DestroyPeer(std::uint64_t peer);
    // NRadEngine::CBroadcaster::Send(CMatSym, int, void const*, unsigned long long, void const*, unsigned long long,
    //     SPeer, unsigned long long, float, CSymbol64), 0x25e7e94 (thunk 0x1002de0). ReVault records no parameter names.
    void Send(std::uint64_t type, int arg1, const void* arg2, unsigned long long arg3, const void* arg4,
              unsigned long long arg5, std::uint64_t peer, unsigned long long arg6, float arg7,
              std::uint64_t symbol);
};

namespace {
template <typename T>
T load(const std::byte* base, std::size_t offset) {
    T value;
    std::memcpy(&value, base + offset, sizeof(value));
    return value;
}

template <typename T>
void store(std::byte* base, std::size_t offset, T value) {
    std::memcpy(base + offset, &value, sizeof(value));
}
}  // namespace

void CBroadcaster::Disconnect(std::uint64_t peer) {
    std::byte* const self = reinterpret_cast<std::byte*>(this);

    // 0x25ebf78-0x25ebf84: data = *this (SBroadcasterData); the allocator is the first word of data.
    CAllocator* const data_allocator = load<CAllocator*>(load<const std::byte*>(self, 0), 0);
    // 0x25ebf88-0x25ebf94: remember the current allocator, make the data's allocator current.
    CAllocator* const previous_allocator = CMemoryContext::CurrentPtr();
    CMemoryContext::SetCurrent(data_allocator);

    // 0x25ebf98: data is read from *this again after the two calls.
    std::byte* const data = load<std::byte*>(self, 0);
    // 0x25ebf9c-0x25ebfa4: data+0x500 points to a table of u32 slot indices, entered at the entry peer & 0xfff.
    const std::byte* link = load<const std::byte*>(data, 0x500) + (peer & 0xfffU) * sizeof(std::uint32_t);

    // 0x25ebfac-0x25ebfd8: walk the chain. A slot index of 0xffffffff ends it. The index is split by data+0x560 (slots
    // per chunk; udiv by zero gives 0) into a chunk of the table at data+0x518 and a slot in it; a peer record is 0x948
    // bytes, its key is the u64 at +0x10, and its +0x00 u32 is the next index of the chain.
    std::byte* record = nullptr;
    for (;;) {
        const auto index = load<std::uint32_t>(link, 0);
        if (index == UINT32_MAX) {
            break;
        }
        const auto per_chunk = load<std::uint32_t>(data, 0x560);
        const auto chunks = load<const std::byte*>(data, 0x518);
        const std::uint32_t chunk = per_chunk == 0 ? 0U : index / per_chunk;
        std::byte* const chunk_base = load<std::byte*>(chunks, static_cast<std::size_t>(chunk) * 8);
        const std::uint32_t slot = index - chunk * per_chunk;
        std::byte* const candidate = chunk_base + static_cast<std::size_t>(slot) * 0x948;
        if (load<std::uint64_t>(candidate, 0x10) == peer) {
            record = candidate;
            break;
        }
        link = candidate;
    }

    if (record != nullptr) {
        // 0x25ebfe4: the record's state word is the u32 at +0x244.
        const auto state = load<std::uint32_t>(record, 0x244);
        if ((state & 8U) == 0) {  // 0x25ebfe8: tbnz bit 3, exit
            if ((state & 2U) != 0) {
                // 0x25ec050-0x25ec058: bit 1 set, DestroyPeer(this, peer).
                DestroyPeer(peer);
            } else if ((state & 4U) != 0) {  // 0x25ebff0: tbz bit 2, exit
                // 0x25ebffc-0x25ec03c: Send(this, SBroadcasterDisconnectEvent::TypeId(), 0, &local, 0, nullptr, 0,
                // peer, ~0, 1.0f, ~0). The 8 bytes at sp+0x18 that x3 points to are never written (x4 is 0).
                std::uint64_t payload;
                Send(SBroadcasterDisconnectEvent::TypeId(), 0, &payload, 0, nullptr, 0, peer,
                     0xffffffffffffffffULL, 1.0F, 0xffffffffffffffffULL);
                // 0x25ec040-0x25ec048: the state word is read again after Send and bit 3 is set.
                store(record, 0x244, load<std::uint32_t>(record, 0x244) | 8U);
            }
        }
    }

    // 0x25ec05c-0x25ec060: restore the allocator that was current on entry.
    CMemoryContext::SetCurrent(previous_allocator);
}

}  // namespace NRadEngine
