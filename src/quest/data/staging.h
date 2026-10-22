/* The filesystem side of the first-run data step: staging, the swap, leftovers and the space check
 * (docs/adr/0011-quest-game-data-first-run.md). Plain POSIX, no exceptions.
 *
 * The data is extracted into `<root>/_data.staging-<sha prefix>` and moved into place by one rename to `<root>/_data`,
 * so `_data` is either absent, the old tree, or the complete new one. A `_data` being replaced is renamed to
 * `<root>/_data.replaced-<utc>` first and kept: the step never deletes it.
 *
 * Ownership is a marker file beside the item, `<item>.nevr-owned`, created before the item exists. A name that only
 * starts with our prefix is not ours: CleanLeftovers deletes a marked staging directory and nothing else, and
 * RecoverReplaced puts back only a marked replaced directory. Every rename is followed by an fsync of the root.
 */
#pragma once

#include <cstdint>
#include <string>

namespace nevr_quest::data {

enum class StageStatus { kOk = 0, kRenameFailed, kStatFailed, kNoSpace, kRemoveFailed, kBadRoot };

const char* StageStatusName(StageStatus status) noexcept;

// `<root>/_data.staging-<first 12 characters of sha256_hex>`
std::string StagingPath(const std::string& root, const std::string& sha256_hex);

// Removes a file or a whole tree without following symbolic links. A path that does not exist is success.
bool RemoveTree(const std::string& path) noexcept;

// `<item>.nevr-owned`
std::string OwnerMarkerPath(const std::string& item);

// Creates the marker beside `item` (the item need not exist yet) and syncs its directory. Call it before creating the
// staging directory or the download. A marker is an empty regular file: when something is already at that name, it is
// accepted only if it is such a file (an earlier marker of ours) and is never truncated; a file with content, a
// directory or a link there makes the call fail and is left as it is.
bool MarkOwned(const std::string& item) noexcept;
// True when `item`'s marker is an empty regular file (not a directory, a link, or a file with content).
bool IsOwned(const std::string& item) noexcept;

// fsync of a directory, so a rename or create in it survives a power loss.
bool SyncDir(const std::string& dir) noexcept;

// Deletes each `<root>/_data.staging-*` that has its marker (and the marker). A marker whose item is gone is removed
// too. Nothing else is deleted: not an unmarked name, not a `_data.replaced-*`, not the download. Returns how many
// staging directories it removed; `failed` (when not null) gets how many it could not.
int CleanLeftovers(const std::string& root, int* failed) noexcept;

// The crash between the two renames of InstallStaged leaves no `<root>/_data` and the old tree under a marked
// `<root>/_data.replaced-<utc>`. When `_data` is missing it renames the newest such directory back (and removes its
// marker); a marked name that is not a real directory (a link) is skipped. Returns true when it restored one. Run it before CleanLeftovers and before the data decision.
bool RecoverReplaced(const std::string& root, std::string* restored_from) noexcept;

// Called by InstallStaged at the points where a crash would leave a state the next launch must handle: "before_first_rename"
// (the aside marker is written, nothing is moved) and "between_renames" (the old tree is aside, `_data` is absent). For the
// tests, which look at the directory at that moment; null in production.
struct InstallProbe {
  void (*at)(void* context, const char* point) = nullptr;
  void* context = nullptr;
};

// Moves a complete `staging` directory into place as `<root>/_data`. When `<root>/_data` exists it is first renamed to
// `<root>/_data.replaced-<utc_stamp>`; when that rename fails nothing else is touched. When the final rename fails the
// old tree is moved back.
StageStatus InstallStaged(const std::string& root, const std::string& staging, const std::string& utc_stamp,
                          std::string* replaced_to, const InstallProbe* probe = nullptr);

// Whether the filesystem holding `root` has at least `needed` bytes free for an unprivileged writer.
StageStatus CheckSpace(const std::string& root, std::uint64_t needed, std::uint64_t* available);

}  // namespace nevr_quest::data
