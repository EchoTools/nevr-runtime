#include "map_sets.h"

#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <limits>
#include <mutex>
#include <utility>

#include "hook_log.h"

namespace nevr_quest::map_sets {

namespace {

using nevr_resource_sets::HeldMapFile;
using nevr_resource_sets::Refusal;
using nevr_resource_sets::Registry;

constexpr std::uint64_t kIndexSpace = nevr_shadow::kChunkIndexLimit - nevr_shadow::kFirstChunkIndex;

// The registry and its lock live in one function-local object, made on first use: a namespace-scope object with a destructor would
// give this file a static initializer, which the sentinel's build refuses (#1616, tools/check_quest_static_init.sh).
struct State {
  std::mutex mutex;
  Registry registry{kIndexSpace, nevr_resource_sets::kAndroidTypes};
};
State& Shared() {
  static State state;
  return state;
}
nevr_resource_sets::MountObserver g_observer;  // two atomics with constant initializers: no static initializer

// The Quest's half of a map's file set: a descriptor's size, its positioned reads (pread never moves the offset, which is the
// engine's), and a read-only shared mapping of the whole file for the check. The mapping is the one memory mapping this file makes:
// PROT_READ of a data file, never a code page (N151, docs/adr/0021), released before the registration returns.
class HeldFd : public HeldMapFile {
 public:
  explicit HeldFd(std::int64_t fd) : fd_(fd) {}

  bool Size(std::uint64_t* size, std::string* problem) override {
    if (fd_ < 0 || fd_ > std::numeric_limits<int>::max()) {
      *problem = "bad_handle";
      return false;
    }
    struct stat st {};
    if (::fstat(static_cast<int>(fd_), &st) != 0) {
      *problem = "fstat error " + std::to_string(errno);
      return false;
    }
    if (!S_ISREG(st.st_mode)) {
      *problem = "not_a_file";
      return false;
    }
    *size = static_cast<std::uint64_t>(st.st_size);
    return true;
  }

  bool ReadAt(std::uint64_t offset, void* buffer, std::size_t length, std::size_t* got, std::string* problem) override {
    for (;;) {
      const ssize_t n = ::pread(static_cast<int>(fd_), buffer, length, static_cast<off_t>(offset));
      if (n < 0 && errno == EINTR) continue;
      if (n < 0) {
        *problem = "pread error " + std::to_string(errno);
        return false;
      }
      *got = static_cast<std::size_t>(n);
      return true;
    }
  }

  const void* Map(std::uint64_t size, std::string* problem) override {
    void* const address = ::mmap(nullptr, static_cast<std::size_t>(size), PROT_READ, MAP_SHARED, static_cast<int>(fd_), 0);
    if (address == MAP_FAILED) {
      *problem = "mmap error " + std::to_string(errno);
      return nullptr;
    }
    return address;
  }

  void Unmap(const void* view, std::uint64_t size) override {
    ::munmap(const_cast<void*>(view), static_cast<std::size_t>(size));
  }

 private:
  std::int64_t fd_;
};

void CloseAll(const std::vector<std::int64_t>& fds) {
  for (const std::int64_t fd : fds) {
    if (fd >= 0 && fd <= std::numeric_limits<int>::max()) ::close(static_cast<int>(fd));
  }
}

void LogOutcome(const MapFileRequest& request, const Outcome& outcome) {
  sentinel::LogFields(outcome.ok() ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn, "map_set",
                      {{"result", outcome.ok() ? "registered" : "refused"},
                       {"owner", request.owner.c_str()},
                       {"label", request.label.c_str()},
                       {"rehashed", request.rehash ? "true" : "false"},
                       {"reason", nevr_resource_sets::RefusalName(outcome.refusal)},
                       {"detail", outcome.detail.c_str()},
                       {"msg", outcome.ok() ? "the map's set is held for the mount"
                                            : "nothing was registered, so none of the map's levels or resources is served"}});
}

}  // namespace

Outcome RegisterFileResourceSet(const MapFileRequest& request, std::int64_t held_fd) {
  HeldFd file(held_fd);
  const Outcome outcome = nevr_resource_sets::RegisterMapFile(
      request, file,
      [] {
        State& st = Shared();
        std::lock_guard<std::mutex> lock(st.mutex);
        return st.registry.mounted();
      },
      [&](const void* view, std::uint64_t size) {
        State& st = Shared();
        std::lock_guard<std::mutex> lock(st.mutex);
        return st.registry.AddMapFile(request.owner, request.label, view, size, held_fd);
      });
  LogOutcome(request, outcome);
  return outcome;
}

void Withdraw(const std::string& owner) {
  std::vector<std::int64_t> dropped;
  {
    State& st = Shared();
    std::lock_guard<std::mutex> lock(st.mutex);
    dropped = st.registry.DropMap(owner);
  }
  CloseAll(dropped);
  if (!dropped.empty()) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "map_set",
                        {{"result", "withdrawn"},
                         {"owner", owner.c_str()},
                         {"count", static_cast<long long>(dropped.size())},
                         {"msg", "the map did not mount whole, so its set is not served"}});
  }
}

