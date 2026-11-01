// @0x12ccf34 — NRadEngine::CTimer::MilliSeconds
/* @confidence: M — arithmetic and global cache follow Quest raw; source clock methods are named in the raw body. */
#include <cstdint>

namespace NRadEngine {
namespace CSysTime {
std::int64_t GetTick();
std::uint64_t GetTicksPerSecond();
}
extern std::uint64_t DAT_0386c190;
extern std::uint64_t DAT_0386c198;

namespace CTimer {
std::uint64_t MilliSeconds() {
    std::uint64_t now_us = DAT_0386c198;
    if (DAT_0386c190 == 0) {
        const auto ticks = CSysTime::GetTick();
        const auto ticks_per_second = CSysTime::GetTicksPerSecond();
        now_us = ticks_per_second == 0
            ? 0
            : static_cast<std::uint64_t>(ticks) * 1'000'000ULL / ticks_per_second;
    }
    return now_us / 1000;
}
}  // namespace CTimer
}  // namespace NRadEngine
