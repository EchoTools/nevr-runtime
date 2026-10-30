// pfxshotpos install step. The probe reads the particle renderer's nodes at three points: the node create, the sim frame after
// the engine's compose, and the ribbon mirror. The renderer's hook surface is the particle-effect renderer (issue #1631, the
// `pfx` switch), which this build does not carry yet, so there is nothing to hook: the install logs that and returns false.
// Once #1631 lands, the hooks go here and the decisions in pfxshotpos_core.h feed their log lines.
#include "pfxshotpos.h"

#include "module_switch.h"
#include "nevr/plugin_interface.h"
#include "plugin_log.h"

namespace q15t {

bool InstallPfxShotPos() {
  if (ModuleSwitchPresent("pfxshotpos.off")) {
    Log(NEVR_LOG_INFO, "pfxshotpos: off reason=kill_file file=pfxshotpos.off");
    return false;
  }
  Log(NEVR_LOG_WARNING,
      "pfxshotpos: not installed reason=pfx_renderer_absent: the particle renderer's hook points are not in this build, so the probe has no node to read (issue 1631)");
  return false;
}

}  // namespace q15t
