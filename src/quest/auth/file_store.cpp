#include "quest/auth/file_store.h"

#include "core/device_auth_flow.h"

#include <cerrno>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <fcntl.h>
#include <unistd.h>

namespace nevr::quest_auth {

namespace {
using nevr::auth::LogLevel;

void Emit(const nevr::auth::LogSink& log, LogLevel level, const std::string& message) {
  if (log) log(level, message);
}

// Writes `data` to `path` atomically: fresh temp file (O_EXCL, O_NOFOLLOW, mode 0600),
// fsync, rename over `path`, fsync of the directory. On any failure before the rename the
// temp file is removed and `path` is untouched. A failed directory fsync after the rename
// is reported through `warning` only: the new file is in place.
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

// Reads a whole file without following a symlink. Returns false on any error; `missing`
// distinguishes "no such file" from the rest.
bool ReadNoFollow(const std::string& path, std::string& out, bool& missing, std::string& error) {
  missing = false;
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    missing = (errno == ENOENT);
    error = std::string("open: ") + std::strerror(errno);
    return false;
  }
  char buf[4096];
  for (;;) {
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR) continue;
      error = std::string("read: ") + std::strerror(errno);
      ::close(fd);
      return false;
    }
    if (n == 0) break;
    out.append(buf, static_cast<size_t>(n));
    if (out.size() > 1024 * 1024) {
      error = "file larger than 1 MiB";
      ::close(fd);
      return false;
    }
  }
  ::close(fd);
  return true;
}
}  // namespace

std::string AppInternalFilesDirFromCmdline(const std::string& cmdline, unsigned uid) {
  const std::string name = cmdline.substr(0, cmdline.find('\0'));
  // A Java package name: dotted identifiers, at least two segments, no ':' process suffix.
  if (name.size() < 3 || name.size() > 128 || name.front() == '.' || name.back() == '.') return "";
  bool dot = false;
  for (const char c : name) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
    if (!ok) return "";
    if (c == '.') dot = true;
  }
  if (!dot || name.find("..") != std::string::npos) return "";
  return "/data/user/" + std::to_string(uid / 100000) + "/" + name + "/files";
}

std::string AppInternalFilesDir() {
  std::string cmdline;
  bool missing = false;
  std::string error;
  if (!ReadNoFollow("/proc/self/cmdline", cmdline, missing, error)) return "";
  return AppInternalFilesDirFromCmdline(cmdline, static_cast<unsigned>(::getuid()));
}

std::string JoinPath(const std::string& dir, const std::string& name) {
  if (dir.empty()) return name;
  return dir.back() == '/' ? dir + name : dir + "/" + name;
}

FileCredentialStore::FileCredentialStore(std::string path, nevr::auth::LogSink log)
    : path_(std::move(path)), log_(std::move(log)) {}

CachedAuthToken FileCredentialStore::Load(uint64_t now) {
  if (path_.empty()) {
    Emit(log_, LogLevel::Error,
         "[NEVR.AUTH] no app-internal directory for the credential cache; cached login unavailable");
    return {};
  }
  std::string contents;
  bool missing = false;
  std::string error;
  if (!ReadNoFollow(path_, contents, missing, error)) {
    if (missing) {
      Emit(log_, LogLevel::Info, "[NEVR.AUTH] credential cache not present path=" + path_);
    } else {
      Emit(log_, LogLevel::Warning, "[NEVR.AUTH] credential cache unreadable path=" + path_ + " error=" + error);
    }
    return {};
  }
  CachedAuthToken auth = ParseCredentialsJson(contents, now);
  if (auth.token.empty() && auth.refresh_token.empty()) {
    Emit(log_, LogLevel::Warning, "[NEVR.AUTH] credential cache held no usable login path=" + path_);
  }
  return auth;
}

bool FileCredentialStore::Save(const CachedAuthToken& auth) {
  if (auth.refresh_token.empty()) return false;
  if (path_.empty()) {
    Emit(log_, LogLevel::Error,
         "[NEVR.AUTH] no app-internal directory for the credential cache; refresh token not persisted");
    return false;
  }
  std::string error;
  std::string warning;
  if (!AtomicWrite(path_, SerializeCredentialsJson(auth), error, warning)) {
    Emit(log_, LogLevel::Warning,
         "[NEVR.AUTH] failed to write credential cache path=" + path_ + " error=" + error +
             " -- refresh token not persisted, previous file unchanged");
    return false;
  }
  if (!warning.empty()) {
    Emit(log_, LogLevel::Warning, "[NEVR.AUTH] credential cache written but " + warning + " path=" + path_);
  }
  return true;
}

FileLinkPresenter::FileLinkPresenter(std::string path, nevr::auth::LogSink log)
    : path_(std::move(path)), log_(std::move(log)) {}

std::string FormatLoginPrompt(const LoginPrompt& prompt) {
  char when[32] = "unknown";
  const time_t t = static_cast<time_t>(prompt.expires_unix);
  tm utc{};
  if (gmtime_r(&t, &utc) != nullptr) std::strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return "URL: " + prompt.url + "\nCode: " + prompt.code +
         "\nOpen the URL on a phone or computer, sign in with Discord and enter the code (or open " + prompt.link +
         " , which has it filled in).\nExpires: " + when + " (unix " + std::to_string(prompt.expires_unix) + ")\n";
}

intptr_t FileLinkPresenter::Present(const LoginPrompt& prompt) {
  std::string error;
  std::string warning;
  if (path_.empty() || !AtomicWrite(path_, FormatLoginPrompt(prompt), error, warning)) {
    Emit(log_, LogLevel::Error, "[NEVR.AUTH] could not write the login link file path=" + path_ +
                                    " error=" + (path_.empty() ? std::string("no path") : error));
    // The login must still be possible: the direct link goes to the log, where logcat shows it.
    Emit(log_, LogLevel::Info, "[NEVR.AUTH] login link (file not written): " + prompt.link);
    return nevr::auth::kBrowserOpenAcceptedAbove + 1;
  }
  Emit(log_, LogLevel::Info, "[NEVR.AUTH] login link written for the player path=" + path_);
  return nevr::auth::kBrowserOpenAcceptedAbove + 1;
}

void FileLinkPresenter::Clear() {
  if (::unlink(path_.c_str()) != 0 && errno != ENOENT) {
    Emit(log_, LogLevel::Warning, "[NEVR.AUTH] could not remove the login link file path=" + path_ +
                                      " error=" + std::strerror(errno));
  }
}

}  // namespace nevr::quest_auth
