#include "wwise_post.hpp"

#include <cstddef>
#include <cstdint>

namespace NRadEngine::AudioPost {

// @0x2a53580 — AllocateAudioCommandStorage
/* @confidence: M — includes the ring-full wait, queue drain and retry from the raw allocator. */
std::byte* AllocateAudioCommandStorage(std::uint64_t context,
                                       std::uint16_t command_kind,
                                       std::uint32_t command_size) {
    auto stored_size = command_size;
    auto* block = AllocateAudioCommandRegion(context, &stored_size);
    while (block == nullptr) {
        if (AudioCommandQueueCapacity(
                reinterpret_cast<const void*>(context)) < stored_size) {
            return nullptr;
        }

        LockAudioMutex(&DAT_038ced68);
        block = AllocateAudioCommandRegion(context, &stored_size);
        if (block == nullptr) {
            WaitForAudioCommandActivity(context);
            std::uint8_t process_flags = 0;
            ProcessAudioCommandQueue(context, 1, &process_flags);
            block = AllocateAudioCommandRegion(context, &stored_size);
        }
        UnlockAudioMutex(&DAT_038ced68);
    }

    *reinterpret_cast<std::uint16_t*>(block) =
        static_cast<std::uint16_t>(stored_size);
    *reinterpret_cast<std::uint16_t*>(block + 2) = command_kind;
    return block;
}

}  // namespace NRadEngine::AudioPost
