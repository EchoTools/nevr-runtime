// NRadEngine::CSoundPlayer::SendGlobalEvent
// Quest libr15.so VA 0x144c254; ReVault binary 576.
// ReVault raw parent 100227799, reconstruction node 100600472; grade H: all 21 instructions mapped; the only unnamed edge is the event poster, kept as an external boundary.
// @0x144c254 — NRadEngine::CSoundPlayer::SendGlobalEvent
/* @confidence: H — all 21 instructions mapped: an unused `this`, the no-audio switch read
 * through the pointer at 0x3820f48, one eight-argument poster call, and a !=0 result. The
 * only unnamed edge is that poster, kept as an external boundary. */
#include <cstdint>

namespace NRadEngine {

// The "no audio" switch is read through a pointer stored at 0x3820f48 (adrp/ldr at
// 0x144c258/0x144c25c, then ldr w8,[x8]); the decompiler names the pointee `noaudio`.
// /* UNRESOLVED: global pointer 0x3820f48 */
std::uint32_t NoAudioValue();

// /* UNRESOLVED: FUN_02a4b500 */ eight-argument engine event poster, called here with the
// event symbol and seven zero arguments (w0, x1, w2, x3, x4, w5, x6, w7 at 0x144c274).
// A non-zero return means the event was posted.
std::uint32_t FUN_02a4b500(std::uint32_t id, std::uint64_t arg2, std::uint32_t arg3,
                           std::uint64_t arg4, std::uint64_t arg5, std::uint32_t arg6,
                           std::uint64_t arg7, std::uint32_t arg8);

bool CSoundPlayer_SendGlobalEvent(void* player, std::uint32_t event_symbol) {
  (void)player;  // x0 is never read by 0x144c254
  if (NoAudioValue() != 0) {
    return true;  // 0x144c264: mov w0,#1
  }
  return FUN_02a4b500(event_symbol, 0, 0, 0, 0, 0, 0, 0) != 0;  // cset w0, ne
}
}  // namespace NRadEngine
