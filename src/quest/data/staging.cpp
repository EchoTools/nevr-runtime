#include "quest/data/staging.h"

#include <dirent.h>
#include <ftw.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#include "quest/data/plan.h"

namespace nevr_quest::data {

namespace {

int RemoveEntry(const char* path, const struct stat*, int, struct FTW*) { return ::remove(path); }

bool StartsWith(const char* name, const char* prefix) { return std::strncmp(name, prefix, std::strlen(prefix)) == 0; }

}  // namespace

const char* StageStatusName(StageStatus status) noexcept {
  switch (status) {
    case StageStatus::kOk: return "ok";
    case StageStatus::kRenameFailed: return "rename_failed";
    case StageStatus::kStatFailed: return "stat_failed";
    case StageStatus::kNoSpace: return "no_space";
    case StageStatus::kRemoveFailed: return "remove_failed";
    case StageStatus::kBadRoot: return "bad_root";
  }
  return "unknown";
}

std::string StagingPath(const std::string& root, const std::string& sha256_hex) {
  return root + "/" + kStagingPrefix + sha256_hex.substr(0, 12);
}

bool RemoveTree(const std::string& path) noexcept {
  struct stat st;
  if (::lstat(path.c_str(), &st) != 0) return errno == ENOENT;
  if (!S_ISDIR(st.st_mode)) return ::unlink(path.c_str()) == 0;
  // FTW_DEPTH: children first. FTW_PHYS: a link is removed, never followed.
  return ::nftw(path.c_str(), RemoveEntry, 16, FTW_DEPTH | FTW_PHYS) == 0;
}

int CleanLeftovers(const std::string& root, int* failed) noexcept {
  if (failed != nullptr) *failed = 0;
  DIR* dir = ::opendir(root.c_str());
  if (dir == nullptr) return 0;
  std::vector<std::string> doomed;
  while (const dirent* e = ::readdir(dir)) {
    if (StartsWith(e->d_name, kStagingPrefix) || StartsWith(e->d_name, kReplacedPrefix)) {
      doomed.emplace_back(root + "/" + e->d_name);
    }
  }
  ::closedir(dir);
  int removed = 0;
  for (const std::string& path : doomed) {
    if (RemoveTree(path)) {
      ++removed;
    } else if (failed != nullptr) {
      ++*failed;
    }
  }
  return removed;
}

StageStatus InstallStaged(const std::string& root, const std::string& staging, const std::string& utc_stamp,
                          std::string* replaced_to) {
  if (replaced_to != nullptr) replaced_to->clear();
  const std::string target = root + "/_data";
  struct stat st;
  const bool had_old = ::lstat(target.c_str(), &st) == 0;
  std::string aside;
  if (had_old) {
    aside = root + "/" + kReplacedPrefix + utc_stamp;
    if (::rename(target.c_str(), aside.c_str()) != 0) return StageStatus::kRenameFailed;
    if (replaced_to != nullptr) *replaced_to = aside;
  }
  if (::rename(staging.c_str(), target.c_str()) != 0) {
    if (had_old && ::rename(aside.c_str(), target.c_str()) == 0 && replaced_to != nullptr) replaced_to->clear();
    return StageStatus::kRenameFailed;
  }
  return StageStatus::kOk;
}

StageStatus CheckSpace(const std::string& root, std::uint64_t needed, std::uint64_t* available) {
  struct statvfs vfs;
  if (::statvfs(root.c_str(), &vfs) != 0) return StageStatus::kStatFailed;
  const std::uint64_t free_bytes = static_cast<std::uint64_t>(vfs.f_bavail) * vfs.f_frsize;
  if (available != nullptr) *available = free_bytes;
  return free_bytes >= needed ? StageStatus::kOk : StageStatus::kNoSpace;
}

}  // namespace nevr_quest::data
