// The Android side of the Quest update root and the shim's version record (update_root.h). Compiled into the sentinel
// (libovrplatformloader.so), the shim, which is the host on Quest.

#include "quest/shim/update_root.h"

#include <cerrno>
#include <cstddef>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include "quest/auth/app_files_rules.h"
#include "quest/sentinel/android_glue_layout.h"
#include "quest/sentinel/hook_log.h"

namespace nevr_quest::update_root {
namespace {

// The shim is the Quest host, so its records carry the host's tag (docs/standards/logging.md, NEVR.HOST).
constexpr char kHostTag[] = "NEVR.HOST";
constexpr char kOwnCmdlinePath[] = "/proc/self/cmdline";
constexpr std::size_t kCmdlineChunkBytes = 4096;
constexpr std::size_t kCmdlineMaxBytes = 64 * 1024;

// This process's command line, NUL-separated. Empty when it cannot be read or is over the cap: the root is then empty.
// The same read as quest/auth/file_store.cpp's, which the shim does not link.
std::string OwnCmdline() {
  const int fd = ::open(kOwnCmdlinePath, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return std::string();
  std::string out;
  char buf[kCmdlineChunkBytes];
  for (;;) {
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR) continue;
      out.clear();
      break;
    }
    if (n == 0) break;
    out.append(buf, static_cast<std::size_t>(n));
    if (out.size() > kCmdlineMaxBytes) {
      out.clear();
      break;
    }
  }
  ::close(fd);
  return out;
}

}  // namespace

std::string Resolve() {
  const unsigned uid = static_cast<unsigned>(::getuid());
  return RootFromFilesDir(::nevr_quest::app_files::FilesDirFromCmdline(OwnCmdline(), uid));
}

void LogRootAtAndroidMain(const void* app) noexcept {
  const void* activity = sentinel::glue::ReadPointer(app, sentinel::glue::kAppActivity);
  const char* internal_data_path =
      static_cast<const char*>(sentinel::glue::ReadPointer(activity, sentinel::glue::kActivityInternalDataPath));
  const std::string root = Resolve();
  if (internal_data_path == nullptr) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "update_root",
                        {{"tag", kHostTag},
                         {"status", "no_internal_data_path"},
                         {"root", root.c_str()},
                         {"consequence", "the update root is not checked against the folder the activity names"}});
    return;
  }
  const bool match = RootMatchesInternalDataPath(root, internal_data_path);
  sentinel::LogFields(match ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn, "update_root",
                      {{"tag", kHostTag},
                       {"match", match ? "true" : "false"},
                       {"root", root.c_str()},
                       {"internal_data_path", internal_data_path},
                       {"consequence", match ? "the update reads and writes the folder the activity names"
                                             : "the update would read and write a folder the activity does not name"}});
}

void LogVersions(const VersionsFacts& facts) {
  const std::vector<std::pair<std::string, std::string>> fields = VersionsFields(facts);
  std::vector<sentinel::LogField> list;
  list.reserve(fields.size() + 1);
  list.emplace_back("tag", kHostTag);
  for (const auto& field : fields) list.emplace_back(field.first.c_str(), field.second.c_str());
  sentinel::LogFieldList(sentinel::LogLevel::kInfo, "versions", list.data(), list.size());
}

}  // namespace nevr_quest::update_root
