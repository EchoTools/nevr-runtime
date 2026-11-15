#pragma once
// PAYLOAD SPEED and the payload PAUSE (#2242, EchoCombat nevr-pack 82ad7ba, plugin/veilnet.cpp PayloadSpeed and PageTexts): the
// decisions veilnet.cpp's stub takes on the payload maps (Fission, Gauss), kept pure so test_payload_speed_rules.cpp drives them.
// The stub site, the clock and the mailbox stay in veilnet.cpp.
//
// The payload's data (r14 at the site, 0x58 bytes) holds its flags at +0x30; the stub's third argument is the speed the game worked
// out for the track mover. Revault (echovr.exe): the site is 0xD858ED, 0x8FD into the function at 0xD84FF0 (revault names it
// FUN_140d84ff0; the name CR15NetPayloadCS::UpdatePreTrackMover is from EchoCombat's notes, agent.md:719 and plugin/veilnet.cpp:2715),
// `movss [rax+rdx*8], xmm3` followed by `mov r9d, [r14+30h]`, 9 bytes.
//
// KNOWN-WRONG #2242: the Quest has none of this. Match control (the RULES page, PAUSE, the server's 'G') is PC only, there is no
// payload speed hook in src/quest/combat, and a Quest client neither shows the row nor scales the payload.
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "runtime/combat/rules_state.h"

namespace nevr_combat::payload_speed {

// The payload's flags (data +0x30).
inline constexpr std::size_t kFlagsOffset = 0x30;
inline constexpr std::uint32_t kFlagPaused = 1;      // the script's checkpoint hold (R15NetPayloadSetPaused)
inline constexpr std::uint32_t kFlagAutodrive = 2;   // the game places the payload itself: a round's start, a checkpoint
inline constexpr std::uint32_t kFlagLocked = 8;      // the round is over (R15NetPayloadSetLocked)

// A speed above this (the game's units) is the payload moving forward.
inline constexpr float kMovingAbove = 0.05f;
inline constexpr float kQuartersPerWhole = 4.0f;

/// The speed the track mover is given for `quarters` of PAYLOAD SPEED (0 reads as x1): the game's speed times quarters / 4.
inline float Scaled(float speed, std::uint8_t quarters) {
  return quarters != 0 && quarters != rules::kRateDefault ? speed * static_cast<float>(quarters) / kQuartersPerWhole : speed;
}

/// What a server's step does to its "the payload moved this round" bookkeeping.
enum class Moved { kKeep, kClear, kSet };
struct Step {
  float speed;
  Moved moved;
};

/// The server's stub: PAUSE holds the payload (autodrive too), AUTODRIVE passes the game's speed and clears the bookkeeping (the
/// round's start or a checkpoint), anything else is scaled and a forward speed sets it; LOCKED (the round is over) clears it unless
/// the payload still moves forward.
inline Step ServerStep(std::uint32_t flags, bool paused, std::uint8_t quarters, float speed) {
  if ((flags & kFlagAutodrive) != 0) return {paused ? 0.0f : speed, Moved::kClear};
  if (paused) return {0.0f, (flags & kFlagLocked) != 0 ? Moved::kClear : Moved::kKeep};
  const float out = Scaled(speed, quarters);
  if (out > kMovingAbove) return {out, Moved::kSet};
  return {out, (flags & kFlagLocked) != 0 ? Moved::kClear : Moved::kKeep};
}

/// A client's stub: PAUSE stands still here too, AUTODRIVE is the game's, anything else is scaled by the server's quarters.
inline float ClientSpeed(std::uint32_t flags, bool paused, std::uint8_t quarters, float speed) {
  if (paused) return 0.0f;
  if ((flags & kFlagAutodrive) != 0) return speed;
  return Scaled(speed, quarters);
}

/// A PAUSE asked while the payload moves waits for a checkpoint (its hold flag); before the attackers start, or in a checkpoint's
/// buffer, it happens at once.
inline bool PauseWaitsForCheckpoint(bool payload_map, bool moved, std::uint32_t flags) {
  return payload_map && moved && (flags & kFlagPaused) == 0;
}

// ---- the RULES row: PAYLOAD SPEED on a payload map, POINT RATE elsewhere (the same asks and steps set either)
inline constexpr const char* kPayloadRowLabel = "PAYLOAD SPEED";
inline constexpr const char* kRateRowLabel = "POINT RATE";
inline constexpr std::size_t kQuarterTextMax = 16;
inline constexpr std::uint8_t kQuartersPerUnit = 4;
inline constexpr std::uint8_t kHalfStep = 2;
inline constexpr std::uint8_t kWholeThree = 3;      // from x3 on the text drops the ".0"
inline constexpr std::uint8_t kHundredthsPerQuarter = 25;

/// "1.0" / "2.5" / "0.75" / "3" from quarters (0 reads as x1).
inline void QuarterText(char* out, std::size_t n, std::uint8_t q) {
  if (q == 0) q = rules::kRateDefault;
  if (q % kQuartersPerUnit == 0 && q / kQuartersPerUnit >= kWholeThree) {
    std::snprintf(out, n, "%d", q / kQuartersPerUnit);
  } else if (q % kQuartersPerUnit == 0) {
    std::snprintf(out, n, "%d.0", q / kQuartersPerUnit);
  } else if (q % kHalfStep == 0) {
    std::snprintf(out, n, "%d.5", q / kQuartersPerUnit);
  } else {
    std::snprintf(out, n, "%d.%02d", q / kQuartersPerUnit, q % kQuartersPerUnit * kHundredthsPerQuarter);
  }
}

/// The text of the page's third row: which label applies (the payload map byte) and the quarters that row shows.
inline void RowText(char* out, std::size_t n, bool payload_map, std::uint8_t payload_quarters, std::uint8_t rate_quarters) {
  char num[kQuarterTextMax];
  QuarterText(num, sizeof(num), payload_map ? payload_quarters : rate_quarters);
  std::snprintf(out, n, "%s: %sx", payload_map ? kPayloadRowLabel : kRateRowLabel, num);
}

// ---- the server's 'G' word: payload map in bit 5, PAYLOAD SPEED (quarters) in bits 16-23 (the rest is veilnet.cpp's)
inline constexpr unsigned kMapShift = 5;
inline constexpr unsigned kSpeedShift = 16;
inline constexpr std::uint32_t kBitMask = 1;
inline constexpr std::uint32_t kByteMask = 0xff;

inline std::uint32_t PackWord(bool payload_map, std::uint8_t quarters) {
  return (payload_map ? kBitMask : 0u) << kMapShift | static_cast<std::uint32_t>(quarters) << kSpeedShift;
}
inline bool WordMap(std::uint32_t word) { return (word >> kMapShift & kBitMask) != 0; }
inline std::uint8_t WordQuarters(std::uint32_t word) { return static_cast<std::uint8_t>(word >> kSpeedShift & kByteMask); }

}  // namespace nevr_combat::payload_speed
