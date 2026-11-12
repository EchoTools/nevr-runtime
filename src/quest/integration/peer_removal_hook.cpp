#include "quest/integration/peer_removal_hook.h"

#include <cstdint>
#include <cstring>

#include "quest/integration/peer_removal_targets.h"
#include "quest/sentinel/callback_thunk.h"
#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/hook_log.h"
#include "quest/sentinel/libr15_identity.h"
#include "quest/sentinel/pinned_targets.h"
#include "runtime/compat/peer_timeout_log.h"

namespace nevr_quest::integration::peer_removal {

namespace {

struct DestroyPeerTag {};
using DestroyThunk = sentinel::CallbackThunk<DestroyPeerTag, DestroyPeerSig>;

// The handler the thunk calls in place of the game's DestroyPeer: log, then the game's function with the same arguments.
void DestroyPeerHandler(DestroyThunk::Fn original, void* self, std::uint64_t index, const char* reason) noexcept {
  LogRemoval(index, reason);
  original(self, index, reason);
}

NEVR_HOOK_RECORD(kDestroyRecord, DestroyThunk, &DestroyPeerHandler);

sentinel::GotHook g_hook;

}  // namespace

void LogRemoval(std::uint64_t index, const char* reason) noexcept {
  const char* text = reason != nullptr ? reason : "";
  const bool recognized = nevr_peer_timeout_log::RecognizedRemovalReason(text);
  const bool warning = nevr_peer_timeout_log::LevelForRemovalReason(text) == nevr_peer_timeout_log::RemovalLevel::kWarning;
  sentinel::LogFields(warning ? sentinel::LogLevel::kWarn : sentinel::LogLevel::kInfo, "peer_removed",
                      {{"reason", text}, {"slot", index}, {"reason_recognized", recognized ? "yes" : "no"},
                       {"consequence", recognized ? "the game removed the peer for this reason"
                                                  : "the game removed the peer for a reason this log does not name"}});
}

sentinel::GotStatus InstallPeerRemovalHookAt(sentinel::ImageLookup lookup) noexcept {
  if (lookup == nullptr) return sentinel::GotStatus::kModuleNotLoaded;
  sentinel::ElfImage image;
  if (!lookup(sentinel::pinned::kLibR15, &image)) return sentinel::GotStatus::kModuleNotLoaded;
  char id[64] = {};
  if (!sentinel::ReadBuildId(image, id, sizeof(id)) ||
      std::strcmp(id, sentinel::pinned::LibR15PatchedBuildId()) != 0) {
    return sentinel::GotStatus::kBuildIdMismatch;
  }
  // The pinned check: the slot must hold DestroyPeer's body (kDestroyPeerFnVa) when the hook goes in (got_hook.cpp: kOriginalMismatch).
  sentinel::GotTarget target(sentinel::pinned::kLibR15, kDestroyPeerSymbol, sentinel::RelocKind::kJumpSlot,
                             sentinel::pinned::LibR15PatchedBuildId(), kDestroyPeerSlotVa);
  target.expectedOriginal = reinterpret_cast<const void*>(image.base + kDestroyPeerFnVa);
  target = target.Owned("integration/peer_removal");
  DestroyThunk::Arm(kDestroyRecord);
  const sentinel::GotStatus status = sentinel::InstallThunk<DestroyThunk>(g_hook, target, lookup);
  const bool installed = status == sentinel::GotStatus::kOk;
  sentinel::LogFields(installed ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "peer_removal_install",
                      {{"result", installed ? "installed" : "refused"}, {"hook", sentinel::GotStatusName(status)},
                       {"consequence", installed ? "each peer removal is logged with its reason, then the game's function runs unchanged"
                                                 : "peer removals are not logged; the game's calls are unchanged"}});
  return status;
}

sentinel::GotStatus InstallPeerRemovalHook() noexcept { return InstallPeerRemovalHookAt(&sentinel::FindLoadedImage); }

bool Installed() noexcept { return g_hook.installed(); }

void ResetForTest() noexcept {
  g_hook.Forget();
  DestroyThunk::Disarm();
}

}  // namespace nevr_quest::integration::peer_removal
