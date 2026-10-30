// pfxshotpos (q15t, the on-list switch of that name): a log-only probe of where the bullet effects (muzzle, trail, impact nodes)
// are placed against the shot. The decisions are in pfxshotpos_core.h; this module is the install step.
#pragma once

namespace q15t {

// Installs the probe, or logs why it is not installed. Returns true only when a probe hook is in place. On by default; the kill
// file `pfxshotpos.off` in the plugin's data directory turns it off (module_switch.h).
bool InstallPfxShotPos();

}  // namespace q15t
