// Built with -fno-exceptions (callback_thunk.h, reached through pinned_targets.h, requires it).
#include "quest/plugin/input_seam_hook.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "hook_install.h"
#include "hook_log.h"
#include "pinned_targets.h"
#include "quest/plugin/script_redirect.h"

namespace nevr_quest::input_seam {

namespace {

using InputStateThunk = sentinel::pinned::LibR15GetCurrentInputStateThunk;

// The one registration. Written once, before it is published through g_published; never changed after.
struct Registration {
  NevrQuestInputFilterFn filter = nullptr;
  void* user = nullptr;
  CapsFn caps = nullptr;
  char owner[64] = {};
};

Registration g_registration;
std::atomic<const Registration*> g_published{nullptr};
// 0: free; 1: a registration is being decided; 2: registered.
std::atomic<int> g_claim{0};
HandCache g_hands;
sentinel::GotHook g_hook;

std::uint32_t HandOf(CapsFn caps, void* ovr, std::uint32_t deviceId) noexcept {
  std::uint32_t hand = NEVR_QUEST_HAND_UNKNOWN;
  if (g_hands.Find(deviceId, &hand)) return hand;
  VrapiTrackedRemoteCaps c{};
  c.type = kControllerTypeTrackedRemote;
  c.deviceId = deviceId;
  if (caps(ovr, &c) != 0) return NEVR_QUEST_HAND_UNKNOWN;  // not cached: asked again at the next poll
  hand = HandFromCaps(c.controllerCapabilities);
  g_hands.Store(deviceId, hand);
  return hand;
}

// vrapi_GetCurrentInputState. Never logs, allocates or throws: it is live across the call into the runtime and the
// capabilities call (callback_thunk.h rule 1).
int Handler(InputStateThunk::Fn original, sentinel::pinned::OvrMobileOpaque* ovr, std::uint32_t deviceId,
            void* inputState) noexcept {
  const int rc = original(ovr, deviceId, inputState);
  const Registration* reg = g_published.load(std::memory_order_acquire);
  if (rc != 0 || reg == nullptr || inputState == nullptr) return rc;
  auto* vr = static_cast<VrapiTrackedRemoteState*>(inputState);
  if (vr->header.controllerType != kControllerTypeTrackedRemote) return rc;
  const NevrQuestControllerState before = ReadState(*vr, HandOf(reg->caps, ovr, deviceId));
  NevrQuestControllerState after = before;
  reg->filter(reg->user, &after);
  WriteChanged(before, after, vr);
  return rc;
}

NEVR_HOOK_RECORD(kHook, InputStateThunk, &Handler);

void LogResult(std::int32_t result, const char* pluginId, const char* ownedBy, const sentinel::GotStatus* status) noexcept {
  const char* const plugin = pluginId != nullptr ? pluginId : "(null)";
  const char* const outcome = result == NEVR_QUEST_FILTER_OK ? "installed" : "refused";
  const sentinel::LogLevel level = result == NEVR_QUEST_FILTER_OK ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError;
  const char* const slot = sentinel::pinned::kGetCurrentInputStateSymbol;
  if (status != nullptr) {
    sentinel::LogFields(level, "input_seam",
                        {{"result", outcome}, {"reason", ResultName(result)}, {"plugin", plugin}, {"slot", slot},
                         {"status", sentinel::GotStatusName(*status)}});
  } else if (ownedBy != nullptr) {
    sentinel::LogFields(level, "input_seam",
                        {{"result", outcome}, {"reason", ResultName(result)}, {"plugin", plugin},
                         {"owned_by", ownedBy}, {"slot", slot}});
  } else {
    sentinel::LogFields(level, "input_seam",
                        {{"result", outcome}, {"reason", ResultName(result)}, {"plugin", plugin}, {"slot", slot}});
  }
}

// Not a NevrQuestFilterResult: the token of a registration refused because the capabilities slot did not resolve. It is
// reported to the plugin as HOOK_REFUSED.
constexpr std::int32_t kCapsUnresolved = -1;

sentinel::GotStatus InstallPinned() noexcept {
  return sentinel::InstallThunk<InputStateThunk>(g_hook, sentinel::pinned::LibR15GetCurrentInputState());
}

// The function libr15's vrapi_GetInputDeviceCapabilities slot holds: the build ID must be the patched one (ADR 0021) and
// the pinned slot must resolve. nullptr otherwise.
CapsFn ResolvePinnedCaps() noexcept {
  sentinel::ElfImage image;
  if (!sentinel::FindLoadedImage(sentinel::pinned::kLibR15, &image)) return nullptr;
  char id[80] = {};
  if (!sentinel::ReadBuildId(image, id, sizeof(id)) || std::strcmp(id, sentinel::pinned::LibR15PatchedBuildId()) != 0) {
    return nullptr;
  }
  const sentinel::SlotResolution slot =
      sentinel::ResolveSlot(image, sentinel::pinned::LibR15GetInputDeviceCapabilities(), sentinel::kNativeRelocs);
  if (slot.status != sentinel::GotStatus::kOk || slot.slot == nullptr) return nullptr;
  return reinterpret_cast<CapsFn>(__atomic_load_n(slot.slot, __ATOMIC_ACQUIRE));
}

std::int32_t RegisterDev(const char* pluginId, NevrQuestInputFilterFn filter, void* user) {
  Environment env;
  env.devBuild = true;
  env.install = &InstallPinned;
  env.resolveCaps = &ResolvePinnedCaps;
  return RegisterWith(env, pluginId, filter, user);
}

std::int32_t RegisterRelease(const char* pluginId, NevrQuestInputFilterFn filter, void* user) {
  Environment env;
  env.devBuild = false;
  return RegisterWith(env, pluginId, filter, user);
}

// The host API is one struct, and this file owns its initializers; the script redirect (script_redirect.h) is the
// other member, offered in both builds: it registers a combat unit's library names, not an input filter.
std::int32_t RegisterScriptRedirect(const char* pluginId, const std::uint64_t* hashes, std::uint32_t count) {
  return static_cast<std::int32_t>(script_redirect::Register(pluginId, hashes, count));
}

const NevrQuestHostApi kDevApi{sizeof(NevrQuestHostApi), &RegisterDev, &RegisterScriptRedirect};
const NevrQuestHostApi kReleaseApi{sizeof(NevrQuestHostApi), &RegisterRelease, &RegisterScriptRedirect};

}  // namespace

const NevrQuestHostApi* HostApi(bool devBuild) noexcept { return devBuild ? &kDevApi : &kReleaseApi; }

const char* ResultName(std::int32_t result) noexcept {
  switch (result) {
    case NEVR_QUEST_FILTER_OK: return "ok";
    case NEVR_QUEST_FILTER_BAD_ARGUMENT: return "bad_argument";
    case NEVR_QUEST_FILTER_NOT_DEV_BUILD: return "not_dev_build";
    case NEVR_QUEST_FILTER_OWNED: return "owned";
    case NEVR_QUEST_FILTER_HOOK_REFUSED: return "hook_refused";
    case kCapsUnresolved: return "caps_unresolved";
    default: return "unknown";
  }
}

std::int32_t RegisterWith(const Environment& env, const char* pluginId, NevrQuestInputFilterFn filter,
                          void* user) noexcept {
  if (pluginId == nullptr || pluginId[0] == '\0' || filter == nullptr) {
    LogResult(NEVR_QUEST_FILTER_BAD_ARGUMENT, pluginId, nullptr, nullptr);
    return NEVR_QUEST_FILTER_BAD_ARGUMENT;
  }
  if (!env.devBuild) {
    LogResult(NEVR_QUEST_FILTER_NOT_DEV_BUILD, pluginId, nullptr, nullptr);
    return NEVR_QUEST_FILTER_NOT_DEV_BUILD;
  }
  int expected = 0;
  if (!g_claim.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
    char ownedBy[80];
    std::snprintf(ownedBy, sizeof(ownedBy), "plugin:%s", expected == 2 ? g_registration.owner : "(registering)");
    LogResult(NEVR_QUEST_FILTER_OWNED, pluginId, ownedBy, nullptr);
    return NEVR_QUEST_FILTER_OWNED;
  }
  const CapsFn caps = env.resolveCaps();
  if (caps == nullptr) {
    g_claim.store(0, std::memory_order_release);
    LogResult(kCapsUnresolved, pluginId, nullptr, nullptr);
    return NEVR_QUEST_FILTER_HOOK_REFUSED;
  }
  g_registration.filter = filter;
  g_registration.user = user;
  g_registration.caps = caps;
  std::snprintf(g_registration.owner, sizeof(g_registration.owner), "%s", pluginId);
  // Published before the slot changes, so the first call through the hook already sees the filter.
  g_published.store(&g_registration, std::memory_order_release);
  InputStateThunk::Arm(kHook);
  const sentinel::GotStatus status = env.install();
  if (status != sentinel::GotStatus::kOk) {
    // Nothing reaches the handler through an untouched slot; through a poisoned one it finds nothing published and passes
    // the call through.
    g_published.store(nullptr, std::memory_order_release);
    g_claim.store(0, std::memory_order_release);
    LogResult(NEVR_QUEST_FILTER_HOOK_REFUSED, pluginId, nullptr, &status);
    return NEVR_QUEST_FILTER_HOOK_REFUSED;
  }
  g_claim.store(2, std::memory_order_release);
  LogResult(NEVR_QUEST_FILTER_OK, pluginId, nullptr, &status);
  return NEVR_QUEST_FILTER_OK;
}

void ResetForTest() noexcept {
  g_published.store(nullptr, std::memory_order_release);
  g_registration = Registration{};
  g_hands.Clear();
  g_claim.store(0, std::memory_order_release);
}

}  // namespace nevr_quest::input_seam
