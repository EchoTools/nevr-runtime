# Resolution and foveation on the stock VrApi path

Scope: the establish step for the Quest combat fold's rows 034 to 039 (issue #1737): how the game sizes the eye buffer, whether Echo's own
auto-resolution can run on the stock VrApi path without OpenXR, and which foveation level the game sets. It adds no code, hook, switch or
pinned address. The owner's decision on the options (D3) is open; this file costs them and does not choose.

Sources: ReVault project `echovr`, binary `libr15.so` (1,041 binaries in the project; the Quest game library is the one read here).
Upstream `kotorvr/echoquest-combat` at `c5a67e8`, cited by its file headers and not reproduced.

Status of each claim: **measured** = read from the disassembly or decompiler in ReVault (the instruction bytes are cited where they decide the
answer); **folded** = the decompiler's raw tier shows a literal where the code loads a constant (see "Constants"); **inferred** = derived, with
the reasoning given; **not established** = not checked here.

## Answer

1. **The eye buffer does not depend on OpenXR.** `CVR::RefreshRTSize` (ReVault VA `0x1b709a0`) takes the per-eye size from
   `vrapi_GetSystemPropertyInt` ids 5 and 6 (`0x1b709f4`, `0x1b70a04`), each multiplied by `CGRenderConfig::CustomViewportScale(0)`. The
   only other input is an override pair set by `CVR::SetRTSizeOverride`. The two OpenXR names searched, `xrCreateInstance` and
   `xrEnumerateViewConfigurationViews`, give 0 hits in all 1,041 binaries (`revault_search_strings`, project scope).
2. **Echo's auto-resolution is off on the stock build.** It is a complete path in `libr15.so` (setters, a frame-budget controller, a flag).
   The flag `DAT_0389c1f4` gates both the scale it uses (`CGRenderConfig::ViewportSize` reads `gAutoResScale` only when the flag is nonzero)
   and the controller. Its two direct call sites pass 0: `CR15Game::Render` passes a literal 0, and `ApplyRuntimeGfxSettings` passes a
   constant that the decompiler folds to 0 (see "Auto-resolution"). No file under `src/quest`, `docs/quest` or `docs/adr` mentions
   `AutoResolution` (`grep -rni AutoResolution`).
3. **Foveation.** The game sets the level through `CVR::SetFoveationLevel` (VA `0x1b71294`), which calls `vrapi_SetPropertyInt(java, 0xf, n)`.
   Writers and values are in "Foveation". In a match, unpaused, the level is 4 (a folded constant; ADR 0022 measured 4 on unpause); it is
   0 while paused and once the match state is left. Dynamic foveation is set to 1 by `ApplyRuntimeGfxSettings`.
4. **Headset lines: not run.** This session has no headset. The `render_config` lines the combat fold table asks for (Quest 2 and Quest 3, combat
   match) are owed. ADR 0022 records lines from an earlier session (`eye_source=runtime_suggested`, `foveation_dynamic=1`, foveation 0 on
   pause and 4 on unpause); they are cited here and were not re-measured.

## Addresses and the store build

- VA is the ReVault address (the disassembly). ELF is VA minus `0x100000`, the convention of `src/quest/sentinel/pinned_targets.h`.
- Cross-checks that the ReVault binary has the store layout: the import `vrapi_GetSystemPropertyInt` is at VA `0x37fa728` (ELF `0x36fa728`,
  pinned in `pinned_targets.h`); `vrapi_SetPropertyInt` at VA `0x37ffcf0` (ELF `0x36ffcf0`, ADR 0022). Upstream's ELF `0x20e8f8c`
  (its `afk_drr` header) is the VA `0x21e8f8c` where this file's enable constant is loaded, which is consistent with the upstream numbering.
- Not established: the identity of the file. ReVault's `libr15.so` has sha256 `8dd9a961b9dca8566069a4f65b3ddee9c65682c4e9c91a6d41e3c5727b1d8b20`.
  The store file is not available in this checkout (`NEVR_STORE_APK` is unset), so its constants could not be read from the file.

