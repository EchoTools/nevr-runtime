// NRadEngine::CTimer::DeltaMicroSeconds
// Quest libr15.so VA 0x12c8c28; ReVault binary 576.
// ReVault raw parent 100219489, reconstruction node 100600270; grade M: arithmetic, cache branch and receiver offset follow Quest raw.
// @0x12c8c28 — NRadEngine::CTimer::DeltaMicroSeconds
/* @confidence: M — arithmetic, cache branch, and receiver offset follow Quest raw. */
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
std::int64_t DeltaMicroSeconds(const void* timer) {
    std::uint64_t now_us = DAT_0386c198;
    if (DAT_0386c190 == 0) {
        const auto ticks = CSysTime::GetTick();
        const auto ticks_per_second = CSysTime::GetTicksPerSecond();
        now_us = ticks_per_second == 0
            ? 0
            : static_cast<std::uint64_t>(ticks) * 1'000'000ULL / ticks_per_second;
    }
    const auto last_us = *reinterpret_cast<const std::int64_t*>(
        static_cast<const std::byte*>(timer) + 0x18);
    return static_cast<std::int64_t>(now_us) - last_us;
}
}  // namespace CTimer
}  // namespace NRadEngine
