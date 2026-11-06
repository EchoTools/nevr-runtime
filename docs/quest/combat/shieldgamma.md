# Module `shieldgamma` (row R58): the shield colour and the tube-window mirror

Files: `src/quest/combat/shieldgamma.{h,cpp}`, `src/quest/combat/shieldgamma_targets.h` (the pins), `src/quest/combat/combat_scope.{h,cpp}` (the level answer the handler
reads), `src/quest/tests/shieldgamma_test.cpp`, `src/quest/tests/got_pinned_test.cpp`, `src/quest/combat/mutants/shieldgamma.sh`. The module is a normal combat step,
registered after `hudmeter` (`src/quest/combat/combat_install.cpp`): it runs in the `q15t` and `combat` APKs and in no other. Switches in `config.yaml`:
`combat.shieldgamma_tube_mirror` (default true) and `combat.shieldgamma_gamma` (default false, a test switch in the reference build). Both false installs nothing and logs
`combat_shieldgamma_install` with `reason: off_config`. No file in the app's files directory turns either part on or off.

Origins: our implementation of the reference build's `runtime/native/afkxr/afk_shieldgamma.cpp` (the decode and the material :1-12, the layout :15-19, the slot and function
:57, the two switches' defaults :243-244; pinned in `docs/audits/2026-11-05-q15t-origin-citations.md`, rows 129-132). The module was imported into the q15t plugin from
kotorvr's echoquest-combat work (#1591) and moved here.

## Row covered

R58: `NRadEngine::CSetMaterialPropertyNode::Update(SSetMaterialPropertyNodeData const&)`, JUMP_SLOT ELF 0x36d2020, real function ELF 0x272d55c (disassembly 0x282d55c, size 760).
`readelf -rW` of the store libr15 (sha256 `8dd9a961...`):
`00000000036d2020  00009e8b00000402 R_AARCH64_JUMP_SLOT    000000000272d55c _ZN10NRadEngine24CSetMaterialPropertyNode6UpdateERKNS_28SSetMaterialPropertyNodeDataE + 0`.
The function is libr15's own, so the target names it as the slot's expected original (`base + 0x272d55c`). The handler is a `CallbackThunk` installed through `InstallThunk`;
nothing is written to code.

The data layout is read from Update's body, five instructions pinned in `shieldgamma_targets.h` and checked against the real file by `got_pinned_test`:
`ldr w22, [x19, #0x4c]` (0x272d7e4, the colourspace), `ldp x2, x3, [x19, #0x20]` (0x272d7f4, the material and the property), `ldp s0, s1, [x19, #0x38]` and
`ldp s2, s3, [x19, #0x40]` (0x272d7f8, 0x272d7fc, the colour), and Update's first instruction (0x272d55c). The value type at +0x30 is the reference build's reading; no word pins it.

## What it does

Both parts act only on a vec4 write (data +0x30 == 1) in colourspace 0 (data +0x4c == 0) to material `dbde1b05fa8f6200` (the spawn-shield material, which the dyson and
gauss barrier panes use too; data +0x20). The PC pixel shader decodes every material colour with `pow(saturate(x), 2.2)`; the Quest decodes only colourspace 1, so the team colour
comes out yellow-orange instead of red-orange.

- **gamma** (`combat.shieldgamma_gamma`): for that one call rgb := pow(sat(rgb), 2.2) in the node-data buffer, alpha untouched, the real Update runs, the original rgb is put back.
- **tube mirror** (`combat.shieldgamma_tube_mirror`): the launch-tube window scene binds material `b28e940eb21a5798`, not `dbde1b05...`, so the script's team-colour writes never
  reach the windows (they draw a fixed blue). For a write of the ring colour (property `dd35e419672ee0ba`) to `dbde1b05...`, the handler first calls Update on a 0x58-byte copy of
  the node data with the material set to the window's and rgb decoded as above, then makes the original call; the original call's result is the one returned. Update only
  resolves the actor's model, calls `CModelCS::SetMaterialProperty` and reads the node's start time (0x272d55c..0x272d7e0), so the extra call is the write alone. The decoded rgb is
  used for the mirror whatever the gamma switch says; the original call's own rgb is decoded only with gamma on. The tube windows' pack row (an asset change) is not this module's.

