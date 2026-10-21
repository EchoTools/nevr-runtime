/* Which directory the game reads its data from (#749).
 *
 * The game asks CSysInfo::GetDataRootDir (libr15 0xf87250), which returns gOBBPath when it is set and
 * otherwise the literal /storage/emulated/0/readyatdawn (pinned_targets.h). The OBB skip (obb_skip_hook.h)
 * decides what gOBBPath holds, so the root it serves is chosen here: the first candidate that holds the game's
 * `_data` directory.
 *
 *   1. /sdcard/Android/media/com.readyatdawn.r15/files   where the echo.taxi installer app puts the data
 *   2. /storage/emulated/0/readyatdawn                   where tools/quest-install.sh puts it, and the
 *                                                        game's own fallback
 *
 * Neither holding `_data` resolves to the game's own fallback, found=false: what the game does without this
 * code, and the caller says so in one line. Pure: the filesystem is the caller's `exists`.
 */
#pragma once

#include <cstddef>
#include <cstdio>

namespace nevr_quest::data_root {

// The directory the installer app writes under (sub-directory `files`; the installer's layout is unsettled, see #749).
inline constexpr const char* kMediaRoot = "/sdcard/Android/media/com.readyatdawn.r15/files";
// The game's own fallback root and tools/quest-install.sh's target.
inline constexpr const char* kLegacyRoot = "/storage/emulated/0/readyatdawn";
// What proves a root holds the game's data.
inline constexpr const char* kMarker = "_data";

struct Candidate {
  const char* root;
  const char* source;  // the token the log line carries
};

inline constexpr Candidate kCandidates[] = {
    {kMediaRoot, "android_media"},
    {kLegacyRoot, "legacy"},
};

using ExistsFn = bool (*)(const char* path) noexcept;

struct Resolution {
  const char* root;    // never null: the winner, or the game's own fallback when none holds the marker
  const char* source;  // "android_media", "legacy", or "none"
  bool found;
};

// True when `root/_data` exists. A path that does not fit the buffer does not exist.
inline bool HoldsData(const char* root, ExistsFn exists) noexcept {
  char path[512];
  const int n = std::snprintf(path, sizeof(path), "%s/%s", root, kMarker);
  return n > 0 && static_cast<std::size_t>(n) < sizeof(path) && exists(path);
}

inline Resolution Resolve(ExistsFn exists) noexcept {
  for (const Candidate& c : kCandidates) {
    if (HoldsData(c.root, exists)) return {c.root, c.source, true};
  }
  return {kLegacyRoot, "none", false};
}

}  // namespace nevr_quest::data_root
