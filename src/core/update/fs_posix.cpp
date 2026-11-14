// The POSIX UpdateFs (core/update/fs.h): the calls ADR 0024 names for Quest, which the Linux host tests run as well. Every path
// lies under the nevr root that MakePosixUpdateFs was given. The root is opened as named: it is the app's own folder, and the
// system may keep links above it. Every folder between the root and a path is opened with O_NOFOLLOW and must be a folder, so
// a link at any folder, or at the file itself, refuses the operation: nothing is read or written through a link (ADR 0024,
// section 4, item 2). Paths are the engine's own, composed from the root and the closed vocabulary of core/update/names.h.
// No Log() here: the engine logs every decision.

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>  // renameat(2) is declared with the stdio functions on glibc
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/update/fs.h"
#include "core/update/names.h"

namespace nevr_update {
namespace {

namespace names = nevr_update_names;

constexpr char kSeparator = '/';
constexpr int kNoFd = -1;
// The mode of a file the engine creates: its owner reads and writes it, nobody else.
constexpr mode_t kNewFileMode = 0600;

// A descriptor, closed when it goes out of scope. Closing keeps errno, so a cleanup never changes the reason of a failure.
class Fd {
 public:
  explicit Fd(int fd = kNoFd) noexcept : fd_(fd) {}
  ~Fd() { Close(); }
  Fd(Fd&& other) noexcept : fd_(other.Release()) {}
  Fd& operator=(Fd&& other) noexcept {
    if (this != &other) {
      Close();
      fd_ = other.Release();
    }
    return *this;
  }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;

  bool ok() const noexcept { return fd_ != kNoFd; }
  int get() const noexcept { return fd_; }
  int Release() noexcept {
    const int fd = fd_;
    fd_ = kNoFd;
    return fd;
  }
  void Close() noexcept {
    if (fd_ == kNoFd) return;
    const int saved = errno;
    ::close(fd_);
    errno = saved;
    fd_ = kNoFd;
  }

