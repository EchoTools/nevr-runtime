# ADR 0007: Quest OBB mount skip

Status: accepted, implemented (`src/quest/sentinel/obb_skip_hook.{h,cpp}`, #319). Off by default until a headset run confirms it.

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

**The feature flag.** `features.obb_skip` in `nevr-quest.json`, row `obb_skip` of `kFeatures` in
`sentinel/quest_config.cpp`. It defaults off, needs no other feature, and is not forced off by any prerequisite.

**The ctor steps.** `register_obb_skip_counters` (with the other counters, before the single reporter start) and
`install_obb_skip` (right after the clock hook, so it is in place before libr15 runs `CSysFile::Init`). Skipped with
`obb_skip_off` when the flag is off, and with `counters_refused` when a counter was refused.

**The hooks** (`pinned_targets.h`: `LibR15MountObb` JUMP_SLOT `0x36c39d0`, `LibR15GetMountedObbPath` JUMP_SLOT
`0x36f5b48`; build ID `b243509c08ce677aeb95fa348016949b3fc45230`; both imports have value 0):
- `AStorageManager_mountObb`: when the call has a callback, set the "skipped" state and call
  `cb(filename, AOBB_STATE_MOUNTED, data)` synchronously on the calling thread; the original is not called. A call
  without a callback goes to the original.
- `AStorageManager_getMountedObbPath`: after the skip, return `/storage/emulated/0/readyatdawn`; before it, the original.
- The path hook is installed first and the mount hook only when it took: a skipped mount with no path served would record
  an empty path and the game would retry for the same time.

Both run on the caller's thread (the `android_main` thread inside `CSysOS::Init`); no thread is created and no looper is
involved. Neither logs on the game's call path. Counters: `obb_mount_skipped`, `obb_path_served` (calls) and
`obb_mount_thunk_faults`, `obb_path_thunk_faults` (faults).

**What it leaves alone.** `libpnsovr.so`, `libpnsrad.so` and `libpnsradmatchmaking.so` each carry a static copy of
`CSysFile::Init`, the callback and `GetDataRootDir` and import `gOBBPath` from libr15. Their slots are not hooked; only
one `Loading OBB from` sequence ran in the measured launch.

## Prediction (logcat, with the flag on)

- one `Loading OBB from '<path>'`, then `OBB mounted at '/storage/emulated/0/readyatdawn'`, then
  `OBB loading complete (took 0 ms)`;
- no `mounting encrypted OBBs is no longer supported`, no `Error loading OBB`, no second `Loading OBB from`;
- engine initialization follows immediately instead of at +30 s.

It does not claim the not-responding dialog is gone: `android_main` still runs the rest of `CSysOS::Init` before it
services the looper, and that remaining time is not measured. A missing extracted tree under
`/storage/emulated/0/readyatdawn` fails the way it does after the timeout today, sooner.

## Tests

| test | pins |
| --- | --- |
| `obb_skip_hook_test` (`just test-quest-hooks`) | the handlers through the thunk entries against a model of the callback and the wait loop: no wait when armed, the callback gets state 1 and its own arguments, the root is served, a call without a callback and a path request before the skip go to the original, a thunk without an original is a fault |
| `integration_sequence_test` | skipped by default with `obb_skip_off`; runs alone; counters before the reporter; `counters_refused`; a failing or throwing step leaves the others unchanged |
| `integration_hooks_test` | the counter budget with the four counters |
| `quest_config_test` | off by default; only a file boolean turns it on; a rejected file leaves it off |
| `got_pinned_test` (`just test-quest-hooks-pinned`) | both slots resolve in the pinned `libr15.so` |
