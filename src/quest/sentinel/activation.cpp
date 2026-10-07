#include "activation.h"

#include "sentinel_log.h"

#include "generated/nevr_builtin_defaults.h"

#include <cerrno>
#include <cstring>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace sentinel {

namespace {

std::once_flag g_once;
nevr_quest::ResolvedConfig g_config;

enum class ReadStatus { kRead, kAbsent, kError };

// Reads at most kMaxConfigBytes + 1 bytes so an oversized file is detected without loading it.
ReadStatus ReadConfigFile(const std::string& path, std::string* out, int* err) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    *err = errno;
    return *err == ENOENT ? ReadStatus::kAbsent : ReadStatus::kError;
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

void Resolve() {
  const std::string path = nevr_quest::ConfigFilePath(FilesDir());
  std::string text;
  int err = 0;
  const ReadStatus status = ReadConfigFile(path, &text, &err);
  if (status == ReadStatus::kError) {
    Emit(nevr_quest::LogLevel::kError,
         std::string("config file ") + nevr_quest::kConfigFileName + " unreadable errno=" + std::to_string(err) +
             " (" + std::strerror(err) + "), using embedded defaults");
  }

  nevr_quest::EmbeddedDefaults defaults;
  defaults.socketUri = nevr_builtin::kSocketUri;
  defaults.httpUri = nevr_builtin::kHttpUri;
  defaults.httpKey = nevr_builtin::kPublicApiKey;
  defaults.serverKey = nevr_builtin::kPublicSocketKey;

  nevr_quest::LoadResult result =
      nevr_quest::ResolveConfig(defaults, status == ReadStatus::kRead ? &text : nullptr);
  for (const nevr_quest::LogEvent& e : result.events) Emit(e.level, e.message);
  g_config = std::move(result.config);
}

}  // namespace

void InitActivation() {
  std::call_once(g_once, [] {
    // A failure here must not take the host process down: fall back to the all-off config.
    try {
      Resolve();
    } catch (const std::exception& e) {
      g_config = nevr_quest::ResolvedConfig();
      Emit(nevr_quest::LogLevel::kError, std::string("config resolution failed: ") + e.what() +
                                             "; all features off, no embedded defaults");
    }
  });
}

const nevr_quest::ResolvedConfig& ActiveConfig() {
  InitActivation();
  return g_config;
}

bool FeatureEnabled(nevr_quest::Feature feature) {
  return nevr_quest::FeatureEnabled(ActiveConfig(), feature);
}

}  // namespace sentinel
