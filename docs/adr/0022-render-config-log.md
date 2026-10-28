# ADR 0022: the nevr log records the render configuration

Status: proposed (#1139). Written before any code; the "Prediction" section is what the first runs must show.

## Context

A report of stutter or low frame rate arrives with a log that does not say what the game was rendering, so the player's setup cannot be reproduced from the log alone. On a Quest 2 the Meta OVR Metrics CSV showed 120 Hz, 1440x1584 eye buffers, render scale 101 and foveation level 0; nothing in the nevr log said so, and nobody knew which game setting picked those values.

Wanted: one `render_config` line at the first rendered frame and again on every change, on both platforms, built only from data the game or its runtime already holds (an import the game calls, an exported function, a game global at a pinned address). No Quest code page is written (ADR 0021).

## What the Quest game does (pinned libr15, build `b243509c08ce677aeb95fa348016949b3fc45230`)

Addresses are pinned link-time addresses (ReVault shows each 0x100000 higher; `docs/adr/0006`).

| Fact | Where the game holds it | Evidence (ReVault, libr15.so) |
| --- | --- | --- |
| Refresh rate the game asks for | `vrapi_SetDisplayRefreshRate(ovr, rate)`, JUMP_SLOT `0x36ec5f0`, one call site in `CVR::UpdateTracking` (`0x1b71de8` in ReVault), after the game picks the highest supported rate not above 90 from `vrapi_GetSystemPropertyFloatArray` id `0x41`; logs `Device type '%s' is setting refresh rate to %2.1f` on success, `vrapi_SetDisplayRefreshRate failed: %i - current refresh rate is %2.2f` otherwise | `UpdateTracking @ 0x1b71b60` decompilation, strings at `0x327aae7`..`0x327ab9b` |
| Refresh rate the runtime reports | `vrapi_GetSystemPropertyFloat(java, 4)`, JUMP_SLOT `0x36c06f8`; the game's own failure log prints it as "current refresh rate" | same function; ADR 0006 (id 4, per frame) |
| CPU and GPU level | `vrapi_SetClockLevels(ovr, cpu, gpu)`, JUMP_SLOT `0x36c75f0`: `(2,2)` when `CGRenderConfig::StablePowerState()` is 0, `(5,5)` otherwise, once per VR-mode entry | `UpdateTracking`: `uVar16 = 2` / `5` before the call at `0x1b71cc8` |
| Headset type | `vrapi_GetSystemPropertyInt(java, 0)`, JUMP_SLOT `0x36fa728`; the game labels `0x140` "Quest 2" and anything else "quest" | `UpdateTracking`, `pcVar4 = "Quest 2"` |
| Foveation level | `CVR::SetFoveationLevel(n)` stores `n` at `0x37a3398` and calls `vrapi_SetPropertyInt(java, 0xf, n)` (JUMP_SLOT `0x36ffcf0`). Callers set 0 on pause and on net-game state 9, 4 on unpause | `SetFoveationLevel @ 0x1b71294`; search "Foveation" over libr15 |
| Dynamic foveation | `CVR::SetEnableDynamicFoveation(b)` stores `b` at `0x37a339c` and calls `vrapi_SetPropertyInt(java, 0x1e, b != 0)`; `ApplyRuntimeGfxSettings` always passes 1 | `SetEnableDynamicFoveation @ 0x1b72edc`, `ApplyRuntimeGfxSettings @ 0x21e8e0c` |
| Eye buffer size | `CVR::RefreshRTSize` stores the per-eye pair `{int32 w, int32 h}` at `0x37a33e0` = `vrapi_GetSystemPropertyInt` ids 5 and 6 times `CustomViewportScale`, or the override pair at `0x37a33e8` when it is non-zero; `CVR::Resize` creates the swap chain from it once | `RefreshRTSize @ 0x1b709a0` (reconstructed); ADR 0015 |
| Render scale | `CustomViewportScale(0)` = `kR15Quest1ResolutionScale` / `kR15Quest2ResolutionScale`, both measured 1.0 in the store build, chosen by `CVR::QuestHeadsetType()` (`0x37a3374`) | `ApplyRuntimeGfxSettings` |
| MSAA | `CGRenderConfig::MSAAMode()` is the global at `0x379c080`; `ApplyRuntimeGfxSettings` sets mode 1 (2 samples) from `kR15Quest*MSAALevel` = 2. The settings struct's own `msaa` field is only logged | `MSAAMode @ 0x198c894`, `ApplyRuntimeGfxSettings` comment |
| The game's graphics settings | `ApplyRuntimeGfxSettings(SR15GfxSettings&, ...)` is called from `CR15Game::InitEngine` (boot) and the settings translator (menu). On a Quest it sets eye size, MSAA and dynamic foveation from constants, not from the struct; the struct feeds shadows, TAA, bloom, mesh quality and the like | `ApplyRuntimeGfxSettings` |
| Frames | `vrapi_SubmitFrame2`, JUMP_SLOT `0x36e7010`: one call per rendered frame | libr15 imports |
| Device model, OS build | Android system properties `ro.product.model`, `ro.build.display.id`; libr15 reads the same properties through `__system_property_get` | ADR 0006 table |

The answer to "which setting picked 1440x1584 at scale 1.0": none of the player-facing graphics settings. The eye size is what the headset runtime suggests, times a constant 1.0, unless nevr's `graphics.texture_quality` (ADR 0015) or the game's RT-size override replaces it.

## Decision (Quest)

A development-free, always-on module `render_config` in the sentinel (`src/quest/sentinel/render_config*.{h,cpp}`):

- **Five GOT hooks**, each a `CallbackThunk` handler that calls the original with the game's arguments, stores the arguments and result in atomics and returns the original's result: `vrapi_SetDisplayRefreshRate`, `vrapi_SetClockLevels`, `vrapi_SetPropertyInt`, `vrapi_GetSystemPropertyFloat`, `vrapi_SubmitFrame2`. None logs, allocates or throws on the game's call path.
- **A poller thread** (`nevr-renderconfig`, created once, ends with the process) wakes once a second on a condition variable, reads the atomics and the game globals above (after proving libr15's build id, then `base + pinned vaddr`; reads only), builds a snapshot and logs `render_config` through the sentinel log: once when the first frame has been submitted and a size exists, then once for each poll whose snapshot differs from the last logged one, with `changed` naming the fields.
- The hardware dump (ADR 0006) hooks four of the same slots. A slot owned by `render_config` is skipped by the dump and reported with status `owned_by_render_config`; the dump's other 14 hooks and its file are unchanged. The shared-slot state is header-only, like `sysprop_int_share.h`.
- Device model and OS build come from `__system_property_get`, called by the poller.

Fields of the line: `platform`, `reason` (`first_frame` | `change`), `changed`, `device_model`, `os_build`, `vrapi_device_type`, `refresh_requested_hz`, `refresh_request_result`, `refresh_reported_hz`, `eye_w`, `eye_h`, `eye_source` (`nevr_texture_quality` | `game_rt_size_override` | `runtime_suggested`), `texture_quality`, `texture_quality_source` (`config` | `default`), `foveation_level`, `foveation_dynamic`, `cpu_level`, `gpu_level`, `clock_result`, `msaa_mode`, `spacewarp`, `frames`.

## Prediction (Quest)

On a headset, the first `render_config` line lands within a second of the first submitted frame. With the default config: `eye_source=runtime_suggested`, `texture_quality=stock`, `texture_quality_source=default`, `eye_w`/`eye_h` equal to the game's own `Creating OVR swap chain with size W x H` line, `foveation_dynamic=1`, `msaa_mode=1`, `cpu_level=gpu_level=2`, `refresh_requested_hz` the highest supported rate not above 90, and `refresh_reported_hz` the rate the runtime reports (a value above the request is the 120 Hz case). Pausing the game then produces a second line with `changed=foveation_level` and `foveation_level=0`; unpausing produces a third with 4. With `graphics.texture_quality: high`, `eye_source=nevr_texture_quality` and the size is the level's.

What would prove it wrong: no line (hooks refused: the install lines name the status); `eye_w`/`eye_h` different from the swap-chain line (the game creates the swap chain from a path other than `g_rtSize`); `foveation_level` never changing across a pause (the game sets foveation through a path other than `CVR::SetFoveationLevel`); `refresh_reported_hz` of 0 (id 4 is not the refresh rate in this runtime).

## Not available yet (Quest)

- **Spacewarp on or off.** It is a flag on the layers libr15 passes to `vrapi_SubmitFrame2` (`ovrSubmitFrameDescription2`). The struct layout is the SDK's and is not in this tree or in ReVault's types, so this ADR does not read it. The line carries `spacewarp=unknown`; reading the layer flags is tracked as a follow-up once the layout is sourced.
- **The game's `SR15GfxSettings` values** (graphics quality name, shadows, TAA) and what set them (saved profile or menu). The struct's address is not pinned. They do not drive eye size, MSAA or foveation on a Quest, so the fields that explain a frame-rate report are covered without them.
- **The native eye size** when `texture_quality` is `stock`: the final size and `eye_source` are logged; the runtime's untouched answer is only counted when the texture-quality hook is installed (ADR 0015).
- **Actual GPU clock, thermal state and achieved FPS** are not render configuration; the runtime's once-a-second stats line carries them.

## PCVR

(see the PCVR section)