## The eye buffer

| Fact | Where | Status |
| --- | --- | --- |
| Per-eye size = `vrapi_GetSystemPropertyInt(5)`, `(6)` times `CustomViewportScale(0)`, unless the override pair is nonzero | `CVR::RefreshRTSize` VA `0x1b709a0` | measured (decompile and the two call sites) |
| Called from `CVR::Resize` (first statement), `ApplyRuntimeGfxSettings` (`0x21e8fc4`, when `CVR::Initialized()`), `SetRTSizeOverride` and four more sites | xrefs to `RefreshRTSize_plt` VA `0x1008c50` | measured (xrefs) |
| The eye swap chain is created once per process from that size, logging `Creating OVR swap chain with size %i x %i` | `CVR::Resize`; string VA `0x327abe1` | measured (string); `CVR::Resize` as the creator is from the annotation |
| Meaning of ids 5 and 6 (eye texture width and height) | `src/quest/sentinel/texture_quality.h` header; no VrApi header in the tree | not established against the VrApi header |
| `CustomViewportScale(0)` is `kR15Quest1ResolutionScale` or `kR15Quest2ResolutionScale`, chosen by the headset type (`QuestHeadsetType` ORed with 1 equals 3) | `ApplyRuntimeGfxSettings` decompile | measured (selection); value of the constants not read from the file |

The sentinel already has a lever on this path. `src/quest/sentinel/texture_quality.h` documents that a texture-quality level other than
`stock` answers ids 5 and 6 with its own size (ADR 0015). The `render_config` line (ADR 0022) logs `eye_source` and `eye_w`/`eye_h` from
the GOT hooks in `src/quest/sentinel/render_config_hook.cpp`.

## Auto-resolution

Setters, in `CGRenderConfig`, with their VAs (`revault_search_code`, project scope): `SetEnableAutoResolution` `0x198d2b4`
(stores the argument at `DAT_0389c1f4`, the instruction `str w0,[x8,#0x1f4]` at `0x198d2b8`); `SetAutoResolutionFrameRate` `0x198d350`;
`SetAutoResolutionHeadRoom` `0x198d370`; `SetAutoResolutionMinScale` `0x198d2f0` (clamped to [0.01, 1]); `SetAutoResolutionMaxScale`
`0x198d320`; `SetAutoResolutionChangeSpeed` `0x198d3a0`; `SetAutoResolutionQuantization` `0x198d3d0`; `SetAutoResolutionScale` `0x198d2c0`.

**The controller.** `CGRenderer::UpdateAutoResolution(float)` (VA `0x198e0d0`, raw tier) computes a target from the frame rate and headroom,
`(1 - headroom) / frame_rate`; it moves the scale by `changeSpeed * scale * (target - measured) / target`; it clamps to the maximum scale and
quantizes when the quantization is nonzero; when the flag is zero it sets the scale to the default at `DAT_0389c1f0`. The raw decompile does
not show a lower clamp in this function; `CGRenderer::Begin` (VA `0x198f638`) has the same update. Its callers are data references only (VA
`0x3385304`, `0x34f05a0`), so its caller is reached through a table or vtable, not resolved here.

**The gate.** `CGRenderConfig::ViewportSize` (VA `0x192db14`, raw): `if (DAT_0389c1f4 != 0)` uses `gAutoResScale`; otherwise a different float
(the default at `DAT_0389c1f0`). `SetFromFSEffectSettings` (`0x194374c`) has the same flag test. `BlitToStereoRenderTargets` (`0x1986c60`)
multiplies by `gAutoResScale` in the part shown, with no flag test there; the flag's effect on that function is not established. With the
flag at 0, `UpdateAutoResolution` keeps the scale at the default, so the two agree in that case.

