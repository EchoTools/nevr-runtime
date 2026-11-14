// The POSIX side of the owned-removal guard (core/owned_remove.h): the algorithm the guard is specified by, on file
// descriptors. Every parent is opened relative to the one before it with O_NOFOLLOW, so no path is looked up again after
// its check, and the last component is unlinked by name with unlinkat, which never follows a link.

#include "core/owned_remove.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <new>
#include <string>
#include <vector>

#include "core/owned_remove_rules.h"

namespace nevr_owned {
namespace {

// Closes its descriptor when it leaves scope; a close failure has nothing to undo, so it is ignored.
class Fd {
 public:
  explicit Fd(int fd) : fd_(fd) {}
  Fd(Fd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  Fd& operator=(Fd&&) = delete;
  ~Fd() {
    if (fd_ >= 0) ::close(fd_);
  }
  int get() const { return fd_; }

 private:
  int fd_;
};

Result Make(const std::string& entry, Outcome outcome, int os_error) {
  return Result{entry, outcome, os_error};
}

}  // namespace

const char* OutcomeName(Outcome outcome) noexcept {
  switch (outcome) {
    case Outcome::kRemoved:
      return "removed";
    case Outcome::kAbsent:
      return "absent";
    case Outcome::kNotEmpty:
      return "not_empty";
    case Outcome::kRefusedName:
      return "refused_name";
    case Outcome::kRefusedLink:
      return "refused_link";
    case Outcome::kRefusedOutsideRoot:
      return "refused_outside_root";
    case Outcome::kFailed:
      return "failed";
  }
  return "unknown";
}

namespace {

Result RemoveEntryChecked(const std::string& root_utf8, const std::string& entry) {
  {
    if (!ValidEntry(entry) || !ValidRoot(root_utf8)) return Make(entry, Outcome::kRefusedName, 0);

    // The root is opened once, and every later step works from this descriptor, so no path is looked up twice. A root
    // that is itself a link is followed (O_DIRECTORY without O_NOFOLLOW): the documented choice, with everything under the
    // folder it names judged as usual.
    const Fd root(::open(root_utf8.c_str(), O_DIRECTORY | O_CLOEXEC));
    if (root.get() < 0) {
      const int err = errno;
      return Make(entry, err == ENOENT ? Outcome::kAbsent : Outcome::kFailed, err);
    }
    struct stat root_st = {};
    if (::fstat(root.get(), &root_st) != 0) return Make(entry, Outcome::kFailed, errno);

    const std::vector<std::string> parts = SplitEntry(entry);

    // Every parent, one openat at a time, each held open until the entry is done.
    std::vector<Fd> parents;
    parents.reserve(parts.size());
    int dir_fd = root.get();
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
      const int fd = ::openat(dir_fd, parts[i].c_str(), O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (fd < 0) {
        const int err = errno;
        if (err == ENOENT) return Make(entry, Outcome::kAbsent, err);
        if (err == ELOOP || err == ENOTDIR) {
          // With O_DIRECTORY|O_NOFOLLOW, a link gives ENOTDIR on Linux and ELOOP elsewhere. A plain file gives ENOTDIR too,
          // and then nothing under it can be the recorded entry.
          struct stat st = {};
          if (err == ELOOP || (::fstatat(dir_fd, parts[i].c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(st.st_mode))) {
            return Make(entry, Outcome::kRefusedLink, err);
          }
          return Make(entry, Outcome::kAbsent, err);
        }
        return Make(entry, Outcome::kFailed, err);
      }
      parents.emplace_back(fd);
      struct stat st = {};
      if (::fstat(fd, &st) != 0) return Make(entry, Outcome::kFailed, errno);
      if (st.st_dev != root_st.st_dev) return Make(entry, Outcome::kRefusedOutsideRoot, 0);
      dir_fd = fd;
    }

    // The last component, by name, without following it.
    const std::string& last = parts.back();
    struct stat st = {};
    if (::fstatat(dir_fd, last.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) {
      const int err = errno;
      return Make(entry, err == ENOENT ? Outcome::kAbsent : Outcome::kFailed, err);
    }
    if (S_ISDIR(st.st_mode)) {
      if (::unlinkat(dir_fd, last.c_str(), AT_REMOVEDIR) != 0) {
        const int err = errno;
        if (err == ENOTEMPTY || err == EEXIST) return Make(entry, Outcome::kNotEmpty, err);
        return Make(entry, err == ENOENT ? Outcome::kAbsent : Outcome::kFailed, err);
      }
      return Make(entry, Outcome::kRemoved, 0);
    }
    // A file, or a link as the last component: unlinked, and its target is never touched.
    if (::unlinkat(dir_fd, last.c_str(), 0) != 0) {
      const int err = errno;
      return Make(entry, err == ENOENT ? Outcome::kAbsent : Outcome::kFailed, err);
    }
    return Make(entry, Outcome::kRemoved, 0);
  }
}

}  // namespace

// The Quest data library builds with -fno-exceptions (src/quest/CMakeLists.txt:234): there an allocation failure aborts,
// as it does everywhere else in that code, and there is nothing to catch. Where exceptions are on (the Linux host tests),
// bad_alloc becomes kFailed so the noexcept promise holds.
Result RemoveEntry(const std::string& root_utf8, const std::string& entry) noexcept {
#if defined(__cpp_exceptions)
  try {
    return RemoveEntryChecked(root_utf8, entry);
  } catch (const std::bad_alloc&) {
    return Result{std::string(), Outcome::kFailed, ENOMEM};
  }
#else
  return RemoveEntryChecked(root_utf8, entry);
#endif
}

std::vector<Result> RemoveOwned(const std::string& root_utf8, const std::vector<std::string>& entries) noexcept {
  std::vector<Result> results;
#if defined(__cpp_exceptions)
  try {
#endif
    results.reserve(entries.size());
    for (const std::string& entry : entries) results.push_back(RemoveEntry(root_utf8, entry));
#if defined(__cpp_exceptions)
  } catch (const std::bad_alloc&) {
    // Fewer results than entries: the entries after the last result were not tried.
  }
#endif
  return results;
}

}  // namespace nevr_owned
