// Quest libr15.so VA 0x144c440; ReVault binary 576.
// ReVault raw parent 100227805, reconstruction node 100600512; grade M.
#include "wwise_post.hpp"

namespace NRadEngine::nevr_reconstruction {

// @0x144c440 — CSoundPlayer::RegisterParameterRequest
/* @confidence: M — the 32-slot search/allocation, mapping-table update, sentinel kind and packed result match the raw body; global layouts are inferred from offsets. */
std::uint64_t RegisterParameterRequest(std::uint32_t symbol,
                                       std::uint32_t object_id) {
    if (*reinterpret_cast<const std::uint32_t*>(g_audio_disabled_flag) != 0) {
        return 0xffffffffULL;
    }

    const std::uint64_t extended_object_id =
        object_id == 0xffffffffU ? 0xffffffffffffffffULL : object_id;
    CLockReadWriteWriteLock(&DAT_0386eeb0);

    const auto request_count = DAT_0386ee28;
    if (request_count != 0) {
        for (std::uint32_t slot_index = 0; slot_index < request_count;
             ++slot_index) {
            auto& slot = DAT_0386eed0[slot_index];
            if (static_cast<std::uint32_t>(slot.symbol) == symbol &&
                slot.object_id == extended_object_id) {
                const auto mapping_value =
                    DAT_03852374[slot.pool_index].value;
                CLockReadWriteWriteUnlock(&DAT_0386eeb0);
                return (static_cast<std::uint64_t>(mapping_value) << 32) |
                       slot.pool_index;
            }
        }
    }

    if (request_count >= 0x20U) {
        NWriteLogWriteLog(
            4, nullptr,
            "Exhausted available slots for RTPC queries. Fetch a programmer.");
        CLockReadWriteWriteUnlock(&DAT_0386eeb0);
        return 0xffffffffULL;
    }

    auto& slot = DAT_0386eed0[request_count];
    slot.symbol = symbol;
    slot.object_id = extended_object_id;
    slot.status = 0;
    slot.request_kind = object_id == 0xffffffffU ? 1U : 2U;
    slot.output_value = 0;

    const auto pool_index = slot.pool_index;
    auto& mapping = DAT_03852374[pool_index];
    const auto mapping_value = mapping.value;
    mapping.request_slot = request_count;
    DAT_0386ee28 = request_count + 1;

    CLockReadWriteWriteUnlock(&DAT_0386eeb0);
    return (static_cast<std::uint64_t>(mapping_value) << 32) | pool_index;
}

}  // namespace NRadEngine::nevr_reconstruction
