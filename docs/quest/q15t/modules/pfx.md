# Module `pfx` (issue #1631): the particle-effect renderer's gate

Files: `src/quest/plugins/q15t/src/pfx.{h,cpp}`, `src/quest/tests/q15t/pfx_test.cpp`, `src/quest/tests/q15t/mod/pfx.sh`.
Option: `Q15T_PFX` (default ON). Off switch: `pfx.off` in the q15t data directory (module_switch.h).

## What the switch decides

- The source build's `On()` (the particle-effect renderer's gate, read by every `pfx*` switch) is off unless a switch file
  `pfx.on` exists in the data directory, or the debug property reads `1`. Its per-switch reads are in the same function.
- The working build has the renderer on. This gate is therefore ON by default, and `pfx.off` turns it off.
- The gate installs no hook and writes no byte. `InitPfxGate` (called from `NvrPluginInitEx` after the pinned-build check)
  stores the decision and logs one line: `[NEVR.PLUGIN] plugin=q15t pfx gate module=pfx status=on|off reason=default|off_file switch=pfx.off`.
- `PfxGateOn()` reads the stored decision. It reads off until `InitPfxGate` has run. A future `pfx*` module reads it and does
  nothing while it is off.

## What is not ported

The gate is the only part of the switch this module reconstructs. Not in the tree yet, and each is needed before the
Quest draws the particle effects the working build draws:

- The renderer object: the source build creates one `CGParticleEffectRenderer`, publishes it in the engine's global, and
  drives its Initialize, OnSimulation, UploadConstants, UpdateFinished and Render calls, which the Quest build never calls.
- The compute passes (Update, two Emission passes, UpdateBillboards) with their per-resource state, and the billboard draw
  (the render-pool loop, its pipelines and descriptor sets).
- The CreateEffect wrapper on the engine's JUMP_SLOT, the effect-id tier switches (`pfxt*`), and the warm-up passes (`pfxwarm`,
  `pfxmeshwarm`). Each `pfx*` row of the parity table is its own issue (#1632 to #1671).
- The shader and pack assets the renderer loads from the content pack.

Until those land, turning the gate on changes nothing in the game. The gate's log line is the only observable effect.

## Not shown (unverified)

- That the renderer's gate is the only thing the working build reads for `pfx`. The working build's own switch files were not read.
- That the pinned build's engine global the renderer publishes is free on the Quest. No address is pinned here.

## Headset test (3 lines)

1. Install the build with the plugin packaged and start the game on the pinned build.
2. `adb logcat | grep "pfx gate"`: expect one line with `status=on reason=default`.
3. Put an empty `pfx.off` in `/sdcard/Android/data/<pkg>/files/q15t/`, relaunch, expect `status=off reason=off_file`.
