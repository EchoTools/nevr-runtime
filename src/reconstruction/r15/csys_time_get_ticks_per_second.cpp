// Quest libr15.so VA 0x1084e64; ReVault binary 576.
// ReVault raw parent 100207923, reconstruction node 100600577; grade H.
#include <cstdint>

namespace NRadEngine::CSysTime {

// @0x1084e64 — NRadEngine::CSysTime::GetTicksPerSecond
/* @confidence: H — the complete three-instruction body returns the constant 1,000,000,000. */
std::uint64_t GetTicksPerSecond() {
    return 1'000'000'000ULL;
}

}  // namespace NRadEngine::CSysTime
