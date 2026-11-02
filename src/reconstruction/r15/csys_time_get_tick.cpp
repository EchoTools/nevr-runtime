// Quest libr15.so VA 0x1084e20; ReVault binary 576.
// ReVault raw parent 100207922, reconstruction node 100600576; grade H.
#include <cstdint>
#include <time.h>

namespace NRadEngine::CSysTime {

// @0x1084e20 — NRadEngine::CSysTime::GetTick
/* @confidence: H — calls clock_gettime(CLOCK_MONOTONIC), returns seconds × 1,000,000,000 + nanoseconds, and returns -1 on failure; all 17 instructions match. */
std::int64_t GetTick() {
    timespec value{};
    if (::clock_gettime(CLOCK_MONOTONIC, &value) < 0) {
        return -1;
    }

    const auto nanoseconds =
        static_cast<std::uint64_t>(value.tv_sec) * 1'000'000'000ULL +
        static_cast<std::uint64_t>(value.tv_nsec);
    return static_cast<std::int64_t>(nanoseconds);
}

}  // namespace NRadEngine::CSysTime