void SetMountObserver(void (*observer)(const MountReport& report)) { g_observer.Set(observer); }

MountReport Mount(const StockView& stock, PublishFn publish) {
  MountReport report;
  const nevr_shadow::Shadow* shadow = nullptr;
  {
    State& st = Shared();
    std::lock_guard<std::mutex> lock(st.mutex);
    if (st.registry.mounted()) return report;
    report = st.registry.Mount(stock);
    shadow = st.registry.shadow();
  }
  CloseAll(report.handles_to_close);
  for (const MountReport::Refused& r : report.refusals) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "map_set_mount",
                        {{"result", "refused"},
                         {"owner", r.plugin.c_str()},
                         {"label", r.label.c_str()},
                         {"reason", nevr_resource_sets::RefusalName(r.outcome.refusal)},
                         {"detail", r.outcome.detail.c_str()},
                         {"msg", "its levels are neither served nor listed as installed"}});
  }
  sentinel::LogFields(report.sets != 0 || report.refused != 0 ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kDebug,
                      "map_set_mount",
                      {{"result", shadow != nullptr ? "mounted" : "none"},
                       {"sets", static_cast<long long>(report.sets)},
                       {"refused", static_cast<long long>(report.refused)},
                       {"records", static_cast<long long>(report.records)},
                       {"levels", static_cast<long long>(report.levels)},
                       {"stock_records", static_cast<long long>(report.stock_records)},
                       {"shadow", nevr_shadow::StatusName(report.shadow_status)}});
  g_observer.Notify(report);
  if (shadow != nullptr && publish != nullptr) publish(shadow);
  return report;
}

void Abandon() {
  std::vector<std::int64_t> dropped;
  {
    State& st = Shared();
    std::lock_guard<std::mutex> lock(st.mutex);
    if (st.registry.mounted()) return;
    dropped = st.registry.Abandon();
  }
  CloseAll(dropped);
  g_observer.Notify(MountReport{});
}

std::size_t Held() {
  State& st = Shared();
  std::lock_guard<std::mutex> lock(st.mutex);
  return st.registry.mounted() ? st.registry.MountedSets().size() : st.registry.held();
}

std::vector<std::uint64_t> MountedLevelNames() {
  State& st = Shared();
  std::lock_guard<std::mutex> lock(st.mutex);
  return st.registry.MountedLevels();
}

std::vector<std::pair<std::string, std::vector<std::uint64_t>>> MountedOwnerLevels() {
  std::vector<std::pair<std::string, std::vector<std::uint64_t>>> owners;
  State& st = Shared();
  std::lock_guard<std::mutex> lock(st.mutex);
  if (!st.registry.mounted()) return owners;
  for (const nevr_resource_sets::Set* set : st.registry.MountedSets()) owners.emplace_back(set->plugin, set->levels);
  return owners;
}

bool HoldsKey(std::uint64_t type, std::uint64_t name) {
  State& st = Shared();
  std::lock_guard<std::mutex> lock(st.mutex);
  return st.registry.Holds(type, name);
}

void ResetForTest() {
  State& st = Shared();
  std::lock_guard<std::mutex> lock(st.mutex);
  std::vector<std::int64_t> held;
  for (const nevr_resource_sets::Set* set : st.registry.HeldSets()) held.push_back(set->handle);
  CloseAll(held);
  st.registry = Registry(kIndexSpace, nevr_resource_sets::kAndroidTypes);
  g_observer.Reset();
}

}  // namespace nevr_quest::map_sets
