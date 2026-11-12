#include "quest/cargo/posix_env.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "quest/auth/atomic_write.h"
#include "quest/data/sha256.h"
#include "quest/sentinel/hook_log.h"

namespace nevr_quest::cargo {

namespace {

bool WriteAll(int fd, const void* data, std::size_t length) noexcept {
  const char* p = static_cast<const char*>(data);
  while (length > 0) {
    const ssize_t n = ::write(fd, p, length);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    p += n;
    length -= static_cast<std::size_t>(n);
  }
  return true;
}

class Sha256Hasher : public nevr_cargo::Hasher {
 public:
  void Update(const void* data, std::size_t length) override { sha_.Update(data, length); }
  std::string Hex() override { return sha_.Hex(); }

 private:
  nevr_quest::data::Sha256 sha_;
};

// A file that only grows: opened for appending (created when absent) and not through a link. Size is read from the file each time, so
// it is what a second process or a kill left, not what this object thinks.
class PosixAppend : public nevr_cargo::AppendFile {
 public:
  explicit PosixAppend(int fd) : fd_(fd) {}
  ~PosixAppend() override {
    if (fd_ >= 0) ::close(fd_);
  }
  std::uint64_t Size() const override {
    struct stat st;
    return ::fstat(fd_, &st) == 0 ? static_cast<std::uint64_t>(st.st_size) : 0;
  }
  bool Append(const void* data, std::size_t length) override { return WriteAll(fd_, data, length); }
  bool Truncate(std::uint64_t size) override { return size <= Size() && ::ftruncate(fd_, static_cast<off_t>(size)) == 0; }
  bool Sync() override { return ::fsync(fd_) == 0; }

 private:
  int fd_;
};

// The files the process holds open now: the entries of /proc/self/fd, less the directory handle that reads it.
bool CountOpenFds(std::uint64_t* out) noexcept {
  DIR* d = ::opendir("/proc/self/fd");
  if (d == nullptr) return false;
  std::uint64_t n = 0;
  while (const dirent* e = ::readdir(d)) {
    if (e->d_name[0] != '.') ++n;
  }
  ::closedir(d);
  *out = n > 0 ? n - 1 : 0;
  return true;
}

// MemAvailable of /proc/meminfo, in bytes.
bool ReadMemAvailable(std::uint64_t* out) noexcept {
  FILE* f = std::fopen("/proc/meminfo", "r");
  if (f == nullptr) return false;
  char line[256];
  bool found = false;
  while (std::fgets(line, sizeof(line), f) != nullptr) {
    unsigned long long kb = 0;
    if (std::sscanf(line, "MemAvailable: %llu kB", &kb) == 1) {
      *out = static_cast<std::uint64_t>(kb) * 1024u;
      found = true;
      break;
    }
  }
  std::fclose(f);
  return found;
}

}  // namespace

bool PosixEnv::ReadSmallFile(const std::string& path, std::string* out, std::size_t max_bytes) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return false;
  struct stat st;
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || static_cast<std::uint64_t>(st.st_size) > max_bytes) {
    ::close(fd);
    return false;
  }
  std::string text(static_cast<std::size_t>(st.st_size), '\0');
  std::size_t got = 0;
  while (got < text.size()) {
    const ssize_t n = ::read(fd, &text[got], text.size() - got);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    got += static_cast<std::size_t>(n);
  }
  ::close(fd);
  if (got != text.size()) return false;
  *out = std::move(text);
  return true;
}

bool PosixEnv::MakeDirs(const std::string& dir) {
  if (dir.empty() || dir[0] != '/') return false;
  std::string prefix;
  std::size_t at = 1;
  while (at <= dir.size()) {
    const std::size_t slash = dir.find('/', at);
    const std::size_t end = slash == std::string::npos ? dir.size() : slash;
    prefix = dir.substr(0, end);
    if (!prefix.empty() && ::mkdir(prefix.c_str(), 0700) != 0 && errno != EEXIST) return false;
    if (slash == std::string::npos) break;
    at = slash + 1;
  }
  struct stat st;
  return ::stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::unique_ptr<nevr_cargo::AppendFile> PosixEnv::OpenAppend(const std::string& path) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) return nullptr;
  struct stat st;
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    ::close(fd);
    return nullptr;
  }
  return std::unique_ptr<nevr_cargo::AppendFile>(new PosixAppend(fd));
}

