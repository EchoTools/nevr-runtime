/* The zero-package stub manifests the engine needs at boot when the game's data is not on the headset (the whole-game mode of the
 * data step, data_step.h; docs/adr/0045-cdn-texture-delivery.md).
 *
 * The engine adds two package sets at boot (`CEngine::Initialize` -> `InitGlobals`, libr15 0x2ba87ec: `r14netclient` and
 * `r14netclient_en_US`), reading each set's manifest FILE directly through `CPackageManifest::AddPackage`, with no resolver hook in
 * the path. A manifest that is absent makes the add return non-zero and the engine stops at init; a manifest that is present with
 * package count 0 is accepted and the add returns 0 (libr15 0x2640a60, `cbz w8` over the block-size check, the chunk and resource
 * merges and the package opens). Every resource is then served by the resolver hook, which is the only place the tables are
 * searched on Quest (`FindEntry` has no caller there).
 *
 * The file is the shipped container (`ZSTD`, alignment 0x10, decompressed length, compressed length, one zstd frame) around a 0xc0 byte
 * body with every count 0 and the shipped block size 0x80000. The byte-level facts are in `docs/reference/package-manifest-format.md`
 * ("A zero-package manifest"): package count at +0x00, block size at +0x04 (not checked when the count is 0), resource, file and chunk
 * counts at +0x38, +0x78 and +0xb8; every other byte is read and ignored (libr15 `SPackageManifestData::Inspect` 0x26461c0 and the three
 * table inspectors). The decompressor takes the first dword as the magic and the compressed length must be nonzero (libr15 0x2b9dbcc).
 *
 * This is a Quest file because only the Quest data step exists: PC reads the installed `_data` and never fetches or writes it. The
 * set ids, the version folder and the platform directory are one table row for the one binary this step runs in.
 *
 * Pure: bytes and paths. Nothing here touches the filesystem.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace nevr_quest::data::boot_stub {

// CSymbol64("r14netclient") and CSymbol64("r14netclient_en_US"): the two sets the engine adds at boot. The id is the manifest file's name
// as `%016llx`. Another locale's second set is not written (the engine reads its locale's name, which this table does not know).
inline constexpr std::uint64_t kSetIds[] = {0x48037dc70b0ecab2ULL, 0x2b47aab238f60515ULL};
inline constexpr std::size_t kSetCount = sizeof(kSetIds) / sizeof(kSetIds[0]);

// The folder under `<root>/_data` the engine reads: the stock data's own, which is the data version the pinned data set (plan.h) names.
inline constexpr const char* kVersionFolder = "5932408047";
// What follows it: the engine's project path and the platform directory of the Quest build.
inline constexpr const char* kPlatformPath = "rad15/android";

// `%016llx`: the file name of a set's manifest.
std::string SetFileName(std::uint64_t set_id);

// `<root>/_data/<version>/rad15/android/manifests`
std::string ManifestsDir(const std::string& root);

// `<ManifestsDir(root)>/<SetFileName(set_id)>`
std::string ManifestPath(const std::string& root, std::uint64_t set_id);

// The whole file: the container and the zstd frame of the zero-package body. The same bytes for every set.
std::string StubFileBytes();

}  // namespace nevr_quest::data::boot_stub
