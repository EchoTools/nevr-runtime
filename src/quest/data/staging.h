/* The filesystem side of the first-run data step: staging, the swap, leftovers and the space check
 * (docs/adr/0011-quest-game-data-first-run.md). Plain POSIX, no exceptions.
 *
 * The data is extracted into `<root>/_data.staging-<sha prefix>` and moved into place by one rename to `<root>/_data`,
 * so `_data` is either absent, the old tree, or the complete new one. A `_data` being replaced is renamed to
 * `<root>/_data.replaced-<utc>` first. Leftovers (a staging directory, a replaced one) carry our prefixes; they are
 * the only things CleanLeftovers deletes.
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

// Deletes `<root>/_data.staging-*` and `<root>/_data.replaced-*`. Returns how many it removed; `failed` (when not null)
// gets how many it could not.
int CleanLeftovers(const std::string& root, int* failed) noexcept;

// Moves a complete `staging` directory into place as `<root>/_data`. When `<root>/_data` exists it is first renamed to
// `<root>/_data.replaced-<utc_stamp>`; when that rename fails nothing else is touched. When the final rename fails the
// old tree is moved back.
StageStatus InstallStaged(const std::string& root, const std::string& staging, const std::string& utc_stamp,
                          std::string* replaced_to);

// Whether the filesystem holding `root` has at least `needed` bytes free for an unprivileged writer.
StageStatus CheckSpace(const std::string& root, std::uint64_t needed, std::uint64_t* available);

}  // namespace nevr_quest::data
