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
std::atomic<RequestFn> g_request{nullptr};
std::atomic<StorageNeededFn> g_needed{nullptr};
std::atomic<OriginalKind> g_original{OriginalKind::kUnknown};
std::atomic<bool> g_spent{false};
std::atomic<unsigned> g_pollMs{kPollMs};
std::atomic<unsigned> g_timeoutMs{kTimeoutMs};
std::atomic<unsigned long long> g_requests{0};
std::atomic<unsigned long long> g_waits{0};
std::atomic<unsigned long long> g_granted{0};
std::atomic<unsigned long long> g_timeouts{0};
std::atomic<unsigned long long> g_gameRequests{0};
std::atomic<unsigned long long> g_noRequest{0};

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

// Whether the engine reads shared storage this launch. Null predicate: needed (the conservative answer).
bool StorageNeeded() noexcept {
  const StorageNeededFn f = g_needed.load(std::memory_order_acquire);
  return f == nullptr || f();
}

// The names to ask when the storage permissions are not needed: every name but the storage ones. `out` has kMaxNames room.
std::uint64_t CollectAsked(const char* const* names, std::uint64_t count, const char** out) noexcept {
  std::uint64_t n = 0;
  for (std::uint64_t i = 0; i < count; ++i) {
    if (names[i] != nullptr && !IsStorage(names[i])) out[n++] = names[i];
  }
  return n;
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

// The outcome of the boot request when nothing waits: it checks the storage permissions once, never blocks, and logs why
// nothing was awaited. `reason` is "storage_not_needed", "nothing_to_ask" or "original_no_op".
StorageOutcome ProbeStorage(const char* const* names, std::uint64_t count, const char* reason) noexcept {
  const CheckFn check = g_check.load(std::memory_order_acquire);
  const char* storage[kMaxNames];
  std::size_t n = 0;
  if (names != nullptr) {
    for (std::uint64_t i = 0; i < count && n < kMaxNames; ++i) {
      if (IsStorage(names[i])) storage[n++] = names[i];
    }
  }
  if (check == nullptr) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "permission_wait",
                        {{"state", "not_requested"}, {"reason", reason}, {"storage", "unknown"}});
    return StorageOutcome::kCheckUnavailable;
  }
  if (n == 0) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "permission_wait",
                        {{"state", "not_requested"}, {"reason", reason}, {"storage", "none_named"}});
    return StorageOutcome::kNoStoragePermission;
  }
  const bool granted = AllGranted(check, storage, n);
  const bool benign = granted || std::strcmp(reason, "storage_not_needed") == 0;
  sentinel::LogFields(benign ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn, "permission_wait",
                      {{"state", "not_requested"}, {"reason", reason}, {"storage", granted ? "granted" : "denied"}});
  return granted ? StorageOutcome::kAlreadyGranted : StorageOutcome::kDeniedNotRequested;
}

