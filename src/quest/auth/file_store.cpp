#include "quest/auth/file_store.h"

#include "core/device_auth_flow.h"

#include <cerrno>
#include <cstring>
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

// Writes `data` to `path` atomically (temp + fsync + rename), mode 0600. On any failure
// the temp file is removed and `path` is untouched. Returns errno-style text on failure.
bool AtomicWrite(const std::string& path, const std::string& data, std::string& error) {
  std::error_code ec;
  const std::filesystem::path target(path);
  if (target.has_parent_path()) {
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) {
      error = "create_directories: " + ec.message();
      return false;
    }
  }
  const std::string tmp = path + ".tmp";
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
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
  return true;
}
}  // namespace

std::string JoinPath(const std::string& dir, const std::string& name) {
  if (dir.empty()) return name;
  return dir.back() == '/' ? dir + name : dir + "/" + name;
}

FileCredentialStore::FileCredentialStore(std::string path, nevr::auth::LogSink log)
    : path_(std::move(path)), log_(std::move(log)) {}

CachedAuthToken FileCredentialStore::Load(uint64_t now) {
  std::ifstream file(path_, std::ios::binary);
  if (!file.is_open()) {
    Emit(log_, LogLevel::Info, "[NEVR.AUTH] credential cache not present path=" + path_);
    return {};
  }
  std::ostringstream contents;
  contents << file.rdbuf();
  if (file.bad()) {
    Emit(log_, LogLevel::Warning, "[NEVR.AUTH] credential cache unreadable path=" + path_);
    return {};
  }
  CachedAuthToken auth = ParseCredentialsJson(contents.str(), now);
  if (auth.token.empty() && auth.refresh_token.empty()) {
    Emit(log_, LogLevel::Warning, "[NEVR.AUTH] credential cache held no usable login path=" + path_);
  }
  return auth;
}

bool FileCredentialStore::Save(const CachedAuthToken& auth) {
  if (auth.refresh_token.empty()) return false;
  std::string error;
  if (!AtomicWrite(path_, SerializeCredentialsJson(auth), error)) {
    Emit(log_, LogLevel::Warning,
         "[NEVR.AUTH] failed to write credential cache path=" + path_ + " error=" + error +
             " -- refresh token not persisted, previous file unchanged");
    return false;
  }
  return true;
}

FileLinkPresenter::FileLinkPresenter(std::string path, nevr::auth::LogSink log)
    : path_(std::move(path)), log_(std::move(log)) {}

intptr_t FileLinkPresenter::Present(const std::string& login_url_with_code) {
  std::string error;
  if (!AtomicWrite(path_, login_url_with_code + "\n", error)) {
    Emit(log_, LogLevel::Error, "[NEVR.AUTH] could not write the login link file path=" + path_ +
                                    " error=" + error);
    return 0;
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
