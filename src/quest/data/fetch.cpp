#include "quest/data/fetch.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "quest/data/sha256.h"
#include "quest/data/staging.h"

namespace nevr_quest::data {

namespace {

class Fd {
 public:
  explicit Fd(int fd) : fd_(fd) {}
  ~Fd() {
    if (fd_ >= 0) ::close(fd_);
  }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  int get() const { return fd_; }

 private:
  int fd_;
};

void Emit(const FetchConfig& c, const char* name, std::vector<std::pair<std::string, std::string>> fields) {
  if (!c.on_event) return;
  Event e;
  e.name = name;
  e.fields = std::move(fields);
  c.on_event(e);
}

std::string Num(std::uint64_t v) { return std::to_string(v); }

bool Cancelled(const FetchConfig& c) { return c.keep_going && !c.keep_going(); }

std::uint64_t PartSize(const std::string& part) {
  struct stat st;
  return ::stat(part.c_str(), &st) == 0 && S_ISREG(st.st_mode) ? static_cast<std::uint64_t>(st.st_size) : 0;
}

std::uint64_t FreeBytes(const FetchConfig& c) {
  if (c.available_bytes) return c.available_bytes(c.root);
  struct statvfs vfs;
  if (::statvfs(c.root.c_str(), &vfs) != 0) return 0;
  return static_cast<std::uint64_t>(vfs.f_bavail) * vfs.f_frsize;
}

bool MakeRoot(const std::string& root) {
  // mkdir -p of the data root. A root that is not a directory after the loop is a failure.
  std::string path;
  std::size_t start = 0;
  while (start < root.size()) {
    std::size_t slash = root.find('/', start + 1);
    if (slash == std::string::npos) slash = root.size();
    path = root.substr(0, slash);
    if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) return false;
    start = slash;
  }
  struct stat st;
  return ::stat(root.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

enum class Download { kDone, kFailed, kCancelled };

// One attempt: bring `part` to `config.bytes` bytes. On any failure the part keeps the bytes that were written and
// checked to be in order, except where noted.
Download DownloadOnce(const FetchConfig& c, Transport& transport, const std::string& part, std::string* why) {
  std::uint64_t have = PartSize(part);
  if (have > c.bytes) {  // longer than the pin: nothing in it can be trusted
    ::unlink(part.c_str());
    have = 0;
  }
  if (have == c.bytes) return Download::kDone;
  Fd out(::open(part.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644));
  if (out.get() < 0) {
    *why = "open_part_failed";
    return Download::kFailed;
  }
  std::uint64_t written = have;  // bytes of the zip now in the part
  std::uint64_t cursor = have;   // where the next body byte lands
  bool restarted = false;
  bool write_failed = false;
  bool overrun = false;
  const auto on_body = [&](long status, const void* data, std::size_t length) -> bool {
    if (status != 206 && status != 200) return true;  // an error page: dropped, the status ends the attempt
    if (status == 200 && have != 0 && !restarted) {
      // The server ignored the Range header: the body is the whole zip from byte 0.
      if (::ftruncate(out.get(), 0) != 0) {
        write_failed = true;
        return false;
      }
      restarted = true;
      cursor = 0;
      written = 0;
    }
    if (cursor + length > c.bytes) {
      overrun = true;
      return false;
    }
    const unsigned char* p = static_cast<const unsigned char*>(data);
    std::size_t left = length;
    while (left != 0) {
      const ssize_t n = ::pwrite(out.get(), p, left, static_cast<off_t>(cursor));
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) {
        write_failed = true;
        return false;
      }
      p += n;
      left -= static_cast<std::size_t>(n);
      cursor += static_cast<std::uint64_t>(n);
    }
    written = cursor;
    if (c.on_progress) c.on_progress(written, c.bytes);
    return true;
  };
  const TransportResult r = transport.GetRange(c.url, have, on_body, [&c] { return !Cancelled(c); });
  static_cast<void>(::fsync(out.get()));
  if (r.interrupted || Cancelled(c)) return Download::kCancelled;
  if (write_failed) {
    *why = "write_failed";
    return Download::kFailed;
  }
  if (overrun) {
    static_cast<void>(::ftruncate(out.get(), 0));
    *why = "body_longer_than_pin";
    return Download::kFailed;
  }
  if (!r.completed) {
    *why = "transport_" + std::to_string(r.code) + (r.detail.empty() ? "" : ":" + r.detail);
    return Download::kFailed;
  }
  if (r.http_status != 206 && r.http_status != 200) {
    *why = "http_" + std::to_string(r.http_status);
    return Download::kFailed;
  }
  if (r.http_status == 200 && have != 0 && !restarted) {
    // A 200 with an empty body to a ranged request: nothing was written, the part is as it was.
    *why = "http_200_without_body";
    return Download::kFailed;
  }
  if (written != c.bytes) {
    *why = "short_body";
    return Download::kFailed;
  }
  return Download::kDone;
}

// The whole-file digest of `path`.
bool HashFile(const std::string& path, std::string* hex) {
  Fd in(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
  if (in.get() < 0) return false;
  Sha256 h;
  unsigned char buf[64 * 1024];
  while (true) {
    const ssize_t n = ::read(in.get(), buf, sizeof(buf));
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) return false;
    if (n == 0) break;
    h.Update(buf, static_cast<std::size_t>(n));
  }
  *hex = h.Hex();
  return true;
}

}  // namespace

const char* FetchStatusName(FetchStatus status) noexcept {
  switch (status) {
    case FetchStatus::kOk: return "ok";
    case FetchStatus::kBadRoot: return "bad_root";
    case FetchStatus::kNoSpace: return "no_space";
    case FetchStatus::kRetriesExhausted: return "retries_exhausted";
    case FetchStatus::kExtractFailed: return "extract_failed";
    case FetchStatus::kSwapFailed: return "swap_failed";
    case FetchStatus::kCancelled: return "cancelled";
  }
  return "unknown";
}

FetchResult RunFetch(const FetchConfig& c, Transport& transport) {
  FetchResult result;
  if (!MakeRoot(c.root)) {
    result.status = FetchStatus::kBadRoot;
    result.detail = "cannot create the data root";
    Emit(c, "data_fetch", {{"outcome", "bad_root"}});
    return result;
  }
  int failed_cleanups = 0;
  const int cleaned = CleanLeftovers(c.root, &failed_cleanups);
  if (cleaned != 0 || failed_cleanups != 0) {
    Emit(c, "data_fetch", {{"step", "cleanup"}, {"removed", Num(static_cast<std::uint64_t>(cleaned))},
                           {"failed", Num(static_cast<std::uint64_t>(failed_cleanups))}});
  }

  const std::string part = c.root + "/" + kPartName;
  const std::uint64_t have = PartSize(part);
  const std::uint64_t remaining = c.bytes > have ? c.bytes - have : 0;
  const std::uint64_t needed = (remaining + c.extracted_bytes) * (c.replacing ? 2 : 1);
  const std::uint64_t available = FreeBytes(c);
  if (available < needed) {
    result.status = FetchStatus::kNoSpace;
    result.detail = "need " + Num(needed) + " bytes, " + Num(available) + " free";
    Emit(c, "data_fetch", {{"outcome", "no_space"}, {"needed", Num(needed)}, {"available", Num(available)}});
    return result;
  }
  Emit(c, "data_fetch", {{"step", "start"}, {"resume_from", Num(have)}, {"total", Num(c.bytes)},
                         {"replacing", c.replacing ? "true" : "false"}});

  bool verified = false;
  std::string last_why;
  for (int attempt = 1; attempt <= c.max_attempts && !verified; ++attempt) {
    result.attempts = attempt;
    if (attempt > 1 && c.backoff) c.backoff(attempt);
    if (Cancelled(c)) {
      result.status = FetchStatus::kCancelled;
      return result;
    }
    std::string why;
    const Download d = DownloadOnce(c, transport, part, &why);
    if (d == Download::kCancelled) {
      result.status = FetchStatus::kCancelled;
      Emit(c, "data_fetch", {{"outcome", "cancelled"}, {"attempt", Num(static_cast<std::uint64_t>(attempt))}});
      return result;
    }
    if (d == Download::kFailed) {
      last_why = why;
      Emit(c, "data_fetch", {{"outcome", "attempt_failed"}, {"attempt", Num(static_cast<std::uint64_t>(attempt))},
                             {"reason", why}, {"part_bytes", Num(PartSize(part))}});
      continue;
    }
    std::string digest;
    if (!HashFile(part, &digest)) {
      last_why = "hash_read_failed";
      Emit(c, "data_fetch", {{"outcome", "attempt_failed"}, {"attempt", Num(static_cast<std::uint64_t>(attempt))},
                             {"reason", last_why}});
      continue;
    }
    if (digest != c.sha256) {
      ::unlink(part.c_str());  // a mismatch deletes the part: the next attempt starts from zero
      last_why = "sha256_mismatch";
      Emit(c, "data_fetch", {{"outcome", "attempt_failed"}, {"attempt", Num(static_cast<std::uint64_t>(attempt))},
                             {"reason", last_why}, {"actual", digest}, {"expected", c.sha256}});
      continue;
    }
    verified = true;
  }
  if (!verified) {
    result.status = FetchStatus::kRetriesExhausted;
    result.detail = last_why;
    Emit(c, "data_fetch", {{"outcome", "retries_exhausted"}, {"reason", last_why}});
    return result;
  }
  Emit(c, "data_fetch", {{"outcome", "verified"}, {"attempts", Num(static_cast<std::uint64_t>(result.attempts))},
                         {"bytes", Num(c.bytes)}});

  const std::string staging = StagingPath(c.root, c.sha256);
  RemoveTree(staging);
  std::uint64_t extracted = 0;
  std::string detail;
  const ZipStatus zs = ExtractZip(part, staging, kZipPrefix, c.zip_limits, &extracted, &detail);
  if (zs != ZipStatus::kOk) {
    RemoveTree(staging);
    // A verified zip that does not extract is a bad pin or a failing disk. Keep the zip only when the disk was the
    // reason, so the next run does not download it again.
    if (zs != ZipStatus::kWriteFailed) ::unlink(part.c_str());
    result.status = FetchStatus::kExtractFailed;
    result.detail = std::string(ZipStatusName(zs)) + (detail.empty() ? "" : ":" + detail);
    Emit(c, "data_extract", {{"outcome", ZipStatusName(zs)}, {"entry", detail}});
    return result;
  }
  Emit(c, "data_extract", {{"outcome", "ok"}, {"bytes", Num(extracted)}});

  const StageStatus ss = InstallStaged(c.root, staging, c.utc_stamp, &result.replaced_to);
  if (ss != StageStatus::kOk) {
    RemoveTree(staging);
    result.status = FetchStatus::kSwapFailed;
    result.detail = StageStatusName(ss);
    Emit(c, "data_swap", {{"outcome", StageStatusName(ss)}});
    return result;
  }
  ::unlink(part.c_str());
  Emit(c, "data_swap", {{"outcome", "ok"}, {"replaced_to", result.replaced_to}});
  result.status = FetchStatus::kOk;
  return result;
}

}  // namespace nevr_quest::data
