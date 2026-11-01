#include "wwise_post.hpp"

namespace NRadEngine::AudioPost {

// @0x2a49100 — CommitAudioCommandScope
/* @confidence: M — follows the raw call to the shared audio-command queue. */
void CommitAudioCommandScope(void*) {
    SubmitAudioCommandStorage(DAT_038cec78);
}

}  // namespace NRadEngine::AudioPost