**The writers of the flag.** `revault_xrefs` for the thunk `0x100d880` (the call target) lists two calls: `0x12fc158` in `CR15Game::Render`
and `0x21e8fa0` in `ApplyRuntimeGfxSettings`. The thunk is `adrp x16; ldr x17,[x16,#0x328]; add x16,...; br x17`: it jumps through the data
slot at VA `0x37de328` (ELF `0x36de328`), which holds the function's address.

- `CR15Game::Render` (`0x12fc028`): `0x12fc154` is `mov w0,wzr` and `0x12fc158` is the call, so the argument is 0. The call sits in the branch
  taken when bit 0 of the byte at `CR15Game+0x7adc` is set (the raw decompile tests bit 32 of the 64-bit word at `+0x7ad8`; the `tbz` at
  `0x12fc150` skips it when the bit is clear). What that bit means is not established.
- `ApplyRuntimeGfxSettings` (`0x21e8e0c`): the disassembly, read directly, is `adrp x8,0x381a000; ldr x8,[x8,#0x7d8]; ldr x8,[x8]; cmp x8,#0;
  cset w0,ne; bl 0x100d880` (`0x21e8f8c` to `0x21e8fa0`). The argument is therefore `(kR15QuestEnableAutoRes != 0)`, where the constant is read
  through a slot. The raw decompile shows the literal `CGRenderConfig::SetEnableAutoResolution(0)`: the decompiler folded the constant to 0. The
  reconstructed tiers of the same function show the literal too. Upstream's `afk_drr` header states the same value (`= 0`).
- Two data words hold the function's address at VA `0x338500c` and `0x34efc90`. Nothing references those words in the database, so their
  users are not established. A call through either with an argument of 1 is not excluded here.

So in the shipped code the only calls that reach the flag pass 0 (`Render`) or the constant's value (`ApplyRuntimeGfxSettings`, inferred 0),
and no code path in the database passes 1 that this file has found.

**What upstream says it does (not reproduced).** `kotorvr/echoquest-combat` at `c5a67e8`, its fxlane afk_drr source and the
afk_drr header, describes a test-only step in which the runtime calls `SetEnableAutoResolution(1)` every frame on a combat level, with
`SetAutoResolutionFrameRate` and `SetAutoResolutionMinScale`, and says the game's `CR15Game::Render` resets the flag. It also says the
OpenXR extension is not needed for Echo's controller, which uses its own GPU timer, and that enabling the OpenXR extension adds about
0.7 ms of CPU per frame. Those are upstream's measurements on their own device runs. This file did not reproduce them, and the reset in `Render` is the
bit described above.

## Constants

The exported `const` data named in `libr15.so`'s string table includes `kR15QuestEnableAutoRes` (string VA `0x476354`),
`kR15QuestEnableDynamicFoveation` (`0x47e561`), `kR15QuestFoveationLevel` (`0x4776f9`), and the resolution and MSAA constants. The code reads
each through a slot (an `adrp` and `ldr` chain), and the raw decompile folds some of those reads to literals. A fold means the decompiler took
the image's initial value. The name-to-slot mapping is measured only where a source names it:

| Use | Read in the code | Raw decompile | Other evidence | Status |
| --- | --- | --- | --- | --- |
| Enable argument of `SetEnableAutoResolution`, in `ApplyRuntimeGfxSettings` | `0x21e8f8c` to `0x21e8fa0`, the slot named `kR15QuestEnableAutoRes` by upstream | `SetEnableAutoResolution(0)` | upstream: `= 0` | folded and upstream agree; file value not read |
| Argument of `SetEnableDynamicFoveation`, in `ApplyRuntimeGfxSettings` | before the call at `0x21e8fb8`; slot name by inference (`kR15QuestEnableDynamicFoveation`) | `SetEnableDynamicFoveation(1)` | ADR 0022: `foveation_dynamic=1` on the headset | folded; agrees with the headset |
| Level argument at unpause (`0x12fc55c`) and at state 9 entry (`0x135bd7c`) | slot name by inference (`kR15QuestFoveationLevel`) | `SetFoveationLevel(4)` in both | ADR 0022: 4 on unpause | folded; agrees with the headset |
| Level argument at `OnUnpausedCB` (`0x136b310`) and its proxy (`0x13e44e0`) | slot not decoded | not shown | none | value not established here |

