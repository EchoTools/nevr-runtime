#include "quest/integration/libr15_attach_step.h"

#include <atomic>
#include <cstring>

#include "got_hook.h"
#include "libr15_identity.h"
#include "quest/plugin/plugin_platform.h"

namespace nevr_quest::integration {
namespace {

std::atomic<bool> g_identityRegistered{false};

void RegisterIdentityOnce() {
  if (g_identityRegistered.exchange(true, std::memory_order_acq_rel)) return;
  plugin::SetLibr15PatchSource(&PatchStateForPlugins);
  sentinel::SetBuildIdAlias(&BuildIdAliasForPlugins);
}

}  // namespace

libr15_patch::AttachOptions BuildAttachOptions() {
  libr15_patch::AttachOptions options;
  options.extraRows = sentinel::pinned::LibR15ExtraRows();
  options.extraRowsSha256 = sentinel::pinned::LibR15ExtraRowsSha256();
  return options;
}

plugin::Libr15PatchState PatchStateForPlugins() {
  plugin::Libr15PatchState state;
  const char* const id = libr15_patch::VerifiedBuildId();
  state.verified = libr15_patch::Verified() && id != nullptr;
  if (state.verified) state.verifiedBuildId = id;
  return state;
}

const char* BuildIdAliasForPlugins(const char* liveNote) {
  const plugin::PluginView view =
      plugin::ResolvePluginView(liveNote, libr15_patch::Verified(), libr15_patch::VerifiedBuildId());
  return view.patched ? view.buildId : nullptr;
}

const char* AttachDetail(const libr15_patch::AttachResult& result) {
  using libr15_patch::AttachStatus;
  switch (result.status) {
    case AttachStatus::kVerified: return "verified";
    case AttachStatus::kAlreadyVerified: return "already_verified";
    case AttachStatus::kNotPatched: return libr15_patch::kReasonNotPatched;
    case AttachStatus::kPinnedServiceOnly: return libr15_patch::kReasonPinnedServiceOnly;
    case AttachStatus::kBuildIdMismatch: return libr15_patch::kReasonBuildIdMismatch;
    case AttachStatus::kRowFailed:
    case AttachStatus::kTrampolineFailed: {
      // Attach has ended the process already; the token is for the log of a seam that returned.
      for (const char* known : {libr15_patch::kReasonRowStub, libr15_patch::kReasonRowSlot, libr15_patch::kReasonRowSegment,
                                libr15_patch::kReasonRowWords, libr15_patch::kReasonTrampolineFailed}) {
        if (result.reason == known) return known;
      }
      return libr15_patch::AttachStatusName(result.status);
    }
    case AttachStatus::kModuleNotLoaded:
    case AttachStatus::kNoBuildId: break;
  }
  return libr15_patch::AttachStatusName(result.status);
}

AttachStepOutcome RunAttachStep(const libr15_patch::AttachOptions& options) {
  const libr15_patch::AttachResult result = libr15_patch::Attach(options);
  AttachStepOutcome outcome;
  outcome.detail = AttachDetail(result);
  outcome.verified =
      result.status == libr15_patch::AttachStatus::kVerified || result.status == libr15_patch::AttachStatus::kAlreadyVerified;
  if (outcome.verified) RegisterIdentityOnce();
  return outcome;
}

void ResetAttachStepForTest() noexcept { g_identityRegistered.store(false, std::memory_order_release); }

}  // namespace nevr_quest::integration
