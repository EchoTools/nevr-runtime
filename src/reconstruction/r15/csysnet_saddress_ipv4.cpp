// Quest libr15.so VA 0x1094448; ReVault binary libr15.so.
// ReVault raw parent 100208251, reconstruction node 100600600; grade H.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace NRadEngine::CSysNet {

class SAddress {
public:
    std::uint32_t Ipv4() const;
};

std::uint32_t SAddress::Ipv4() const {
    std::uint32_t address = 0;
    std::memcpy(&address, reinterpret_cast<const std::byte*>(this) + 4,
                sizeof(address));
    return address;
}

}  // namespace NRadEngine::CSysNet
