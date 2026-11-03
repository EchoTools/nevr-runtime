/* The first-run data step (docs/adr/0011-quest-game-data-first-run.md): decide from the data roots, fetch when the
 * data is missing, and say so in words a player can read when it cannot be had.
 *
 * Decide (plan.h) picks use, fetch or replace; RunFetch does the download, verification, extraction and swap. When the
 * data is in use and its manifest is in the pinned table (package_pins.h), the package files the manifest points into are
 * checked and a `data_packages` event says the result; a failure is logged and the data is still used. A fetch
 * that fails is shown through `present` (the visible message, the floor of the plan) and held through `hold` so it can
 * be read before the caller ends the process. Nothing here exits, throws or touches the game: the caller (the
 * permission-wait hook's after-storage step) owns the process.
 *
 * Built without exceptions: it runs on the game's thread.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "quest/data/fetch.h"
#include "quest/data/package_pins.h"
#include "quest/sentinel/data_root.h"

namespace nevr_quest::data {

enum class StepOutcome {
  kUsed,     // the data was there
  kFetched,  // the data was fetched and is in place
  kFailed,   // the data could not be had; the floor message was presented
  kMissing,  // the data is not there and this build does not fetch (allow_fetch false); kGameDataMissing was presented
};

const char* StepOutcomeName(StepOutcome outcome) noexcept;

struct FloorInfo {
  FetchStatus status = FetchStatus::kRetriesExhausted;
  std::string detail;
  std::string root;      // the data root the fetch targeted
  std::string build_id;  // the game build the data is for
  bool replacing = false;
  std::uint64_t needed_bytes = 0;
  std::uint64_t available_bytes = 0;
};

// The visible message: which path, which build, what to do. A no-space failure names the free and needed MiB.
std::string BuildFloorMessage(const FloorInfo& info);

// Shown when a download is about to start, so a first-run player does not read the wait as a hang.
inline constexpr const char* kDownloadNotice =
    "Downloading Echo VR game data, about 900 MB, this can take a few minutes.";

// Shown when the storage permission was not granted and the media root could not be written either. Without the grant
// the legacy root cannot be looked at: data the player already has there is only usable once Storage is allowed.
inline constexpr const char* kNeedsStoragePermission =
    "Echo VR needs the Storage permission to set up its game data. Open the headset's Settings, find Echo VR under "
    "Apps, allow Storage (Files and media), then start the game again.";

// Shown when the game data is not on the headset and this build does not download it: a player installs the
// production build through Echo Launcher once, which puts the data there (docs/adr/0013-quest-game-data-from-the-launcher.md).
inline constexpr const char* kGameDataMissing =
    "Echo VR game data is missing. Install Echo VR with Echo Launcher first, then start the game again.";

struct StepConfig {
  data_root::ExistsFn exists = nullptr;  // the filesystem, for Resolve and Decide
  // The legacy root (/storage/emulated/0/readyatdawn) is read only with the storage permission; the media root
  // (Android/media/<package>, the app's own directory) needs none. False: the legacy root is neither recovered nor
  // looked at, the decision is made on the media root alone, and one `data_legacy_skipped` event says so.
  bool legacy_root_readable = true;
  // Shown instead of the step's own message when the root cannot be written (FetchStatus::kBadRoot); empty: the step's
  // own. The caller that has no Storage grant sets it, so the player reads one message, held once.
  std::string bad_root_message;
  // The roots whose leftovers of an interrupted install are put right before anything is decided (RecoverReplaced,
  // then CleanLeftovers, on each). Empty: every data-root candidate (data_root.h) the config may read.
  std::vector<std::string> recovery_roots;
  FetchConfig fetch;                      // root and replacing are set by the step
  // False in a release build: the step only looks. Nothing is fetched, recovered or cleaned (no write at all); when the
  // data is not there it presents kGameDataMissing and ends as kMissing.
  bool allow_fetch = true;
  // The one root the engine reads this launch (the legacy root without an OBB; the root the OBB skip serves with one).
  // Not empty: only this root counts, as data and as the target of a fetch. Empty: the two data-root candidates.
  std::string engine_root;
  std::string build_id;
  // Test seam: where a fetch writes when it is not empty. Production leaves it empty and the fetch goes where Decide
  // says (the media root, or the root that holds a `_data` of another build).
  std::string fetch_root_override;
  std::function<void()> announce;                           // once, when a download is certain to start (after the space check); null: none
  std::function<void(const std::string& message)> present;  // the visible message; null: none
  std::function<void()> hold;                                // after the message, so it can be read; null: none
  std::function<void(const Event&)> on_step;                // data_check, data_packages; null: none
  // The events the step logs at Warning (a `data_packages` failure, a marker that could not be written). Null: they go
  // to `on_step`, at its level.
  std::function<void(const Event&)> on_step_warn;
  // Read-only seams for the data version the step logs (`data_version`, `manifest_sha256`). Null: the real filesystem.
  // `list_dir` gives the names in a directory (none when it cannot be read); `read_file` fills `bytes` with a whole
  // file of at most 16 MiB and says whether it could.
  std::function<std::vector<std::string>(const std::string& dir)> list_dir;
  std::function<bool(const std::string& path, std::string& bytes)> read_file;
  // Seams for the package check (package_pins.h). Null: the real filesystem.
  // `file_size` gives the length of a regular file and says whether it is there. `hash_file` streams a file through
  // sha256 and gives the 64 lowercase hex characters; false when it cannot be read to its end (a test injects results
  // so it needs no 690 MB file). `write_file` writes `bytes` as the whole content of `path` (a temporary, then a
  // rename) and says whether it did.
  std::function<bool(const std::string& path, std::uint64_t& bytes)> file_size;
  std::function<bool(const std::string& path, std::string& sha256_hex)> hash_file;
  std::function<bool(const std::string& path, const std::string& bytes)> write_file;
  // Test seam: the table of pinned data sets. Null: the pinned table (package_pins.h).
  const PinnedDataSet* pinned_sets = nullptr;
  std::size_t pinned_set_count = 0;
};

// The roots the leftovers are put right on: `recovery_roots`, or every candidate when it is empty, without the legacy
// root when the config may not read it.
std::vector<std::string> RecoveryRootsFor(const StepConfig& config);

struct StepResult {
  StepOutcome outcome = StepOutcome::kUsed;
  FetchStatus status = FetchStatus::kOk;
  DataAction action = DataAction::kUse;
  std::string root;
  std::string message;  // set when the outcome is kFailed
};

StepResult RunDataStep(const StepConfig& config, Transport& transport);

}  // namespace nevr_quest::data
