// NRadEngine::CBroadcaster::Address
// Quest libr15.so VA 0x25ecc24; ReVault binary 576.
// ReVault raw parent 100318041, reconstruction node 100600778; grade H: all 57 instructions (0x25ecc24 to the ret at 0x25ecd04, 228 bytes) checked against the listing; no callees; NRadEngine::SPeer::kSelf and NRadEngine::CSysNet::SAddress::kInvalid are real mangled names read through GOT slots 0x381aae0 and 0x381d538; the broadcaster block and peer entries are raw byte offsets; no placeholders.
// @0x25ecc24 — NRadEngine::CBroadcaster::Address
/* @confidence: H */
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine {

namespace {

// Reads a T at base + offset. The offsets are raw: no ReVault layout names these members.
template <typename T>
T load(const std::byte* base, std::size_t offset) {
    T value{};
    std::memcpy(&value, base + offset, sizeof(value));
    return value;
}

}  // namespace

// NRadEngine::SPeer. A peer is one 64-bit word, as the merged tree passes it (cr15_net_damageable_cs_heal.cpp
// takes std::uint64_t peer); the decompiler shows it as ulong param_3, in x1.
struct SPeer {
    // NRadEngine::SPeer::kSelf, an 8-byte value; the GOT slot at 0x381aae0 holds its address.
    static const std::uint64_t kSelf;
};

namespace CSysNet {

// NRadEngine::CSysNet::SAddress: a 128-byte record returned through the hidden pointer (x8).
class SAddress {
public:
    // NRadEngine::CSysNet::SAddress::kInvalid, 0x80 bytes; the GOT slot at 0x381d538 holds its address.
    static const std::byte kInvalid[0x80];

    alignas(8) std::byte storage[0x80];
};
static_assert(sizeof(SAddress) == 0x80);

}  // namespace CSysNet

// NRadEngine::CBroadcaster. The word at offset 0 of the object is a pointer to the broadcaster's data
// block (ldr x9, [x0] at 0x25ecc30); the block has no ReVault layout, so it is read at raw offsets.
class CBroadcaster {
public:
    // NRadEngine::CBroadcaster::Address(NRadEngine::SPeer) const, VA 0x25ecc24.
    CSysNet::SAddress Address(std::uint64_t peer) const;
};

CSysNet::SAddress CBroadcaster::Address(std::uint64_t peer) const {
    CSysNet::SAddress result{};
    const auto* self = reinterpret_cast<const std::byte*>(this);
    // data = *this (0x25ecc30).
    const auto* data = load<const std::byte*>(self, 0);

    // peer == SPeer::kSelf (0x25ecc34): copy the 128 bytes at data+0x280 (0x25ecc3c to 0x25ecc5c).
    if (peer == SPeer::kSelf) {
        std::memcpy(result.storage, data + 0x280, sizeof(result.storage));
        return result;
    }

    // The pointer at data+0x500 addresses the bucket heads: 4096 uint32 indexes, chosen by peer & 0xfff
    // (0x25ecc60 to 0x25ecc68).
    const auto* buckets = load<const std::byte*>(data, 0x500);
    std::uint32_t index = load<std::uint32_t>(buckets, static_cast<std::size_t>(peer & 0xfffU) * 4);

    for (;;) {
        // index == 0xffffffff: the chain is empty; copy kInvalid (0x25eccdc to 0x25ecd04).
        if (index == 0xffffffffU) {
            std::memcpy(result.storage, CSysNet::SAddress::kInvalid, sizeof(result.storage));
            return result;
        }

        // chunk = index / capacity, capacity at data+0x560; a zero capacity gives chunk 0 (AArch64 udiv
        // yields 0), and slot is index - chunk * capacity (0x25ecc7c to 0x25ecc8c).
        const auto capacity = load<std::uint32_t>(data, 0x560);
        const std::uint32_t chunk = capacity == 0 ? 0U : index / capacity;
        const std::uint32_t slot = index - chunk * capacity;

        // The chunk-pointer table at data+0x518 holds the base of each chunk; entries are 0x948 bytes
        // (0x25ecc88 to 0x25ecc90).
        const auto* chunks = load<const std::byte*>(data, 0x518);
        const auto* chunk_base = load<const std::byte*>(chunks, static_cast<std::size_t>(chunk) * 8);
        const std::byte* entry = chunk_base + static_cast<std::size_t>(slot) * 0x948;

        // entry+0x10 is the peer key. On a match copy the 128 bytes at entry+0x18 (0x25ecca0 to 0x25eccd8).
        if (load<std::uint64_t>(entry, 0x10) == peer) {
            std::memcpy(result.storage, entry + 0x18, sizeof(result.storage));
            return result;
        }

        // entry+0x00 is the next index in the chain (0x25ecc70, ldr w11, [x11]).
        index = load<std::uint32_t>(entry, 0x00);
    }
}

}  // namespace NRadEngine
