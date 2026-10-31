// Built with -fno-exceptions (callback_thunk.h refuses otherwise).
#include "path_rewrite_hook.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <dirent.h>
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include "callback_thunk.h"
#include "hook_install.h"
#include "hook_log.h"
#include "path_rewrite.h"
#include "pinned_targets.h"

namespace nevr_quest::path_rewrite {

namespace {

enum Slot : int { kOpen, kFopen, kAccess, kMkdir, kOpendir, kRemove, kRename, kRmdir, kSlotCount };

// Link-time slot addresses (readelf --dyn-relocations -W on the pinned file; the stock layout, the same build ID).
struct SlotRow {
  const char* symbol;
  std::uint64_t slot;
};
constexpr SlotRow kSlots[kSlotCount] = {
    {"open", 0x36db728ULL},  {"fopen", 0x36e2640ULL},  {"access", 0x36fbe18ULL}, {"mkdir", 0x36da1a0ULL},
    {"opendir", 0x36e45d8ULL}, {"remove", 0x36fd2e8ULL}, {"rename", 0x36c8e30ULL}, {"rmdir", 0x36cdc28ULL},
};

sentinel::GotTarget Target(Slot s) {
  return sentinel::GotTarget{sentinel::pinned::kLibR15, kSlots[s].symbol, sentinel::RelocKind::kJumpSlot,
                             sentinel::pinned::LibR15PatchedBuildId(), kSlots[s].slot}
      .ServiceSide();
}

constexpr int kHookCount = 8;
static_assert(kHookCount == kSlotCount, "one GotHook per slot");
sentinel::GotHook g_hooks[kHookCount];
char g_package[128] = {};  // written once before the first install, read-only after
std::atomic<std::uint64_t> g_rewrites[kSlotCount] = {};
std::atomic<long long> g_lastCountsMs{0};
constexpr long long kCountsIntervalMs = 10000;
constexpr int kFirstSlots = 32;
std::atomic<std::uint64_t> g_seenDirs[kFirstSlots] = {};

long long NowMs() noexcept {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
  return static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// FNV-1a of the directory part (up to the last '/'); never 0.
std::uint64_t DirHash(const char* path) noexcept {
  const char* const end = std::strrchr(path, '/');
  std::uint64_t h = 1469598103934665603ULL;
  for (const char* p = path; end != nullptr && p < end; ++p) {
    h ^= static_cast<unsigned char>(*p);
    h *= 1099511628211ULL;
  }
  return h == 0 ? 1 : h;
}

void LogCounts() noexcept {
  sentinel::LogFields(sentinel::LogLevel::kInfo, "path_rewrite_counts",
                      {{"open", static_cast<long long>(g_rewrites[kOpen].load(std::memory_order_relaxed))},
                       {"fopen", static_cast<long long>(g_rewrites[kFopen].load(std::memory_order_relaxed))},
                       {"access", static_cast<long long>(g_rewrites[kAccess].load(std::memory_order_relaxed))},
                       {"mkdir", static_cast<long long>(g_rewrites[kMkdir].load(std::memory_order_relaxed))},
                       {"opendir", static_cast<long long>(g_rewrites[kOpendir].load(std::memory_order_relaxed))},
                       {"remove", static_cast<long long>(g_rewrites[kRemove].load(std::memory_order_relaxed))},
                       {"rename", static_cast<long long>(g_rewrites[kRename].load(std::memory_order_relaxed))},
                       {"rmdir", static_cast<long long>(g_rewrites[kRmdir].load(std::memory_order_relaxed))},
                       {"window_ms", kCountsIntervalMs}});
}

// The rewritten path, or `path`; counts and logs a rewrite. errno is kept as the caller left it.
const char* Apply(Slot s, const char* path, char* buf) noexcept {
  const char* const out = Rewrite(path, g_package, buf, kBufferSize);
  if (out == path) return path;
  const int savedErrno = errno;
  g_rewrites[s].fetch_add(1, std::memory_order_relaxed);
  const std::uint64_t h = DirHash(out);
  for (std::atomic<std::uint64_t>& seen : g_seenDirs) {
    std::uint64_t cur = seen.load(std::memory_order_acquire);
    if (cur == h) break;
    if (cur == 0 && seen.compare_exchange_strong(cur, h, std::memory_order_acq_rel)) {
      sentinel::LogFields(sentinel::LogLevel::kInfo, "path_rewrite_first",
                          {{"symbol", kSlots[s].symbol}, {"from", path}, {"to", out},
                           {"consequence", "the reference build's afk data path is served from the running package's "
                                           "media directory"}});
      break;
    }
    if (cur == h) break;
  }
  const long long now = NowMs();
  long long last = g_lastCountsMs.load(std::memory_order_relaxed);
  if (now - last >= kCountsIntervalMs && g_lastCountsMs.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
    LogCounts();
  }
  errno = savedErrno;
  return out;
}

struct OpenTag {};
struct FopenTag {};
struct AccessTag {};
struct MkdirTag {};
struct OpendirTag {};
struct RemoveTag {};
struct RenameTag {};
struct RmdirTag {};
// open is variadic; on AArch64 (AAPCS64, Linux) the mode argument is passed in x2 as a named int would be.
using OpenThunk = sentinel::CallbackThunk<OpenTag, int(const char*, int, int)>;
using FopenThunk = sentinel::CallbackThunk<FopenTag, FILE*(const char*, const char*)>;
using AccessThunk = sentinel::CallbackThunk<AccessTag, int(const char*, int)>;
using MkdirThunk = sentinel::CallbackThunk<MkdirTag, int(const char*, mode_t)>;
using OpendirThunk = sentinel::CallbackThunk<OpendirTag, DIR*(const char*)>;
using RemoveThunk = sentinel::CallbackThunk<RemoveTag, int(const char*)>;
using RenameThunk = sentinel::CallbackThunk<RenameTag, int(const char*, const char*)>;
using RmdirThunk = sentinel::CallbackThunk<RmdirTag, int(const char*)>;

int HookedOpen(OpenThunk::Fn original, const char* path, int flags, int mode) noexcept {
  char buf[kBufferSize];
  return original(Apply(kOpen, path, buf), flags, mode);
}
FILE* HookedFopen(FopenThunk::Fn original, const char* path, const char* how) noexcept {
  char buf[kBufferSize];
  return original(Apply(kFopen, path, buf), how);
}
int HookedAccess(AccessThunk::Fn original, const char* path, int how) noexcept {
  char buf[kBufferSize];
  return original(Apply(kAccess, path, buf), how);
}
int HookedMkdir(MkdirThunk::Fn original, const char* path, mode_t mode) noexcept {
  char buf[kBufferSize];
  return original(Apply(kMkdir, path, buf), mode);
}
DIR* HookedOpendir(OpendirThunk::Fn original, const char* path) noexcept {
  char buf[kBufferSize];
  return original(Apply(kOpendir, path, buf));
}
int HookedRemove(RemoveThunk::Fn original, const char* path) noexcept {
  char buf[kBufferSize];
  return original(Apply(kRemove, path, buf));
}
int HookedRename(RenameThunk::Fn original, const char* from, const char* to) noexcept {
  char a[kBufferSize];
  char b[kBufferSize];
  return original(Apply(kRename, from, a), Apply(kRename, to, b));
}
int HookedRmdir(RmdirThunk::Fn original, const char* path) noexcept {
  char buf[kBufferSize];
  return original(Apply(kRmdir, path, buf));
}

NEVR_HOOK_RECORD(kOpenHook, OpenThunk, &HookedOpen);
NEVR_HOOK_RECORD(kFopenHook, FopenThunk, &HookedFopen);
NEVR_HOOK_RECORD(kAccessHook, AccessThunk, &HookedAccess);
NEVR_HOOK_RECORD(kMkdirHook, MkdirThunk, &HookedMkdir);
NEVR_HOOK_RECORD(kOpendirHook, OpendirThunk, &HookedOpendir);
NEVR_HOOK_RECORD(kRemoveHook, RemoveThunk, &HookedRemove);
NEVR_HOOK_RECORD(kRenameHook, RenameThunk, &HookedRename);
NEVR_HOOK_RECORD(kRmdirHook, RmdirThunk, &HookedRmdir);

template <typename Thunk>
sentinel::GotStatus InstallOne(Slot s, const sentinel::HookRecord<Thunk>& record) noexcept {
  Thunk::Arm(record);
  const sentinel::GotStatus status = sentinel::InstallThunk<Thunk>(g_hooks[s], Target(s));
  if (status != sentinel::GotStatus::kOk) Thunk::Disarm();
  char slot[19];
  sentinel::LogFields(status == sentinel::GotStatus::kOk ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn,
                      "path_rewrite_install",
                      {{"symbol", kSlots[s].symbol}, {"slot", sentinel::HexString(slot, kSlots[s].slot)},
                       {"status", sentinel::GotStatusName(status)}});
  return status;
}

bool ReadRunningPackage() noexcept {
  char cmdline[256] = {};
  const int fd = ::open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  const ssize_t n = ::read(fd, cmdline, sizeof cmdline - 1);
  ::close(fd);
  return n > 0 && PackageFromCmdline(cmdline, static_cast<std::size_t>(n), g_package, sizeof g_package);
}

std::atomic<bool> g_done{false};

}  // namespace

bool InstallForServiceOnly() noexcept {
  if (g_done.exchange(true, std::memory_order_acq_rel)) return true;
  if (!ReadRunningPackage()) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "path_rewrite",
                        {{"status", "off"}, {"reason", "running_package_unreadable"},
                         {"consequence", "the reference build reads and writes its afk media directory as it is"}});
    return false;
  }
  if (std::strcmp(g_package, kFromPackage) == 0) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "path_rewrite",
                        {{"status", "off"}, {"reason", "running_package_is_target"}, {"package", g_package},
                         {"consequence", "the afk media directory is this package's own: nothing to rewrite"}});
    return true;
  }
  int ok = 0;
  ok += InstallOne(kOpen, kOpenHook) == sentinel::GotStatus::kOk;
  ok += InstallOne(kFopen, kFopenHook) == sentinel::GotStatus::kOk;
  ok += InstallOne(kAccess, kAccessHook) == sentinel::GotStatus::kOk;
  ok += InstallOne(kMkdir, kMkdirHook) == sentinel::GotStatus::kOk;
  ok += InstallOne(kOpendir, kOpendirHook) == sentinel::GotStatus::kOk;
  ok += InstallOne(kRemove, kRemoveHook) == sentinel::GotStatus::kOk;
  ok += InstallOne(kRename, kRenameHook) == sentinel::GotStatus::kOk;
  ok += InstallOne(kRmdir, kRmdirHook) == sentinel::GotStatus::kOk;
  sentinel::LogFields(ok == kSlotCount ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "path_rewrite",
                      {{"status", ok == kSlotCount ? "on" : "partial"}, {"package", g_package}, {"from", kFromPackage},
                       {"installed", ok}, {"of", static_cast<int>(kSlotCount)},
                       {"consequence", ok == kSlotCount
                                           ? "libr15's afk media paths resolve to the running package's media directory"
                                           : "some of libr15's file calls still use the afk media directory"}});
  return ok == kSlotCount;
}

}  // namespace nevr_quest::path_rewrite
