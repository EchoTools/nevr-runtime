/* The one place the memory a device can still give is read, and the one function every memory-share limit goes through
 * (docs/adr/0045-cdn-texture-delivery.md decision 8). The cargo limits (cargo_limits.h) and the first-run data step's manifest read
 * (quest/data/data_step.h) take their sizes from the same number with the same arithmetic, so a device with less memory gets a smaller
 * bound on every path and one with more gets a larger one. (The resolver's pin is thread-local and needs no size: cargo_resolver.h.)
 */
#pragma once

#include <cstdint>

namespace nevr_device {

// `MemAvailable` of the text of /proc/meminfo, in bytes. False when the line is absent, malformed or does not fit 64 bits.
bool ParseMemAvailable(const char* meminfo_text, std::uint64_t* bytes) noexcept;

// What the system says a process can still use without swapping, read from /proc/meminfo (on Windows, GlobalMemoryStatusEx's
// available physical memory). False when it cannot be read; nothing is guessed in its place (a caller that needs the number then
// does nothing, and says so).
bool MemoryAvailable(std::uint64_t* bytes) noexcept;

// `whole / divisor`: the share a limit may take. A divisor of 0 gives 0.
constexpr std::uint64_t Share(std::uint64_t whole, std::uint64_t divisor) noexcept { return divisor == 0 ? 0 : whole / divisor; }

}  // namespace nevr_device