// RequestPermissions. The handler touches the game only through `original` and reaches the rest through a pointer. The
// names asked are all of them when storage is needed, else the non-storage ones. The request is made through the
// requester with kRequestCode; the game's function, which passes no request code (#947), runs only when the requester did
// not make the call, and on a build whose function is the reference build's `ret` nothing is asked by it. The wait runs
// only when the storage permissions were asked and a request reached Java.
void RequestHandler(RequestThunk::Fn original, const char* const* names, std::uint64_t count) noexcept {
  const bool trimmable = names != nullptr && count <= kMaxNames;
  const bool needed = !trimmable || StorageNeeded();
  const char* trimmed[kMaxNames];
  const std::uint64_t askCount = needed ? count : CollectAsked(names, count, trimmed);
  const char* const* askNames = needed ? names : trimmed;
  const OriginalKind kind = g_original.load(std::memory_order_acquire);
  const char* result = "not_needed";
  const char* path = "none";
  bool made = false;  // a request reached Java
  if (askCount > 0) {
    const RequestFn request = g_request.load(std::memory_order_acquire);
    result = request != nullptr ? request(askNames, askCount, kRequestCode) : "no_requester";
    const bool ours = std::strcmp(result, "requested") == 0;
    path = "ours";
    made = true;
    if (!ours) {
      original(askNames, askCount);
      made = kind != OriginalKind::kReturnsAtOnce;  // on the reference build the original is `ret`: nothing was asked
      path = made ? "game" : "game_noop";
      if (made) g_gameRequests.fetch_add(1, std::memory_order_relaxed);
    }
  } else {
    g_noRequest.fetch_add(1, std::memory_order_relaxed);
  }
  const unsigned long long n = g_requests.fetch_add(1, std::memory_order_relaxed);
  if (n < kMaxLoggedRequests) {
    const bool ok = std::strcmp(path, "ours") == 0 || std::strcmp(path, "none") == 0;
    if (std::strcmp(path, "ours") == 0) {
      sentinel::LogFields(sentinel::LogLevel::kInfo, "permission_request",
                          {{"result", result}, {"path", path}, {"permissions", static_cast<long long>(count)},
                           {"asked", static_cast<long long>(askCount)}, {"storage_needed", needed ? "yes" : "no"},
                           {"request_code", kRequestCode}});
    } else {
      sentinel::LogFields(ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn, "permission_request",
                          {{"result", result}, {"path", path}, {"permissions", static_cast<long long>(count)},
                           {"asked", static_cast<long long>(askCount)}, {"storage_needed", needed ? "yes" : "no"},
                           {"effect", std::strcmp(path, "none") == 0 ? "no permission to ask: no request is made"
                                      : ok ? "the request was made"
                                      : std::strcmp(path, "game") == 0
                                          ? "the requester could not ask, so the game's own request ran instead and reached Java"
                                          : "the request did not reach Java: the requester could not ask and the game's own request on this build returns at once"}});
    }
  }
  if (g_spent.exchange(true, std::memory_order_acq_rel)) return;  // only the boot request waits
  StorageOutcome outcome;
  if (needed && made) {
    const WaitFn wait = g_wait.load(std::memory_order_acquire);
    outcome = wait != nullptr ? wait(names, count) : StorageOutcome::kCheckUnavailable;
  } else {
    outcome = ProbeStorage(names, count, !needed ? "storage_not_needed" : askCount == 0 ? "nothing_to_ask" : "original_no_op");
  }
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

// The libr15 base when the module is loaded and its build ID is the pinned patched build's; null otherwise, with `*reason`
// naming why.
const unsigned char* LivePinnedBase(sentinel::ImageLookup lookup, const char** reason) noexcept {
  sentinel::ElfImage image;
  if (!lookup(sentinel::pinned::kLibR15, &image)) {
    *reason = "module_not_loaded";
    return nullptr;
  }
  char id[64] = {};
  if (!sentinel::ReadBuildId(image, id, sizeof(id))) {
    *reason = "no_build_id";
    return nullptr;
  }
  if (!Equal(id, sentinel::pinned::LibR15PatchedBuildId())) {
    *reason = "build_id_mismatch";
    return nullptr;
  }
  return reinterpret_cast<const unsigned char*>(image.base);
}

// Proves libr15's CheckPermission is the pinned build's (build ID, then its first four instructions) and returns it.
// `*reason` names the outcome.
CheckFn ResolveCheck(sentinel::ImageLookup lookup, const char** reason) noexcept {
  const unsigned char* base = LivePinnedBase(lookup, reason);
  if (base == nullptr) return nullptr;
  const unsigned char* fn = base + sentinel::pinned::kCheckPermissionVaddr;
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

// Reads the first word of libr15's RequestPermissions from the live module and classifies it. `*word` is 0 when the module
// was not checked. `*reason` names the outcome.
OriginalKind ResolveOriginal(sentinel::ImageLookup lookup, const char** reason, std::uint32_t* word) noexcept {
  const unsigned char* base = LivePinnedBase(lookup, reason);
  if (base == nullptr) return OriginalKind::kUnknown;
  std::memcpy(word, base + sentinel::pinned::kRequestPermissionsVaddr, sizeof(*word));
  const OriginalKind kind = ClassifyOriginalWord(*word);
  *reason = kind == OriginalKind::kUnknown ? "prologue_mismatch" : "resolved";
  return kind;
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

void SetRequester(RequestFn fn) noexcept { g_request.store(fn, std::memory_order_release); }

void SetStorageNeeded(StorageNeededFn fn) noexcept { g_needed.store(fn, std::memory_order_release); }

const char* OriginalKindName(OriginalKind kind) noexcept {
  switch (kind) {
    case OriginalKind::kCallsJava: return "calls_java";
    case OriginalKind::kReturnsAtOnce: return "returns_at_once";
    case OriginalKind::kUnknown: break;
  }
  return "unknown";
}

OriginalKind ClassifyOriginalWord(std::uint32_t firstWord) noexcept {
  if (firstWord == sentinel::pinned::kRequestPermissionsStockWord) return OriginalKind::kCallsJava;
  if (firstWord == sentinel::pinned::kRequestPermissionsReturnWord) return OriginalKind::kReturnsAtOnce;
  return OriginalKind::kUnknown;
}

void SetOriginalKind(OriginalKind kind) noexcept { g_original.store(kind, std::memory_order_release); }

OriginalKind CurrentOriginalKind() noexcept { return g_original.load(std::memory_order_acquire); }

bool InstallAtWith(sentinel::ImageLookup lookup, const sentinel::GotTarget& target, const sentinel::GotTarget& checkTarget,
                   CheckFn check) noexcept {
  const char* reason = "injected";
  if (check == nullptr) check = ResolveCheck(lookup, &reason);
  g_check.store(check, std::memory_order_release);
  g_spent.store(false, std::memory_order_relaxed);
  g_micSeen.store(false, std::memory_order_relaxed);
  g_pollMs.store(kPollMs, std::memory_order_relaxed);
  g_timeoutMs.store(kTimeoutMs, std::memory_order_relaxed);
  g_wait.store(&WaitForStorage, std::memory_order_release);
  // The kind of libr15's own RequestPermissions, read from the live module before the thunk is installed.
  const char* origReason = "injected";
  std::uint32_t word = 0;
  const OriginalKind kind = ResolveOriginal(lookup, &origReason, &word);
  g_original.store(kind, std::memory_order_release);
  if (kind == OriginalKind::kCallsJava) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "permission_wait_original",
                        {{"kind", OriginalKindName(kind)}, {"reason", origReason},
                         {"effect", "the game's own request runs when the requester cannot make it"}});
  } else if (kind == OriginalKind::kReturnsAtOnce) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "permission_wait_original",
                        {{"kind", OriginalKindName(kind)}, {"reason", origReason},
                         {"effect", "the game's own request returns at once, so the requester is the only request and the wait covers it"}});
  } else {
    char va[19], expectedCalls[19], expectedReturn[19], actual[19];
    sentinel::LogFields(sentinel::LogLevel::kWarn, "permission_wait_original",
                        {{"kind", "unknown"},
                         {"reason", origReason},
                         {"effect", "the first instruction is neither pinned build's: the game's own request is not trusted to be the one the wait expects"},
                         {"va", sentinel::HexString(va, sentinel::pinned::kRequestPermissionsVaddr)},
                         {"expected_calls_java", sentinel::HexString(expectedCalls, sentinel::pinned::kRequestPermissionsStockWord)},
                         {"expected_returns_at_once", sentinel::HexString(expectedReturn, sentinel::pinned::kRequestPermissionsReturnWord)},
                         {"actual", sentinel::HexString(actual, word)}});
  }
  RequestThunk::Arm(kRequestHook);
  CheckThunk::Arm(kCheckHook);
  const sentinel::GotStatus status = sentinel::InstallThunk<RequestThunk>(g_hook, target, lookup);
  const bool ok = status == sentinel::GotStatus::kOk;
  sentinel::LogFields(ok ? (check != nullptr ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn)
                         : sentinel::LogLevel::kError,
                      "permission_wait_install",
                      {{"result", ok ? "installed" : "failed"},
                       {"status", sentinel::GotStatusName(status)},
                       {"check_permission", reason}});
  // The CheckPermission slot: without it a headset that holds the microphone permission is never reached.
  const sentinel::GotStatus checkStatus = sentinel::InstallThunk<CheckThunk>(g_checkHook, checkTarget, lookup);
  const bool checkOk = checkStatus == sentinel::GotStatus::kOk;
  sentinel::LogFields(checkOk ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "permission_wait_check_install",
                      {{"result", checkOk ? "installed" : "failed"},
                       {"status", sentinel::GotStatusName(checkStatus)}});
  return ok && checkOk;
}

