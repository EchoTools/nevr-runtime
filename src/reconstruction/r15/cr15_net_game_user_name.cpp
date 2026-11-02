// NRadEngine::NRadGame::CR15NetGame::UserName
// Quest libr15.so VA 0x1363a50; ReVault binary 576.
// ReVault raw parent 100223628, reconstruction node 100600698; grade M: all 20 instructions traced, every name real, no placeholders; parameter std::uint32_t per the merged declaration, ReVault's demangled name says SR15NetUserId; tail-call target is Ghidra's thunk resolution via GOT slot 0x37eaed8; syntax-checked.
// @0x1363a50 — NRadEngine::NRadGame::CR15NetGame::UserName
/* @confidence: M */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine::NRadGame {

class CR15NetGame {
  public:
    // NRadEngine::NRadGame::CR15NetGame::UserName(unsigned int); ReVault 0x1363a50 (thunk 0x10094c0). The
    // argument is the record's +0x20 word; the result is the display name for the log lines.
    const char* UserName(std::uint32_t user) const;

    // NRadEngine::NRadGame::CR15NetGame::UserName(unsigned short) const; ReVault 0x134f73c (thunk 0x1026fe0).
    // Tail-called from 0x1363a90 with the slot value, not the user id, as its second argument.
    const char* UserName(std::uint16_t slot) const;
};

}  // namespace NRadEngine::NRadGame

namespace {

// Slot table at this+0x138: entry i (0..15) holds a uint16 value at 0x138+4i and a uint16 serial at 0x13a+4i.
// 0xffff in either word is the empty mark.
constexpr std::size_t kSlotTableOffset = 0x138;
constexpr std::size_t kSlotStride = 4;
constexpr std::size_t kSerialOffset = 2;
constexpr std::uint16_t kEmptyWord = 0xffff;
// Bits 4..15 of the id must be zero, so only entries 0..15 are reachable.
constexpr std::uint32_t kHighIndexBitsMask = 0xfff0u;
// Returned for every rejected id; the string sits in libr15 rodata at 0x2caa2a7.
constexpr char kInvalidName[] = "[INVALID]";

// Raw load at a byte offset; the object layout is not recovered beyond the slot table.
template <typename T>
T LoadAt(const void* base, std::size_t offset) {
    T value{};
    std::memcpy(&value, static_cast<const std::byte*>(base) + offset, sizeof value);
    return value;
}

}  // namespace

namespace NRadEngine::NRadGame {

const char* CR15NetGame::UserName(std::uint32_t user) const {
    // tst x1, #0xfff0
    if ((user & kHighIndexBitsMask) != 0) {
        return kInvalidName;
    }
    // ubfx x8, x1, #16, #16: the serial is bits 16..31 of the id.
    const auto serial = static_cast<std::uint16_t>(user >> 16);
    if (serial == kEmptyWord) {
        return kInvalidName;
    }
    // and x8, x1, #0xffff: the index is bits 0..15 of the id, 0..15 after the mask above.
    const std::size_t entry = kSlotTableOffset + static_cast<std::size_t>(user & 0xffffu) * kSlotStride;
    // ldrh w9, [this + index*4 + 0x13a]: the serial stored with the entry must equal the id's serial.
    if (serial != LoadAt<std::uint16_t>(this, entry + kSerialOffset)) {
        return kInvalidName;
    }
    // ldrh w1, [this + index*4 + 0x138]: the entry's value; 0xffff is empty.
    const auto slot = LoadAt<std::uint16_t>(this, entry);
    if (slot == kEmptyWord) {
        return kInvalidName;
    }
    // b 0x1026fe0: tail call with this unchanged and the slot value in w1.
    return UserName(slot);
}

}  // namespace NRadEngine::NRadGame
