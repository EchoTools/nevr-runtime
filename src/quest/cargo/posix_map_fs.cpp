#include "quest/cargo/posix_map_fs.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace nevr_quest::maps {

namespace {

constexpr std::uint64_t kNanosecondsPerSecond = 1000000000;
// The descriptors nftw may hold open at once while it walks a tree: one per directory level it keeps open (deeper levels are
// reopened). The cache's own depth is <id>/<platform>/<version>/<member dir>, well inside it.
constexpr int kWalkDescriptors = 16;
// "/proc/self/fd/<n>" with the largest int.
constexpr std::size_t kFdLinkBytes = sizeof("/proc/self/fd/") + std::numeric_limits<int>::digits10 + 1;

int RemoveEntry(const char* path, const struct stat*, int flag, struct FTW*) {
  return flag == FTW_DP ? ::rmdir(path) : ::unlink(path);
}

}  // namespace

bool PosixMapFs::StampOf(std::int64_t handle, nevr_maps::FileStamp* out) {
  struct stat st {};
  if (handle < 0 || handle > std::numeric_limits<int>::max() || ::fstat(static_cast<int>(handle), &st) != 0 || !S_ISREG(st.st_mode)) {
    return false;
  }
  out->volume = static_cast<std::uint64_t>(st.st_dev);
  out->id_hi = 0;
  out->id_lo = static_cast<std::uint64_t>(st.st_ino);
  out->size = static_cast<std::uint64_t>(st.st_size);
  out->last_write = static_cast<std::uint64_t>(st.st_mtim.tv_sec) * kNanosecondsPerSecond + static_cast<std::uint64_t>(st.st_mtim.tv_nsec);
  return true;
}

std::string PosixMapFs::FinalPathOf(std::int64_t handle) {
  if (handle < 0 || handle > std::numeric_limits<int>::max()) return {};
  char link[kFdLinkBytes];
  std::snprintf(link, sizeof(link), "/proc/self/fd/%d", static_cast<int>(handle));
  char target[PATH_MAX];
  const ssize_t length = ::readlink(link, target, sizeof(target));
  if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(target)) return {};
  return std::string(target, static_cast<std::size_t>(length));
}

bool PosixMapFs::OpenHeld(const std::string& path, std::int64_t* handle, std::uint64_t* size) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return false;
  struct stat st {};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    ::close(fd);
    return false;
  }
  *handle = fd;
  *size = static_cast<std::uint64_t>(st.st_size);
  return true;
}

// Sorted, as the PC's ListDir returns them, so a caller that walks a directory sees one order on both platforms.
bool PosixMapFs::ListDir(const std::string& dir, std::vector<std::string>* names) {
  DIR* const d = ::opendir(dir.c_str());
  if (d == nullptr) return false;
  std::vector<std::string> listed;
  while (const dirent* entry = ::readdir(d)) {
    const std::string name = entry->d_name;
    if (name != "." && name != "..") listed.push_back(name);
  }
  ::closedir(d);
  std::sort(listed.begin(), listed.end());
  *names = std::move(listed);
  return true;
}

// The contract is the PC's: true when the folder is gone or never was, and a path that is not a folder is false.
bool PosixMapFs::RemoveTree(const std::string& dir) {
  struct stat st {};
  if (::lstat(dir.c_str(), &st) != 0) return errno == ENOENT;
  if (S_ISLNK(st.st_mode)) return ::unlink(dir.c_str()) == 0;  // a link to a folder: the link goes, never what it names
  if (!S_ISDIR(st.st_mode)) return false;
  // FTW_PHYS: a link is removed as itself and never followed; FTW_DEPTH: a directory after what it holds.
  return ::nftw(dir.c_str(), RemoveEntry, kWalkDescriptors, FTW_DEPTH | FTW_PHYS) == 0;
}

bool PosixMapFs::RenameDir(const std::string& from, const std::string& to) {
  struct stat st {};
  // rename(2) would replace an empty directory at `to`; the contract refuses any `to` that exists. Only this process writes the
  // cache, so nothing can create `to` between the check and the rename.
  if (::lstat(to.c_str(), &st) == 0 || errno != ENOENT) return false;
  return ::rename(from.c_str(), to.c_str()) == 0;
}

}  // namespace nevr_quest::maps
