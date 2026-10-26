# ADR 0015: Quest texture quality is a config key applied by a GOT hook

Status: accepted, implemented (`src/quest/sentinel/texture_quality.h`, `texture_quality_hook.{h,cpp}`, `sysprop_int_share.h`, #943). On in every Quest build; the default level is `high`.
Not yet observed on a headset: the "Prediction" section is what the first run must show.

## Context

The community installer's patcher (EchoTools/echoApkPatch, by marshmallow-mia, for EVRCE) offers a Graphics Patch setting for the Quest APK. It changes the size of the buffer the game renders each eye into. This repository's Quest build repacks the APK with a shim and does not ship a patched `libr15.so`, so the setting has to come from the runtime. Only facts about the game are used from the patch repository (the setting's name, its levels and the absolute per-eye sizes); no code or bytes are.

What the game does (pinned `libr15.so`, build id `b243509c08ce677aeb95fa348016949b3fc45230`; `src/quest/sentinel/pinned_targets.h`):

- `CVR::RefreshRTSize` (ELF `0x1a709b0`) asks the headset runtime for the suggested eye-buffer size with `vrapi_GetSystemPropertyInt` property id 5 (width, call at `0x1a709f4`) and id 6 (height, `0x1a70a04`), multiplies each by `CustomViewportScale` (1.0 unless a script sets it) and stores the pair.
- `CVR::Resize` (`0x1a70d0c`) creates the swap chain from the pair, once per process, and logs `Creating OVR swap chain with size %i x %i (2 slices, %u sample[s])`.
- `vrapi_GetSystemPropertyInt` is a `JUMP_SLOT` in libr15 at ELF `0x36fa728`; libr15 is `BIND_NOW`.

## Decision

A config key, `graphics.texture_quality`, read from the Quest `config.yaml` (`src/quest/sentinel/quest_config.cpp`). Values: `stock`, `low`, `medium`, `high`, `very_high`, `ultra`, lower case. The default (key absent) is `high`.

One GOT hook on the `vrapi_GetSystemPropertyInt` slot. The handler calls the original with the game's own arguments every time and returns its answer, except for property ids 5 and 6 when a level other than `stock` is latched: those return the level's width or height. Every other id returns exactly what the runtime answered. `stock` installs nothing and the game gets the runtime's own size.

| level | width | height |
| --- | --- | --- |
| low | 1680 | 1760 |
| medium | 2376 | 2568 |
| high | 3072 | 3380 |
| very_high | 3584 | 3944 |
| ultra | 4096 | 4506 |

The sizes are the installer's absolute sizes, reported for a Quest 3; nothing here measured any headset's native size. Samples stay at 2. The width/height ratio is 0.9545 for `low` and between 0.9087 and 0.9252 for the other four, as the table gives it (`texture_quality.h`).

The level is latched by the first install in the process and never changes: the swap chain is created once, so its size has to stay what it was. A second install with another level is refused and logged. A change of the key takes effect at the next launch.

A value that is not one of the six words keeps the default and logs a warning naming the word (`quest_config.cpp`, `rejected reason=unknown_level`).

The handler never logs on the game's call path. The install logs one line, whatever the level and the outcome (`texture_quality`, fields `result`, `quality`, `source`, `w`, `h`; `quality` and not `level`, because the log line's own `level` is its severity). Counters `gfx_native_eye_width` and `gfx_native_eye_height` carry the runtime's last answer to ids 5 and 6, which is the headset's native size, and `gfx_eye_size_served` counts the answers replaced.

### Why the PC loader accepts the section

The Quest reads the same `config.yaml` through the PC's `nevr_config` (`src/core/nevr_config.cpp`), which rejects a file with an unknown top-level key as a whole. A key under an existing section would have put render quality under a name that means something else (`diagnostics`, `social`); a new section keeps the key where it belongs, and the PC accepts `graphics` and reads nothing from it, as it does for `diagnostics`.

### The shared slot

`GotHook` allows one owner per slot, and the hardware dump (ADR 0006) already records every `vrapi_GetSystemPropertyInt` query. Two hooks on one slot would be refused. The texture-quality hook installs first (`src/quest/integration/ctor_sequence.cpp`) and marks the slot owned (`sysprop_int_share.h`); the dump then registers an observer instead of installing, and the handler tells it each query's id and the runtime's own answer (before any override). The dump therefore keeps recording what the runtime said, not what the game was told, and reports the slot as `shared`. When the texture-quality hook installs nothing (`stock`, or a refusal), the dump installs its own hook as before.

## Prediction (written before a headset run)

Default (no `graphics` section): a `texture_quality` line `result=installed quality=high source=default w=3072 h=3380`, and the game's `Creating OVR swap chain with size 3072 x 3380 (2 slices, 2 sample[s])`. With `low`: `1680 x 1760`. With `stock`: `result=not_installed`, no hook, and the runtime's own size in the game's line. The counters `gfx_native_eye_width` and `gfx_native_eye_height` show the native size at the levels that install the hook.

What would prove it wrong: the swap-chain line shows a size other than the level's (the game reads the size somewhere other than ids 5 and 6, or creates the swap chain before the hook installs); no `texture_quality` line; a crash or a blank frame at `ultra` (GPU memory).

## Not claimed

That the table's sizes suit a Quest 2: `high` is above its native size and costs frame time. The native sizes of the Quest 2 and Quest 3 are not measured here (the counters above measure them on the first run); a level defined as a scale of the native size is tracked separately (#953). That `low` is the installer's own number, not one derived from the other levels. That GPU memory suffices at `ultra`. The in-game menu control for the setting is separate work. The PC build has its own render scale and MSAA setting (`resolutionscale` and `msaa` in its saved graphics settings); whether this key gets a PC counterpart is tracked in #956.
