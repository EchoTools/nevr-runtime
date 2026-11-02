// Quest libr15.so VA 0x2a59e40; ReVault binary 576.
// ReVault raw parent 100336071, reconstruction node 100600416; grade M.
#include "wwise_post.hpp"

#include <cstdint>

namespace NRadEngine::AudioPost {

// @0x2a59e40 — ReserveAudioRingCursor
/* @confidence: M — matches the raw exclusive-store loop with an address compare-and-swap. */
bool ReserveAudioRingCursor(std::uintptr_t* cursor, std::uintptr_t expected,
                            std::uintptr_t next) {
    for (;;) {
        if (__atomic_load_n(cursor, __ATOMIC_RELAXED) != expected) {
            return false;
        }
        auto observed = expected;
        if (__atomic_compare_exchange_n(cursor, &observed, next, false,
                                       __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {
            return true;
        }
    }
}

}  // namespace NRadEngine::AudioPost
