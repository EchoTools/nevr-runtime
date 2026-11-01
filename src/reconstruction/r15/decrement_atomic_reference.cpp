#include "wwise_post.hpp"

#include <cstdint>

namespace NRadEngine::AudioPost {

// @0x2a513c0 — DecrementAtomicReference
/* @confidence: M — reproduces the raw exclusive-monitor decrement with acquire/release ordering. */
std::int32_t DecrementAtomicReference(std::int32_t* reference_count) {
    return __atomic_sub_fetch(reference_count, 1, __ATOMIC_ACQ_REL);
}

}  // namespace NRadEngine::AudioPost
