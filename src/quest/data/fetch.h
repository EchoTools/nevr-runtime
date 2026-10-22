/* The first-run data fetch (docs/adr/0011-quest-game-data-first-run.md): download the pinned zip into
 * `<root>/_data.zip.part` (resuming from its size), verify its whole-file sha256, extract into a staging directory,
 * swap it in as `<root>/_data`, and clean up.
 *
 * Every outcome is a FetchStatus; nothing here exits, throws or blocks past its bounds. The steps are reported as
 * events (`data_fetch`, `data_extract`, `data_swap`) with string fields, never a URL: the caller formats and logs them.
 * Built without exceptions: it runs on the game's thread.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "quest/data/plan.h"
#include "quest/data/transport.h"
#include "quest/data/zip.h"

namespace nevr_quest::data {

enum class FetchStatus {
  kOk = 0,
  kBadRoot,           // the data root cannot be created or written
  kNoSpace,           // not enough free space; nothing was downloaded
  kRetriesExhausted,  // every attempt failed (network, status, size or digest)
  kExtractFailed,     // the verified zip did not extract
  kSwapFailed,        // the staged tree could not be moved into place
  kCancelled,
};

const char* FetchStatusName(FetchStatus status) noexcept;

// The wait before retry `attempt` (2, 3, ...): 2 s, 4 s, 8 s, 16 s, then 30 s, so five attempts wait 30 s in all.
inline constexpr unsigned kBackoffBaseMs = 2000;
inline constexpr unsigned kBackoffMaxMs = 30000;
inline constexpr unsigned kBackoffSliceMs = 250;
unsigned DefaultBackoffMs(int attempt) noexcept;

struct Event {
  std::string name;  // data_fetch, data_extract or data_swap
  std::vector<std::pair<std::string, std::string>> fields;
};

struct FetchConfig {
  std::string root;       // the data root; created when missing
  bool replacing = false;  // a `_data` of another version is there: keep it aside, and need twice the space
  std::string url = kZipUrl;
  std::string sha256 = kZipSha256;
  std::uint64_t bytes = kZipBytes;
  std::uint64_t extracted_bytes = kZipExtractedBytes;
  int max_attempts = 5;
  ZipLimits zip_limits;
  std::string utc_stamp;  // names the replaced tree, `YYYYMMDDTHHMMSSZ`
  // Seams. Defaults (a null function) mean: statvfs, the default backoff below, never cancelled, no events.
  std::function<std::uint64_t(const std::string& root)> available_bytes;
  // Called before retry `attempt` (2, 3, ...). Null: DefaultBackoffMs(attempt) of real sleep, in slices of
  // kBackoffSliceMs through `sleep_ms`, ending early when `keep_going` says stop.
  std::function<void(int attempt)> backoff;
  std::function<void(unsigned ms)> sleep_ms;           // null: nanosleep

  std::function<bool()> keep_going;                   // false cancels
  std::function<void(const Event&)> on_event;
  std::function<void(std::uint64_t downloaded, std::uint64_t total)> on_progress;
};

struct FetchResult {
  FetchStatus status = FetchStatus::kOk;
  std::string detail;      // the failing step's reason, for the visible message
  int attempts = 0;
  std::string replaced_to;  // where the old `_data` went, when one was replaced
  std::uint64_t needed_bytes = 0;     // kNoSpace: what the fetch needed
  std::uint64_t available_bytes = 0;  // kNoSpace: what was free
};

FetchResult RunFetch(const FetchConfig& config, Transport& transport);

}  // namespace nevr_quest::data
