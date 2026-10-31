# ADR 0007: Quest OBB mount skip

Status: accepted, implemented (`src/quest/sentinel/obb_skip_hook.{h,cpp}`, #319). On in every Quest build, with no feature
flag (#572). Not yet observed on a headset: the "Prediction" section is what the first run must show.

## Context

Every Quest launch shows "Echo VR is not responding". `CSysFile::Init` (`libr15.so` `0xf85260`) runs on the
`android_main` thread, before that thread services the looper:

1. `access()` on `/sdcard/Android/obb/com.readyatdawn.r15/main.<n>.com.readyatdawn.r15.obb` (`0xf853b8`); a missing file logs
   `OBB not found` and returns at once.
2. Clears the completion flag byte at `0x37623c0`, calls `AStorageManager_new` (`0xf8541c`), stores the manager at
   `0x37623e0`, then `AStorageManager_mountObb(mgr, path, "37c70a1635a1ad7a", CSysFile_OnObbStateChange, nullptr)`
   (`0xf85434`).
3. Polls for the flag: 100 ms sleeps until 20 s have passed, then up to 51 retries of the mount, 200 ms apart
   (nominal 30.2 s; measured request to engine initialization 30.225 s).

Horizon OS refuses keyed OBB mounts (`NStorage: mounting encrypted OBBs is no longer supported`) and never calls the
callback, so every launch waits the whole time. After the wait `CSysInfo::GetDataRootDir` (`0xf87250`) returns its
fallback, `/storage/emulated/0/readyatdawn` (`0x2b2f5c3`), because `gOBBPath` (`0x3762470`) is still empty; the game
reads its assets from there.

The callback (`CSysFile_OnObbStateChange`, `0xf86c28`) reads only its `state` argument. For state `1` it calls
`AStorageManager_getMountedObbPath` with the manager and the path buffer it recorded (`0xf86c6c`), copies a non-null
result into `gOBBPath`, logs `OBB mounted at '%s'` and sets the flag byte. `CSysFile::Init` then logs
`OBB loading complete (took %llu ms)`.

## Decision

**No feature flag.** The skip runs on every launch: nothing in the configuration turns it off, and it needs no other
feature. A fix has no reason to be switched off, and a flag is a second code path to support.

**The ctor steps.** `register_obb_skip_counters` (with the other counters, before the single reporter start) and
`install_obb_skip` (right after the clock hook, so it is in place before libr15 runs `CSysFile::Init`), both on every
launch, also when the configuration cannot be resolved. `install_obb_skip` is skipped with `counters_refused` when a
counter was refused.

**The hooks** (`pinned_targets.h`: `LibR15MountObb` JUMP_SLOT `0x36c39d0`, `LibR15GetMountedObbPath` JUMP_SLOT
`0x36f5b48`, `LibR15GetDataRootDir` JUMP_SLOT `0x36f1df8`; build ID `b243509c08ce677aeb95fa348016949b3fc45230`; the first two
imports have value 0, the third is defined in libr15 itself):
- `AStorageManager_mountObb`: when the call has a callback, set the "skipped" state and call
  `cb(filename, AOBB_STATE_MOUNTED, data)` synchronously on the calling thread; the original is not called. A call
  without a callback goes to the original.
- `AStorageManager_getMountedObbPath`: after the skip, return the root `ResolveRoot` chose (the media root when it holds
  `_data`, else the legacy root `/storage/emulated/0/readyatdawn`); before it, the original.
- `CSysInfo::GetDataRootDir` (#866): return the root `ResolveRoot` chose, the media root unless the legacy root alone holds
  the game's data (#932, #965), in which case the legacy root. The file's own body is not consulted once `ResolveRoot` has
  run; before it, the original answers. It exists for the launch with no OBB file, where neither hook
  above runs. By code reading (ADR 0013, Premise C; not observed on a headset), libr15 calls the function from two sites
  only, both through this slot, and both read the pointer. It is installed whether or not the
  other two took, and `Install()` returns true only when all three did. `obb_skip::DataRootRedirect()` is non-null whenever the
  slot took and `ResolveRoot` has run; the data step takes the engine's root from it (ADR 0013, Design 3).
- The path hook is installed first and the mount hook only when it took: a skipped mount with no path served would record
  an empty path and the game would retry for the same time.

Both run on the caller's thread (the `android_main` thread inside `CSysOS::Init`); no thread is created and no looper is
involved. None logs on the game's call path. Counters: `obb_mount_skipped`, `obb_path_served`, `data_root_served` (calls) and
`obb_mount_thunk_faults`, `obb_path_thunk_faults`, `data_root_thunk_faults` (faults).

**What it leaves alone.** `libpnsovr.so`, `libpnsrad.so` and `libpnsradmatchmaking.so` each carry a static copy of
`CSysFile::Init`, the callback and `GetDataRootDir` (their copies of `GetDataRootDir` are not hooked), and each defines and exports its own `gOBBPath` (`readelf -rW`:
GLOB_DAT slot `0x6e4c98`, `0x735c18`, `0x6bdc90`; symbol values `0x70c670`, `0x75d760`, `0x6ef1b8`). Their slots are not
hooked; only one `Loading OBB from` sequence ran in the measured launch.

**Known gap: the static initializers.** Each of the three libraries clears its `gOBBPath` through that GLOB_DAT slot with
`CMemory::Fill(gOBBPath, 0, 0x200)` in an unnamed function (`libpnsrad.so` `0x1b8db0`, the call is the function's tail;
`libpnsovr.so` `0x1b6710`; `libpnsradmatchmaking.so` `0x1ae95c`), probably a static initializer. Two outcomes are
possible and which holds was not measured: the slot binds to libr15's `gOBBPath` (then loading the library wipes the
path the hook caused to be recorded, and a later `CSysFile::Init` of that library loses its early-out and does the real,
refused mount: the same ~30 s, elsewhere), or it binds to the library's own copy (then that library's `Init`, if it is
ever called, runs the refused mount regardless of this hook). The sentinel counters do not show either; only the logcat
deviation lines do (a second `Loading OBB from`, any `mounting encrypted OBBs is no longer supported`, a ~30 s gap).

## Reference build

The community reference libr15 (EchoTools/echoApkPatch, `configs/libr15.so.yaml`) carries two patches in this module's
area. `CSysFile::Init` (`0xf85260`) becomes `ret` ("Remove OBB loading", yaml:1-9), so Init returns at its first word and
the mount and path hooks are never called. `CSysInfo::GetDataRootDir` (`0xf87250`) becomes `return <media literal>`
(`adrp x0`, `add x0`, `ret`; yaml:46-60), so the body never reads `gOBBPath`. Our answer does not depend on the body: once
`ResolveRoot` has run, the hook answers the same root on either file, and on a file whose media literal is renamed to
the r15 package it is the same string the body would return.

| File | `CSysFile::Init` | `GetDataRootDir` body | Engine answer with the legacy root holding no data alone |
| --- | --- | --- | --- |
| stock | runs; the mount is answered by the hook | `gOBBPath`, else the legacy literal | media (ResolveRoot) |
| reference, package renamed to `r15` | returns at its first word | media literal | media (ResolveRoot) |
| reference, as shipped (`afk`) | returns at its first word | media literal, `afk` | media, `r15` (`kMediaRoot`); the body's `afk` path is not returned |

## Prediction (logcat, every launch that has an OBB file)

- one `Loading OBB from '<path>'`, then `OBB mounted at '/storage/emulated/0/readyatdawn'`, then
  `OBB loading complete (took 0 ms)`;
- no `mounting encrypted OBBs is no longer supported`, no `Error loading OBB`, no second `Loading OBB from`;
- engine initialization follows immediately instead of at +30 s.

A launch with no OBB file logs `OBB not found` at step 1 of the Context and returns before the mount, so none of these lines
appear and the skip never runs; the engine reads the root `ResolveRoot` chose, the media root unless the legacy root alone holds the data, through the
`GetDataRootDir` hook (ADR 0013).

It does not claim the not-responding dialog is gone: `android_main` still runs the rest of `CSysOS::Init` before it
services the looper, and that remaining time is not measured. A missing extracted tree under
`/storage/emulated/0/readyatdawn` fails the way it does after the timeout today, sooner.

## Tests

| test | pins |
| --- | --- |
| `obb_skip_hook_test` (`just test-quest-hooks`) | the handlers through the thunk entries against a model of the callback and the wait loop: no wait when armed, the callback gets state 1 and its own arguments, the root is served, a call without a callback and a path request before the skip go to the original, a thunk without an original is a fault; the `GetDataRootDir` hook answers the media root only when the manifest is found there and the hook is installed (a `_data` directory alone does not count), otherwise the original; `InstallWith`, driven with fake slot installers, arms the handler and marks the hook live only when the slot took |
| `integration_sequence_test` | installed with every feature off and when the configuration cannot be resolved; counters before the reporter; `counters_refused`; a failing or throwing step leaves the others unchanged |
| `integration_hooks_test` | the counter budget with the six counters |
| `quest_config_test` | not a feature: the feature report does not name it |
| `got_pinned_test` (`just test-quest-hooks-pinned`) | the three slots resolve in the pinned `libr15.so` |
| `data_wiring_test` (`just test-quest-data-net`) | with no OBB file and `_data` only under the media root the data step uses it with and without the storage grant, in a release and a development build; with the data in the legacy root, or the hook not installed, the grant is still asked for |
| `test_quest_data_root_wiring.py` | `Install()` resolves the root first and installs the `GetDataRootDir` slot, its result is part of the return, the handler does no file or log work, and the data step reads the engine's root from the hook |
