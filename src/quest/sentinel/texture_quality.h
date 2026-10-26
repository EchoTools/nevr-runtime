/* The Quest texture-quality levels and the eye-buffer size each one asks the headset runtime for (#943).
 *
 * Pure: no Android, no game header, no hook, so quest_config.cpp (built with exceptions) and the hook
 * (texture_quality_hook.h, built with -fno-exceptions) share one table.
 *
 * What a level means. libr15's CVR::RefreshRTSize (ELF 0x1a709b0) asks the headset runtime for the suggested eye
 * buffer size with vrapi_GetSystemPropertyInt ids 5 (width) and 6 (height), multiplies each by CustomViewportScale
 * (1.0 unless a script sets it) and CVR::Resize creates the swap chain from the result once per process, logging
 * "Creating OVR swap chain with size %i x %i (2 slices, %u sample[s])". A level other than `stock` answers those two
 * ids with the sizes below instead of the runtime's. The sizes are the absolute per-eye sizes the community patch
 * repository's Graphics Patch setting (EchoTools/echoApkPatch, by marshmallow-mia) uses for the same names, reported
 * for a Quest 3; nothing here measured the native size of any headset. The samples stay at two.
 *
 * Aspect. Levels low..ultra are 1680x1760, 2376x2568, 3072x3380, 3584x3944 and 4096x4506: the ratio width/height is
 * 0.9545, 0.9252, 0.9089, 0.9087 and 0.9090. The first is the table's own; the others are what the table gives.
 *
 * `stock` installs no hook: the game asks the runtime and gets the runtime's own answer.
 */
#pragma once

#include <cstddef>
#include <string_view>

namespace nevr_quest::texture_quality {

// vrapi_GetSystemPropertyInt property ids libr15 passes at RefreshRTSize (ELF 0x1a709f4 and 0x1a70a04).
inline constexpr int kEyeWidthPropertyId = 5;
inline constexpr int kEyeHeightPropertyId = 6;

enum class Level { kStock, kLow, kMedium, kHigh, kVeryHigh, kUltra };

inline constexpr Level kDefaultLevel = Level::kHigh;
inline constexpr std::size_t kLevelCount = 6;

struct EyeSize {
  int width;
  int height;
};

// The size a level answers ids 5 and 6 with. `stock` has none: {0, 0}.
constexpr EyeSize SizeFor(Level level) noexcept {
  switch (level) {
    case Level::kStock: return {0, 0};
    case Level::kLow: return {1680, 1760};
    case Level::kMedium: return {2376, 2568};
    case Level::kHigh: return {3072, 3380};
    case Level::kVeryHigh: return {3584, 3944};
    case Level::kUltra: return {4096, 4506};
  }
  return {0, 0};
}

// The config.yaml word for a level (graphics.texture_quality).
constexpr const char* LevelName(Level level) noexcept {
  switch (level) {
    case Level::kStock: return "stock";
    case Level::kLow: return "low";
    case Level::kMedium: return "medium";
    case Level::kHigh: return "high";
    case Level::kVeryHigh: return "very_high";
    case Level::kUltra: return "ultra";
  }
  return "unknown";
}

// Exactly one of the six words, lower case. Returns false and leaves `out` alone for anything else.
inline bool ParseLevel(std::string_view word, Level* out) noexcept {
  for (std::size_t i = 0; i < kLevelCount; ++i) {
    const Level level = static_cast<Level>(i);
    if (word == LevelName(level)) {
      *out = level;
      return true;
    }
  }
  return false;
}

}  // namespace nevr_quest::texture_quality
