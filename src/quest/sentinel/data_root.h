/* Which directory the game reads its data from (#749).
 *
 * The game asks CSysInfo::GetDataRootDir (libr15 0xf87250), which returns gOBBPath when it is set and
 * otherwise the literal /storage/emulated/0/readyatdawn (pinned_targets.h). The OBB skip (obb_skip_hook.h)
 * decides what gOBBPath holds, so the root it serves is chosen here: the first candidate that holds the game's
 * `_data` directory. The hook on GetDataRootDir answers the media root unless the legacy root alone holds the
 * data (obb_skip_hook.cpp, ResolveRoot), whatever the file's own body returns. With no OBB file nothing is
 * mounted and gOBBPath stays empty.
 *
 *   1. /sdcard/Android/media/com.readyatdawn.r15/files   the data root: Android's app-specific media directory, where
 *                                                        the echo.taxi installer puts the data
 *   2. /storage/emulated/0/readyatdawn                   the legacy root: where tools/quest-install.sh puts it, and
 *                                                        the game's own fallback
 *
 * Where `_data` sits under the media directory is not confirmed: the installer's maintainer names `files/` as the
 * root, and nobody has listed it. So the media directory is also served when it exists without `_data` directly
 * under it (source "android_media_unmarked"), after both roots were checked for `_data`. Neither existing resolves
 * to the game's own fallback, found=false: what the game does without this code, and the caller says so in one
 * line. Pure: the filesystem is the caller's `exists`.
 */
#pragma once

#include <cstddef>
#include <cstdio>

namespace nevr_quest::data_root {

// The data root: the installer app's directory under Android/media (needs no storage permission to read).
inline constexpr const char* kMediaRoot = "/sdcard/Android/media/com.readyatdawn.r15/files";
// The directory every media root sits under: the reference build's GetDataRootDir body names the afk package under it.
inline constexpr const char* kMediaDirPrefix = "/sdcard/Android/media/";
// The legacy root: the game's own fallback and tools/quest-install.sh's target.
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
  const char* source;  // "android_media", "legacy", "android_media_unmarked" or "none"
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
  if (exists(kMediaRoot)) return {kMediaRoot, "android_media_unmarked", true};
  return {kLegacyRoot, "none", false};
}

}  // namespace nevr_quest::data_root
