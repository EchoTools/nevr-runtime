#include "obb_skip_hook.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#include "data_root.h"
#include "hook_install.h"
#include "hook_log.h"
#include "hook_report.h"
#include "quest/data/plan.h"

namespace nevr_quest::obb_skip {

namespace {

using sentinel::pinned::AStorageManagerOpaque;
using sentinel::pinned::ObbCallbackFn;

sentinel::GotHook g_mountHook;
sentinel::GotHook g_pathHook;
sentinel::GotHook g_dataRootHook;

std::atomic<std::uint64_t> g_mountSkipped{0};
std::atomic<std::uint64_t> g_pathServed{0};
std::atomic<std::uint64_t> g_dataRootServed{0};
// Set before the callback is called (the callback asks for the path), never cleared in production: the
// game's mount request was answered, and every later path request is for that mount.
std::atomic<bool> g_skipped{false};
// The root served for the skipped mount (#749): chosen by ResolveRoot before the game runs, so the game's call path
// never touches the filesystem or the log. A string literal in data_root.h, never freed.
std::atomic<const char*> g_root{sentinel::pinned::kObbFallbackDataRoot};
// The root the data-root hook answers with (#866): the media root when ResolveRoot found the manifest there, else null (the
// original answers). Chosen by ResolveRoot before the hook is installed; a string literal, never freed.
std::atomic<const char*> g_dataRootRedirect{nullptr};
// Set once the data-root slot holds our thunk: only then is the engine reading the redirected root.
std::atomic<bool> g_dataRootHookLive{false};

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
  return g_root.load(std::memory_order_acquire);
}

// CSysInfo::GetDataRootDir. Redirected only to the media root that holds the game's data; otherwise the original.
const char* DataRootHandler(DataRootThunk::Fn original) noexcept {
  const char* const redirect = g_dataRootRedirect.load(std::memory_order_acquire);
  if (redirect == nullptr) return original();
  g_dataRootServed.fetch_add(1, std::memory_order_relaxed);
  return redirect;
}

NEVR_HOOK_RECORD(kMountHook, MountThunk, &MountHandler);
NEVR_HOOK_RECORD(kPathHook, PathThunk, &PathHandler);
NEVR_HOOK_RECORD(kDataRootHook, DataRootThunk, &DataRootHandler);

}  // namespace

bool DirectoryExists(const char* path) noexcept {
  struct stat st;
  return ::stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool PathExists(const char* path) noexcept {
  struct stat st;
  if (::stat(path, &st) != 0) return false;
  if (S_ISDIR(st.st_mode)) return true;
  return S_ISREG(st.st_mode) && st.st_size > 0;
}

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
      {"data_root_served", &g_dataRootServed, sentinel::ReportKind::kCalls},
      {"data_root_thunk_faults", &DataRootThunk::FaultCounter(), sentinel::ReportKind::kFaults},
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

const char* CurrentRoot() noexcept { return g_root.load(std::memory_order_acquire); }

const char* DataRootRedirect() noexcept {
  return g_dataRootHookLive.load(std::memory_order_acquire) ? g_dataRootRedirect.load(std::memory_order_acquire)
                                                            : nullptr;
}

namespace {

// True when `root` holds the manifest the data step takes as "this version of the data is there" (quest/data/plan.h).
bool HoldsManifest(const char* root, data_root::ExistsFn fileExists) noexcept {
  char path[512];
  const int n = std::snprintf(path, sizeof(path), "%s/%s", root, data::kManifestPath);
  return n > 0 && static_cast<std::size_t>(n) < sizeof(path) && fileExists(path);
}

}  // namespace

void ResolveRoot(data_root::ExistsFn exists) noexcept { ResolveRoot(exists, exists); }

void ResolveRoot(data_root::ExistsFn dirExists, data_root::ExistsFn fileExists) noexcept {
  const data_root::Resolution r = data_root::Resolve(dirExists);
  g_root.store(r.root, std::memory_order_release);
  // The engine is told the media root only when it holds the manifest: a `_data` directory alone may be a partial or an
  // older copy, and the legacy root may hold the complete one (#932). An unmarked media root is served to a skipped
  // mount only.
  const bool mediaWithData = r.found && std::strcmp(r.source, "android_media") == 0;
  const bool mediaComplete = mediaWithData && HoldsManifest(r.root, fileExists);
  g_dataRootRedirect.store(mediaComplete ? r.root : nullptr, std::memory_order_release);
  if (r.found && std::strcmp(r.source, "android_media_unmarked") == 0) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "data_root",
                        {{"result", "found"},
                         {"source", r.source},
                         {"root", r.root},
                         {"detail", "the media directory exists but holds no _data directly under it"}});
  } else if (r.found) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "data_root",
                        {{"result", "found"},
                         {"source", r.source},
                         {"root", r.root},
                         {"engine_redirect", mediaComplete ? "media_complete" : mediaWithData ? "media_incomplete" : "no"}});
  } else {
    sentinel::LogFields(sentinel::LogLevel::kError, "data_root",
                        {{"result", "not_found"},
                         {"detail", "neither the Android/media directory nor the legacy root exists with the game's _data"},
                         {"tried_media", data_root::kMediaRoot},
                         {"tried_legacy", data_root::kLegacyRoot}});
  }
}

