#include "map_sets.h"

#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <mutex>
#include <utility>

#include "hook_log.h"
#include "quest/data/sha256.h"

namespace nevr_quest::map_sets {

namespace {

using nevr_resource_sets::Refusal;
using nevr_resource_sets::Registry;

constexpr std::uint64_t kIndexSpace = nevr_shadow::kChunkIndexLimit - nevr_shadow::kFirstChunkIndex;
constexpr std::size_t kMaxTextBytes = 128;
constexpr std::size_t kHashReadBytes = 1u << 20;

struct Mapping {
  int fd;
  void* address;
  std::size_t size;
};

std::mutex g_mutex;
Registry g_registry(kIndexSpace, nevr_resource_sets::kAndroidTypes);
std::vector<Mapping> g_mappings;
void (*g_observer)(const MountReport&) = nullptr;

Outcome Refuse(Refusal refusal, std::string detail) { return Outcome{refusal, std::move(detail)}; }

bool TextOk(const std::string& text) {
  if (text.empty() || text.size() > kMaxTextBytes) return false;
  for (const char c : text) {
    if (c < 0x20 || c > 0x7e || c == '"' || c == '\\') return false;
  }
  return true;
}

bool PinOk(const std::string& sha256) {
  if (sha256.size() != 64) return false;
  return std::all_of(sha256.begin(), sha256.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// The file's SHA-256 through `fd`, by positioned reads (the descriptor's offset is the engine's, never moved here).
bool HashFile(int fd, std::uint64_t size, std::string* hex, std::string* problem) {
  std::vector<unsigned char> buffer(kHashReadBytes);
  nevr_quest::data::Sha256 sha;
  std::uint64_t at = 0;
  while (at < size) {
    const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), size - at));
    const ssize_t got = ::pread(fd, buffer.data(), want, static_cast<off_t>(at));
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) {
      *problem = got < 0 ? "read error " + std::to_string(errno) : "file ended at " + std::to_string(at);
      return false;
    }
    sha.Update(buffer.data(), static_cast<std::size_t>(got));
    at += static_cast<std::uint64_t>(got);
  }
  *hex = sha.Hex();
  return true;
}

// Unmaps and closes each listed descriptor. Caller holds g_mutex.
void ReleaseLocked(const std::vector<std::int64_t>& fds) {
  for (const std::int64_t fd : fds) {
    const auto it = std::find_if(g_mappings.begin(), g_mappings.end(),
                                 [fd](const Mapping& m) { return m.fd == static_cast<int>(fd); });
    if (it == g_mappings.end()) continue;
    ::munmap(it->address, it->size);
    ::close(it->fd);
    g_mappings.erase(it);
  }
}

void LogOutcome(const std::string& owner, const std::string& label, const Outcome& outcome) {
  sentinel::LogFields(outcome.ok() ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn, "map_set",
                      {{"result", outcome.ok() ? "registered" : "refused"},
                       {"owner", owner.c_str()},
                       {"label", label.c_str()},
                       {"reason", nevr_resource_sets::RefusalName(outcome.refusal)},
                       {"detail", outcome.detail.c_str()}});
}

Outcome Register(const std::string& owner, std::int64_t held_fd, const std::string& label, const std::string& sha256,
                 bool rehash) {
  if (!nevr_resource_sets::IsMapOwner(owner) || !TextOk(owner)) return Refuse(Refusal::kFormat, "bad_owner");
  if (!TextOk(label)) return Refuse(Refusal::kFormat, "bad_label");
  if (rehash && !PinOk(sha256)) return Refuse(Refusal::kFormat, "bad_pin");
  if (held_fd < 0 || held_fd > 0x7fffffff) return Refuse(Refusal::kFormat, "bad_handle");
  const int fd = static_cast<int>(held_fd);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_registry.mounted()) return Refuse(Refusal::kTooLate, "the run's resource sets are already mounted");
  }
  struct stat st {};
  if (::fstat(fd, &st) != 0) return Refuse(Refusal::kFormat, "fstat error " + std::to_string(errno));
  if (!S_ISREG(st.st_mode)) return Refuse(Refusal::kFormat, "not_a_file");
  if (st.st_size <= 0) return Refuse(Refusal::kFormat, "empty_file");
  const std::uint64_t size = static_cast<std::uint64_t>(st.st_size);
  if (rehash) {
    std::string hex, problem;
    if (!HashFile(fd, size, &hex, &problem)) return Refuse(Refusal::kFormat, "hash " + problem);
    if (hex != sha256) return Refuse(Refusal::kHash, "file " + hex + " pinned " + sha256);
  }
  void* const address = ::mmap(nullptr, static_cast<std::size_t>(size), PROT_READ, MAP_SHARED, fd, 0);
  if (address == MAP_FAILED) return Refuse(Refusal::kFormat, "mmap error " + std::to_string(errno));
  std::lock_guard<std::mutex> lock(g_mutex);
  // Listed before the Add, so on ok() the registry's handle always has its mapping here to release.
  g_mappings.push_back(Mapping{fd, address, static_cast<std::size_t>(size)});
  const Outcome outcome = g_registry.AddMapFile(owner, label, address, size, held_fd);
  if (!outcome.ok()) {
    g_mappings.pop_back();
    ::munmap(address, static_cast<std::size_t>(size));
  }
  return outcome;
}

}  // namespace

