// shieldgamma (row R58): the spawn-tube windows (and the dyson / gauss barrier panes) show the team colour as on the PC.
// Port of the source build's shieldgamma (RE-34 TEXTURE-2 G7 cause 2; the TUBE MIRROR of ART 10-24 S-02). A GOT wrapper on
// CSetMaterialPropertyNode::Update's JUMP_SLOT (ELF 0x36d2020) acting on a vec4 SetMaterialProperty, colourspace 0:
//   gamma:  to material dbde1b05fa8f6200 (the shield / barrier material): rgb := pow(sat(rgb), 2.2) for that one call;
//   mirror: the ring colour (property dd35e419672ee0ba) written to dbde1b05 is also written to the tubefork window material
//           b28e940eb21a5798, rgb decoded the same way, by a second call of Update on a copy of the node data.
#pragma once

#include <cstdint>

#include "import_hook.h"

namespace q15t {

// `gamma`: the source's "shieldgamma.on" (a TEST switch, off in the source). `mirror`: the source's tube mirror (ON in the
// source; "tubemirror.off" turns it off). Both false: nothing installed. Logs one structured line.
InstallResult InstallShieldGamma(std::uintptr_t base, bool gamma, bool mirror);

struct ShieldGammaStats {
  std::uint32_t decoded, mirrored, unreadable;
};
ShieldGammaStats GetShieldGammaStats();
void ResetShieldGammaForTest();

}  // namespace q15t
