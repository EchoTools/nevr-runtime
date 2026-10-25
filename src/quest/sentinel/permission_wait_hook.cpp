#include "permission_wait_hook.h"

#include <time.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "hook_install.h"
#include "hook_log.h"

namespace nevr_quest::permission_wait {

namespace {

using WaitFn = StorageOutcome (*)(const char* const* names, std::uint64_t count) noexcept;

constexpr std::size_t kMaxNames = 16;
constexpr const char kStorageSuffix[] = "_EXTERNAL_STORAGE";
constexpr const char kMicPermission[] = "android.permission.RECORD_AUDIO";
constexpr const char kStoragePermissions[2][48] = {"android.permission.READ_EXTERNAL_STORAGE",
                                                   "android.permission.WRITE_EXTERNAL_STORAGE"};

sentinel::GotHook g_hook;
sentinel::GotHook g_checkHook;
std::atomic<bool> g_micSeen{false};
std::atomic<CheckFn> g_check{nullptr};
std::atomic<WaitFn> g_wait{nullptr};
std::atomic<AfterStorageFn> g_after{nullptr};
std::atomic<bool> g_spent{false};
std::atomic<unsigned> g_pollMs{kPollMs};
std::atomic<unsigned> g_timeoutMs{kTimeoutMs};
std::atomic<unsigned long long> g_requests{0};
std::atomic<unsigned long long> g_waits{0};
std::atomic<unsigned long long> g_granted{0};
std::atomic<unsigned long long> g_timeouts{0};

bool IsStorage(const char* name) noexcept {
  if (name == nullptr) return false;
  const std::size_t n = std::strlen(name);
  const std::size_t s = sizeof(kStorageSuffix) - 1;
  return n >= s && std::memcmp(name + n - s, kStorageSuffix, s) == 0;
}

std::uint64_t NowMs() noexcept {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000u + static_cast<std::uint64_t>(ts.tv_nsec) / 1000000u;
}

void SleepMs(unsigned ms) noexcept {
  timespec ts{};
  ts.tv_sec = static_cast<time_t>(ms / 1000);
  ts.tv_nsec = static_cast<long>(ms % 1000) * 1000000L;
  nanosleep(&ts, nullptr);
}

// True when every one of `storage` is granted. Stops at the first denial, as the game's own probe is one call.
bool AllGranted(CheckFn check, const char* const* storage, std::size_t n) noexcept {
  for (std::size_t i = 0; i < n; ++i) {
    if (!check(storage[i])) return false;
  }
  return true;
}

// After the original RequestPermissions returned. Reached only through g_wait.
StorageOutcome WaitForStorage(const char* const* names, std::uint64_t count) noexcept {
  const CheckFn check = g_check.load(std::memory_order_acquire);
  if (check == nullptr) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "permission_wait", {{"state", "check_unavailable"}});
    return StorageOutcome::kCheckUnavailable;
  }
  const char* storage[kMaxNames];
  std::size_t n = 0;
  if (names != nullptr) {
    for (std::uint64_t i = 0; i < count && n < kMaxNames; ++i) {
      if (IsStorage(names[i])) storage[n++] = names[i];
    }
  }
  if (n == 0) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "permission_wait", {{"state", "no_storage_permission"}});
    return StorageOutcome::kNoStoragePermission;
  }
  if (AllGranted(check, storage, n)) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "permission_wait",
                        {{"state", "already_granted"}, {"permissions", static_cast<long long>(n)}});
    return StorageOutcome::kAlreadyGranted;
  }
  g_waits.fetch_add(1, std::memory_order_relaxed);
  const unsigned timeoutMs = g_timeoutMs.load(std::memory_order_relaxed);
  const unsigned pollMs = g_pollMs.load(std::memory_order_relaxed);
  sentinel::LogFields(sentinel::LogLevel::kInfo, "permission_wait",
                      {{"state", "waiting"}, {"permissions", static_cast<long long>(n)}, {"timeout_ms", timeoutMs}});
  const std::uint64_t start = NowMs();
  for (;;) {
    SleepMs(pollMs);
    const std::uint64_t waited = NowMs() - start;
    if (AllGranted(check, storage, n)) {
      g_granted.fetch_add(1, std::memory_order_relaxed);
      sentinel::LogFields(sentinel::LogLevel::kInfo, "permission_wait",
                          {{"state", "granted"}, {"waited_ms", static_cast<long long>(waited)}});
      return StorageOutcome::kGranted;
    }
    if (waited >= timeoutMs) {
      g_timeouts.fetch_add(1, std::memory_order_relaxed);
      sentinel::LogFields(sentinel::LogLevel::kWarn, "permission_wait",
                          {{"state", "timeout"}, {"waited_ms", static_cast<long long>(waited)}});
      return StorageOutcome::kTimeout;
    }
  }
}

