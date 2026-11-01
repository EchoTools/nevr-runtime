#include "wwise_post.hpp"

#include <cstddef>
#include <cstdint>

namespace NRadEngine::AudioPost {

// @0x2a59d60 — AllocateAudioRingBlock
/* @confidence: M — reproduces the aligned ring reservation and wrap marker from the raw body. */
std::byte* AllocateAudioRingBlock(std::uint64_t context,
                                  std::uint32_t* requested_size) {
    auto size = static_cast<std::uint32_t>(*requested_size + 3U) & ~3U;
    *requested_size = size;
    auto* pool = reinterpret_cast<std::uint64_t*>(context);
    for (;;) {
        const auto write_cursor = static_cast<std::uintptr_t>(pool[1]);
        const auto read_cursor = static_cast<std::uintptr_t>(pool[0]);
        const auto begin = static_cast<std::uintptr_t>(pool[2]);
        const auto end = static_cast<std::uintptr_t>(pool[3]);
        const auto reservation_size = static_cast<std::uintptr_t>(size) + 4U;

        if (write_cursor < read_cursor) {
            if (read_cursor - write_cursor <= reservation_size) {
                return nullptr;
            }
            if (ReserveAudioRingCursor(
                    reinterpret_cast<std::uintptr_t*>(pool + 1), write_cursor,
                    write_cursor + size)) {
                return reinterpret_cast<std::byte*>(write_cursor);
            }
        } else {
            if (reservation_size < end - write_cursor) {
                if (ReserveAudioRingCursor(
                        reinterpret_cast<std::uintptr_t*>(pool + 1), write_cursor,
                        write_cursor + size)) {
                    return reinterpret_cast<std::byte*>(write_cursor);
                }
            } else {
                if (read_cursor - begin <= reservation_size) {
                    return nullptr;
                }
                if (ReserveAudioRingCursor(
                        reinterpret_cast<std::uintptr_t*>(pool + 1), write_cursor,
                        begin + size)) {
                    if (write_cursor + sizeof(std::uint32_t) <= end) {
                        *reinterpret_cast<std::uint32_t*>(write_cursor) = 0x390004U;
                    }
                    return reinterpret_cast<std::byte*>(begin);
                }
            }
        }
    }
}

}  // namespace NRadEngine::AudioPost