 private:
  int fd_;
};

std::string WithoutTrailingSeparators(std::string root) {
  while (!root.empty() && root.back() == kSeparator) root.pop_back();
  return root;
}

// The names below the root that `path` spells: the root, one separator, then one or more plain names. An empty name, '.', '..'
// or a NUL refuses the whole path, so no path reaches a file the root does not hold.
bool SplitBelow(const std::string& root, const std::string& path, std::vector<std::string>* parts) {
  if (root.empty() || path.size() <= root.size() + 1 || path.compare(0, root.size(), root) != 0 ||
      path[root.size()] != kSeparator || path.find('\0') != std::string::npos) {
    return false;
  }
  for (std::size_t begin = root.size() + 1; begin < path.size();) {
    std::size_t end = path.find(kSeparator, begin);
    if (end == std::string::npos) end = path.size();
    std::string part = path.substr(begin, end - begin);
    if (part.empty() || part == "." || part == "..") return false;
    parts->push_back(std::move(part));
    begin = end + 1;
  }
  return !parts->empty();
}

// The folder the path is in, reached from the root one name at a time. Each name is opened with O_NOFOLLOW and must be a
// folder: a link at any of them fails the walk with ELOOP, and a file where a folder should be fails it with ENOTDIR. `*leaf`
// is the last name of the path. On failure errno says why and the descriptor is not open.
Fd OpenParentOf(const std::string& root, const std::string& path, std::string* leaf) {
  std::vector<std::string> parts;
  if (!SplitBelow(root, path, &parts)) {
    errno = EINVAL;
    return Fd();
  }
  Fd dir(::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  if (!dir.ok()) return dir;
  for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
    Fd next(::openat(dir.get(), parts[i].c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC));
    if (!next.ok()) return next;
    struct stat st{};
    if (::fstat(next.get(), &st) != 0) return Fd();
    if (!S_ISDIR(st.st_mode)) {
      errno = ENOTDIR;
      return Fd();
    }
    dir = std::move(next);
  }
  *leaf = parts.back();
  return dir;
}

// The errors that say a name is not there: a missing file or folder, or a file where a folder should be.
bool AbsentOnPath(int err) noexcept { return err == ENOENT || err == ENOTDIR; }

void SetError(int* os_error, int code) noexcept {
  if (os_error != nullptr) *os_error = code;
}

// How a fill of `text` from a file ended: every byte was read; the file ended before `text` was full (it is shorter than the size
// fstat reported, so it is damaged, as the PC host reads it); or a read failed (the file is unreadable).
enum class FillResult { kFull, kEndedEarly, kReadError };

FillResult ReadAll(int fd, std::string* text) {
  std::size_t got = 0;
  while (got < text->size()) {
    const ssize_t n = ::read(fd, &(*text)[got], text->size() - got);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) return FillResult::kReadError;
    if (n == 0) return FillResult::kEndedEarly;
    got += static_cast<std::size_t>(n);
  }
  return FillResult::kFull;
}

// Writes all of `text`. False on a write error.
bool WriteAll(int fd, const std::string& text) {
  std::size_t done = 0;
  while (done < text.size()) {
    const ssize_t n = ::write(fd, text.data() + done, text.size() - done);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    done += static_cast<std::size_t>(n);
  }
  return true;
}

// The entries of the folder are on disk when this returns: a rename or a removal in it survives a power cut, which the journal's
// recovery depends on (ADR 0024, section 4, item 5).
bool FlushFolder(int dir_fd) noexcept { return ::fsync(dir_fd) == 0; }

void RemoveChildren(int dir_fd) noexcept;

// Removes the entry `name` of the folder `dir_fd`, and for a folder all that is in it. A link is removed itself and never
// entered: fstatat does not follow it, and a folder is opened with O_NOFOLLOW (ADR 0024, section 4, item 2).
void RemoveEntry(int dir_fd, const char* name) noexcept {
  struct stat st{};
  if (::fstatat(dir_fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) return;
  if (!S_ISDIR(st.st_mode)) {
    static_cast<void>(::unlinkat(dir_fd, name, 0));
    return;
  }
  {
    Fd folder(::openat(dir_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (folder.ok()) RemoveChildren(folder.get());
  }
  static_cast<void>(::unlinkat(dir_fd, name, AT_REMOVEDIR));
}

void RemoveChildren(int dir_fd) noexcept {
  const int copy = ::fcntl(dir_fd, F_DUPFD_CLOEXEC, 0);  // fdopendir owns its descriptor, so it gets a copy
  if (copy < 0) return;
  DIR* listing = ::fdopendir(copy);
  if (listing == nullptr) {
    ::close(copy);
    return;
  }
  while (const dirent* entry = ::readdir(listing)) {
    if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) continue;
    RemoveEntry(dir_fd, entry->d_name);
  }
  ::closedir(listing);
}

std::uint64_t MillisecondsSince(std::chrono::steady_clock::time_point start) noexcept {
  const std::chrono::milliseconds spent =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
  return spent.count() < 0 ? 0 : static_cast<std::uint64_t>(spent.count());
}

// update/lock held by a flock on its descriptor. The lock is let go when the descriptor closes. The file stays (the interface
// leaves that to the implementation), so nothing needs an unlink that another process could race.
class FlockLock final : public FsLock {
 public:
  explicit FlockLock(int fd) noexcept : fd_(fd) {}
  ~FlockLock() override { Release(); }
  void Release() noexcept override {
    if (fd_ == kNoFd) return;
    static_cast<void>(::close(fd_));
    fd_ = kNoFd;
  }

 private:
  int fd_;
};

class PosixFs final : public UpdateFs {
 public:
  explicit PosixFs(std::string root) : root_(WithoutTrailingSeparators(std::move(root))) {}

  char Separator() const noexcept override { return kSeparator; }

  // A file of update/, at most the cap the engine applies. A link, a file that is not regular and a file another program holds are
  // unreadable, never absent.
  ReadResult ReadSmallFile(const std::string& path, std::string* out) const noexcept override {
    try {
      std::string leaf;
      Fd dir = OpenParentOf(root_, path, &leaf);
      if (!dir.ok()) return AbsentOnPath(errno) ? ReadResult::kAbsent : ReadResult::kUnreadable;
      Fd file(::openat(dir.get(), leaf.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
      if (!file.ok()) return AbsentOnPath(errno) ? ReadResult::kAbsent : ReadResult::kUnreadable;
      struct stat st{};
      if (::fstat(file.get(), &st) != 0 || !S_ISREG(st.st_mode)) return ReadResult::kUnreadable;
      if (st.st_size < 0 || static_cast<unsigned long long>(st.st_size) > names::kMaxJsonBytes) return ReadResult::kTooBig;
      std::string text(static_cast<std::size_t>(st.st_size), '\0');
      const FillResult filled = ReadAll(file.get(), &text);
      if (filled == FillResult::kReadError) return ReadResult::kUnreadable;
      if (filled == FillResult::kEndedEarly) return ReadResult::kTooBig;
      *out = std::move(text);
      return ReadResult::kOk;
    } catch (const std::bad_alloc&) {
      return ReadResult::kUnreadable;
    }
  }

  // A temporary beside the target, flushed, then renamed over it: a reader never sees half a file. The name is the host's own
  // spelling (`<file>.tmp-<pid>`), which a start removes after a kill between the write and the rename.
  bool WriteByRename(const std::string& path, const std::string& text) const noexcept override {
    try {
      std::string leaf;
      Fd dir = OpenParentOf(root_, path, &leaf);
      if (!dir.ok()) return false;
      const std::string temporary = leaf + ".tmp-" + std::to_string(::getpid());
      Fd file(::openat(dir.get(), temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, kNewFileMode));
      if (!file.ok()) return false;
      const bool written = WriteAll(file.get(), text) && ::fsync(file.get()) == 0;
      file.Close();
      if (!written || ::renameat(dir.get(), temporary.c_str(), dir.get(), leaf.c_str()) != 0) {
        static_cast<void>(::unlinkat(dir.get(), temporary.c_str(), 0));
        return false;
      }
      return FlushFolder(dir.get());
    } catch (const std::bad_alloc&) {
      return false;
    }
  }

  // False when the file was not removed, or the removal is not yet on disk.
  bool RemoveFile(const std::string& path) const noexcept override {
    try {
      std::string leaf;
      Fd dir = OpenParentOf(root_, path, &leaf);
      if (!dir.ok()) return false;
      if (::unlinkat(dir.get(), leaf.c_str(), 0) != 0) return false;
      return FlushFolder(dir.get());
    } catch (const std::bad_alloc&) {
      return false;
    }
  }

  // A folder whose way is refused, or that is absent, is left alone: nothing below a link is entered.
  void RemoveTree(const std::string& path) const noexcept override {
    try {
      std::string leaf;
      Fd dir = OpenParentOf(root_, path, &leaf);
      if (dir.ok()) RemoveEntry(dir.get(), leaf.c_str());
    } catch (const std::bad_alloc&) {
      // out of memory: the folder stays, and the next stage removes what no staged.json names
    }
  }

  // A link at the file, or at any folder on the way, is a link (the engine then refuses to write through the path). A path that
  // names nothing under the root, or whose file is absent, is not.
  bool IsLink(const std::string& path) const noexcept override {
    try {
      std::string leaf;
      Fd dir = OpenParentOf(root_, path, &leaf);
      if (!dir.ok()) {
        const int err = errno;
        return !(AbsentOnPath(err) || err == EINVAL);
      }
      struct stat st{};
      if (::fstatat(dir.get(), leaf.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) return !AbsentOnPath(errno);
      return S_ISLNK(st.st_mode);
    } catch (const std::bad_alloc&) {
      return true;  // cannot tell: assume a link, so the engine does not write through it
    }
  }

  // Something is at the path, a file, a folder or a link. A link on the way counts as something (the engine waits rather than
  // stage over it).
  bool Exists(const std::string& path) const noexcept override {
    try {
      std::string leaf;
      Fd dir = OpenParentOf(root_, path, &leaf);
      if (!dir.ok()) {
        const int err = errno;
        return !(AbsentOnPath(err) || err == EINVAL);
      }
      struct stat st{};
      if (::fstatat(dir.get(), leaf.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0) return true;
      return !AbsentOnPath(errno);
    } catch (const std::bad_alloc&) {
      return true;  // cannot tell: assume it exists, so the engine waits instead of staging over a pending release
    }
  }

  // Each try takes the lock with flock(LOCK_EX | LOCK_NB). Only a lock another descriptor holds (EWOULDBLOCK) is waited for, one
  // `step_ms` at a time, up to `timeout_ms`; every other failure (a link, an unreadable folder) is returned at once.
  std::unique_ptr<FsLock> TakeLock(const std::string& update_dir, std::uint32_t timeout_ms, std::uint32_t step_ms,
                                   int* os_error) const noexcept override {
    try {
      std::string leaf;
      Fd dir = OpenParentOf(root_, update_dir + kSeparator + names::kLockFile, &leaf);
      if (!dir.ok()) {
        SetError(os_error, errno);
        return nullptr;
      }
      const std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
      for (;;) {
        Fd file(::openat(dir.get(), leaf.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, kNewFileMode));
        if (!file.ok()) {
          SetError(os_error, errno);
          return nullptr;
        }
        struct stat st{};
        if (::fstat(file.get(), &st) != 0) {
          SetError(os_error, errno);
          return nullptr;
        }
        if (!S_ISREG(st.st_mode)) {
          SetError(os_error, EINVAL);
          return nullptr;
        }
        if (::flock(file.get(), LOCK_EX | LOCK_NB) == 0) {
          FlockLock* held = new (std::nothrow) FlockLock(file.get());
          if (held == nullptr) {
            SetError(os_error, ENOMEM);
            return nullptr;
          }
          static_cast<void>(file.Release());  // the lock owns the descriptor from here
          return std::unique_ptr<FsLock>(held);
        }
        const int err = errno;
        file.Close();
        if (err != EWOULDBLOCK || MillisecondsSince(started) >= timeout_ms) {
          SetError(os_error, err);
          return nullptr;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(step_ms == 0 ? 1 : step_ms));
      }
    } catch (const std::bad_alloc&) {
      SetError(os_error, ENOMEM);
      return nullptr;
    }
  }

  // A rename within the root, never a copy. A file where `to` is going is a failure (rename(2) would replace it, so the target is
  // checked first; the caller holds update/lock for every write, ADR 0024, section 4, item 1). A move across volumes fails with
  // EXDEV, which the journal undoes.
  bool Move(const std::string& from, const std::string& to, int* os_error) const noexcept override {
    try {
      std::string from_leaf;
      Fd from_dir = OpenParentOf(root_, from, &from_leaf);
      if (!from_dir.ok()) {
        SetError(os_error, errno);
        return false;
      }
      std::string to_leaf;
      Fd to_dir = OpenParentOf(root_, to, &to_leaf);
      if (!to_dir.ok()) {
        SetError(os_error, errno);
        return false;
      }
      struct stat st{};
      if (::fstatat(to_dir.get(), to_leaf.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0) {
        SetError(os_error, EEXIST);
        return false;
      }
      if (errno != ENOENT) {
        SetError(os_error, errno);
        return false;
      }
      if (::renameat(from_dir.get(), from_leaf.c_str(), to_dir.get(), to_leaf.c_str()) != 0) {
        SetError(os_error, errno);
        return false;
      }
      // Both folders' entries are on disk when this returns. A failed flush reports as a failed move; the journal then undoes it.
      const bool flushed = FlushFolder(to_dir.get()) && FlushFolder(from_dir.get());
      if (!flushed) SetError(os_error, errno);
      return flushed;
    } catch (const std::bad_alloc&) {
      SetError(os_error, ENOMEM);
      return false;
    }
  }

 private:
  std::string root_;
};

}  // namespace

std::unique_ptr<UpdateFs> MakePosixUpdateFs(const std::string& root_dir) noexcept {
  try {
    return std::unique_ptr<UpdateFs>(new PosixFs(root_dir));
  } catch (const std::bad_alloc&) {
    return nullptr;
  }
}

}  // namespace nevr_update