Reading the values from the store file (the slots' relocations in `.rela.dyn`, the values in `.data.rel.ro` or `.rodata`) is what would make
them measured. That needs `NEVR_STORE_APK`, which is unset in this checkout.

## Foveation

Every writer of `CVR::SetFoveationLevel` in `libr15.so` (`revault_xrefs` on the thunk VA `0xff6cd0`, seven call sites):

| Function (VA) | Site | Argument | Status |
| --- | --- | --- | --- |
| `CR15Game::PausedNotification` (`0x12fc3f0`) | `0x12fc454` | 0 (`mov w0,wzr`) | measured |
| `CR15Game::UnpausedNotification` (`0x12fc4f8`) | `0x12fc55c` | a constant, folded to 4 | folded; ADR 0022 measured 4 |
| `CR15NetGame::SwitchTo` (`0x135b8b4`), old state 9 | `0x135b95c` | 0 (`cmp w8,#9; b.eq` to `mov w0,wzr`) | measured; raw: `if (*param_1 == 9) SetFoveationLevel(0)` |
| `CR15NetGame::SwitchTo`, new state 9 (`case 9`) | `0x135bd7c` | a constant, folded to 4 | folded; raw: `case 9: SetFoveationLevel(4); BeginMultiplayerGameplay()` |
| `CR15NetGame::OnPausedCB` (`0x136b0ec`) | `0x136b108` | 0 | measured |
| `CR15NetGame::OnUnpausedCB` (`0x136b300`) | `0x136b310` | a constant read through a slot (slot not decoded) | value not established here |
| `Proxy<CR15NetGame, OnUnpausedCB>` (`0x13e44d0`) | `0x13e44e0` | a constant read through a slot (slot not decoded) | value not established here |

Correction to ADR 0022's phrase "Callers set 0 on pause and on net-game state 9": the 0 is written on leaving state 9 (the old state), and
entering state 9 writes 4 (`case 9`). The ADR's table row is read against the raw decompile here.

**Combat.** The state path here does not name a combat state. State 9 is the state in which `SwitchTo` calls
`CR15Game::BeginMultiplayerGameplay`, so a match is state 9 (inferred). Its name in `GameStateString` is not established. So in a match, unpaused, the level is 4 (folded); the level is 0
while paused and once the match ends (on leaving state 9). What 0 to 4 mean to VrApi is not in the tree and is not established.

**Dynamic foveation.** `CVR::SetEnableDynamicFoveation` (VA `0x1b72edc`) has one direct call, `0x21e8fb8` in `ApplyRuntimeGfxSettings`, with
argument `(kR15QuestEnableDynamicFoveation != 0)`, folded to 1 (see the constants table). `ApplyRuntimeGfxSettings` runs at boot
(`CR15Game::InitEngine`) and from the settings translator (decompile comment).

**Upstream's foveation (not reproduced).** The afk_ffr source in the same upstream commit hooks EchoFrame's property read, a runtime that is
not on the stock path, and sets the rings to medium (Quest 3) or high (Quest 2). Its header reports GPU-bound combat at medium. That is
upstream's claim and it describes a different runtime.

## What the sentinel already does on this path

- `render_config` (ADR 0022): reads the foveation, dynamic foveation, eye and refresh values from the GOT hooks; `render_config_hook.cpp`
  records property 0xf and 0x1e (the constants `kFoveationLevelProperty`, `kDynamicFoveationProperty`).
- `texture_quality` (ADR 0015): the eye-size lever above.
- `refresh_rate.cpp` (row 040): lowers the rate on combat maps.
- No file under `src/quest`, `docs/quest` or `docs/adr` mentions `AutoResolution` (`grep -rni`).

## Options for decision D3 (costed, not chosen)

Each option names what it would need. An establish step is required before any implementation (the rule for new addresses in the combat fold table).

**A. Dynamic resolution on Echo's own setters, on combat maps only.** Set the flag to 1 and the controller's inputs (the frame rate, the
headroom, the minimum and maximum scale, the change speed) through the setters above.
- Hook shape: replace the data slot at ELF `0x36de328` (the one the thunk jumps through), with the sentinel's checked data-slot operation
  (no code write, N151). The replacement intercepts the call from `ApplyRuntimeGfxSettings` and `Render`; calls that reach the function
  through the two data words above would not be intercepted. It has to handle `Render`'s reset, whose condition is the bit above, which is
  not established.
- Establish first: the slot's section and its relocation (from the store file); the flag word at ELF `0x379c1f4` (VA `0x389c1f4`) and its
  section (if it is `.bss`, the rule for a word with no file value applies: pin that its address lies in `.bss` and pin the writing instruction
  at ELF `0x188d2b8`); the three constants read from the file; the meaning of the bit at `CR15Game+0x7adc`; the VrApi header for ids 5 and 6.
- Scope: `combat_scope` so arena pays nothing (combat README rule 9).
- Logs and tests: a new `render_config` field for the flag value (read, not written); one install record; mutants; a pinned test against the
  store build (`just test-quest-hooks-pinned` with `NEVR_STORE_APK`).
- Headset rows owed: eye size and GPU level under a combat match, with the flag on and off.
- Upstream's notes describe the controller working on their test device. That is not evidence here.

**B. Keep the headset runtime's defaults.** The eye size is the VrApi suggestion, the texture level stays `stock` (ADR 0022 records
`texture_quality=stock` on the default config), the game sets foveation as in "Foveation" (4 in a match, 0 while paused), and dynamic
foveation stays 1.
- Cost: no code, no pins. Rows 035 to 039 of the combat table in issue #1737 stay WON'T PORT as built, and row 040 (refresh rate) is unchanged.
- Already logged: `render_config` shows every value this option depends on.

