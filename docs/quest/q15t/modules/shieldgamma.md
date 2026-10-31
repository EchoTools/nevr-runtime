# Module `shieldgamma` (row R58)

Files: `src/shieldgamma.{h,cpp}`, `src/quest/plugins/q15t/src/pbhud_log.h`, `src/quest/tests/q15t/shieldgamma_test.cpp`, `src/quest/tests/q15t/pbhud_image.h`,
`src/quest/tests/q15t/mod/shieldgamma.sh` (+ `src/quest/tests/q15t/mod/pbhud_ndk.sh`), `src/quest/tests/q15t/mutants.d/shieldgamma.sh`.

Option macro for the main seat to wire: `Q15T_SHIELDGAMMA`, a bitmask, default **2** (mirror ON, gamma OFF), the source's defaults:
`g_gamma = Switch(... "shieldgamma.on" ...)  // TEST, off by default` and `g_mirror = !Switch(... "tubemirror.off" ...) // ship
form, on by default` (`<source build>/the source build's shieldgamma:243-244`). Bit 1 = gamma, bit 2 = tube mirror.
API: `InstallResult InstallShieldGamma(std::uintptr_t base, bool gamma, bool mirror)`; both false installs nothing (reason `off`, one
INFO line). No tick.

## Row covered

R58: `NRadEngine::CSetMaterialPropertyNode::Update(SSetMaterialPropertyNodeData const&)`, JUMP_SLOT ELF 0x36d2020, real function ELF
0x272d55c = disassembly 0x282d55c (size 760). `R1_functions.md:71`: `| R58 | D7 CSetMaterialPropertyNode::Update -> 0x282d55c (760) | node Update | JUMP_SLOT 0x36d2020 | 1 | n/a | GOT |`.
Source constants `kGot = 0x36d2020, kFn = 0x272d55c` (the source build's shieldgamma:57). disassembly entry words at 0x282d55c, read this
session: `fc0f1cf8 f75b01a9 f55302a9 f37b03a9`, equal to the first four of the source's 12 pinned words (`:68`: `{0x272d55c,
0xf81c0ffc}, {0x272d560, 0xa9015bf7}, ...`); all 12 match disassembly. `readelf`: `JUMP_SLOT 0x36d2020 -> 0x272d55c`
(`R0_readelf_r.txt`). The brief's "chains on cvbind": it only accepts cvbind's wrapper as the next hop; see hudmeter.md (not needed).

## What it does

Both parts act only on a vec4 write (data +0x30 == 1) with colourspace 0 (data +0x4c == 0) to material `dbde1b05fa8f6200` (the
spawn-tube shield / dyson and gauss barrier material, data +0x20).
The PC pixel shader decodes every material colour with `pow(saturate(x), 2.2)`; the Quest decodes only colourspace 1, with the sRGB
curve, so the team colour comes out yellow-orange instead of red-orange (the source build's shieldgamma:1-12).

- **gamma** (source TEST switch): for that one call rgb := pow(sat(rgb), 2.2) in the node-data buffer, alpha untouched, the real
  Update runs, the original rgb is put back. Update reads the value at data +0x38.. itself (`ldp s0, s1, [x19,#0x38]`, source `:15-19`).
- **tube mirror** (source: ON by default): the launch-tube window scene binds material M = `b28e940eb21a5798`, not `dbde1b05...`,
  so the script's team-colour writes never reach the windows (they draw a fixed blue). For a write of the ring colour (property
  `dd35e419672ee0ba`) to `dbde1b05...`, the wrapper first calls Update on a 0x58-byte copy of the node data with material := M and
  rgb decoded as above, then makes the original call; the original call's result is the one returned. Update only resolves the
  actor's model, calls `CModelCS::SetMaterialProperty` and reads the node's start time (ELF 0x272d55c..0x272d7e0, source `:33-35`),
  so the extra call is the write alone. The decoded rgb is used for the mirror regardless of the gamma switch (as in the source);
  the original call's own rgb is decoded only with gamma on.

