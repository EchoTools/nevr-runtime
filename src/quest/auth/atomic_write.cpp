#include "quest/auth/atomic_write.h"

#include <cerrno>
#include <cstring>
#include <filesystem>

#include <fcntl.h>
#include <unistd.h>

namespace nevr::quest_auth {

bool AtomicWrite(const std::string& path, const std::string& data, std::string& error, std::string& warning) {
  std::error_code ec;
  const std::filesystem::path target(path);
  const std::filesystem::path parent = target.has_parent_path() ? target.parent_path() : std::filesystem::path(".");
  if (target.has_parent_path()) {
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      error = "create_directories: " + ec.message();
      return false;
    }
  }
  const std::string tmp = path + ".tmp";
  ::unlink(tmp.c_str());  // a stale temp from a crashed run; a directory squatting here fails the open below
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) {
    error = std::string("open: ") + std::strerror(errno);
    return false;
  }
  size_t written = 0;
  while (written < data.size()) {
    const ssize_t n = ::write(fd, data.data() + written, data.size() - written);
    if (n < 0) {
      if (errno == EINTR) continue;
      error = std::string("write: ") + std::strerror(errno);
      ::close(fd);
      ::unlink(tmp.c_str());
      return false;
    }
    written += static_cast<size_t>(n);
  }
  if (::fsync(fd) != 0) {
    error = std::string("fsync: ") + std::strerror(errno);
    ::close(fd);
    ::unlink(tmp.c_str());
    return false;
  }
  if (::close(fd) != 0) {
    error = std::string("close: ") + std::strerror(errno);
    ::unlink(tmp.c_str());
    return false;
  }
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    error = std::string("rename: ") + std::strerror(errno);
    ::unlink(tmp.c_str());
    return false;
  }
  const int dfd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dfd < 0) {
    warning = std::string("open directory for fsync: ") + std::strerror(errno);
  } else {
    if (::fsync(dfd) != 0) warning = std::string("fsync directory: ") + std::strerror(errno);
    ::close(dfd);
  }
  return true;
}

}  // namespace nevr::quest_auth