Outcome RegisterFileResourceSet(const std::string& owner, std::int64_t held_fd, const std::string& label,
                                const std::string& sha256, bool rehash) {
  const Outcome outcome = Register(owner, held_fd, label, sha256, rehash);
  LogOutcome(owner, label, outcome);
  return outcome;
}

void Withdraw(const std::string& owner) {
  std::lock_guard<std::mutex> lock(g_mutex);
  ReleaseLocked(g_registry.DropMap(owner));
}

void SetMountObserver(void (*observer)(const MountReport& report)) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_observer = observer;
}

MountReport Mount(const StockView& stock, PublishFn publish) {
  MountReport report;
  const nevr_shadow::Shadow* shadow = nullptr;
  void (*observer)(const MountReport&) = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_registry.mounted()) return report;
    report = g_registry.Mount(stock);
    ReleaseLocked(report.handles_to_close);
    shadow = g_registry.shadow();
    observer = g_observer;
  }
  for (const MountReport::Refused& r : report.refusals) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "map_set_mount",
                        {{"result", "refused"},
                         {"owner", r.plugin.c_str()},
                         {"label", r.label.c_str()},
                         {"reason", nevr_resource_sets::RefusalName(r.outcome.refusal)},
                         {"detail", r.outcome.detail.c_str()}});
  }
  sentinel::LogFields(report.sets != 0 || report.refused != 0 ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kDebug,
                      "map_set_mount",
                      {{"result", shadow != nullptr ? "mounted" : "none"},
                       {"sets", report.sets},
                       {"refused", report.refused},
                       {"records", report.records},
                       {"levels", report.levels},
                       {"stock_records", report.stock_records},
                       {"shadow", nevr_shadow::StatusName(report.shadow_status)}});
  if (observer != nullptr) observer(report);
  if (shadow != nullptr && publish != nullptr) publish(shadow);
  return report;
}

void Abandon() {
  void (*observer)(const MountReport&) = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_registry.mounted()) return;
    ReleaseLocked(g_registry.Abandon());
    observer = g_observer;
  }
  if (observer != nullptr) observer(MountReport{});
}

std::size_t Held() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_registry.mounted() ? g_registry.MountedSets().size() : g_registry.held();
}

std::vector<std::uint64_t> MountedLevelNames() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_registry.MountedLevels();
}

std::vector<std::pair<std::string, std::vector<std::uint64_t>>> MountedOwnerLevels() {
  std::vector<std::pair<std::string, std::vector<std::uint64_t>>> owners;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_registry.mounted()) return owners;
  for (const nevr_resource_sets::Set* set : g_registry.MountedSets()) owners.emplace_back(set->plugin, set->levels);
  return owners;
}

bool HoldsKey(std::uint64_t type, std::uint64_t name) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_registry.Holds(type, name);
}

void ResetForTest() {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (const Mapping& m : g_mappings) {
    ::munmap(m.address, m.size);
    ::close(m.fd);
  }
  g_mappings.clear();
  g_registry = Registry(kIndexSpace, nevr_resource_sets::kAndroidTypes);
  g_observer = nullptr;
}

}  // namespace nevr_quest::map_sets
