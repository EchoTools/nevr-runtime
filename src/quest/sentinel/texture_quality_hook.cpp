#include "texture_quality_hook.h"

#include <atomic>
#include <cstdint>

#include "hook_install.h"
#include "hook_log.h"
#include "hook_report.h"

namespace nevr_quest::texture_quality {

namespace {

using sentinel::pinned::OvrJavaOpaque;

sentinel::GotHook g_hook;

constexpr int kUnset = kLevelUnset;
// The applied level (a Level, or kUnset): the level the swap chain and the render targets are built for, which the answers follow.
// Written by the first install in the process, once by the start resolver, then by CommitApplied.
std::atomic<int> g_latched{kUnset};
// The level last asked for (a Level, or kUnset) and who asked.
std::atomic<int> g_requested{kUnset};
std::atomic<int> g_requestSource{static_cast<int>(RequestSource::kMenu)};
std::atomic<StartResolver> g_resolver{nullptr};
std::atomic<bool> g_startAsked{false};

std::atomic<std::uint64_t> g_nativeWidth{0};
std::atomic<std::uint64_t> g_nativeHeight{0};
std::atomic<std::uint64_t> g_served{0};
std::atomic<std::uint64_t> g_sizeQueries{0};

std::uint64_t NonNegative(int value) noexcept { return value > 0 ? static_cast<std::uint64_t>(value) : 0U; }

// The first eye-size query comes before the game delivers its settings, so the level the player saved is asked here, once. The
// resolver is the live apply's (it reads the settings file); with none set the configuration's level stands.
void AskStartResolverOnce() noexcept {
  if (g_startAsked.load(std::memory_order_acquire)) return;
  if (g_startAsked.exchange(true, std::memory_order_acq_rel)) return;
  const StartResolver resolver = g_resolver.load(std::memory_order_acquire);
  if (resolver == nullptr) return;
  const int saved = resolver();
  if (saved == kUnset || g_latched.load(std::memory_order_acquire) == kUnset) return;
  g_latched.store(saved, std::memory_order_release);
}

// vrapi_GetSystemPropertyInt. Calls the original with the game's own arguments every time, tells the hardware dump's
// observer (if any) what the runtime answered, then replaces the answer to the eye width and height ids when the applied level
// has a size. The size is the applied level's for the headset the runtime's own answer names (HeadsetForStockSize), so the
// same level is a Quest 2 table on a Quest 2. Never logs or throws: it is live across the call into the runtime
// (callback_thunk.h rule 1).
int Handler(SystemPropertyIntThunk::Fn original, const OvrJavaOpaque* java, int id) noexcept {
  const int native = original(java, id);
  const sentinel::SysPropIntObserver observer = sentinel::GetSysPropIntObserver();
  if (observer != nullptr) observer(java, id, native);
  if (id != kEyeWidthPropertyId && id != kEyeHeightPropertyId) return native;
  AskStartResolverOnce();
  g_sizeQueries.fetch_add(1, std::memory_order_relaxed);
  const int latched = g_latched.load(std::memory_order_acquire);
  if (id == kEyeWidthPropertyId) {
    g_nativeWidth.store(NonNegative(native), std::memory_order_relaxed);
    if (latched != kUnset) {
      const int width = SizeFor(static_cast<Level>(latched), HeadsetForStockSize(native)).width;
      if (width > 0) {
        g_served.fetch_add(1, std::memory_order_relaxed);
        return width;
      }
    }
  } else {
    g_nativeHeight.store(NonNegative(native), std::memory_order_relaxed);
    if (latched != kUnset) {
      const int height = SizeFor(static_cast<Level>(latched), HeadsetForStockSize(native)).height;
      if (height > 0) {
        g_served.fetch_add(1, std::memory_order_relaxed);
        return height;
      }
    }
  }
  return native;
}

NEVR_HOOK_RECORD(kHook, SystemPropertyIntThunk, &Handler);

enum class Latch { kNew, kSame, kOther };

// The first non-stock level to ask wins; a different one later does not change it.
Latch TryLatch(Level level) noexcept {
  int expected = kUnset;
  if (g_latched.compare_exchange_strong(expected, static_cast<int>(level), std::memory_order_acq_rel)) return Latch::kNew;
  return expected == static_cast<int>(level) ? Latch::kSame : Latch::kOther;
}

void Unlatch() noexcept { g_latched.store(kUnset, std::memory_order_release); }

sentinel::GotStatus RealInstall() noexcept {
  return sentinel::InstallThunk<SystemPropertyIntThunk>(g_hook, sentinel::pinned::LibR15GetSystemPropertyInt());
}

}  // namespace

bool RegisterCounters() noexcept {
  struct Entry {
    const char* name;
    const std::atomic<std::uint64_t>* value;
    sentinel::ReportKind kind;
  };
  const Entry entries[kCounterCount] = {
      {"gfx_native_eye_width", &g_nativeWidth, sentinel::ReportKind::kCalls},
      {"gfx_native_eye_height", &g_nativeHeight, sentinel::ReportKind::kCalls},
      {"gfx_eye_size_served", &g_served, sentinel::ReportKind::kCalls},
      {"gfx_thunk_faults", &SystemPropertyIntThunk::FaultCounter(), sentinel::ReportKind::kFaults},
  };
  int registered = 0;
  for (const Entry& e : entries) registered += sentinel::RegisterReportCounter(e.name, e.value, e.kind) ? 1 : 0;
  if (registered != kCounterCount) {
    sentinel::LogFields(sentinel::LogLevel::kError, "texture_quality_counters",
                        {{"result", "refused"}, {"registered", registered}, {"wanted", kCounterCount}});
    return false;
  }
  return true;
}

bool InstallVia(Level level, bool fromConfig, InstallFn install) noexcept {
  const char* const quality = LevelName(level);
  const char* const source = fromConfig ? "config" : "default";
  if (TryLatch(level) != Latch::kNew) {
    // A second install in one process: the first level keeps answering and the slot is not touched again.
    sentinel::LogFields(sentinel::LogLevel::kError, "texture_quality",
                        {{"result", "refused"}, {"quality", quality}, {"source", source}, {"w", 0}, {"h", 0},
                         {"why", "level_already_latched"}, {"latched", LevelName(LatchedLevel())}});
    return false;
  }
  // The level is latched before the thunk can be entered, so its first call already sees it.
  SystemPropertyIntThunk::Arm(kHook);
  const sentinel::GotStatus status = install();
  if (status != sentinel::GotStatus::kOk) {
    Unlatch();
    sentinel::LogFields(sentinel::LogLevel::kError, "texture_quality",
                        {{"result", "refused"}, {"quality", quality}, {"source", source}, {"w", 0}, {"h", 0},
                         {"status", sentinel::GotStatusName(status)}});
    return false;
  }
  sentinel::MarkSysPropIntSlotOwned(true);
  // The sizes depend on the headset, which only the first query names; the swap-chain line shows the size served.
  sentinel::LogFields(sentinel::LogLevel::kInfo, "texture_quality",
                      {{"result", "installed"}, {"quality", quality}, {"source", source}, {"w", 0}, {"h", 0},
                       {"sizes", "per_headset"}, {"slot", sentinel::pinned::kGetSystemPropertyIntSymbol},
                       {"status", sentinel::GotStatusName(status)}});
  return true;
}

bool Install(Level level, bool fromConfig) noexcept { return InstallVia(level, fromConfig, &RealInstall); }

bool InstallIfCounted(Level level, bool fromConfig, bool countersRegistered) noexcept {
  if (!countersRegistered) {
    sentinel::LogFields(sentinel::LogLevel::kError, "texture_quality",
                        {{"result", "skipped"}, {"quality", LevelName(level)}, {"source", fromConfig ? "config" : "default"},
                         {"w", 0}, {"h", 0}, {"why", "counters_refused"}});
    return false;
  }
  return Install(level, fromConfig);
}

bool ArmForTest(Level level) noexcept {
  if (TryLatch(level) == Latch::kOther) return false;
  SystemPropertyIntThunk::Arm(kHook);
  return true;
}

void ResetForTest() noexcept {
  Unlatch();
  g_requested.store(kUnset, std::memory_order_release);
  g_requestSource.store(static_cast<int>(RequestSource::kMenu), std::memory_order_relaxed);
  g_resolver.store(nullptr, std::memory_order_release);
  g_startAsked.store(false, std::memory_order_release);
  g_nativeWidth.store(0, std::memory_order_relaxed);
  g_nativeHeight.store(0, std::memory_order_relaxed);
  g_served.store(0, std::memory_order_relaxed);
  g_sizeQueries.store(0, std::memory_order_relaxed);
  sentinel::MarkSysPropIntSlotOwned(false);
}

bool IsLatched() noexcept { return g_latched.load(std::memory_order_acquire) != kUnset; }

Level LatchedLevel() noexcept {
  const int value = g_latched.load(std::memory_order_acquire);
  return value == kUnset ? Level::kStock : static_cast<Level>(value);
}

void SetStartResolver(StartResolver resolver) noexcept { g_resolver.store(resolver, std::memory_order_release); }

void RequestLevel(Level level, RequestSource source) noexcept {
  g_requestSource.store(static_cast<int>(source), std::memory_order_relaxed);
  g_requested.store(static_cast<int>(level), std::memory_order_release);
}

int RequestedLevelValue() noexcept { return g_requested.load(std::memory_order_acquire); }

RequestSource LastRequestSource() noexcept {
  return static_cast<RequestSource>(g_requestSource.load(std::memory_order_relaxed));
}

void CommitApplied(Level level) noexcept { g_latched.store(static_cast<int>(level), std::memory_order_release); }

unsigned long long SizeQueries() noexcept { return g_sizeQueries.load(std::memory_order_relaxed); }

Counts CurrentCounts() noexcept {
  return {g_nativeWidth.load(std::memory_order_relaxed), g_nativeHeight.load(std::memory_order_relaxed),
          g_served.load(std::memory_order_relaxed)};
}

}  // namespace nevr_quest::texture_quality
