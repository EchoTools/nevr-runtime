#include <cstdint>

#include "hook_install.h"
#include "hook_log.h"
#include "irradiance_default.h"
#include "irradiance_default_hook.h"
#include "libr15_attach.h"
#include "pinned_targets.h"

namespace nevr_quest::irradiance_default {
namespace {

// The image upload helper has no symbol and the engine calls it directly: it is the row `irradiance_default.upload` of the
// build-time patch table (quest/patch/libr15_patch_table.h; docs/adr/0021), whose address and stock words the patcher checks
// against the store file. The hook is one atomic store into the row's slot.
constexpr const char* kUploadRow = "irradiance_default.upload";

}  // namespace

bool InstallUploadRow() noexcept {
  namespace attach = nevr_quest::libr15_patch;
  char targetText[19];
  // Nothing is read from libr15 before Attach has verified it (the live ID is then the patched file's).
  if (!attach::Verified()) {
    sentinel::LogFields(sentinel::LogLevel::kError, "irradiance_default",
                        {{"status", attach::ActivateStatusName(attach::ActivateStatus::kNotVerified)},
                         {"hook", "image upload helper"},
                         {"row", kUploadRow},
                         {"target", sentinel::HexString(targetText, 0)},
                         {"consequence", "the default 3D texture keeps the game's black texel"}});
    return false;
  }
  ArmUploadHandler();
  const attach::ActivateStatus st = sentinel::InstallRowThunk<UploadThunk>(kUploadRow);
  const bool ok = st == attach::ActivateStatus::kOk;
  sentinel::LogFields(ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "irradiance_default",
                      {{"status", ok ? "installed" : attach::ActivateStatusName(st)},
                       {"hook", "image upload helper"},
                       {"row", kUploadRow},
                       {"target", sentinel::HexString(targetText, attach::RowSiteAddress(kUploadRow))}});
  return ok;
}

bool InstallAt(const sentinel::GotTarget& initialize, sentinel::ImageLookup lookup) noexcept {
  // The GOT hook first: with it refused nothing marks the Initialize call, so the row would never substitute, and the helper is
  // left alone.
  const sentinel::GotStatus status = InstallInitializeHook(initialize, lookup);
  const bool hooked = status == sentinel::GotStatus::kOk;
  sentinel::LogFields(hooked ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "irradiance_default",
                      {{"status", hooked ? "installed" : sentinel::GotStatusName(status)},
                       {"hook", "CGEngineResources::Initialize"},
                       {"symbol", initialize.symbol != nullptr ? initialize.symbol : "(none)"}});
  if (!hooked) return false;
  return InstallUploadRow();
}

bool Install() noexcept { return InstallAt(sentinel::pinned::LibR15CGEngineResourcesInitialize()); }

}  // namespace nevr_quest::irradiance_default
