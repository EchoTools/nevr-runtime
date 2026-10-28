# ADR 0015: Quest texture quality is a config key applied by a GOT hook

Status: accepted, implemented (`src/quest/sentinel/texture_quality.h`, `texture_quality_hook.{h,cpp}`, `sysprop_int_share.h`, #943). On in every Quest build; the default level is `stock`, which installs no hook.
Not yet observed on a headset: the "Prediction" section is what the first run must show. The procedure for the run, and the thresholds that decide the default, are in `docs/testing/quest-texture-quality-headset-sheet.md`.

## Context

The community installer's patcher (EchoTools/echoApkPatch, by marshmallow-mia, for EVRCE) offers a Graphics Patch setting for the Quest APK. It changes the size of the buffer the game renders each eye into. This repository's Quest build repacks the APK with a shim and does not ship a patched `libr15.so`, so the setting has to come from the runtime. Only facts about the game are used from the patch repository (the setting's name, its levels and the absolute per-eye sizes); no code or bytes are.

What the game does (pinned `libr15.so`, build id `b243509c08ce677aeb95fa348016949b3fc45230`; `src/quest/sentinel/pinned_targets.h`):

- `CVR::RefreshRTSize` (ELF `0x1a709a0`) asks the headset runtime for the suggested eye-buffer size with `vrapi_GetSystemPropertyInt` property id 5 (width, call at `0x1a709f4`) and id 6 (height, `0x1a70a04`), multiplies each by `CustomViewportScale` (1.0 unless a script sets it) and stores the pair.
- `CVR::Resize` (`0x1a70d0c`) creates the swap chain from the pair, once per process, and logs `Creating OVR swap chain with size %i x %i (2 slices, %u sample[s])`.
- `vrapi_GetSystemPropertyInt` is a `JUMP_SLOT` in libr15 at ELF `0x36fa728`; libr15 is `BIND_NOW`.

## Decision

A config key, `graphics.texture_quality`, read from the Quest `config.yaml` (`src/quest/sentinel/quest_config.cpp`). Values: `stock`, `very_low`, `low`, `medium`, `high`, `very_high`, `ultra`, lower case. The default (key absent) is `stock`: no hook, the runtime's own size. `texture_quality.h` holds the default (`kDefaultLevel`) and, separately, the level every mistake gives (`kFallbackLevel`, always `stock`).

One GOT hook on the `vrapi_GetSystemPropertyInt` slot. The handler calls the original with the game's own arguments every time and returns its answer, except for property ids 5 and 6 when a level other than `stock` is latched: those return the level's width or height for the headset the runtime's own answer to the same call names (`HeadsetForStockSize`: a stock size of 1680 or more is a Quest 3, below it a Quest 2). Every other id returns exactly what the runtime answered. `stock` installs nothing and the game gets the runtime's own size.

| level | Quest 3 width x height | Quest 2 width x height |
| --- | --- | --- |
| very_low | 1344 x 1408 | 1152 x 1264 |
| low (the stock size) | 1680 x 1760 | 1440 x 1584 |
| medium | 2376 x 2568 | 1728 x 1904 |
| high | 3072 x 3380 | 2016 x 2224 |
| very_high | 3584 x 3944 | 2304 x 2528 |
| ultra | 4096 x 4506 | 2592 x 2848 |

The sizes are the absolute per-eye sizes of EchoTools/echoApkPatch's Texture Resolution setting (graphics v9, `native/texres/texres.c` at `8aa27ecdc`, by marshmallow-mia); the Quest 3 column is what #943 shipped. Nothing here measured any headset's native size. Samples stay at 2. The sizes are per headset because a Quest 2 at the Quest 3 table renders about 4.5 times its stock pixels at `high` (#943, #1012).

The level is latched by the first install and fixed for the process: a change of the key takes effect at the next launch. Changing a level while the game runs (the in-game control and the live change the patch repository makes with trampolines into libr15) is not in this tree; see #1012.

The level is latched by the first install in the process and never changes: the swap chain is created once, so its size has to stay what it was. A second install with another level is refused and logged. A change of the key takes effect at the next launch.

Any mistake in the key gives `stock`, never the default and never `high`, with one Warning that names the key (`quest_config.cpp`, `config file key=graphics.texture_quality rejected reason=<why> (falls back to stock; ...)`). The reasons: `unknown_level` (a word that is not one of the six: wrong case, a number, empty; the word is named when it is short printable ASCII), `not_a_word` (a mapping, a list, or the key with no value), `graphics_not_a_mapping` (`graphics: ultra`, `graphics: [ultra]`), `unset_variable` (a bare `${VAR}` that is not set) and `file_rejected` (the file is unusable as a whole, so it says nothing about this key). The fall back is separate from the default so that raising the default cannot turn a typo into the level that costs the most. A key under `graphics` other than `texture_quality` (`texture_qualty`, `Texture_Quality`) is one Warning that names it (`reason=unknown_key name=<key>`; the first eight, then one line counting the rest) and changes nothing else: the level is the default, as for an absent key, so a misspelling is heard instead of silently giving the default.

### Known path: a stored size skips the query

`CVR::RefreshRTSize` queries ids 5 and 6 only when the stored override pair at ELF `0x37a33e8` is zero. `CVR::SetRTSizeOverride` is the only writer found. Whether a Quest run sets it is not known: static reading finds one caller, `ConfigureEngine` (`bl` at ELF `0x11f3db8`), behind a flag test, passing 1920x1080. Where the game stores a size, it never asks the runtime and the key has no effect. This is a static reading of the pinned library, not observed on a headset; the first Quest run shows the swap-chain line either way.

The handler never logs on the game's call path. The install logs one line, whatever the level and the outcome (`texture_quality`, fields `result`, `quality`, `source`, `w`, `h`; `quality` and not `level`, because the log line's own `level` is its severity). Counters `gfx_native_eye_width` and `gfx_native_eye_height` carry the runtime's last answer to ids 5 and 6, which is the headset's native size, and `gfx_eye_size_served` counts the answers replaced.

### Why the PC loader accepts the section

The Quest reads the same `config.yaml` through the PC's `nevr_config` (`src/core/nevr_config.cpp`), which rejects a file with an unknown top-level key as a whole. A key under an existing section would have put render quality under a name that means something else (`diagnostics`, `social`); a new section keeps the key where it belongs, and the PC accepts `graphics` and reads nothing from it, as it does for `diagnostics`.

### The shared slot

`GotHook` allows one owner per slot, and the hardware dump (ADR 0006) already records every `vrapi_GetSystemPropertyInt` query. Two hooks on one slot would be refused. The texture-quality hook installs first (`src/quest/integration/ctor_sequence.cpp`) and marks the slot owned (`sysprop_int_share.h`); the dump then registers an observer instead of installing, and the handler tells it each query's id and the runtime's own answer (before any override). The dump therefore keeps recording what the runtime said, not what the game was told, and reports the slot as `shared`. When the texture-quality hook installs nothing (`stock`, or a refusal), the dump installs its own hook as before.

## Prediction (written before a headset run)

Default (no `graphics` section): a `texture_quality` line `result=not_installed quality=stock source=default why=stock`, no hook, and the runtime's own size in the game's `Creating OVR swap chain` line. With `high`: `result=installed quality=high source=config w=0 h=0 sizes=per_headset`, and then `Creating OVR swap chain with size 3072 x 3380 (2 slices, 2 sample[s])` on a Quest 3 or `2016 x 2224` on a Quest 2. With `very_low`: `1344 x 1408` on a Quest 3, `1152 x 1264` on a Quest 2. With `low`: the runtime's own size, which is `1680 x 1760` or `1440 x 1584`. With `stock`: `result=not_installed`, no hook, and the runtime's own size in the game's line. The counters `gfx_native_eye_width` and `gfx_native_eye_height` show the native size at the levels that install the hook.

What would prove it wrong: the swap-chain line shows a size other than the level's (the game reads the size somewhere other than ids 5 and 6, or creates the swap chain before the hook installs); no `texture_quality` line; a crash or a blank frame at `ultra` (GPU memory).

## Not claimed

That the Quest 2 sizes work on a headset: they are the patch repository's values, measured there on a Quest 3 only (its `UPDATE.md`, Graphics v10), and `high` and `ultra` cost frame time and GPU memory on either headset; this is why the default is `stock`. The default becomes `high` (one line, `kDefaultLevel`, and the test that pins it) after a Quest 2 has been run at `high` and its frame time and memory measured. The native sizes of the Quest 2 and Quest 3 are not measured here (the counters above measure them on the first run); a level defined as a scale of the native size is tracked separately (#953). That `low` is the installer's own number, not one derived from the other levels. That GPU memory suffices at `ultra`. The in-game menu control for the setting is separate work. The PC build has its own render scale and MSAA setting (`resolutionscale` and `msaa` in its saved graphics settings); whether this key gets a PC counterpart is tracked in #956.
