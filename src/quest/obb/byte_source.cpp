#include "quest/obb/byte_source.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace nevr_quest::obb {

namespace {
Status ShortRead(std::uint64_t offset, std::size_t length, std::uint64_t size) {
  return Status::Fail("source_short_read", "read of " + std::to_string(length) + " bytes at offset " + std::to_string(offset) +
                                               " is not inside the " + std::to_string(size) + " bytes of the source");
}
bool Inside(std::uint64_t offset, std::size_t length, std::uint64_t size) { return offset <= size && length <= size - offset; }
}  // namespace

Status MemorySource::ReadAt(std::uint64_t offset, void* dst, std::size_t length) const {
  if (!Inside(offset, length, bytes_.size())) return ShortRead(offset, length, bytes_.size());
  if (length != 0) std::memcpy(dst, bytes_.data() + offset, length);
  return Status::Ok();
}

FileSource::~FileSource() {
  if (fd_ >= 0) ::close(fd_);
}

Status FileSource::Open(const std::string& path) {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  path_ = path;
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return Status::Fail("source_open_failed", "cannot open '" + path + "': " + std::strerror(errno));
  struct stat st;
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    const int err = errno;
    ::close(fd);
    return Status::Fail("source_open_failed", "'" + path + "' is not a regular file" + (err ? std::string(": ") + std::strerror(err) : std::string()));
  }
  fd_ = fd;
  size_ = static_cast<std::uint64_t>(st.st_size);
  return Status::Ok();
}

Status FileSource::ReadAt(std::uint64_t offset, void* dst, std::size_t length) const {
  if (fd_ < 0) return Status::Fail("source_read_failed", "the source is not open");
  if (!Inside(offset, length, size_)) return ShortRead(offset, length, size_);
  std::uint8_t* out = static_cast<std::uint8_t*>(dst);
  std::size_t done = 0;
  while (done < length) {
    const ssize_t n = ::pread(fd_, out + done, length - done, static_cast<off_t>(offset + done));
    if (n < 0) {
      if (errno == EINTR) continue;
      return Status::Fail("source_read_failed", "read of '" + path_ + "' at offset " + std::to_string(offset + done) + ": " + std::strerror(errno));
    }
    if (n == 0) return Status::Fail("source_short_read", "'" + path_ + "' ended at offset " + std::to_string(offset + done) + " (it was " + std::to_string(size_) + " bytes when opened)");
    done += static_cast<std::size_t>(n);
  }
  return Status::Ok();
}

}  // namespace nevr_quest::obb
