#include "quest/redirect/hook_adapter.h"

#include <atomic>
#include <cstdint>
#include <exception>
#include <mutex>

#include "hook_log.h"

namespace nevr_quest::redirect {
namespace {

using sentinel::GotStatus;
std::atomic<ServiceRedirector*> g_redirector{nullptr};

struct Installation {
  std::mutex mutex;
  sentinel::GotHook libr15Hook;
  sentinel::GotHook matchmakingHook;
  ServiceRedirector* redirector = nullptr;  // never freed: a call in flight may still hold it
  InstallOptions options{{sentinel::GotTarget("", "", sentinel::RelocKind::kJumpSlot),
                          sentinel::GotTarget("", "", sentinel::RelocKind::kJumpSlot)},
                         sentinel::FindLoadedImage, nullptr, nullptr};
  InstallReport report;
};

Installation& State() {
  static Installation* const state = new Installation();  // leaked on purpose, like the pool
  return *state;
}

void LogInstall(const char* what, GotStatus status) {
  sentinel::LogFields(status == GotStatus::kOk ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kWarn,
                      "redirect_install", {{"target", what}, {"status", sentinel::GotStatusName(status)}});
}

GotStatus InstallMatchmakingLocked(Installation& s, sentinel::ImageLookup lookup) {
  if (s.redirector == nullptr) return GotStatus::kNotInstalled;
  if (s.matchmakingHook.installed()) return GotStatus::kAlreadyInstalled;
  ArmThunk(Slot::kMatchmaking, true);
  const GotStatus status = s.matchmakingHook.Install(
      s.options.targets.matchmaking, ThunkEntry(Slot::kMatchmaking),
      ThunkOriginalOut(Slot::kMatchmaking), lookup);
  if (status != GotStatus::kOk) ArmThunk(Slot::kMatchmaking, false);
  s.report.matchmaking = status;
  LogInstall("matchmaking_tstring", status);
  return status;
}

}  // namespace

const char* ApplyActive(const char* key, const char* result) noexcept {
  ServiceRedirector* const redirector = g_redirector.load(std::memory_order_acquire);
  return redirector == nullptr ? result : redirector->Apply(key, result);
}

InstallReport InstallRedirectHooksWith(const nevr_quest::ResolvedConfig& config, const InstallOptions& options) {
  Installation& s = State();
  const std::lock_guard<std::mutex> lock(s.mutex);

  if (s.redirector != nullptr) {
    LogInstall("redirect", GotStatus::kAlreadyInstalled);
    return s.report;
  }
  InstallReport report;
  report.featureEnabled = nevr_quest::FeatureEnabled(config, nevr_quest::Feature::kRedirect);
  if (!report.featureEnabled || options.intern == nullptr) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "redirect_install",
                        {{"target", "redirect"}, {"status", "feature_off"}, {"action", "nothing_installed"}});
    return report;
  }

  ServiceRedirector* redirector = nullptr;
  try {
    redirector = new ServiceRedirector(config, options.intern, options.bridge);
  } catch (const std::exception&) {
    sentinel::LogFields(sentinel::LogLevel::kError, "redirect_install",
                        {{"target", "redirect"}, {"status", "allocation_failure"}, {"action", "nothing_installed"}});
    report.featureEnabled = false;
    return report;
  }
  redirector->Prewarm();

  s.redirector = redirector;
  s.options = options;
  g_redirector.store(redirector, std::memory_order_release);

  ArmThunk(Slot::kLibR15, true);
  report.libr15 = s.libr15Hook.Install(options.targets.libr15, ThunkEntry(Slot::kLibR15),
                                       ThunkOriginalOut(Slot::kLibR15), options.lookup);
  if (report.libr15 != GotStatus::kOk) ArmThunk(Slot::kLibR15, false);
  LogInstall("libr15_tstring", report.libr15);

  s.report = report;
  report.matchmaking = InstallMatchmakingLocked(s, options.lookup);
  s.report = report;
  return report;
}

InstallReport InstallRedirectHooks(const nevr_quest::ResolvedConfig& config) {
  return InstallRedirectHooksWith(config, {PinnedTargets(), sentinel::FindLoadedImage, &nevr_runtime::lifecycle::InternStableCStr, nullptr});
}

GotStatus InstallMatchmakingRedirectWith(sentinel::ImageLookup lookup) {
  Installation& s = State();
  const std::lock_guard<std::mutex> lock(s.mutex);
  return InstallMatchmakingLocked(s, lookup);
}

GotStatus InstallMatchmakingRedirect() { return InstallMatchmakingRedirectWith(sentinel::FindLoadedImage); }

void RemoveRedirectHooks() {
  Installation& s = State();
  const std::lock_guard<std::mutex> lock(s.mutex);
  if (s.libr15Hook.installed()) LogInstall("libr15_tstring_remove", s.libr15Hook.Remove());
  if (s.matchmakingHook.installed()) LogInstall("matchmaking_tstring_remove", s.matchmakingHook.Remove());
  ArmThunk(Slot::kLibR15, false);
  ArmThunk(Slot::kMatchmaking, false);
  g_redirector.store(nullptr, std::memory_order_release);
  s.redirector = nullptr;
  s.report = InstallReport{};
}

void ArmHandlersForTest(ServiceRedirector* redirector) {
  g_redirector.store(redirector, std::memory_order_release);
  ArmThunk(Slot::kLibR15, redirector != nullptr);
  ArmThunk(Slot::kMatchmaking, redirector != nullptr);
}

}  // namespace nevr_quest::redirect