// RequestPermissions. The handler touches the game only through `original` and reaches the rest through a pointer.
void RequestHandler(RequestThunk::Fn original, const char* const* names, std::uint64_t count) noexcept {
  original(names, count);
  g_requests.fetch_add(1, std::memory_order_relaxed);
  if (g_spent.exchange(true, std::memory_order_acq_rel)) return;  // only the boot request waits
  const WaitFn wait = g_wait.load(std::memory_order_acquire);
  // No wait armed: nothing was checked, so nothing is known about the grant.
  const StorageOutcome outcome = wait != nullptr ? wait(names, count) : StorageOutcome::kCheckUnavailable;
  const AfterStorageFn after = g_after.load(std::memory_order_acquire);
  if (after != nullptr) after(outcome);
}

NEVR_HOOK_RECORD(kRequestHook, RequestThunk, &RequestHandler);

// CheckPermission. The first call in the process is CR15Game::Initialize's RECORD_AUDIO check; the game calls
// RequestPermissions only when it answers "denied". When it answers "granted" no request and no wait will come, so the
// after-storage step runs here, once, with what the game says about the storage permissions (asked through `original`,
// the game's own function, so the hook is not re-entered). Every other call is the game's answer, untouched.
bool CheckHandler(CheckThunk::Fn original, const char* permission) noexcept {
  const bool granted = original(permission);
  if (permission == nullptr || std::strcmp(permission, kMicPermission) != 0) return granted;
  if (g_micSeen.exchange(true, std::memory_order_acq_rel)) return granted;  // only the boot check decides
  if (!granted) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "permission_wait", {{"state", "microphone_denied"}});
    return granted;  // the game requests next; the request path runs the step
  }
  const bool storage = original(kStoragePermissions[0]) && original(kStoragePermissions[1]);
  sentinel::LogFields(sentinel::LogLevel::kInfo, "permission_wait",
                      {{"state", "not_requested"}, {"microphone", "granted"}, {"storage", storage ? "granted" : "denied"}});
  if (g_spent.exchange(true, std::memory_order_acq_rel)) return granted;
  const AfterStorageFn after = g_after.load(std::memory_order_acquire);
  if (after != nullptr) {
    after(storage ? StorageOutcome::kAlreadyGranted : StorageOutcome::kDeniedNotRequested);
  }
  return granted;
}

NEVR_HOOK_RECORD(kCheckHook, CheckThunk, &CheckHandler);

bool Equal(const char* a, const char* b) noexcept { return std::strcmp(a, b) == 0; }

// Proves libr15's CheckPermission is the pinned build's (build ID, then its first four instructions) and returns it.
// `*reason` names the outcome.
CheckFn ResolveCheck(const char** reason) noexcept {
  sentinel::ElfImage image;
  if (!sentinel::FindLoadedImage(sentinel::pinned::kLibR15, &image)) {
    *reason = "module_not_loaded";
    return nullptr;
  }
  char id[64] = {};
  if (!sentinel::ReadBuildId(image, id, sizeof(id))) {
    *reason = "no_build_id";
    return nullptr;
  }
  if (!Equal(id, sentinel::pinned::kLibR15BuildId)) {
    *reason = "build_id_mismatch";
    return nullptr;
  }
  const unsigned char* fn =
      reinterpret_cast<const unsigned char*>(image.base) + sentinel::pinned::kCheckPermissionVaddr;
  if (std::memcmp(fn, sentinel::pinned::kCheckPermissionCode, sizeof(sentinel::pinned::kCheckPermissionCode)) != 0) {
    *reason = "prologue_mismatch";
    return nullptr;
  }
  CheckFn check = nullptr;
  static_assert(sizeof(check) == sizeof(fn), "function pointer size");
  std::memcpy(&check, &fn, sizeof(check));
  *reason = "resolved";
  return check;
}

}  // namespace