namespace {

sentinel::GotStatus InstallPathSlot() noexcept {
  return sentinel::InstallThunk<PathThunk>(g_pathHook, sentinel::pinned::LibR15GetMountedObbPath());
}
sentinel::GotStatus InstallMountSlot() noexcept {
  return sentinel::InstallThunk<MountThunk>(g_mountHook, sentinel::pinned::LibR15MountObb());
}
sentinel::GotStatus InstallDataRootSlot() noexcept {
  return sentinel::InstallThunk<DataRootThunk>(g_dataRootHook, sentinel::pinned::LibR15GetDataRootDir());
}

}  // namespace

bool Install() noexcept {
  return InstallWith({&DirectoryExists, &PathExists, &InstallPathSlot, &InstallMountSlot, &InstallDataRootSlot});
}

bool InstallWith(const InstallEnv& env) noexcept {
  ResolveRoot(env.dirExists, env.fileExists);
  PathThunk::Arm(kPathHook);
  const sentinel::GotStatus path = env.installPath();
  sentinel::GotStatus mount = sentinel::GotStatus::kNotInstalled;
  if (path == sentinel::GotStatus::kOk) {
    MountThunk::Arm(kMountHook);
    mount = env.installMount();
  }
  // The data-root hook does not depend on the other two: with no OBB file they never run and this one is what matters.
  DataRootThunk::Arm(kDataRootHook);
  const sentinel::GotStatus dataRoot = env.installDataRoot();
  g_dataRootHookLive.store(dataRoot == sentinel::GotStatus::kOk, std::memory_order_release);
  const bool ok = path == sentinel::GotStatus::kOk && mount == sentinel::GotStatus::kOk &&
                  dataRoot == sentinel::GotStatus::kOk;
  const char* result = "installed";
  if (!ok) result = path == sentinel::GotStatus::kOk && mount == sentinel::GotStatus::kOk ? "data_root_failed"
                    : path == sentinel::GotStatus::kOk ? "path_only" : "failed";
  sentinel::LogFields(ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "obb_skip_install",
                      {{"result", result},
                       {"path", sentinel::GotStatusName(path)},
                       {"mount", sentinel::GotStatusName(mount)},
                       {"data_root", sentinel::GotStatusName(dataRoot)}});
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
  DataRootThunk::Arm(kDataRootHook);
  g_dataRootHookLive.store(true, std::memory_order_relaxed);
}

void ResetForTest() noexcept {
  g_skipped.store(false, std::memory_order_relaxed);
  g_root.store(sentinel::pinned::kObbFallbackDataRoot, std::memory_order_relaxed);
  g_mountSkipped.store(0, std::memory_order_relaxed);
  g_pathServed.store(0, std::memory_order_relaxed);
  g_dataRootServed.store(0, std::memory_order_relaxed);
  g_dataRootRedirect.store(nullptr, std::memory_order_relaxed);
  g_dataRootHookLive.store(false, std::memory_order_relaxed);
}

Counts CurrentCounts() noexcept {
  return {g_mountSkipped.load(std::memory_order_relaxed), g_pathServed.load(std::memory_order_relaxed),
          g_dataRootServed.load(std::memory_order_relaxed)};
}

}  // namespace nevr_quest::obb_skip