Mechanism: `InstallImportHook(symbol, base, 0x36d2020, &Wrapper, &g_next, 0x272d55c)`, a data-page swap; the slot must hold
`base + 0x272d55c` or the install is refused (ERROR, slot untouched). Wrapper rules: one `SafeRead` of 0x58 bytes (unreadable: the
real function with the same arguments, counted); every non-matching call passes untouched with no log; the node's buffer is only
written for the matching call and restored after it; the flags are stored before the swap and never changed after it.

Logging: INFO `shieldgamma: on` (slot, fn, status, gamma, tube_mirror, material), ERROR on a refusal or `restore_failed`;
`shieldgamma: decode` and `shieldgamma: tube mirror write` for the 1st, 2nd, 4th, 8th ... call (before / after rgb, property);
no per-call lines (the node runs every script update while a duration runs: the test asserts it). Real records from the host test
(`PBHUD_LOG=1`):
```
plugin=q15t shieldgamma: on slot_elf_vaddr=0x36d2020 fn_elf_vaddr=0x272d55c status=ok reason= gamma=1 tube_mirror=0 material=0xdbde1b05fa8f6200
plugin=q15t shieldgamma: decode n=1 prop=0xd1b26f18aaaaaaaa r=0.941 g=0.466 b=0.054 to_r=0.874777 to_g=0.186401 to_b=0.00162655
plugin=q15t shieldgamma: tube mirror write n=1 prop=0xdd35e419672ee0ba r=0.941 g=0.466 b=0.054 to_r=0.874777 to_g=0.186401 to_b=0.00162655
```
(The orange 0.941, 0.466, 0.054 becomes 0.875, 0.186, 0.0016: red-orange.)

## Prediction (written before any headset run)

With `Q15T_SHIELDGAMMA=2` (default): `shieldgamma: on ... gamma=0 tube_mirror=1` at start; in a launch tube, on the first team-colour
write of the ring colour one `shieldgamma: tube mirror write n=1`, and the tube windows show the team colour (red-orange / blue)
instead of a fixed blue in both rooms. The pack must carry the matching tubefork row (the source's pack row a7x, window colour at
CB +164: this module only supplies the write). With `Q15T_SHIELDGAMMA=3`: additionally `shieldgamma: decode` lines and the
barrier panes / spawn shields lose the yellow cast (red-orange, as on PC). No per-call log growth either way.

## Falsifiers

- `shieldgamma: OFF; nothing installed ... status=unexpected_original`: slot word not the pinned function.
- Installed, but no `tube mirror write` line ever, in a tube with the pack row present: the ring colour is not written through this node
  / property, or the material differs on this build (the first decode line on a device is the proof that the writes go through this node, source `:25-26`).
- Windows still fixed blue with `tube mirror write` lines present: the pack's tubefork row is missing (the write lands on M but
  nothing reads it), or M's property id differs.
- Colour too dark / too saturated after `decode` lines: the decode exponent / colourspace assumption is wrong for the materials the
  panes use (the gamma switch is a TEST switch in the source for this reason).
- A crash or a visible flicker back to the stock colour: the buffer was restored too early (the wrapper restores after the real call returns).

## Needs a word patch / not built

- **Needs a word patch:** none (data-page GOT swap). The tube mirror's *pack* row (the source's a source-build script, an asset change) is
  not a code word and not part of this module.
- Not built: the source's code-word pin check (12 words) and md5 identity check; replaced by the exact slot-word check plus the
  contract's build id gate.
- Not built: cvbind (not needed: see hudmeter.md).
- Not built: the source's on-files / setprop switches (`shieldgamma.on`, `tubemirror.off`); the main seat maps `Q15T_SHIELDGAMMA`.
- Unverified: the data offsets (+0x20 material, +0x28 property, +0x30 type, +0x38 value, +0x4c colourspace) are the source's
  reading of Update's body (the source build's shieldgamma:15-19); I verified the entry words and sizes, not Update's body instruction by
  instruction. The materials / property ids are source constants.
