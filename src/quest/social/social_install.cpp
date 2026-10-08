#include "quest/social/social_install.h"

#include <atomic>
#include <cstring>

#include "hook_log.h"
#include "pinned_targets.h"
#include "quest/social/social_abi.h"
#include "quest/social/social_facade.h"

namespace quest_social {
namespace {

using sentinel::LogFields;
using sentinel::LogLevel;

// Each distinct outcome is logged once; Social() runs once, but the log must stay bounded if it ever
// runs more.
std::atomic<std::uint32_t> g_outcomesLogged{0};

void LogOutcomeOnce(std::uint32_t bit, LogLevel level, const char* result) {
  const std::uint32_t before = g_outcomesLogged.fetch_or(bit, std::memory_order_relaxed);
  if ((before & bit) == 0) LogFields(level, "social_select", {{"result", result}});
}

std::atomic<PnsovrLookup> g_lookup{&FindPnsovr};

constexpr std::uint32_t kLogNull = 1U << 0;
constexpr std::uint32_t kLogNotLoaded = 1U << 1;
constexpr std::uint32_t kLogBuild = 1U << 2;
constexpr std::uint32_t kLogForeign = 1U << 3;
constexpr std::uint32_t kLogFacade = 1U << 4;

void* OnSocial(SocialThunk::Fn original, std::uint64_t handle) {
  void* const result = original(handle);
  // The facade is built before the hook is armed; Instance() here only returns it.
  return SelectSocialObject(result, Facade::Instance().Object(), g_lookup.load(std::memory_order_acquire));
}

}  // namespace

const char* InstallStatusName(InstallStatus status) {
  switch (status) {
    case InstallStatus::kOk: return "ok";
    case InstallStatus::kDisabled: return "disabled";
    case InstallStatus::kHookFailed: return "hook_failed";
  }
  return "unknown";
}

sentinel::GotTarget LibR15Social() {
  return {sentinel::pinned::kLibR15, kSocialSymbol, sentinel::RelocKind::kJumpSlot,
          sentinel::pinned::kLibR15BuildId, kSocialSlotVaddr};
}

PnsovrView FindPnsovr() noexcept {
  PnsovrView view;
  sentinel::ElfImage image;
  if (!sentinel::FindLoadedImage(kLibPnsovr, &image)) return view;
  view.found = true;
  view.loadBias = image.base;
  char id[64] = {};
  view.buildIdMatches = sentinel::ReadBuildId(image, id, sizeof(id)) && std::strcmp(id, kLibPnsovrBuildId) == 0;
  return view;
}

PnsovrLookup SetPnsovrLookup(PnsovrLookup lookup) {
  return g_lookup.exchange(lookup != nullptr ? lookup : &FindPnsovr, std::memory_order_acq_rel);
}

SocialThunk::Handler SocialHandler() { return &OnSocial; }

void* SelectSocialObject(void* original, void* facadeObject, PnsovrLookup lookup) noexcept {
  if (original == nullptr) {
    LogOutcomeOnce(kLogNull, LogLevel::kWarn, "provider_returned_null_pass_through");
    return original;
  }
  if (facadeObject == nullptr || lookup == nullptr) return original;
  const PnsovrView pnsovr = lookup();
  if (!pnsovr.found) {
    LogOutcomeOnce(kLogNotLoaded, LogLevel::kWarn, "pnsovr_not_loaded_pass_through");
    return original;
  }
  if (!pnsovr.buildIdMatches) {
    LogOutcomeOnce(kLogBuild, LogLevel::kError, "pnsovr_build_mismatch_pass_through");
    return original;
  }
  std::uintptr_t vptr = 0;
  std::memcpy(&vptr, original, sizeof(vptr));
  if (vptr != pnsovr.loadBias + static_cast<std::uintptr_t>(kOvrSocialVptrVaddr)) {
    LogOutcomeOnce(kLogForeign, LogLevel::kError, "not_a_cnsovrsocial_pass_through");
    return original;
  }
  LogOutcomeOnce(kLogFacade, LogLevel::kInfo, "facade_selected");
  return facadeObject;
}

InstallResult InstallSocialHook(bool enabled) {
  InstallResult result;
  if (!enabled) {
    LogFields(LogLevel::kInfo, "social_install", {{"status", "disabled"}});
    return result;
  }
  static sentinel::GotHook hook;
  Facade::Instance();  // allocate and wire the models before any game thread can reach the handler
  SocialThunk::Arm(&OnSocial);
  result.got = hook.Install(LibR15Social(), SocialThunk::EntryAddress(), SocialThunk::OriginalOut());
  if (result.got != sentinel::GotStatus::kOk) {
    SocialThunk::Arm(nullptr);
    result.status = InstallStatus::kHookFailed;
    LogFields(LogLevel::kError, "social_install",
              {{"status", "hook_failed"}, {"got", sentinel::GotStatusName(result.got)}});
    return result;
  }
  result.status = InstallStatus::kOk;
  LogFields(LogLevel::kInfo, "social_install", {{"status", "ok"}});
  return result;
}

}  // namespace quest_social
