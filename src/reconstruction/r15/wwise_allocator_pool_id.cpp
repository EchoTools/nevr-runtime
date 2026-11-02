// Quest libr15.so VA 0x3859620; ReVault binary 576.
// revault_read_memory at 0x3859620 returned ff ff ff ff (little-endian -1).
// Data word, no function node; read by grow_wwise_post_queue.cpp (node 100600435 at VA 0x2aaee00, raw parent 100340324).
#include "wwise_post.hpp"

#include <cstdint>

namespace NRadEngine::nevr_reconstruction {

// @0x3859620 — DAT_03859620
// ReVault read_memory: ff ff ff ff (little-endian 0xffffffff).
extern const std::int32_t DAT_03859620 = -1;

}  // namespace NRadEngine::nevr_reconstruction