const char* StorageOutcomeName(StorageOutcome outcome) noexcept {
  switch (outcome) {
    case StorageOutcome::kGranted: return "granted";
    case StorageOutcome::kAlreadyGranted: return "already_granted";
    case StorageOutcome::kTimeout: return "timeout";
    case StorageOutcome::kCheckUnavailable: return "check_unavailable";
    case StorageOutcome::kNoStoragePermission: return "no_storage_permission";
    case StorageOutcome::kDeniedNotRequested: return "denied_not_requested";
  }
  return "unknown";
}

void SetAfterStorage(AfterStorageFn fn) noexcept { g_after.store(fn, std::memory_order_release); }

bool InstallAt(const sentinel::GotTarget& target, const sentinel::GotTarget& checkTarget, CheckFn check) noexcept {
  const char* reason = "injected";
  if (check == nullptr) check = ResolveCheck(&reason);
  g_check.store(check, std::memory_order_release);
  g_spent.store(false, std::memory_order_relaxed);
  g_micSeen.store(false, std::memory_order_relaxed);
  g_pollMs.store(kPollMs, std::memory_order_relaxed);
  g_timeoutMs.store(kTimeoutMs, std::memory_order_relaxed);
  g_wait.store(&WaitForStorage, std::memory_order_release);
  RequestThunk::Arm(kRequestHook);
  CheckThunk::Arm(kCheckHook);
  const sentinel::GotStatus status = sentinel::InstallThunk<RequestThunk>(g_hook, target);
  const bool ok = status == sentinel::GotStatus::kOk;
  sentinel::LogFields(ok ? (check != nullptr ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn)
                         : sentinel::LogLevel::kError,
                      "permission_wait_install",
                      {{"result", ok ? "installed" : "failed"},
                       {"status", sentinel::GotStatusName(status)},
                       {"check_permission", reason}});
  // The CheckPermission slot: without it a headset that holds the microphone permission is never reached.
  const sentinel::GotStatus checkStatus = sentinel::InstallThunk<CheckThunk>(g_checkHook, checkTarget);
  const bool checkOk = checkStatus == sentinel::GotStatus::kOk;
  sentinel::LogFields(checkOk ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "permission_wait_check_install",
                      {{"result", checkOk ? "installed" : "failed"},
                       {"status", sentinel::GotStatusName(checkStatus)}});
  return ok && checkOk;
}

bool Install() noexcept {
  return InstallAt(sentinel::pinned::LibR15RequestPermissions(), sentinel::pinned::LibR15CheckPermission(), nullptr);
}

void ArmForTest(CheckFn check, unsigned pollMs, unsigned timeoutMs) noexcept {
  g_check.store(check, std::memory_order_release);
  g_spent.store(false, std::memory_order_relaxed);
  g_pollMs.store(pollMs, std::memory_order_relaxed);
  g_timeoutMs.store(timeoutMs, std::memory_order_relaxed);
  g_wait.store(&WaitForStorage, std::memory_order_release);
  g_micSeen.store(false, std::memory_order_relaxed);
  RequestThunk::Arm(kRequestHook);
  CheckThunk::Arm(kCheckHook);
}

void ResetForTest() noexcept {
  g_check.store(nullptr, std::memory_order_relaxed);
  g_after.store(nullptr, std::memory_order_relaxed);
  g_wait.store(nullptr, std::memory_order_relaxed);
  g_spent.store(false, std::memory_order_relaxed);
  g_micSeen.store(false, std::memory_order_relaxed);
  g_pollMs.store(kPollMs, std::memory_order_relaxed);
  g_timeoutMs.store(kTimeoutMs, std::memory_order_relaxed);
  g_requests.store(0, std::memory_order_relaxed);
  g_waits.store(0, std::memory_order_relaxed);
  g_granted.store(0, std::memory_order_relaxed);
  g_timeouts.store(0, std::memory_order_relaxed);
}

Counts CurrentCounts() noexcept {
  return {g_requests.load(std::memory_order_relaxed), g_waits.load(std::memory_order_relaxed),
          g_granted.load(std::memory_order_relaxed), g_timeouts.load(std::memory_order_relaxed)};
}

}  // namespace nevr_quest::permission_wait
