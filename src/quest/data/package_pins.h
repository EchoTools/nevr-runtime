/* The data sets whose package files the data step checks (docs/adr/0011-quest-game-data-first-run.md, "The packages").
 * Pure data: a set is named by the sha256 of the manifest the game reads, and lists the package files that manifest
 * points into, each with its byte length and, for the files a bundled content pack adds, its whole-file sha256.
 *
 * A package file is `<root>/_data/<version folder>/rad15/android/packages/<manifest id>_<index>` (the manifest id is the
 * last segment of kManifestPath, plan.h; the version folder is the one that holds the manifest, read from the root).
 *
 * A manifest outside the table is not checked, and the data step says so (`data_packages` outcome `unpinned`). The stock
 * manifest is deliberately not in the table: a wrong pin there would be a false alarm on every player's headset.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace nevr_quest::data {

struct PinnedPackage {
  unsigned index;        // the suffix after the manifest id: `<manifest id>_<index>`
  std::uint64_t bytes;   // the file's length
  const char* sha256;    // the file's whole-file digest, 64 lowercase hex characters
  bool hashed;           // true: the step hashes the file on the first launch of a build (the file the pack adds)
};

struct PinnedDataSet {
  const char* manifest_sha256;  // the sha256 of the manifest the game reads
  const PinnedPackage* packages;
  std::size_t count;
};

// Where the numbers come from: the sizes and sha256 digests of the pack's files as recorded in the pack's own
// metadata (q15t-upstream OUTPUTS.md) and, in a release, in the `game-data` entry of the signed release manifest.
// They describe ONE pack release (combatw19s 1.1.9, which requires the stock manifest
// 762c16ae58c9bc70eee071d8c0e2e8fc0408d1f0691032164029294c5cb7ec48) and change with every pack release, so this table
// is updated together with the pack and is never edited alone. Package 0 is the stock package the pack's manifest
// still points into (checked by length only); package 1 is the one the pack adds (length, then digest).
inline constexpr PinnedPackage kCombatPackPackages[] = {
    {0, 929620177u, "14d26efc718a8f8135039e47bd1321cbb556fc4181bf95ffcb5ef59c406a5a3b", false},
    {1, 689617013u, "decc77fdfc8ecd3b75ad3120f33dd1980a6a1f8c4c50f566ceb6483d9092db02", true},
};

inline constexpr PinnedDataSet kPinnedDataSets[] = {
    {"e1c4903622e4acf5b747f591868ef67d623e619ee63fd4af0268a593874a63e1", kCombatPackPackages,
     sizeof(kCombatPackPackages) / sizeof(kCombatPackPackages[0])},
};

inline constexpr std::size_t kPinnedDataSetCount = sizeof(kPinnedDataSets) / sizeof(kPinnedDataSets[0]);

// The set among `sets` whose manifest digest is `manifest_sha256`, or null when the manifest is not in the table. The
// table is the pinned one unless a test passes its own.
inline const PinnedDataSet* FindPinnedDataSet(const std::string& manifest_sha256,
                                              const PinnedDataSet* sets = kPinnedDataSets,
                                              std::size_t count = kPinnedDataSetCount) noexcept {
  for (std::size_t i = 0; i < count; ++i) {
    if (manifest_sha256 == sets[i].manifest_sha256) return &sets[i];
  }
  return nullptr;
}

}  // namespace nevr_quest::data
