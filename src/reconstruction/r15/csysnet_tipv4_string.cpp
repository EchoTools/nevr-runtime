// Quest libr15.so VA 0x1094450; ReVault binary libr15.so.
// ReVault raw parent 100208252, reconstruction node 100600602; grade H.
#include <array>
#include <cstdint>

// @0x1094450 — NRadEngine::CSysNet::TIpv4::String
/* @confidence: H */

extern "C" const char* inet_ntop(int address_family, const void* address,
                                char* destination, std::uint32_t size);

constexpr int kAfInet = 2;

namespace NRadEngine::CMemory {
void Fill(void* destination, unsigned char value, std::uint64_t count);
}

namespace NRadEngine::CSysNet {

class TIpv4 {
public:
    std::array<char, 16> String() const;
};

static_assert(sizeof(std::array<char, 16>) == 0x10);

std::array<char, 16> TIpv4::String() const {
    std::array<char, 16> text;
    NRadEngine::CMemory::Fill(text.data(), 0, text.size());
    inet_ntop(kAfInet, this, text.data(),
              static_cast<std::uint32_t>(text.size()));
    return text;
}

}  // namespace NRadEngine::CSysNet
