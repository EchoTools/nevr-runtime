#include "quest/diag/hwdump_fs.h"

#include "quest/diag/hwdump_field.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>

namespace nevr_quest::hwdump {
namespace {

std::string Join(const std::string& root, const std::string& path) { return root + path; }

}  // namespace

nlohmann::json ReadTextFile(const std::string& root, const std::string& path, std::size_t max_bytes) {
  const std::string source = "read " + path;
  const int fd = ::open(Join(root, path).c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) return Fail(source, ErrnoText("open", errno));
  std::string text;
  char buf[4096];
  bool truncated = false;
  for (;;) {
    const ssize_t n = ::read(fd, buf, sizeof buf);
    if (n < 0) {
      if (errno == EINTR) continue;
      const int err = errno;
      ::close(fd);
      return Fail(source, ErrnoText("read", err));
    }
    if (n == 0) break;
    const std::size_t room = max_bytes - text.size();
    if (static_cast<std::size_t>(n) > room) {
      text.append(buf, room);
      truncated = true;
      break;
    }
    text.append(buf, static_cast<std::size_t>(n));
  }
  ::close(fd);
  nlohmann::json f = Ok(source, text);
  if (truncated) f["truncated"] = true;
  return f;
}

nlohmann::json ReadAttribute(const std::string& root, const std::string& path) {
  nlohmann::json f = ReadTextFile(root, path, 64 * 1024);
  if (f["ok"].get<bool>()) {
    std::string v = f["value"].get<std::string>();
    while (!v.empty() && (v.back() == '\n' || v.back() == '\r')) v.pop_back();
    f["value"] = v;
  }
  return f;
}

nlohmann::json ListDirectory(const std::string& root, const std::string& path, const std::string& prefix) {
  const std::string source = "readdir " + path;
  DIR* dir = ::opendir(Join(root, path).c_str());
  if (dir == nullptr) return Fail(source, ErrnoText("opendir", errno));
  std::vector<std::string> names;
  errno = 0;
  while (const dirent* e = ::readdir(dir)) {
    const std::string name = e->d_name;
    if (name == "." || name == "..") continue;
    if (name.compare(0, prefix.size(), prefix) == 0) names.push_back(name);
  }
  const int err = errno;
  ::closedir(dir);
  if (err != 0) return Fail(source, ErrnoText("readdir", err));
  std::sort(names.begin(), names.end());
  return Ok(source, names);
}

std::vector<std::string> ListedNames(const nlohmann::json& listing) {
  if (!listing.value("ok", false)) return {};
  return listing["value"].get<std::vector<std::string>>();
}

nlohmann::json StatVfs(const std::string& root, const std::string& path) {
  const std::string source = "statvfs " + path;
  struct statvfs st {};
  if (::statvfs(Join(root, path).c_str(), &st) != 0) return Fail(source, ErrnoText("statvfs", errno));
  const unsigned long long frag = st.f_frsize != 0 ? st.f_frsize : st.f_bsize;
  nlohmann::json v = nlohmann::json::object();
  v["block_size"] = static_cast<unsigned long long>(st.f_bsize);
  v["fragment_size"] = static_cast<unsigned long long>(st.f_frsize);
  v["total_bytes"] = static_cast<unsigned long long>(st.f_blocks) * frag;
  v["free_bytes"] = static_cast<unsigned long long>(st.f_bfree) * frag;
  v["available_bytes"] = static_cast<unsigned long long>(st.f_bavail) * frag;
  v["total_inodes"] = static_cast<unsigned long long>(st.f_files);
  v["free_inodes"] = static_cast<unsigned long long>(st.f_ffree);
  return Ok(source, v);
}

}  // namespace nevr_quest::hwdump
