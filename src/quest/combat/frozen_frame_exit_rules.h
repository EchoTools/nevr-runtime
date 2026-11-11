/* The frozen-frame rule (frozen_frame_exit.h, #2377): whether the time since the last frame submit, in VR, is past the limit. Pure: it reads no
 * memory and calls nothing of the game's, so a test drives it with numbers.
 *
 * Built with -fno-exceptions, like every combat translation unit.
 */
#pragma once

#include <cstdint>

namespace nevr_quest::combat::frozen_frame_exit::rules {

// `combat.frozen_frame_exit_s`: the limit in seconds, its range and its default (upstream's hangwd bound: 30 s, 10..900).
inline constexpr int kMinLimitS = 10;
inline constexpr int kMaxLimitS = 900;
inline constexpr int kDefaultLimitS = 30;

// True when the frames have stopped for longer than `limitS` seconds: the game is in VR, a submit was seen since the watch was armed
// (`lastSubmitMs` non-zero), and more than limitS * 1000 ms have passed since it. Exactly the limit is not past it.
constexpr bool Expired(std::int64_t nowMs, std::int64_t lastSubmitMs, bool inVr, int limitS) noexcept {
  if (lastSubmitMs == 0 || !inVr) return false;
  return nowMs - lastSubmitMs > static_cast<std::int64_t>(limitS) * 1000;
}

}  // namespace nevr_quest::combat::frozen_frame_exit::rules