**C. Foveation only (row 034).** Rewrite the level argument at the existing `vrapi_SetPropertyInt` GOT hook (ELF `0x36ffcf0`, already hooked for
`render_config`; the same slot is named by `src/quest/diag/hwdump_install.cpp`).
- Cost: a value change at a live hook, no new address. It overrides both the 0 and the 4 the game writes, so it needs a chosen level, and the
  meaning of the VrApi levels is not established in the tree.
- Headset rows owed: GPU level and visible quality per level.

B and C are independent of A. A and the texture-quality lever (row 043, issue #1737) both change the eye size through the same `RefreshRTSize`
inputs, so they cannot be decided one at a time.

## Not established

- The store file's values for the three constants, and the file's identity (`NEVR_STORE_APK` unset here).
- The runtime value of the flag word and whether any run ever sets it.
- The meaning of bit 0 of the byte at `CR15Game+0x7adc` (the `Render` reset).
- The name of state 9 in `GameStateString`.
- The VrApi meaning of ids 5 and 6 and of foveation levels 0 to 4.
- The default at `DAT_0389c1f0` and the lower clamp of the scale.
- Who calls through the two data words at VA `0x338500c` and `0x34efc90`.
- Every headset value. None was measured in this file.

## ReVault annotations that disagree with the binary

- `DAT_0389c1f4` is annotated "ELF VA 0x389c1f4". The value is the VA; its ELF address is `0x379c1f4`.
- The reconstructed bodies of `ApplyRuntimeGfxSettings` show `SetEnableAutoResolution(0)` and `SetEnableDynamicFoveation(1)` as literals. The
  code reads both through slots; the literals are the decompiler's fold of the image's initial values, as above.

Refs #1737
