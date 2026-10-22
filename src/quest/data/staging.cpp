#include "quest/data/staging.h"

#include <dirent.h>
#include <fcntl.h>
#include <ftw.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#include "quest/data/plan.h"

namespace nevr_quest::data {

namespace {

int RemoveEntry(const char* path, const struct stat*, int, struct FTW*) { return ::remove(path); }

bool StartsWith(const char* name, const char* prefix) { return std::strncmp(name, prefix, std::strlen(prefix)) == 0; }

bool EndsWith(const std::string& name, const char* suffix) {
  const std::size_t n = std::strlen(suffix);
  return name.size() >= n && name.compare(name.size() - n, n, suffix) == 0;
}

bool Lexists(const std::string& path) {
  struct stat st;
  return ::lstat(path.c_str(), &st) == 0;
}

std::string DirOf(const std::string& path) {
  const std::size_t slash = path.rfind('/');
  return slash == std::string::npos ? std::string(".") : path.substr(0, slash == 0 ? 1 : slash);
}

std::vector<std::string> ListNames(const std::string& root) {
  std::vector<std::string> names;
  DIR* dir = ::opendir(root.c_str());
  if (dir == nullptr) return names;
  while (const dirent* e = ::readdir(dir)) names.emplace_back(e->d_name);
  ::closedir(dir);
  return names;
}

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

std::string OwnerMarkerPath(const std::string& item) { return item + kOwnerSuffix; }

bool SyncDir(const std::string& dir) noexcept {
  const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return false;
  const bool ok = ::fsync(fd) == 0;
  ::close(fd);
  return ok;
}

bool MarkOwned(const std::string& item) noexcept {
  const std::string marker = OwnerMarkerPath(item);
  const int fd = ::open(marker.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
  if (fd < 0) return false;
  const bool synced = ::fsync(fd) == 0;
  ::close(fd);
  return synced && SyncDir(DirOf(marker));
}

bool IsOwned(const std::string& item) noexcept {
  struct stat st;
  return ::lstat(OwnerMarkerPath(item).c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

int CleanLeftovers(const std::string& root, int* failed) noexcept {
  if (failed != nullptr) *failed = 0;
  int removed = 0;
  for (const std::string& name : ListNames(root)) {
    if (!StartsWith(name.c_str(), kStagingPrefix)) continue;
    const std::string path = root + "/" + name;
    if (EndsWith(name, kOwnerSuffix)) {
      // A marker whose staging directory is gone.
      if (!Lexists(path.substr(0, path.size() - std::strlen(kOwnerSuffix)))) ::unlink(path.c_str());
      continue;
    }
    if (!IsOwned(path)) continue;  // a name that only looks like ours
    if (RemoveTree(path) && ::unlink(OwnerMarkerPath(path).c_str()) == 0) {
      ++removed;
    } else if (failed != nullptr) {
      ++*failed;
    }
  }
  return removed;
}

bool RecoverReplaced(const std::string& root, std::string* restored_from) noexcept {
  if (restored_from != nullptr) restored_from->clear();
  const std::string target = root + "/_data";
  if (Lexists(target)) return false;
  std::string newest;
  for (const std::string& name : ListNames(root)) {
    if (!StartsWith(name.c_str(), kReplacedPrefix) || EndsWith(name, kOwnerSuffix)) continue;
    if (!IsOwned(root + "/" + name)) continue;
    if (name > newest) newest = name;  // the stamp is UTC, so names sort by time
  }
  if (newest.empty()) return false;
  const std::string from = root + "/" + newest;
  if (::rename(from.c_str(), target.c_str()) != 0) return false;
  SyncDir(root);
  ::unlink(OwnerMarkerPath(from).c_str());
  SyncDir(root);
  if (restored_from != nullptr) *restored_from = from;
  return true;
}

StageStatus InstallStaged(const std::string& root, const std::string& staging, const std::string& utc_stamp,
                          std::string* replaced_to) {
  if (replaced_to != nullptr) replaced_to->clear();
  const std::string target = root + "/_data";
  const bool had_old = Lexists(target);
  std::string aside;
  bool marked_before = false;
  if (had_old) {
    aside = root + "/" + kReplacedPrefix + utc_stamp;
    // The marker first: a crash after the rename must leave the old tree recognisable as ours.
    marked_before = IsOwned(aside);  // the same stamp twice: leave the earlier marker as it was
    if (!MarkOwned(aside)) return StageStatus::kRenameFailed;
    if (::rename(target.c_str(), aside.c_str()) != 0) {
      if (!marked_before) ::unlink(OwnerMarkerPath(aside).c_str());
      return StageStatus::kRenameFailed;
    }
    SyncDir(root);
    if (replaced_to != nullptr) *replaced_to = aside;
  }
  if (::rename(staging.c_str(), target.c_str()) != 0) {
    if (had_old && ::rename(aside.c_str(), target.c_str()) == 0) {
      if (!marked_before) ::unlink(OwnerMarkerPath(aside).c_str());
      SyncDir(root);
      if (replaced_to != nullptr) replaced_to->clear();
    }
    return StageStatus::kRenameFailed;
  }
  SyncDir(root);
  ::unlink(OwnerMarkerPath(staging).c_str());  // the staging directory is `_data` now
  SyncDir(root);
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
