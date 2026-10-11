#include "obb_skip_hook.h"

#include <atomic>
#include <cstdint>

#include "hook_install.h"
#include "hook_log.h"
#include "hook_report.h"

namespace nevr_quest::obb_skip {

namespace {

using sentinel::pinned::AStorageManagerOpaque;
using sentinel::pinned::ObbCallbackFn;

sentinel::GotHook g_mountHook;
sentinel::GotHook g_pathHook;

std::atomic<std::uint64_t> g_mountSkipped{0};
std::atomic<std::uint64_t> g_pathServed{0};
// Set before the callback is called (the callback asks for the path), never cleared in production: the
// game's mount request was answered, and every later path request is for that mount.
std::atomic<bool> g_skipped{false};

// AStorageManager_mountObb. A request without a callback has nobody to answer: the original.
void MountHandler(MountThunk::Fn original, AStorageManagerOpaque* manager, const char* filename, const char* key,
                  ObbCallbackFn callback, void* data) noexcept {
  if (callback == nullptr) {
    original(manager, filename, key, callback, data);
    return;
  }
  g_skipped.store(true, std::memory_order_release);
  g_mountSkipped.fetch_add(1, std::memory_order_relaxed);
  callback(filename, sentinel::pinned::kObbStateMounted, data);
}

// AStorageManager_getMountedObbPath. Before a skipped mount the original answers.
const char* PathHandler(PathThunk::Fn original, AStorageManagerOpaque* manager, const char* filename) noexcept {
  if (!g_skipped.load(std::memory_order_acquire)) return original(manager, filename);
  g_pathServed.fetch_add(1, std::memory_order_relaxed);
  return sentinel::pinned::kObbFallbackDataRoot;
}

NEVR_HOOK_RECORD(kMountHook, MountThunk, &MountHandler);
NEVR_HOOK_RECORD(kPathHook, PathThunk, &PathHandler);

}  // namespace

bool RegisterCounters() noexcept {
  struct Entry {
    const char* name;
    const std::atomic<std::uint64_t>* value;
    sentinel::ReportKind kind;
  };
  const Entry entries[kCounterCount] = {
      {"obb_mount_skipped", &g_mountSkipped, sentinel::ReportKind::kCalls},
      {"obb_path_served", &g_pathServed, sentinel::ReportKind::kCalls},
      {"obb_mount_thunk_faults", &MountThunk::FaultCounter(), sentinel::ReportKind::kFaults},
      {"obb_path_thunk_faults", &PathThunk::FaultCounter(), sentinel::ReportKind::kFaults},
  };
  int registered = 0;
  for (const Entry& e : entries) registered += sentinel::RegisterReportCounter(e.name, e.value, e.kind) ? 1 : 0;
  if (registered != kCounterCount) {
    sentinel::LogFields(sentinel::LogLevel::kError, "obb_skip_counters",
                        {{"result", "refused"}, {"registered", registered}, {"wanted", kCounterCount}});
    return false;
  }
  return true;
}

bool Install() noexcept {
  PathThunk::Arm(kPathHook);
  const sentinel::GotStatus path =
      sentinel::InstallThunk<PathThunk>(g_pathHook, sentinel::pinned::LibR15GetMountedObbPath());
  sentinel::GotStatus mount = sentinel::GotStatus::kNotInstalled;
  if (path == sentinel::GotStatus::kOk) {
    MountThunk::Arm(kMountHook);
    mount = sentinel::InstallThunk<MountThunk>(g_mountHook, sentinel::pinned::LibR15MountObb());
  }
  const bool ok = path == sentinel::GotStatus::kOk && mount == sentinel::GotStatus::kOk;
  sentinel::LogFields(ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "obb_skip_install",
                      {{"result", ok ? "installed" : (path == sentinel::GotStatus::kOk ? "path_only" : "failed")},
                       {"path", sentinel::GotStatusName(path)},
                       {"mount", sentinel::GotStatusName(mount)}});
  return ok;
}

bool InstallIfCounted(bool countersRegistered) noexcept {
  if (!countersRegistered) {
    sentinel::LogFields(sentinel::LogLevel::kError, "obb_skip_install",
                        {{"result", "skipped"}, {"why", "counters_refused"}});
    return false;
  }
  return Install();
}

void ArmForTest() noexcept {
  g_skipped.store(false, std::memory_order_relaxed);
  PathThunk::Arm(kPathHook);
  MountThunk::Arm(kMountHook);
}

void ResetForTest() noexcept {
  g_skipped.store(false, std::memory_order_relaxed);
  g_mountSkipped.store(0, std::memory_order_relaxed);
  g_pathServed.store(0, std::memory_order_relaxed);
}

Counts CurrentCounts() noexcept {
  return {g_mountSkipped.load(std::memory_order_relaxed), g_pathServed.load(std::memory_order_relaxed)};
}

}  // namespace nevr_quest::obb_skip
