/* What the first-run data step decides and where it puts things (docs/adr/0011-quest-game-data-first-run.md).
 *
 * The pins name the one zip the step fetches: its URL, its byte length and its whole-file sha256. The presence check
 * is pinned against that zip's listing: a data root holds this version of the data when the manifest file the zip
 * carries exists under `_data`. Pure: the filesystem is the caller's `exists`.
 */
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "quest/sentinel/data_root.h"

namespace nevr_quest::data {

inline constexpr const char* kZipUrl = "https://mia.cdn.echo.taxi/_data.zip";
inline constexpr const char* kZipSha256 = "fc2eedeacc50d9ddf751e21914bb4188660cf5e79ce48aab24b84e757f4b543c";
inline constexpr std::uint64_t kZipBytes = 937395400;

// Every entry of the zip sits below this prefix; it is removed on extraction.
inline constexpr const char* kZipPrefix = "_data/";
// A file the pinned zip carries, relative to the data root: proves the `_data` there is this version.
inline constexpr const char* kManifestPath = "_data/5932408047/rad15/android/manifests/48037dc70b0ecab2";

// Names below the data root. Our staging directory, the download and a `_data` kept aside carry a marker file beside
// them (`<name>.nevr-owned`, staging.h); only an item with its marker is ours. The step deletes a marked staging
// directory and nothing else: a kept-aside `_data` is never deleted, and a name that only starts like ours (a player's
// own `_data.replaced-my-backup`) is left alone.
inline constexpr const char* kPartName = "_data.zip.part";
inline constexpr const char* kStagingPrefix = "_data.staging-";
inline constexpr const char* kReplacedPrefix = "_data.replaced-";
inline constexpr const char* kOwnerSuffix = ".nevr-owned";

// Disk the fetch needs below the data root: the zip plus its extracted tree (about 2.2 GiB), plus the old `_data`
// kept aside when one is replaced.
inline constexpr std::uint64_t kZipExtractedBytes = 1200000000;

enum class DataAction {
  kUse,      // the data is there: nothing to fetch
  kFetch,    // no data: fetch into the data root
  kReplace,  // a `_data` without this version's manifest: fetch, keep the old one aside
};

struct Decision {
  DataAction action;
  const char* root;    // where the data is, or goes
  const char* source;  // the data_root source token, for the log
};

// `resolution` comes from data_root::Resolve. A fetch goes to the media root (needs no storage permission); the legacy
// root is only ever used when it already holds data. An unmarked media root (it exists, `_data` is not directly under
// it) is a fetch into that directory (ADR 0011, order of fallbacks, case 3): it holds no data, and treating it as data
// would stop a fetch that was interrupted after the directory was made from ever being retried.
inline Decision Decide(const data_root::Resolution& resolution, data_root::ExistsFn exists) noexcept {
  if (!resolution.found) return {DataAction::kFetch, data_root::kMediaRoot, resolution.source};
  if (std::strcmp(resolution.source, "android_media_unmarked") == 0) {
    return {DataAction::kFetch, resolution.root, resolution.source};
  }
  char path[512];
  const int n = std::snprintf(path, sizeof(path), "%s/%s", resolution.root, kManifestPath);
  const bool present = n > 0 && static_cast<std::size_t>(n) < sizeof(path) && exists(path);
  return {present ? DataAction::kUse : DataAction::kReplace, resolution.root, resolution.source};
}

}  // namespace nevr_quest::data
