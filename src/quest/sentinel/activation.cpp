#include "activation.h"

#include "sentinel_log.h"

#include "generated/nevr_build_info.h"
#include "generated/nevr_builtin_defaults.h"

#include <cerrno>
#include <cstring>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace sentinel {

namespace {

// std::once_flag is constant-initialised. The configuration is a function-local static: a
// namespace-scope ResolvedConfig has std::string members and so a dynamic initializer that runs
// after the ELF constructor that resolved it, and would blank the result.
std::once_flag g_once;

nevr_quest::ResolvedConfig& Storage() {
  static nevr_quest::ResolvedConfig config;
  return config;
}

}  // namespace

ReadStatus ReadConfigFile(const std::string& path, std::string* out, int* err) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) {
    *err = errno;
    return *err == ENOENT ? ReadStatus::kAbsent : ReadStatus::kError;
  }
  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    *err = errno;
    ::close(fd);
    return ReadStatus::kError;
  }
  if (!S_ISREG(st.st_mode)) {
    ::close(fd);
    return ReadStatus::kNotRegularFile;
  }
  std::string data;
  char buf[4096];
  for (;;) {
    const ssize_t r = ::read(fd, buf, sizeof(buf));
    if (r < 0) {
      if (errno == EINTR) continue;
      *err = errno;
      ::close(fd);
      return ReadStatus::kError;
    }
    if (r == 0) break;
    data.append(buf, static_cast<std::size_t>(r));
    if (data.size() > nevr_quest::kMaxConfigBytes) break;
  }
  ::close(fd);
  *out = std::move(data);
  return ReadStatus::kRead;
}

nevr_quest::ResolvedConfig ResolveFromDisk(const std::string& path) {
  std::string text;
  int err = 0;
  const ReadStatus status = ReadConfigFile(path, &text, &err);
  if (status == ReadStatus::kError) {
    Emit(nevr_quest::LogLevel::kError,
         std::string("config file ") + nevr_quest::kConfigFileName + " unreadable errno=" + std::to_string(err) +
             " (" + std::strerror(err) + "), using embedded defaults");
  } else if (status == ReadStatus::kNotRegularFile) {
    Emit(nevr_quest::LogLevel::kError, std::string("config file ") + nevr_quest::kConfigFileName +
                                           " is not a regular file, not read, using embedded defaults");
  }

  nevr_quest::EmbeddedDefaults defaults;
  defaults.socketUri = nevr_builtin::kSocketUri;
  defaults.httpUri = nevr_builtin::kHttpUri;
  defaults.httpKey = nevr_builtin::kPublicApiKey;
  defaults.serverKey = nevr_builtin::kPublicSocketKey;
  defaults.features = nevr_build::kDefaultFeatures;

  // Which build this is, so a log names its candidate and commit (neither is a configured value).
  Emit(nevr_quest::LogLevel::kInfo,
       std::string("build version=") + nevr_build::kVersion + " commit=" + nevr_build::kCommit);
  nevr_quest::LoadResult result =
      nevr_quest::ResolveConfig(defaults, status == ReadStatus::kRead ? &text : nullptr);
  for (const nevr_quest::LogEvent& e : result.events) Emit(e.level, e.message);
  return std::move(result.config);
}

void InitActivation() {
  std::call_once(g_once, [] {
    // Nothing may escape the ELF constructor. The handler allocates nothing: an allocation failure
    // lands here too, and a throw from this block would reach std::terminate.
    try {
      Storage() = ResolveFromDisk(nevr_quest::ConfigFilePath(FilesDir()));
    } catch (const std::exception&) {
      Storage() = nevr_quest::ResolvedConfig();
      EmitFixed(nevr_quest::LogLevel::kError, "config resolution threw; all features off, no embedded defaults");
    }
  });
}

const nevr_quest::ResolvedConfig& ActiveConfig() {
  InitActivation();
  return Storage();
}

bool FeatureEnabled(nevr_quest::Feature feature) {
  return nevr_quest::FeatureEnabled(ActiveConfig(), feature);
}

}  // namespace sentinel
