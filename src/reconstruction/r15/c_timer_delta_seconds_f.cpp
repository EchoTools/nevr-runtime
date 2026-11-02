// NRadEngine::CTimer::DeltaSecondsF
// Quest libr15.so VA 0x12cd154; ReVault binary 576.
// ReVault raw parent 100219768, reconstruction node 100600268; grade M: arithmetic and receiver offset follow Quest raw, and the timer globals retain imported names.
// @0x12cd154 — NRadEngine::CTimer::DeltaSecondsF
/* @confidence: M — arithmetic and receiver offset follow Quest raw; timer globals retain imported names. */
#include <cstddef>
#include <cstdint>

namespace NRadEngine {
namespace CSysTime {
std::int64_t GetTick();
std::uint64_t GetTicksPerSecond();
}
extern std::uint64_t DAT_0386c190;
extern std::uint64_t DAT_0386c198;

namespace CTimer {
float DeltaSecondsF(const void* timer) {
    std::uint64_t now_us = DAT_0386c198;
    if (DAT_0386c190 == 0) {
        const auto ticks = CSysTime::GetTick();
        const auto ticks_per_second = CSysTime::GetTicksPerSecond();
        now_us = ticks_per_second == 0
            ? 0
            : static_cast<std::uint64_t>(ticks) * 1'000'000ULL / ticks_per_second;
    }
    const auto last_ms = *reinterpret_cast<const std::int64_t*>(
        static_cast<const std::byte*>(timer) + 0x10);
    return static_cast<float>(static_cast<std::int64_t>(now_us / 1000) - last_ms) * 0.001F;
}
}  // namespace CTimer
}  // namespace NRadEngine
