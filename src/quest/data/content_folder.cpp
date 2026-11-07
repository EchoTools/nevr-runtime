#include "quest/data/content_folder.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <memory>

#include "quest/data/sha256.h"
#include "quest/sentinel/path_rewrite.h"

namespace nevr_quest::content {

namespace {

// The pack this process serves: the shared table, the files that hold its files open, and the package a game path's afk spelling
// is keyed in. Published once Run has built it (and before the hook goes in); the hook only reads it.
struct State {
  std::unique_ptr<nevr_content::Pack> pack;
  std::unique_ptr<PosixPackFiles> files;
  char running_package[128] = {};
};
std::atomic<State*> g_state{nullptr};

// The key of `path`: the afk spelling turned into the running package's (path_rewrite.h), into `out`. No allocation.
const char* KeyOf(const char* path, const char* running_package, char (&out)[path_rewrite::kBufferSize]) noexcept {
  return path_rewrite::Rewrite(path, running_package, out, sizeof(out));
}

}  // namespace

std::string ContentDir(const std::string& media_root) { return media_root + "/" + kContentSubdir; }

std::string StreamSha256(int fd, std::size_t chunk_bytes) {
  if (fd < 0 || chunk_bytes == 0) return std::string();
  std::vector<unsigned char> buffer(chunk_bytes);  // one chunk, whatever the file's size
  data::Sha256 sha;
  off_t offset = 0;
  for (;;) {
    const ssize_t got = ::pread(fd, buffer.data(), buffer.size(), offset);
    if (got < 0) {
      if (errno == EINTR) continue;
      return std::string();
    }
    if (got == 0) break;  // end of file
    sha.Update(buffer.data(), static_cast<std::size_t>(got));
    offset += got;
  }
  return sha.Hex();
}

std::vector<std::string> PosixPackFiles::Folders(const std::string& dir) {
  std::vector<std::string> out;
  DIR* d = ::opendir(dir.c_str());
  if (d == nullptr) return out;
  while (const dirent* e = ::readdir(d)) {
    const std::string name = e->d_name;
    if (name == "." || name == "..") continue;
    struct stat st;
    if (::stat((dir + "/" + name).c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
    out.push_back(name);
  }
  ::closedir(d);
  std::sort(out.begin(), out.end());
  return out;
}

bool PosixPackFiles::ReadText(const std::string& path, std::size_t max_bytes, std::string* text) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  struct stat st;
  bool ok = ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && static_cast<std::uint64_t>(st.st_size) <= max_bytes;
  if (ok) {
    text->assign(static_cast<std::size_t>(st.st_size), '\0');
    std::size_t done = 0;
    while (ok && done < text->size()) {
      const ssize_t got = ::read(fd, &(*text)[done], text->size() - done);
      if (got < 0 && errno == EINTR) continue;
      ok = got > 0;
      if (ok) done += static_cast<std::size_t>(got);
    }
  }
  ::close(fd);
  return ok;
}

nevr_content::HeldFile PosixPackFiles::Hold(const std::string& path) {
  nevr_content::HeldFile held;
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    held.error = static_cast<unsigned long>(errno);
    return held;
  }
  held_.push_back(fd);
  held.opened = true;
  struct stat st;
  if (::fstat(fd, &st) == 0) held.size = static_cast<long long>(st.st_size);
  if (held.size != 0) held.sha256 = StreamSha256(fd, kDigestChunkBytes);
  // The game is handed a fresh open of this descriptor's file: the file that was hashed, even after a rename over `path`, with an
  // offset of its own.
  held.serve_path = "/proc/self/fd/" + std::to_string(fd);
  return held;
}

void PosixPackFiles::ReleaseAll() {
  for (const int fd : held_) ::close(fd);
  held_.clear();
}

std::string PosixPackFiles::Sha256Of(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return std::string();
  const std::string digest = StreamSha256(fd, kDigestChunkBytes);
  ::close(fd);
  return digest;
}

bool PosixPackFiles::Exists(const std::string& path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0;
}

std::string PosixPackFiles::Join(const std::string& dir, const std::string& relative) {
  if (dir.empty() || dir.back() == '/') return dir + relative;
  return dir + "/" + relative;
}

std::string PosixPackFiles::Key(const std::string& path) {
  char buf[path_rewrite::kBufferSize];
  return KeyOf(path.c_str(), running_package_.c_str(), buf);
}

nevr_content::Decision Run(const Config& config) {
  nevr_content::CoreInputs in;
  in.content_dir = config.content_dir;
  in.game_dir = config.engine_root;
  in.data_read_already = config.data_read_already;
  in.scripts_supported = false;
  in.refuse_placed_bundle = true;
  auto files = std::make_unique<PosixPackFiles>(config.running_package);
  std::unique_ptr<nevr_content::Pack> pack;
  nevr_content::Decision d = nevr_content::Decide(in, *files, &pack);
  if (d.outcome == nevr_content::Outcome::kServed) {
    auto state = std::make_unique<State>();
    state->pack = std::move(pack);
    state->files = std::move(files);
    const std::size_t n = std::min(config.running_package.size(), sizeof(state->running_package) - 1);
    config.running_package.copy(state->running_package, n);
    Forget();
    g_state.store(state.release(), std::memory_order_release);
    const char* refused = config.install_hook != nullptr ? config.install_hook() : "no_installer";
    if (refused != nullptr) {
      Forget();
      d.outcome = nevr_content::Outcome::kRefused;
      d.reason = "hook_failed";
      d.detail = std::string("CSysFile::Open slot 0x36f6450 status=") + refused;
    }
  }
  if (config.log != nullptr) {
    nevr_content::LineLevel level = nevr_content::LineLevel::kInfo;
    const std::string line = nevr_content::FormatDecision(d, config.content_dir, &level);
    config.log(level, line.c_str());
  }
  return d;
}

const nevr_content::Served* ServedFor(const char* game_path, unsigned flags) noexcept {
  const State* state = g_state.load(std::memory_order_acquire);
  if (state == nullptr || game_path == nullptr || !nevr_content::IsServedRead(flags)) return nullptr;
  char buf[path_rewrite::kBufferSize];
  return nevr_content::FindKey(state->pack->opens, KeyOf(game_path, state->running_package, buf));
}

nevr_content::LineLevel ServedLine(const nevr_content::Served& served, const char* game_path, int result, char* buf,
                                   std::size_t cap) noexcept {
  const State* state = g_state.load(std::memory_order_acquire);
  const char* id = state != nullptr ? state->pack->id.c_str() : "none";
  return nevr_content::FormatServed(served, id, game_path, static_cast<std::uint32_t>(result), buf, cap);
}

void ServeForTest(std::unique_ptr<nevr_content::Pack> pack, const std::string& running_package) {
  auto state = std::make_unique<State>();
  state->pack = std::move(pack);
  running_package.copy(state->running_package, std::min(running_package.size(), sizeof(state->running_package) - 1));
  Forget();
  g_state.store(state.release(), std::memory_order_release);
}

void Forget() {
  // Only a pack that was never handed to the hook, or a test, is forgotten: once the hook is in, the table stays for the life of
  // the process (a served entry may be in use by the game's thread).
  delete g_state.exchange(nullptr, std::memory_order_acq_rel);
}

}  // namespace nevr_quest::content
