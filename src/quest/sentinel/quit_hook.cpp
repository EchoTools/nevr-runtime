#include "quit_hook.h"

#include <dlfcn.h>

#include <atomic>
#include <cstdio>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

#include "android_glue_layout.h"
#include "hook_install.h"
#include "hook_log.h"
#include "process_end_hook.h"

namespace nevr_quest::quit {

namespace {

sentinel::GotHook g_hook;
std::atomic<FinishFn> g_finish{nullptr};
std::atomic<AppFn> g_app{nullptr};
std::atomic<unsigned long long> g_finished{0};
std::atomic<unsigned long long> g_gameUi{0};
std::atomic<unsigned long long> g_calls{0};

// The Horizon OS build of this headset, read on each QUIT (QUIT is rare): ro.build.display.id and
// ro.build.version.incremental. A host build has no system properties and reads "unknown".
void ReadProperty(const char* name, char (&out)[64]) noexcept {
#if defined(__ANDROID__)
  char value[PROP_VALUE_MAX] = {};
  const int length = __system_property_get(name, value);
  std::snprintf(out, sizeof(out), "%s", length > 0 ? value : "unknown");
#else
  std::snprintf(out, sizeof(out), "%s", "unknown");
  static_cast<void>(name);
#endif
}

// The type libr15 passes to vrapi_ShowSystemUI on this path: a constant in libr15 (`mov w1, #1` in CVR::ShowSystemUI,
// pinned_targets.h). CVR::ShowSystemUI takes no type argument, so the handler cannot read the type at run time.
constexpr int kPinnedVrType = 1;

// CVR::ShowSystemUI, reached only from the menu's QUIT. Finish the activity, so the game ends the way it ends when the
// OS closes it; with no activity or nothing to finish it with, run the game's own call. Each QUIT writes one "quit"
// line before anything else runs: the action, the pinned type, the OS build and the call count (#1276).
void Handler(Thunk::Fn original) noexcept {
  const unsigned long long calls = g_calls.fetch_add(1, std::memory_order_acq_rel) + 1;
  char osBuild[64];
  char osIncremental[64];
  ReadProperty("ro.build.display.id", osBuild);
  ReadProperty("ro.build.version.incremental", osIncremental);
  const FinishFn finish = g_finish.load(std::memory_order_acquire);
  const AppFn app = g_app.load(std::memory_order_acquire);
  void* const activity = sentinel::glue::ReadPointer(app != nullptr ? app() : nullptr, sentinel::glue::kAppActivity);
  if (finish != nullptr && activity != nullptr) {
    g_finished.fetch_add(1, std::memory_order_acq_rel);
    sentinel::LogFields(sentinel::LogLevel::kInfo, "quit",
                        {{"action", "finish_activity"},
                         {"os_build", osBuild},
                         {"os_incremental", osIncremental},
                         {"vr_type", kPinnedVrType},
                         {"vr_type_source", "pinned_constant"},
                         {"calls", calls}});
    finish(static_cast<ANativeActivity*>(activity));
    return;
  }
  g_gameUi.fetch_add(1, std::memory_order_acq_rel);
  sentinel::LogFields(sentinel::LogLevel::kWarn, "quit",
                      {{"action", "game_system_ui"},
                       {"reason", finish == nullptr ? "no_finisher" : "no_activity"},
                       {"os_build", osBuild},
                       {"os_incremental", osIncremental},
                       {"vr_type", kPinnedVrType},
                       {"vr_type_source", "pinned_constant"},
                       {"calls", calls}});
  original();
}

NEVR_HOOK_RECORD(kHook, Thunk, &Handler);

sentinel::pinned::AndroidAppOpaque* GameApp() noexcept { return nevr_quest::process_end::CurrentApp(); }

void Arm(FinishFn finish, AppFn app) noexcept {
  g_finish.store(finish, std::memory_order_release);
  g_app.store(app, std::memory_order_release);
  Thunk::Arm(kHook);
}

}  // namespace

Counts CurrentCounts() noexcept {
  return {g_finished.load(std::memory_order_acquire), g_gameUi.load(std::memory_order_acquire)};
}

bool InstallAt(const sentinel::GotTarget& target, FinishFn finish, AppFn app) noexcept {
  if (finish == nullptr) {
    sentinel::LogFields(sentinel::LogLevel::kError, "quit_install",
                        {{"result", "refused"}, {"status", "no_finisher"}, {"slot", target.symbol}, {"finisher", "missing"}});
    return false;
  }
  Arm(finish, app);
  const sentinel::GotStatus status = sentinel::InstallThunk<Thunk>(g_hook, target);
  const bool ok = status == sentinel::GotStatus::kOk;
  sentinel::LogFields(ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "quit_install",
                      {{"result", ok ? "installed" : "refused"},
                       {"status", sentinel::GotStatusName(status)},
                       {"slot", target.symbol},
                       {"finisher", "found"}});
  return ok;
}

bool Install() noexcept {
  // libr15 NEEDs libandroid.so and imports ANativeActivity_finish from it, so this dlopen returns the copy already
  // loaded. The handle is kept for the life of the process.
  void* const android = dlopen("libandroid.so", RTLD_NOW);
  const FinishFn finish =
      android != nullptr ? reinterpret_cast<FinishFn>(dlsym(android, "ANativeActivity_finish")) : nullptr;
  return InstallAt(sentinel::pinned::LibR15ShowSystemUI(), finish, &GameApp);
}

void ArmForTest(FinishFn finish, AppFn app) noexcept { Arm(finish, app); }

void ResetForTest() noexcept {
  g_finish.store(nullptr, std::memory_order_relaxed);
  g_app.store(nullptr, std::memory_order_relaxed);
  g_finished.store(0, std::memory_order_relaxed);
  g_gameUi.store(0, std::memory_order_relaxed);
  g_calls.store(0, std::memory_order_relaxed);
}

}  // namespace nevr_quest::quit
