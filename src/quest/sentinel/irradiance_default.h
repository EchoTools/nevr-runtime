/* The install of the grey default irradiance texture (#2091), for the constructor sequence. The design, the hooks and the log lines
 * are in irradiance_default_hook.h; this header holds only what a translation unit built with exceptions may include (it names no
 * callback_thunk.h).
 */
#pragma once

#include "got_hook.h"

namespace nevr_quest::irradiance_default {

// Activates the row `irradiance_default.upload`. True when the row is live. Nothing is read from libr15 before Attach has verified
// it; a refusal logs its status and leaves the row pass-through.
bool InstallUploadRow() noexcept;

// Installs both hooks: the GOT hook on `initialize` first (a refusal leaves the helper untouched), then the row. True when both
// are live. `lookup` finds the loaded libr15; a host test passes its own.
bool InstallAt(const sentinel::GotTarget& initialize, sentinel::ImageLookup lookup = sentinel::FindLoadedImage) noexcept;

// The production install: InstallAt on libr15's own slot (pinned_targets.h LibR15CGEngineResourcesInitialize).
bool Install() noexcept;

}  // namespace nevr_quest::irradiance_default
