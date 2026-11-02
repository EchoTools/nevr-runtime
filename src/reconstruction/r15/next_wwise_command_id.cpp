// Quest libr15.so VA 0x2a4b970; ReVault binary 576.
// ReVault raw parent 100335425, reconstruction node 100600371; grade M.
#include "wwise_post.hpp"

#include <cstdint>

namespace NRadEngine::nevr_reconstruction {

// @0x2a4b970 — NextWwiseCommandId
/* @confidence: M — reproduces the raw atomic increment; ordering follows its exclusive-monitor loop. */
std::int32_t NextWwiseCommandId(std::int32_t* state) {
    return __atomic_add_fetch(state, 1, __ATOMIC_ACQ_REL);
}

}  // namespace NRadEngine::nevr_reconstruction
