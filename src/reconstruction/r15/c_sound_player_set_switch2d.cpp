// NRadEngine::CSoundPlayer::SetSwitch2D
// Quest libr15.so VA 0x144c2c4; ReVault binary 576.
// ReVault raw parent 100227801, reconstruction node 100600476; grade H: all 14 instructions mapped, the no-audio switch, the current-emitter guard and a tail branch to SetSwitch.
// @0x144c2c4 — NRadEngine::CSoundPlayer::SetSwitch2D
/* @confidence: H — all 14 instructions mapped: the no-audio switch, the current-emitter
 * guard at this+0x238, and a tail branch to SetSwitch with the emitter id from +0xb8. */
#include <cstddef>
#include <cstdint>

namespace NRadEngine {
namespace {
template <typename T>
T& at(void* object, std::size_t offset) {
  return *reinterpret_cast<T*>(static_cast<std::byte*>(object) + offset);
}
}  // namespace

// The "no audio" switch: a pointer at 0x3820f48 to an int (adrp/ldr at 0x144c2c4/0x144c2c8).
// /* UNRESOLVED: global pointer 0x3820f48 */
std::uint32_t NoAudioValue();

// The four-argument form; reached here by a tail branch (0x144c2f0 -> thunk 0x1011fc0), so
// its return value is this function's return value.
bool CSoundPlayer_SetSwitch(void* player, std::uint32_t group, std::uint32_t state,
                            std::uint32_t emitter_id);

bool CSoundPlayer_SetSwitch2D(void* player, std::uint32_t group, std::uint32_t state) {
  if (NoAudioValue() != 0) {
    return true;  // 0x144c2d4: mov w0,#1
  }

  // this+0x238 is the current emitter (the same field DestroySoundNoLock reads).
  void* const emitter = at<void*>(player, 0x238);
  if (emitter == nullptr) {
    return false;  // 0x144c2f4: mov w0,wzr
  }

  const auto emitter_id = at<std::uint32_t>(emitter, 0xb8);
  return CSoundPlayer_SetSwitch(player, group, state, emitter_id);
}
}  // namespace NRadEngine
