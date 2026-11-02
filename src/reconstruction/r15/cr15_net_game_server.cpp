// NRadEngine::NRadGame::CR15NetGame::Server
// Quest libr15.so VA 0x133ca70; ReVault binary 576.
// ReVault raw parent 100222944, reconstruction node 100600700; grade H: all 19 instructions (0x133ca70 to the ret at 0x133cab8, 76 bytes) checked against the listing and their encodings; the one callee is NRadEngine::CNSLobby::HostPeer (VA 0x1a2d0b4) reached through thunk 0x1027cf0; the globals NRadEngine::SPeer::kSelf and kInvalidPeer are real names read through GOT slots 0x381aae0 and 0x380fc48; no placeholders; syntax-checked.
// @0x133ca70 — NRadEngine::NRadGame::CR15NetGame::Server
/* @confidence: H */

#include <cstddef>
#include <cstdint>

namespace {
// Reads the 8-byte value at base + offset.
std::uint64_t LoadU64(const void* base, std::size_t offset) {
    return *reinterpret_cast<const std::uint64_t*>(
        static_cast<const std::byte*>(base) + offset);
}
}  // namespace

namespace NRadEngine {

// NRadEngine::SPeer::kSelf and NRadEngine::SPeer::kInvalidPeer are 8-byte values. Each is read through a GOT
// slot that holds the address of the value: kSelf through slot 0x381aae0, kInvalidPeer through slot 0x380fc48.
// The _INIT_360 at VA 0x1072810 stores kSelf = 0xfffffffffffffffc.
struct SPeer {
    static const std::uint64_t kSelf;
    static const std::uint64_t kInvalidPeer;
};

// NRadEngine::CNSLobby::HostPeer() const, VA 0x1a2d0b4; reached through thunk 0x1027cf0 by the tail
// branch at 0x133ca98, with this = lobby + 0x18.
class CNSLobby {
public:
    std::uint64_t HostPeer() const;
};

namespace NRadGame {

class CR15NetGame {
public:
    // NRadEngine::NRadGame::CR15NetGame::Server() const, VA 0x133ca70.
    std::uint64_t Server() const;
};

std::uint64_t CR15NetGame::Server() const {
    const void* const self = this;
    // 0x133ca70..0x133ca78: flags = **(u64**)(this + 0x2da0). tbnz tests bit 1 of the low 32 bits of flags.
    const std::uint64_t flags =
        LoadU64(reinterpret_cast<const void*>(LoadU64(self, 0x2da0)), 0);
    // 0x133ca7c..0x133ca88: the lobby branch runs only when bit 1 is clear and (flags & 0x44) != 0x40.
    if (((flags >> 1) & 1) == 0 && (flags & 0x44) != 0x40) {
        // 0x133ca8c..0x133ca90: lobby = *(this + 0x40); a null lobby returns kInvalidPeer.
        const std::uint64_t lobby = LoadU64(self, 0x40);
        if (lobby != 0) {
            // 0x133ca94..0x133ca98: this = lobby + 0x18, then the tail call CNSLobby::HostPeer.
            return reinterpret_cast<const CNSLobby*>(
                reinterpret_cast<const std::byte*>(lobby) + 0x18)->HostPeer();
        }
        // 0x133cab0..0x133cab8: return SPeer::kInvalidPeer.
        return SPeer::kInvalidPeer;
    }
    // 0x133ca9c..0x133caa8: return SPeer::kSelf.
    return SPeer::kSelf;
}

}  // namespace NRadGame
}  // namespace NRadEngine
