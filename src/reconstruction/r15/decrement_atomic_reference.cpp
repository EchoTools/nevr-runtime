// Quest libr15.so VA 0x2a513c0; ReVault binary 576.
// ReVault raw parent 100335675, reconstruction node 100600390; grade M.
#include "wwise_post.hpp"

#include <cstdint>

namespace NRadEngine::nevr_reconstruction {

// @0x2a513c0 — DecrementAtomicReference
/* @confidence: M — reproduces the raw exclusive-monitor decrement with acquire/release ordering. */
std::int32_t DecrementAtomicReference(std::int32_t* reference_count) {
    return __atomic_sub_fetch(reference_count, 1, __ATOMIC_ACQ_REL);
}

}  // namespace NRadEngine::nevr_reconstruction