bool PosixEnv::QueryDevice(const std::string& dir, nevr_cargo::DeviceFacts* out) {
  *out = nevr_cargo::DeviceFacts();
  struct statvfs vfs;
  if (::statvfs(dir.c_str(), &vfs) == 0) {
    out->have_free = true;
    out->free_bytes = static_cast<std::uint64_t>(vfs.f_bavail) * static_cast<std::uint64_t>(vfs.f_frsize);
  }
  struct rlimit lim;
  std::uint64_t open = 0;
  if (::getrlimit(RLIMIT_NOFILE, &lim) == 0 && CountOpenFds(&open)) {
    out->have_fds = true;
    out->fd_soft_limit = lim.rlim_cur == RLIM_INFINITY ? ~std::uint64_t{0} : static_cast<std::uint64_t>(lim.rlim_cur);
    out->fds_open = open;
  }
  std::uint64_t mem = 0;
  if (ReadMemAvailable(&mem)) {
    out->have_memory = true;
    out->memory_available = mem;
  }
  return out->have_free || out->have_fds || out->have_memory;
}

bool PosixEnv::WriteFileAtomically(const std::string& path, const std::string& data) {
  std::string error, warning;
  return nevr::quest_auth::AtomicWrite(path, data, error, warning);
}

bool PosixEnv::OpenBlob(const std::string& path, std::int64_t* handle, std::uint64_t* size) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return false;
  struct stat st;
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    ::close(fd);
    return false;
  }
  *handle = fd;
  *size = static_cast<std::uint64_t>(st.st_size);
  return true;
}

bool PosixEnv::ReadBlob(std::int64_t handle, std::uint64_t offset, void* buffer, std::size_t length, std::size_t* got) {
  std::size_t total = 0;
  char* out = static_cast<char*>(buffer);
  while (total < length) {
    const ssize_t n = ::pread(static_cast<int>(handle), out + total, length - total, static_cast<off_t>(offset + total));
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) break;
    total += static_cast<std::size_t>(n);
  }
  *got = total;
  return true;
}

void PosixEnv::CloseBlob(std::int64_t handle) { ::close(static_cast<int>(handle)); }

std::unique_ptr<nevr_cargo::Hasher> PosixEnv::NewHasher() { return std::unique_ptr<nevr_cargo::Hasher>(new Sha256Hasher()); }

nevr_cargo::HttpResult PosixEnv::Get(const std::string& url, const nevr_cargo::HttpBody& on_body,
                                     const std::function<bool()>& keep_going) {
  nevr_cargo::HttpResult out;
  if (transport_ == nullptr) return out;
  const nevr_quest::data::TransportResult r = transport_->GetRange(url, 0, on_body, keep_going);
  out.completed = r.completed && !r.interrupted;
  out.status = r.http_status;
  out.code = r.code;
  out.cancelled = r.interrupted;
  return out;
}

std::uint64_t PosixEnv::NowMs() {
  timespec ts{};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000u + static_cast<std::uint64_t>(ts.tv_nsec) / 1000000u;
}

void PosixEnv::Event(nevr_cargo::EventLevel level, const char* name, const std::vector<nevr_cargo::Field>& fields) {
  std::vector<sentinel::LogField> list;
  list.reserve(fields.size());
  for (const nevr_cargo::Field& f : fields) {
    if (f.is_number) {
      list.emplace_back(f.key.c_str(), static_cast<long long>(f.number));
    } else {
      list.emplace_back(f.key.c_str(), f.text.c_str());
    }
  }
  const sentinel::LogLevel mapped = level == nevr_cargo::EventLevel::kDebug  ? sentinel::LogLevel::kDebug
                                    : level == nevr_cargo::EventLevel::kInfo ? sentinel::LogLevel::kInfo
                                    : level == nevr_cargo::EventLevel::kWarn ? sentinel::LogLevel::kWarn
                                                                             : sentinel::LogLevel::kError;
  sentinel::LogFieldList(mapped, name, list.data(), list.size());
}

}  // namespace nevr_quest::cargo
