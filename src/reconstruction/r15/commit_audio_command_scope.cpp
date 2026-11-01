// Quest libr15.so VA 0x2a49100; ReVault binary 576.
// ReVault raw parent 100335325, reconstruction node 100600357; grade M.
#include "wwise_post.hpp"

namespace NRadEngine::AudioPost {

// @0x2a49100 — CommitAudioCommandScope
/* @confidence: M — follows the raw call to the shared audio-command queue. */
void CommitAudioCommandScope(void*) {
    DecrementAudioCommandActivity(DAT_038cec78);
}

}  // namespace NRadEngine::AudioPost
