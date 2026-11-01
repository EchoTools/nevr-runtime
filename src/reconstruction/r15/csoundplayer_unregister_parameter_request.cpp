#include "wwise_post.hpp"

#include <utility>

namespace NRadEngine::AudioPost {

// @0x144c584 — CSoundPlayer::UnregisterParameterRequest
/* @confidence: M — generation check, compacting slot swap, remap update and invalidation match the full body; global layouts are inferred from offsets. */
void UnregisterParameterRequest(std::uint64_t handle) {
    if (*reinterpret_cast<const std::uint32_t*>(g_audio_disabled_flag) != 0) {
        return;
    }

    CLockReadWriteWriteLock(&DAT_0386eeb0);
    const auto mapping_index = static_cast<std::uint32_t>(handle);
    const auto generation = static_cast<std::uint32_t>(handle >> 32);
    if (mapping_index < 0x20U) {
        auto& mapping = DAT_03852374[mapping_index];
        const auto request_slot = mapping.request_slot;
        if (request_slot != 0xffffffffU && mapping.value == generation) {
            const auto new_count = DAT_0386ee28 - 1;
            DAT_0386ee28 = new_count;
            if (request_slot != new_count) {
                std::swap(DAT_0386eed0[request_slot],
                          DAT_0386eed0[new_count]);
                const auto moved_pool_index =
                    DAT_0386eed0[request_slot].pool_index;
                DAT_03852374[moved_pool_index].request_slot = request_slot;
            }
            mapping.request_slot = 0xffffffffU;
            mapping.value = generation + 1;
        }
    }
    CLockReadWriteWriteUnlock(&DAT_0386eeb0);
}

}  // namespace NRadEngine::AudioPost