## Scope: a combat level, and nothing else

Update is a generic script node, called every update while a duration runs, and arena's scripts call it too, on the same APK. The handler tests `scope::SeenCombatOrLobby()` first
(`combat_scope.h`: a combat map or the combat lobby was loaded at the last look and no Echo Arena level was, the look made four times a second by the game-thread tick). Outside a combat level it calls the
game's function once with the same arguments and does nothing else: no read of the data, no write, no second call, no counter, no line. `shieldgamma_test` pins that with both
switches on, the ring colour write that is mirrored in combat, in each arena level, the lobbies arena players use, a combat-to-arena transition and before the first look.

The combat body is a separate `noinline` function, `ActInCombat`, that first asks `scope::LookIsFresh()`: when the tick has stopped for more than a second, a true answer is not
believed and the call passes through (pinned by `TheModuleFollowsTheLevelBothWays`). The plugin copy this replaces read 0x58 bytes with `process_vm_readv` on every call in every
level. In an arena match the module now costs the call through the thunk and, in the handler, eight instructions with no frame, no stack canary and no call (NDK r26d clang -O2,
`llvm-objdump -d`): `adrp x8; mov x3, x0; ldr w8, [x8]; tbnz w8, #2; and w8, w8, #3; cbz w8; mov x0, x1; mov x1, x2; br x3`; in combat it costs what the plugin cost plus a clock read. How often Update runs per frame is not measured (no headset run); the arena cost per call is measured on the host (see the pull request
that moved the module).

## Lines (sentinel log)

`combat_shieldgamma_install` (Info installed / Info off / Error refused, with the reason, both switches and the material), `combat_shieldgamma_decode` and `combat_shieldgamma_mirror` for the
1st, 2nd, 4th, 8th ... call (before and after rgb, the property). No per-call lines: `shieldgamma_test` asserts 2000 foreign calls log nothing. `combat_scope_change` says when a combat level
was entered or left.

## Prediction (written before any headset run)

On a combat APK with the defaults: `combat_shieldgamma_install` `installed` with `gamma: 0, tube_mirror: 1` at start; in a launch tube, on the first team-colour write of the ring colour one
`combat_shieldgamma_mirror` `n: 1`, and the tube windows show the team colour (red-orange or blue) instead of a fixed blue in both rooms, if the pack carries the matching tubefork row.
With `combat.shieldgamma_gamma: true`: additionally `combat_shieldgamma_decode` lines, and the barrier panes and spawn shields lose the yellow cast. In an arena match on the same APK: no
`combat_shieldgamma_*` line other than the install line and no change in any colour.

## Falsifiers

- `combat_shieldgamma_install` `refused` with `OriginalMismatch`: the slot held another word. The module is inert.
- Installed, but no `combat_shieldgamma_mirror` line ever, in a tube with the pack row present: either `combat_scope_change` never said `to: 1` (the level answer did not see the combat
  level), or the ring colour is not written through this node and property, or the material differs on this build.
- Windows still fixed blue with `mirror` lines present: the pack's tubefork row is missing (the write lands on the window material but nothing reads it), or the material's property id differs.
- Colour too dark or too saturated after `decode` lines: the decode exponent or colourspace assumption is wrong for the materials the panes use (why the gamma switch is off by default).
- A crash or a visible flicker back to the stock colour: the buffer was restored before the game's call returned.

## Not built, not verified

- Not built: the reference build's `cvbind` (a log-only probe on the same slot) and its on-files and setprop switches.
- Not verified: Update's body instruction by instruction (the layout words above and the entry are pinned; the value type and the material and property ids are the reference build's constants),
  and the node's calls per frame.
