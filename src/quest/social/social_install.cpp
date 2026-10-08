// Built -fno-exceptions (callback_thunk.h refuses otherwise). The handler path below (OnSocialHandler,
// SelectSocialObject, FindPnsovr and the loader helpers in got_hook.cpp) is reachable from the thunk
// entry, so the frame sensor (tests/quest TestHookFramesCarryNoPersonality) requires it to be
// personality-free, and it never logs: it only counts.
#include "quest/social/social_install.h"

#include <atomic>
#include <cstring>

#include "hook_install.h"
#include "hook_log.h"
#include "hook_report.h"
#include "pinned_targets.h"
#include "quest/social/social_abi.h"
#include "quest/social/social_facade.h"
#include "quest/social/social_invite_gate.h"
#include "runtime/compat/social_names.h"

namespace quest_social {
namespace {

using sentinel::LogFields;
using sentinel::LogLevel;

std::atomic<PnsovrLookup> g_lookup{&FindPnsovr};

// The facade's object, published before the callback is armed. The handler reads this and never
// constructs anything.
std::atomic<void*> g_facadeObject{nullptr};

// What the handler counted (constant-initialised: no static initialiser).
std::atomic<std::uint64_t> g_selected{0};
std::atomic<std::uint64_t> g_nullResult{0};
std::atomic<std::uint64_t> g_pnsovrUnavailable{0};
std::atomic<std::uint64_t> g_foreignObject{0};

void Count(std::atomic<std::uint64_t>& counter) noexcept { counter.fetch_add(1, std::memory_order_relaxed); }

}  // namespace

// Runs on the game's thread, once. The frame has no landing pad, so a game exception thrown by the
// original passes through it untouched, and nothing here can throw into the game.
void* OnSocialHandler(SocialThunk::Fn original, std::uint64_t handle) noexcept {
  void* const result = original(handle);
  return SelectSocialObject(result, g_facadeObject.load(std::memory_order_acquire),
                            g_lookup.load(std::memory_order_acquire));
}

NEVR_HOOK_RECORD(kSocialHook, SocialThunk, &OnSocialHandler);

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

GameJson ResolveGameJson(sentinel::ImageLookup lookup) noexcept {
  GameJson json;
  sentinel::ElfImage image;
  if (lookup == nullptr || !lookup(sentinel::pinned::kLibR15, &image)) return json;
  char id[64] = {};
  if (!sentinel::ReadBuildId(image, id, sizeof(id)) || std::strcmp(id, sentinel::pinned::kLibR15BuildId) != 0) return json;
  const auto at = [&image](std::uint64_t vaddr) { return image.base + static_cast<std::uintptr_t>(vaddr); };
  const std::uintptr_t reset = at(kLibR15CJsonResetVaddr);
  const std::uintptr_t decode = at(kLibR15CJsonDecodeFromVaddr);
  const std::uintptr_t encode = at(kLibR15CJsonEncodeToCompactVaddr);
  static_assert(sizeof(json.reset) == sizeof(reset), "function pointer size");
  std::memcpy(&json.reset, &reset, sizeof(json.reset));
  std::memcpy(&json.decode, &decode, sizeof(json.decode));
  std::memcpy(&json.encode, &encode, sizeof(json.encode));
  return json;
}

PnsovrLookup SetPnsovrLookup(PnsovrLookup lookup) {
  return g_lookup.exchange(lookup != nullptr ? lookup : &FindPnsovr, std::memory_order_acq_rel);
}

void PublishFacadeObject() { g_facadeObject.store(Facade::Instance().Object(), std::memory_order_release); }

void* SelectSocialObject(void* original, void* facadeObject, PnsovrLookup lookup) noexcept {
  if (original == nullptr) {
    Count(g_nullResult);
    return original;
  }
  if (facadeObject == nullptr || lookup == nullptr) return original;
  const PnsovrView pnsovr = lookup();
  if (!pnsovr.found || !pnsovr.buildIdMatches) {
    Count(g_pnsovrUnavailable);
    return original;
  }
  std::uintptr_t vptr = 0;
  std::memcpy(&vptr, original, sizeof(vptr));
  if (vptr != pnsovr.loadBias + static_cast<std::uintptr_t>(kOvrSocialVptrVaddr)) {
    Count(g_foreignObject);
    return original;
  }
  Count(g_selected);
  return facadeObject;
}

SocialCounters Counters() noexcept {
  return SocialCounters{g_selected, g_nullResult, g_pnsovrUnavailable, g_foreignObject};
}

void ResetCountersForTest() noexcept {
  g_selected.store(0, std::memory_order_relaxed);
  g_nullResult.store(0, std::memory_order_relaxed);
  g_pnsovrUnavailable.store(0, std::memory_order_relaxed);
  g_foreignObject.store(0, std::memory_order_relaxed);
}

bool RegisterSocialReportCounters() {
  bool ok = true;
  ok = sentinel::RegisterReportCounter("social_calls", &SocialThunk::CallCounter()) && ok;
  ok = sentinel::RegisterReportCounter("social_facade_selected", &g_selected) && ok;
  ok = sentinel::RegisterReportCounter("social_null_result", &g_nullResult, sentinel::ReportKind::kFaults) && ok;
  ok = sentinel::RegisterReportCounter("social_pnsovr_unavailable", &g_pnsovrUnavailable,
                                       sentinel::ReportKind::kFaults) && ok;
  ok = sentinel::RegisterReportCounter("social_foreign_object", &g_foreignObject, sentinel::ReportKind::kFaults) && ok;
  ok = sentinel::RegisterReportCounter("social_thunk_faults", &SocialThunk::FaultCounter(),
                                       sentinel::ReportKind::kFaults) && ok;
  const FacadeCounters facade = FacadeCountersView();
  ok = sentinel::RegisterReportCounter("social_members_hidden", &facade.membersHidden,
                                       sentinel::ReportKind::kFaults) && ok;
  ok = sentinel::RegisterReportCounter("social_events_dropped", &facade.eventsDropped,
                                       sentinel::ReportKind::kFaults) && ok;
  ok = sentinel::RegisterReportCounter("social_send_failed", &facade.sendFailed, sentinel::ReportKind::kFaults) && ok;
  ok = sentinel::RegisterReportCounter("social_join_deferred", &facade.joinDeferred) && ok;
  ok = sentinel::RegisterReportCounter("social_request_timeout", &facade.requestTimeout,
                                       sentinel::ReportKind::kFaults) && ok;
  // Callback deliveries by class: a headset run shows PartyCreatedCB, MemberJoined, JoinFailed and the rest.
  ok = sentinel::RegisterReportCounter("social_cb_created", &facade.cbCreated) && ok;
  ok = sentinel::RegisterReportCounter("social_cb_member_joined", &facade.cbMemberJoined) && ok;
  ok = sentinel::RegisterReportCounter("social_cb_join_failed", &facade.cbJoinFailed) && ok;
  ok = sentinel::RegisterReportCounter("social_cb_other", &facade.cbOther) && ok;
  ok = sentinel::RegisterReportCounter("social_json_failed", &facade.jsonFailed, sentinel::ReportKind::kFaults) && ok;
  ok = sentinel::RegisterReportCounter("social_frames_ignored", &facade.framesIgnored, sentinel::ReportKind::kFaults) && ok;
  ok = RegisterInviteGateCounters() && ok;
  return ok;
}

InstallResult InstallSocialHook(bool enabled) {
  InstallResult result;
  if (!enabled) {
    LogFields(LogLevel::kInfo, "social_install", {{"status", "disabled"}});
    return result;
  }
  static sentinel::GotHook hook;
  // The friend-name decoder (zstd) is registered by this explicit call: the PC registers it from a static
  // initializer, which the sentinel may not carry. Without it no profile is requested and rows show ids.
  SocialNames::RegisterDefaultDecoder();
  // Allocate and wire the models before any game thread can reach the handler.
  PublishFacadeObject();
  const GameJson gameJson = ResolveGameJson(&sentinel::FindLoadedImage);
  SetGameJson(gameJson);
  LogFields(gameJson.reset != nullptr ? LogLevel::kInfo : LogLevel::kWarn, "social_install",
            {{"game_json", gameJson.reset != nullptr ? "resolved" : "unavailable"}});
  SocialThunk::Arm(kSocialHook);
  result.got = sentinel::InstallThunk<SocialThunk>(hook, LibR15Social());
  if (result.got != sentinel::GotStatus::kOk) {
    SocialThunk::Disarm();
    result.status = InstallStatus::kHookFailed;
    LogFields(LogLevel::kError, "social_install",
              {{"status", "hook_failed"}, {"got", sentinel::GotStatusName(result.got)}});
    return result;
  }
  result.status = InstallStatus::kOk;
  LogFields(LogLevel::kInfo, "social_install", {{"status", "ok"}});
  // The facade is live: let the invite "+" reach it (the gate logs its own outcome; the facade works without it).
  InstallInviteGate();
  return result;
}

}  // namespace quest_social
