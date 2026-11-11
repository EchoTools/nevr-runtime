#include "quest/cargo/posix_env.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>

#include "quest/auth/atomic_write.h"
#include "quest/data/sha256.h"
#include "quest/sentinel/hook_log.h"

namespace nevr_quest::cargo {

namespace {

std::atomic<unsigned> g_tempCounter{0};

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

void SyncDirectory(const std::string& dir) noexcept {
  const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return;
  ::fsync(fd);
  ::close(fd);
}

class Sha256Hasher : public nevr_cargo::Hasher {
 public:
  void Update(const void* data, std::size_t length) override { sha_.Update(data, length); }
  std::string Hex() override { return sha_.Hex(); }

 private:
  nevr_quest::data::Sha256 sha_;
};

// A file written beside its final name: created exclusively and not through a link, flushed and renamed in one step.
class PosixTemp : public nevr_cargo::TempFile {
 public:
  PosixTemp(int fd, std::string path, std::string dir) : fd_(fd), path_(std::move(path)), dir_(std::move(dir)) {}
  ~PosixTemp() override { Abort(); }

  bool Write(const void* data, std::size_t length) override { return fd_ >= 0 && WriteAll(fd_, data, length); }

  bool Commit(const std::string& final_path) override {
    if (fd_ < 0) return false;
    const bool synced = ::fsync(fd_) == 0;
    ::close(fd_);
    fd_ = -1;
    if (!synced || ::rename(path_.c_str(), final_path.c_str()) != 0) {
      ::unlink(path_.c_str());
      path_.clear();
      return false;
    }
    SyncDirectory(dir_);
    path_.clear();
    return true;
  }

  void Abort() override {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    if (!path_.empty()) ::unlink(path_.c_str());
    path_.clear();
  }

 private:
  int fd_;
  std::string path_;
  std::string dir_;
};

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

std::unique_ptr<nevr_cargo::TempFile> PosixEnv::CreateTemp(const std::string& dir) {
  const std::string path = dir + "/.tmp-" + std::to_string(::getpid()) + "-" + std::to_string(g_tempCounter.fetch_add(1));
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) return nullptr;
  return std::unique_ptr<nevr_cargo::TempFile>(new PosixTemp(fd, path, dir));
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
  const sentinel::LogLevel mapped = level == nevr_cargo::EventLevel::kInfo   ? sentinel::LogLevel::kInfo
                                    : level == nevr_cargo::EventLevel::kWarn ? sentinel::LogLevel::kWarn
                                                                             : sentinel::LogLevel::kError;
  sentinel::LogFieldList(mapped, name, list.data(), list.size());
}

}  // namespace nevr_quest::cargo
