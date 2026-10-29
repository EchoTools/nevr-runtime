#include "quest/plugin/plugin_platform.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>

#include "quest/plugin/input_seam_hook.h"
#include "quest/plugin/plugin_game_image.h"
#include "quest/plugin/plugin_record_log.h"
#include "quest/sentinel/sentinel_log.h"

namespace nevr_quest::plugin {

namespace {

// A plugin is a shared object of a few hundred kilobytes; a file past this is not one.
constexpr off_t kMaxPluginBytes = 64 * 1024 * 1024;

}  // namespace

std::string DirectoryOf(const char* path) {
  if (path == nullptr) return "";
  const std::string text(path);
  const std::size_t slash = text.find_last_of('/');
  return slash == std::string::npos ? std::string() : text.substr(0, slash);
}

SentinelPlatform::SentinelPlatform(std::string libraryDir, bool emitDebug)
    : libraryDir_(std::move(libraryDir)), emitDebug_(emitDebug) {}

bool SentinelPlatform::ReadFile(const std::string& file, std::vector<std::uint8_t>* bytes) {
  if (libraryDir_.empty()) return false;
  const int fd = ::open((libraryDir_ + "/" + file).c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
  if (fd < 0) return false;
  struct stat st {};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > kMaxPluginBytes) {
    ::close(fd);
    return false;
  }
  bytes->clear();
  std::uint8_t buffer[16 * 1024];
  for (;;) {
    const ssize_t got = ::read(fd, buffer, sizeof(buffer));
    if (got < 0) {
      if (errno == EINTR) continue;
      ::close(fd);
      return false;
    }
    if (got == 0) break;
    bytes->insert(bytes->end(), buffer, buffer + got);
    if (bytes->size() > static_cast<std::size_t>(kMaxPluginBytes)) {
      ::close(fd);
      return false;
    }
  }
  ::close(fd);
  return true;
}

// By name: the linker finds the file in the app's native library directory, the same one the sentinel was mapped from.
void SentinelPlatform::Close(void* handle) {
  if (handle != nullptr) ::dlclose(handle);
}

// The path ReadFile hashed and checked, never the bare name: a bare name makes the linker search, so a name that shadows a
// system or an already-mapped library could load something other than the bytes that were recorded.
void* SentinelPlatform::Open(const std::string& file) {
  if (libraryDir_.empty()) return nullptr;
  return ::dlopen((libraryDir_ + "/" + file).c_str(), RTLD_NOW | RTLD_LOCAL);
}

void* SentinelPlatform::Symbol(void* handle, const char* name) { return ::dlsym(handle, name); }

std::string SentinelPlatform::GameBuildId() {
  char id[128] = {};
  return ReadGameBuildId(id, sizeof(id)) ? std::string(id) : std::string();
}

std::uintptr_t SentinelPlatform::GameBase() { return GameBaseAddress(); }

void SentinelPlatform::Emit(const LoadRecord& record) { LogLoadRecord(record); }

void SentinelPlatform::EmitPlan(std::size_t entries, std::size_t early, std::size_t normal, std::size_t refused,
                                std::size_t disabled) {
  LogPlanSummary(entries, early, normal, refused, disabled);
}

std::optional<nevr_quest::LogLevel> SinkLevel(std::uint32_t level, bool emitDebug) {
  switch (level) {
    case NEVR_LOG_DEBUG: return emitDebug ? std::optional<nevr_quest::LogLevel>(nevr_quest::LogLevel::kInfo) : std::nullopt;
    case NEVR_LOG_INFO: return nevr_quest::LogLevel::kInfo;
    case NEVR_LOG_WARNING: return nevr_quest::LogLevel::kWarn;
    default: return nevr_quest::LogLevel::kError;  // NEVR_LOG_ERROR (the formatter never passes another value)
  }
}

void SentinelPlatform::PluginLine(std::uint32_t level, const std::string& line) {
  const std::optional<nevr_quest::LogLevel> sink = SinkLevel(level, emitDebug_);
  if (sink) sentinel::Emit(*sink, line);
}

const NevrQuestHostApi* SentinelPlatform::QuestHostApi() { return input_seam::HostApi(emitDebug_); }

}  // namespace nevr_quest::plugin
