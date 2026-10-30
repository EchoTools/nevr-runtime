/* The bundled content pack (docs/adr/0029-quest-bundled-content-pack.md): game data an APK carries as assets, placed over
 * the stock data tree after the first-run data step (docs/adr/0011, 0013) has found or fetched that tree.
 *
 * An APK that carries a pack has `assets/nevr-pack/pack.json` (schema 1: the pack's id and version, the stock data version
 * it overlays, the sha-256 of each stock manifest it may replace, and every file with its path below
 * `_data/<version>/rad15/android/`, sha-256 and size) and the files themselves, STORED, at
 * `assets/nevr-pack/files/<path>`. The pack describes itself: nothing here names a hash of the pack.
 *
 * RunPackStep reads the APK with the zip reader (zip.h, raised limits: an APK has hundreds of entries) and:
 *   - no `pack.json`: nothing happens (outcome none, one Info event);
 *   - the stock manifest in the root is neither one of the hashes the pack requires nor the pack's own manifest: nothing is
 *     touched (stock_mismatch, a Warning naming both hashes) and the game starts on the data that is there;
 *   - otherwise each file that is not already in place with its recorded size and sha-256 is written to
 *     `<file>.nevr-pack.tmp` (a marker beside it first, staging.h), fsync'd, checked against `pack.json`, kept aside when it
 *     is the stock manifest (`<manifest>.stock`, never overwriting a backup), and renamed into place: the packages first, the
 *     manifest last, so the game never sees a manifest without the package it names. A kill leaves a marked temporary that the
 *     next run removes (CleanPackLeftovers). All files already in place: outcome current, nothing written.
 * A failure (no space, a digest that does not match, a write error) is shown through `present` and held through `hold`, and
 * the caller ends the process (data_wiring.cpp), like a failed data step.
 *
 * Built without exceptions: it runs on the game's thread.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "quest/data/fetch.h"
#include "quest/data/plan.h"
#include "quest/data/zip.h"

namespace nevr_quest::data {

inline constexpr const char* kPackJsonEntry = "assets/nevr-pack/pack.json";
inline constexpr const char* kPackFilesPrefix = "assets/nevr-pack/files/";
inline constexpr std::size_t kMaxPackJsonBytes = 64 * 1024;
inline constexpr std::size_t kMaxPackFiles = 8;
// Free space kept beyond the files themselves: the temporary and the manifest's backup are written before anything is renamed.
inline constexpr std::uint64_t kPackHeadroomBytes = 64ull * 1024 * 1024;
// A file the pack carries is read through the zip reader's single-entry limit.
inline constexpr std::uint64_t kMaxPackFileBytes = (1ull << 31) - 1;

// Extra files (ADR 0029, "Extra files"): a file a plugin of this APK reads, carried with the pack it belongs to and placed in that
// plugin's data directory, not in the data tree. Optional: a pack.json without `extra_files` is the pack it was before. Only the
// names below exist; each goes to one destination.
inline constexpr const char* kPackExtraPrefix = "assets/nevr-pack/extra/";
inline constexpr const char* kExtraDestQ15tData = "q15t_data";  // /sdcard/Android/data/<package>/files/q15t
inline constexpr std::size_t kMaxPackExtras = 2;
inline constexpr std::uint64_t kMaxPackExtraBytes = 32ull << 20;  // the decals module's own cap on its table
// The extra file names a pack may carry: decals.bin (the q15t decals module's table, docs/quest/q15t/modules/decals.md).
bool IsPackExtraName(const std::string& name) noexcept;

// What an APK is allowed to hold when it is read for a pack: hundreds of entries, none over the reader's entry limit, the whole
// archive under the 4 GiB a zip without zip64 can address (the reader refuses zip64).
ZipLimits PackZipLimits() noexcept;

struct PackFile {
  std::string path;    // below `_data/<stock_data_version>/rad15/android/`
  std::string sha256;  // 64 lowercase hex characters
  std::uint64_t size = 0;
};

struct PackExtra {
  std::string dest;    // kExtraDestQ15tData
  std::string name;    // IsPackExtraName
  std::string sha256;  // 64 lowercase hex characters
  std::uint64_t size = 0;
};

struct PackInfo {
  std::string id;
  std::string version;
  std::string stock_data_version;                  // the folder under `_data`
  std::vector<std::string> requires_stock_sha256;  // the stock manifests the pack may replace (1 or 2)
  std::vector<PackFile> files;
  std::string source_sha256;  // of the pack zip the repack was given
  std::uint64_t source_size = 0;
  std::string manifest_path;  // the entry of `files` that is the game's manifest
  std::string manifest_sha256;
  std::vector<PackExtra> extras;  // empty: the pack carries none
};

// Parses and checks `text` (pack.json). False with a token in `reason`: not_json, too_large, bad_schema, bad_id, bad_version,
// bad_stock_version, bad_requires, bad_file, bad_sha256, bad_size, no_manifest, wrong_manifest, duplicate_file, too_many_files,
// bad_source, bad_extra, duplicate_extra. The rules the repack tool applies (tools/quest_repack_libs.py, pack_problems) are these.
bool ParsePackJson(const std::string& text, PackInfo* out, std::string* reason);

enum class PackOutcome {
  kNone,           // the APK carries no pack
  kInvalid,        // it carries one this build cannot use; nothing was touched
  kStockMismatch,  // the root's manifest is not one the pack replaces; nothing was touched
  kCurrent,        // every file was already in place
  kPlaced,         // the pack's files are in place
  kFailed,         // a file could not be placed; the floor message was presented
};

const char* PackOutcomeName(PackOutcome outcome) noexcept;

enum class PackStatus {
  kOk = 0,
  kBadRoot,        // the root or the pack's directories cannot be written
  kNoSpace,        // not enough free space; nothing was written
  kExtractFailed,  // the APK entry did not read
  kHashMismatch,   // the bytes are not the sha-256 pack.json names
  kWriteFailed,
  kBackupFailed,   // the stock manifest could not be kept aside; it was not replaced
  kInterrupted,    // a test seam stopped the step the way a kill would
};

const char* PackStatusName(PackStatus status) noexcept;

enum class EventLevel { kInfo, kWarn, kError };

struct PackConfig {
  std::string apk_path;  // empty: unknown
  std::string root;      // the data root the engine reads
  std::string q15t_data_dir;  // where a `q15t_data` extra goes; empty: unknown (the extra is not placed, a Warning says so)
  // Seams. A null function means: statvfs, no events, no message, no hold, never interrupted.
  std::function<std::uint64_t(const std::string& root)> available_bytes;  // also asked for the extra file's directory
  std::function<void(EventLevel, const Event&)> on_event;  // pack_check, pack_place, pack_extra, pack_step, data_version
  std::function<void(const std::string& message)> present;
  std::function<void()> hold;
  // Called after a file's temporary is complete ("tmp_written") and after its rename ("renamed"), with the final path. False
  // stops the step on the spot and leaves what is on disk, as a kill would (the tests of the next run's cleanup).
  std::function<bool(const char* point, const std::string& path)> probe;
};

struct PackResult {
  PackOutcome outcome = PackOutcome::kNone;
  PackStatus status = PackStatus::kOk;
  std::string id;
  std::string version;
  std::string message;  // set when the outcome is kFailed
};

// The visible message for a failed placement: what failed and what to do. A no-space failure names the free and needed MiB.
struct PackFloorInfo {
  PackStatus status = PackStatus::kWriteFailed;
  std::string detail;
  std::string id;
  std::uint64_t needed_bytes = 0;
  std::uint64_t available_bytes = 0;
};
std::string BuildPackFloorMessage(const PackFloorInfo& info);

PackResult RunPackStep(const PackConfig& config);

// The APK that holds the library at `library_path` (what dladdr names for the sentinel): `<apk>!/lib/...` is `<apk>`; a library
// extracted to `<dir>/lib/<abi>/<name>` is `<dir>/base.apk` when that exists (`exists` is the filesystem). Empty when neither.
std::string ApkPathForLibrary(const std::string& library_path, bool (*exists)(const char* path) noexcept);

}  // namespace nevr_quest::data
