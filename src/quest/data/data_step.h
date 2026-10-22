/* The first-run data step (docs/adr/0011-quest-game-data-first-run.md): decide from the data roots, fetch when the
 * data is missing, and say so in words a player can read when it cannot be had.
 *
 * Decide (plan.h) picks use, fetch or replace; RunFetch does the download, verification, extraction and swap. A fetch
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
#include "quest/sentinel/data_root.h"

namespace nevr_quest::data {

enum class StepOutcome {
  kUsed,     // the data was there
  kFetched,  // the data was fetched and is in place
  kFailed,   // the data could not be had; the floor message was presented
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

// Shown instead of a data step when the storage permission was not granted: nothing is read, fetched or replaced.
inline constexpr const char* kNeedsStoragePermission =
    "Echo VR needs the Storage permission to set up its game data. Open the headset's Settings, find Echo VR under "
    "Apps, allow Storage (Files and media), then start the game again.";

struct StepConfig {
  data_root::ExistsFn exists = nullptr;  // the filesystem, for Resolve and Decide
  // The roots whose leftovers of an interrupted install are put right before anything is decided (RecoverReplaced,
  // then CleanLeftovers, on each). Empty: every data-root candidate (data_root.h), the media root and the legacy root.
  std::vector<std::string> recovery_roots;
  FetchConfig fetch;                      // root and replacing are set by the step
  std::string build_id;
  // Test seam: where a fetch writes when it is not empty. Production leaves it empty and the fetch goes where Decide
  // says (the media root, or the root that holds a `_data` of another build).
  std::string fetch_root_override;
  std::function<void()> announce;                           // once, when a download is certain to start (after the space check); null: none
  std::function<void(const std::string& message)> present;  // the visible message; null: none
  std::function<void()> hold;                                // after the message, so it can be read; null: none
  std::function<void(const Event&)> on_step;                // data_check; null: none
};

struct StepResult {
  StepOutcome outcome = StepOutcome::kUsed;
  FetchStatus status = FetchStatus::kOk;
  DataAction action = DataAction::kUse;
  std::string root;
  std::string message;  // set when the outcome is kFailed
};

StepResult RunDataStep(const StepConfig& config, Transport& transport);

}  // namespace nevr_quest::data