bool InstallAt(const sentinel::GotTarget& target, const sentinel::GotTarget& checkTarget, CheckFn check) noexcept {
  return InstallAtWith(sentinel::FindLoadedImage, target, checkTarget, check);
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
  g_request.store(nullptr, std::memory_order_relaxed);
  g_needed.store(nullptr, std::memory_order_relaxed);
  g_original.store(OriginalKind::kUnknown, std::memory_order_relaxed);
  g_wait.store(nullptr, std::memory_order_relaxed);
  g_spent.store(false, std::memory_order_relaxed);
  g_micSeen.store(false, std::memory_order_relaxed);
  g_pollMs.store(kPollMs, std::memory_order_relaxed);
  g_timeoutMs.store(kTimeoutMs, std::memory_order_relaxed);
  g_requests.store(0, std::memory_order_relaxed);
  g_waits.store(0, std::memory_order_relaxed);
  g_granted.store(0, std::memory_order_relaxed);
  g_timeouts.store(0, std::memory_order_relaxed);
  g_gameRequests.store(0, std::memory_order_relaxed);
  g_noRequest.store(0, std::memory_order_relaxed);
}

Counts CurrentCounts() noexcept {
  return {g_requests.load(std::memory_order_relaxed), g_waits.load(std::memory_order_relaxed),
          g_granted.load(std::memory_order_relaxed), g_timeouts.load(std::memory_order_relaxed),
          g_gameRequests.load(std::memory_order_relaxed), g_noRequest.load(std::memory_order_relaxed)};
}

}  // namespace nevr_quest::permission_wait
