#include "quest/redirect/hook_adapter.h"

#include <atomic>
#include <cstdint>
#include <exception>
#include <mutex>

#include "hook_log.h"
#include "hook_report.h"

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

// Installs one slot unless it is already installed. A poisoned slot (a failed install left our entry
// possibly reachable through it) is final for this process: it is logged as such and never retried
// here, and the thunk stays disarmed.
//
// The caller retries the matchmaking slot after every dlopen until libpnsradmatchmaking.so is
// loaded, which happens only after a login. A module that is not loaded yet is answered here,
// without asking the backend (which would log its own got_hook line for each attempt), and the
// redirect_install line is written only when the slot's status changes, so the retries before the
// load leave one line, not one per dlopen.
GotStatus InstallSlotLocked(Slot slot, sentinel::GotHook& hook, const sentinel::GotTarget& target,
                            sentinel::ImageLookup lookup, GotStatus* stored, const char* what) {
  if (hook.installed()) return GotStatus::kAlreadyInstalled;
  if (*stored == GotStatus::kSlotPoisoned) return GotStatus::kSlotPoisoned;
  const GotStatus previous = *stored;
  GotStatus status = GotStatus::kModuleNotLoaded;
  sentinel::ElfImage image;
  if (lookup == nullptr || target.module == nullptr || lookup(target.module, &image)) {
    ArmThunk(slot, true);
    status = InstallThunk(slot, hook, target, lookup);
    if (status != GotStatus::kOk) ArmThunk(slot, false);
  }
  *stored = status;
  if (status == GotStatus::kSlotPoisoned) {
    sentinel::LogFields(sentinel::LogLevel::kError, "redirect_install",
                        {{"target", what}, {"status", "slot_poisoned"}, {"action", "not_retried"}});
  } else if (status != previous) {
    LogInstall(what, status);
  }
  return status;
}

GotStatus InstallMatchmakingLocked(Installation& s, sentinel::ImageLookup lookup) {
  if (s.redirector == nullptr) return GotStatus::kNotInstalled;
  return InstallSlotLocked(Slot::kMatchmaking, s.matchmakingHook, s.options.targets.matchmaking, lookup,
                           &s.report.matchmaking, "matchmaking_tstring");
}

const char* ApplyActive(const char* key, const char* result) noexcept {
  ServiceRedirector* const redirector = g_redirector.load(std::memory_order_acquire);
  return redirector == nullptr ? result : redirector->Apply(key, result);
}

}  // namespace

bool RegisterRedirectCounters() noexcept {
  RedirectCounters& c = GlobalCounters();
  bool ok = RegisterThunkCounters();
  ok = sentinel::RegisterReportCounter("redirect_service_reads", &c.reads) && ok;
  ok = sentinel::RegisterReportCounter("redirect_applied", &c.redirected) && ok;
  ok = sentinel::RegisterReportCounter("redirect_policy_runs", &c.policyRuns) && ok;
  ok = sentinel::RegisterReportCounter("redirect_value_too_long", &c.valueTooLong, sentinel::ReportKind::kFaults) && ok;
  ok = sentinel::RegisterReportCounter("redirect_pool_refused", &c.poolRefused, sentinel::ReportKind::kFaults) && ok;
  ok = sentinel::RegisterReportCounter("redirect_exceptions", &c.exceptions, sentinel::ReportKind::kFaults) && ok;
  return ok;
}

InstallReport InstallRedirectHooksWith(const nevr_quest::ResolvedConfig& config, const InstallOptions& options) {
  Installation& s = State();
  const std::lock_guard<std::mutex> lock(s.mutex);

  if (s.redirector == nullptr) {
    InstallReport off;
    off.featureEnabled = nevr_quest::FeatureEnabled(config, nevr_quest::Feature::kRedirect);
    if (!off.featureEnabled) {
      sentinel::LogFields(sentinel::LogLevel::kInfo, "redirect_install",
                          {{"target", "redirect"}, {"status", "feature_off"}, {"action", "nothing_installed"}});
      return off;
    }
    if (options.intern == nullptr) {
      sentinel::LogFields(sentinel::LogLevel::kError, "redirect_install",
                          {{"target", "redirect"}, {"status", "no_string_pool"}, {"action", "nothing_installed"}});
      off.featureEnabled = false;
      return off;
    }
    ServiceRedirector* redirector = nullptr;
    try {
      redirector = new ServiceRedirector(config, options.intern, options.bridge);
    } catch (const std::exception&) {
      sentinel::LogFields(sentinel::LogLevel::kError, "redirect_install",
                          {{"target", "redirect"}, {"status", "allocation_failure"}, {"action", "nothing_installed"}});
      off.featureEnabled = false;
      return off;
    }
    redirector->Prewarm();
    const Counters warmed = redirector->counters();
    sentinel::LogFields(sentinel::LogLevel::kInfo, "redirect_install",
                        {{"target", "redirect"}, {"status", "prewarmed"},
                         {"policy_runs", static_cast<long long>(warmed.policyRuns)},
                         {"redirected", static_cast<long long>(warmed.redirected)},
                         {"failures", static_cast<long long>(warmed.failures)}});
    s.redirector = redirector;
    s.options = options;
    s.report = InstallReport{};
    s.report.featureEnabled = true;
    SetApply(&ApplyActive);
    g_redirector.store(redirector, std::memory_order_release);
  } else {
    LogInstall("redirect", GotStatus::kAlreadyInstalled);
  }

  // A repeated call re-attempts whichever slot is not installed (a failed install leaves the
  // redirector in place), and leaves an installed slot's recorded status alone.
  InstallSlotLocked(Slot::kLibR15, s.libr15Hook, s.options.targets.libr15, s.options.lookup,
                    &s.report.libr15, "libr15_tstring");
  InstallMatchmakingLocked(s, s.options.lookup);
  return s.report;
}

InstallReport InstallRedirectHooks(const nevr_quest::ResolvedConfig& config, InternFn intern, BridgeProbe bridge) {
  return InstallRedirectHooksWith(config, {PinnedTargets(), sentinel::FindLoadedImage,
                                           intern != nullptr ? intern : &nevr::lifecycle::InternStableCStr,
                                           bridge});
}

GotStatus InstallLibR15RedirectWith(sentinel::ImageLookup lookup) {
  Installation& s = State();
  const std::lock_guard<std::mutex> lock(s.mutex);
  if (s.redirector == nullptr) return GotStatus::kNotInstalled;
  return InstallSlotLocked(Slot::kLibR15, s.libr15Hook, s.options.targets.libr15, lookup, &s.report.libr15,
                           "libr15_tstring");
}

GotStatus InstallLibR15Redirect() { return InstallLibR15RedirectWith(sentinel::FindLoadedImage); }

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
  SetApply(nullptr);
  g_redirector.store(nullptr, std::memory_order_release);
  s.redirector = nullptr;
  s.report = InstallReport{};
}

ServiceRedirector* InstalledRedirectorForTest() { return g_redirector.load(std::memory_order_acquire); }

void ArmHandlersForTest(ServiceRedirector* redirector) {
  g_redirector.store(redirector, std::memory_order_release);
  SetApply(redirector != nullptr ? &ApplyActive : nullptr);
  ArmThunk(Slot::kLibR15, redirector != nullptr);
  ArmThunk(Slot::kMatchmaking, redirector != nullptr);
}

}  // namespace nevr_quest::redirect
