#include "wwise_post.hpp"

namespace NRadEngine::AudioPost {

// @0x144c664 — CSoundPlayer::RetrieveParameter
/* @confidence: M — audio-disabled output, generation and active-slot checks, result copy, and read-lock scope match the raw body. */
std::uint32_t RetrieveParameter(std::uint64_t handle,
                                std::uint32_t* output_value) {
    if (*reinterpret_cast<const std::uint32_t*>(g_audio_disabled_flag) != 0) {
        *output_value = 0;
        return 1;
    }

    CLockReadWriteReadLock(&DAT_0386eeb0);
    std::uint32_t result = 0;
    const auto mapping_index = static_cast<std::uint32_t>(handle);
    const auto generation = static_cast<std::uint32_t>(handle >> 32);
    if (mapping_index < 0x20U) {
        const auto& mapping = DAT_03852374[mapping_index];
        if (mapping.request_slot != 0xffffffffU &&
            mapping.value == generation) {
            const auto& slot = DAT_0386eed0[mapping.request_slot];
            if (slot.status != 0) {
                *output_value = slot.output_value;
                result = 1;
            }
        }
    }
    CLockReadWriteReadUnlock(&DAT_0386eeb0);
    return result;
}

}  // namespace NRadEngine::AudioPost
