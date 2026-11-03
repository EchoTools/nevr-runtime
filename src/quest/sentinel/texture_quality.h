/* The Quest texture-quality levels and the eye-buffer size each one asks the headset runtime for (#943, #1012).
 *
 * Pure: no Android, no game header, no hook, so quest_config.cpp (built with exceptions) and the hook
 * (texture_quality_hook.h, built with -fno-exceptions) share one table.
 *
 * What a level means. libr15's CVR::RefreshRTSize (ELF 0x1a709a0) asks the headset runtime for the suggested eye
 * buffer size with vrapi_GetSystemPropertyInt ids 5 (width) and 6 (height), multiplies each by CustomViewportScale
 * (1.0 unless a script sets it) and CVR::Resize creates the swap chain from the result once per process, logging
 * "Creating OVR swap chain with size %i x %i (2 slices, %u sample[s])". A level other than `stock` answers those two
 * ids with the size below instead of the runtime's. The sizes are the absolute per-eye sizes of the community patch
 * repository's Texture Resolution setting (EchoTools/echoApkPatch, by marshmallow-mia, graphics v9: native/texres/texres.c
 * kQ3W/kQ2W/kQ3H/kQ2H); nothing here measured the native size of any headset. The samples stay at two.
 *
 * Per headset. The sizes are the headset's profile's (headset_profile.h, ADR 0031): the identity step names the headset from its
 * system properties once at start, and a size is the row's value for the applied level. The runtime's own answer names no
 * headset. Very Low is about 0.8 times the stock size per axis (the patch's own values: Quest 3 1344x1408, Quest 2 1152x1264,
 * where 0.8 x 1584 would be 1267).
 * Low is the stock size on each headset, so it answers what `stock` would answer; it is a level so that a player can
 * ask for it by name.
 *
 * `stock` answers the runtime's own size, like every other id. The hook is installed at every level, because the game's own
 * settings menu can change the level while the game runs (#1012): the level is the one the game is *applied* at, and the
 * hook gives the size of the applied level (texture_quality_hook.h).
 */
#pragma once

#include <cstddef>
#include <string_view>

#include "quest/headset_profile.h"

namespace nevr_quest::texture_quality {

// vrapi_GetSystemPropertyInt property ids libr15 passes at RefreshRTSize (ELF 0x1a709f4 and 0x1a70a04).
inline constexpr int kEyeWidthPropertyId = 5;
inline constexpr int kEyeHeightPropertyId = 6;

enum class Level { kStock, kVeryLow, kLow, kMedium, kHigh, kVeryHigh, kUltra };

// What the key gives when the file does not set it. `stock` (no hook, the headset runtime's own size) until a Quest 2 has been
// measured at `high` for frame time and memory (#943): the flip is this one line, and the test that pins it.
inline constexpr Level kDefaultLevel = Level::kStock;
// What a mistake in the configuration gives (a word that is not a level, a value that is not a word, a section of the wrong
// shape, a file that cannot be read as a whole): always `stock`, whatever the default is. A typo must never turn on the
// level that costs the most.
inline constexpr Level kFallbackLevel = Level::kStock;
inline constexpr std::size_t kLevelCount = 7;

using EyeSize = headset_profile::EyeSize;

// The size a level answers ids 5 and 6 with on the headset a profile names. `stock` has none: {0, 0}. The values are the
// profile's (headset_profile.h kQuest2Eyes, kQuest3Eyes).
constexpr EyeSize SizeFor(Level level, const headset_profile::Profile& profile) noexcept {
  if (level == Level::kStock) return {0, 0};
  const std::size_t index = static_cast<std::size_t>(level);
  return index < headset_profile::kEyeLevelCount ? profile.eye[index] : EyeSize{0, 0};
}

// ---- the in-game control (#1012) ----------------------------------------------------------------------------------------
//
// The game's own settings menu carries the level as the `graphics|msaa` setting (a field the render path never reads; texture_menu.h
// has the reason and the evidence), as the index of a six-entry STRING ARRAY. The index is the menu's order, lowest resolution
// first, and the remembered choice (nevr_texture_quality.json, texture_menu.h) is that index. `stock` has no entry of its own: the
// menu shows it as Low, because Low is the stock size.
inline constexpr int kMenuLevelCount = 6;
inline constexpr int kDefaultMenuIndex = 1;  // Low
inline constexpr const char* kMenuLabels[kMenuLevelCount] = {"Very Low", "Low", "Medium", "High", "Very High", "Ultra"};
inline constexpr const char* kMenuControlName = "Texture Resolution";
inline constexpr const char* kMenuPageName = "GRAPHICS";

// The level a menu index names; an index outside 0..5 is clamped (the saved value is a number a player or an older build could
// have written).
constexpr Level LevelForMenuIndex(int index) noexcept {
  const int clamped = index < 0 ? 0 : (index >= kMenuLevelCount ? kMenuLevelCount - 1 : index);
  return static_cast<Level>(clamped + 1);  // kVeryLow is 1: the menu's first entry
}
// The index the menu shows for a level. `stock` shows as Low.
constexpr int MenuIndexForLevel(Level level) noexcept {
  return level == Level::kStock ? kDefaultMenuIndex : static_cast<int>(level) - 1;
}

// The eye size the game is given at a level when the runtime's own answer is native: the native size for `stock`, the profile's
// for the others, and the native size too when no profile is known yet (the identity step has not run). Two levels that give the
// same pair need no rebuild between them (a `stock` that is Low on its headset).
constexpr EyeSize EffectiveSize(Level level, EyeSize native, const headset_profile::Profile* profile) noexcept {
  if (level == Level::kStock || profile == nullptr) return native;
  return SizeFor(level, *profile);
}

// The config.yaml word for a level (graphics.texture_quality).
constexpr const char* LevelName(Level level) noexcept {
  switch (level) {
    case Level::kStock: return "stock";
    case Level::kVeryLow: return "very_low";
    case Level::kLow: return "low";
    case Level::kMedium: return "medium";
    case Level::kHigh: return "high";
    case Level::kVeryHigh: return "very_high";
    case Level::kUltra: return "ultra";
  }
  return "unknown";
}

// Exactly one of the seven words, lower case. Returns false and leaves `out` alone for anything else.
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
