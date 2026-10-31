# ADR 0013: Quest game data comes from Echo Launcher; the runtime fetches it in development builds only

Status: accepted 2026-10-23, implemented (#758; the data step it changes is #807). Supersedes the decision of ADR 0011
(who fetches the game data, from where, for whom). ADR 0011's fetch design, toast fallback plan and measurement list stay as
the description of the development-build step and of the open measurements; where 0011 and this record disagree, this one
holds ("What changes in ADR 0011" below). Not yet observed on a headset: "What the first headset run must show" is what the
run decides.

## Requirement

A player who has the production build must end with a game that starts, or with a message in the headset that says what to do.
The game must never return to Home with nothing shown. A player's data is never written, replaced or deleted by a release
build.

**The release build does not meet "never return to Home with nothing shown" yet.** It requests the message and ends the
process at once, and no presenter is known to draw at that point (see "Not established"), so a player can still return to Home
with nothing shown. Issue #829 (milestone v5.5.1) closes that gap: it finds a presenter the headset draws, and the hold
returns for a presenter that was seen to draw. The requirement about player data is met.

## Context

The game reads its data from `<root>/_data/<version>/rad15/android/manifests/<id>`. Which root it reads depends on whether an
OBB file exists:

- With an OBB file, `CSysFile::Init` mounts it and the game records the mounted path; the sentinel's OBB skip answers that
  mount with the first root that holds the game's `_data`: the installer app's `Android/media` directory, then the legacy
  root (`/storage/emulated/0/readyatdawn`) (`obb_skip::ResolveRoot` in `src/quest/sentinel/obb_skip_hook.h`; the candidate order is in `src/quest/sentinel/data_root.h`).
- Without one, `CSysFile::Init` returns at `OBB not found` (the `access()` call at `0xf853b8`, ADR 0007, Context, step 1),
  before `AStorageManager_mountObb`. The OBB skip hooks on `mountObb` and `getMountedObbPath` never run, `gOBBPath` stays
  empty, and `CSysInfo::GetDataRootDir` (`0xf87250`) answers its literal fallback, `/storage/emulated/0/readyatdawn` (ADR 0007,
  Context). A third hook of the OBB-skip module is on `GetDataRootDir` itself: it answers the root `ResolveRoot` chose, which is the
  `Android/media` directory unless the legacy root alone holds the game's data (`DataRootHandler` in
  `src/quest/sentinel/obb_skip_hook.cpp`; the manifest, `data::kManifestPath`, is the file the data step takes as "this
  version of the data is there").

So on a headset with no OBB the engine reads the media root, and the data-root hook is installed, unless the legacy root
alone holds the game's data, in which case it reads the legacy root. A `_data` under the media root without the manifest
(a partial copy, or another build left by an older installer) does not take the engine away from complete data in the legacy
root. Reading the legacy root needs the storage permission (ADR 0009); reading the media root needs
none.

**Premise B (with no OBB and no data-root hook the engine never reads `Android/media`): established by code reading, plus one
headset observation.** The code reading is ADR 0007's Context (the `OBB not found` return at `0xf853b8` and the `GetDataRootDir`
fallback at `0xf87250`). The observation is the Quest 2 run on #758 with `_data` hidden: the game's failure line named the
manifest path under `/storage/emulated/0/readyatdawn`.

**Premise C (the data-root hook makes the engine read the media root): established by code reading; not observed on a headset.**
`GetDataRootDir` is reached from exactly two call sites, both through libr15's PLT stub `0xf34e20` and its one `JUMP_SLOT`
`0x36f1df8` (`CFileTypes::InitializeDirectories`, `bl` at `0x11eb144`; `CEngine::Initialize`, `bl` at `0x2abfe7c`), and no
`R_AARCH64_RELATIVE` or `R_AARCH64_ABS64` relocation names the function, so no call bypasses the slot (a scan of the pinned
`.text` and relocation table; `tools/tests/test_quest_data_root_wiring.py` and `got_pinned_test` pin the slot). Both callers
copy the string. Not observed: a launch with the data present only under `Android/media`, which is what
"What the first headset run must show" decides.

## Design

The chain, each step following from the one before it:

1. **Premise A (stated as the release requirement; not measured in this repository, and no Echo Launcher source or run was
   read for this record):** a player installs the production build once through Echo Launcher,
   and that is what puts `_data` on the headset.
2. **So the runtime does not need to fetch data for players.** Keeping the download out of the runtime keeps the release small
   and avoids a second installer inside the first. This is also the position recorded on #538 before ADR 0011 changed it: no
   player-facing data downloader.
3. **The root the engine reads with no OBB is the media root, unless the legacy root alone holds the game's data, in which case the
   legacy root.** The data step looks at, and a development build fetches into, the one root the engine reads this launch
   (`EngineRoot()` in `src/quest/sentinel/data_wiring.cpp`: the root the OBB skip served when a mount was skipped;
   `obb_skip::DataRootRedirect()` otherwise, which is non-null when `ResolveRoot` has run and the data-root slot holds our
   thunk). `data_step_start` logs `engine_root` and `engine_root_source` (`obb_skip`, `data_root_hook` or `engine_default`).
   A media directory that holds `_data` but not the manifest does not take the engine away from complete data in the legacy
   root: the legacy root is the answer then. Data placed anywhere else is never read, so it is never a success.
4. **The one thing the runtime still owes a player is a visible message when data is missing, instead of a silent exit.**
   A release build, when the data is not there, fetches nothing, writes nothing, recovers nothing, cleans nothing, shows
   `kGameDataMissing` ("Echo VR game data is missing. Install Echo VR with Echo Launcher first, then start the game again."),
   and ends the process nonzero with reason `game_data_missing` (`src/quest/data/data_step.cpp`,
   `src/quest/sentinel/data_wiring.cpp`). A release build holds the message for no time (`Options::hold_ms` is 0,
   `src/quest/sentinel/entry.cpp`): the one headset run found no presenter that draws at this point (see "Not
   established"), and a hold on a message nobody sees only delays the exit. The hold of two periods of five seconds, with
   the message shown again between, stays in development builds and returns in release builds with a presenter a
   headset shows.
5. **Without the storage grant and with the legacy root as the engine root,** nothing is looked at or written: the player is
   shown the Storage message (`kNeedsStoragePermission`) and the process ends nonzero with reason `no_storage_grant`.
6. **When the data step runs.** On the game's thread, once, before the manifest read: from the `RequestPermissions` hook
   after the boot request's wait (ADR 0009), and, when the microphone permission is already granted so that no request follows,
   from a hook on `CheckPermission` (`JUMP_SLOT 0x36c37c8`; `src/quest/sentinel/permission_wait_hook.cpp`, `CheckHandler`).
   `CR15Game::Initialize` calls `CheckPermission(RECORD_AUDIO)` and calls `RequestPermissions` only when that is denied.
7. **Which build fetches** is `nevr_build::kIsDev` (`cmake/nevr_build_info.cmake`: false for a version with no `-dev.`, or
   for a GitHub Actions build of a `vX.Y.Z` or `vX.Y.Z-rc.N` tag ref whose tag points at HEAD; a local build of a tagged commit
   stays a development build, so a release build comes from CI). `entry.cpp` sets
   `Options::allow_fetch` and `Options::probe_enabled` from it; `Options::allow_fetch` is false by default, so a code path that
   does not set it cannot fetch. `tools/tests/test_quest_build_is_dev.py` runs the generator on a tag and on a non-tag and pins
   both results.
8. **Parked, not built:**
   - *A redirect by assigning `gOBBPath`, or by an engine flag such as `-dataroot` or `-datadir`.* Not built: the redirect is the
     hook on `GetDataRootDir` (Design 3), which needs neither. Where `gOBBPath` is assigned, and whether the engine accepts either
     flag, were not read or measured.
   - *A fetch into the media root.* A development build fetches into the engine's root (Design 3), which is the media root only
     when `_data` is already there; a launch with no data anywhere fetches into the legacy root. Whether the media folder
     survives uninstalling the store package was not measured.

## What changes in ADR 0011, and what it got wrong

ADR 0011 assumed the OBB path runs on a sideloaded headset. It does not: with no OBB file `CSysFile::Init` returns before the
mount, so the OBB skip never serves a root; the engine's question is answered by the data-root hook (Design 3). From that
assumption it drew a media-root-first data root (`data_root::Resolve`) and a fetch into the media root.

| ADR 0011 | This record |
| --- | --- |
| Option A, fetch on first launch, for every player, with no switch ("Decisions" 1 and 3) | Development builds only; a release build never fetches (Design 4, 7) |
| Fetch target: the media root, or whichever root `Resolve` returns | The one root the engine reads this launch (Design 3); the media root unless the legacy root alone holds the data |
| The sha256 pin is the trust for a player-facing download (Decision 2) | Unchanged for the development fetch; a release build has no download to trust |
| "Fetch design", "Toast fallback plan", "Measurements to run first" | Still describe the development step and the open measurements; measurements 4 and 5 (media root) are parked with the media root |
| The data step is reached only through `RequestPermissions` | Also through `CheckPermission` when the microphone is already granted (Design 6) |

## Checked against ADR 0007, 0009 and 0011

- **ADR 0007, Prediction ("every launch"):** its predicted `OBB mounted at '/storage/emulated/0/readyatdawn'` line holds only on
  a launch where an OBB file exists. On a launch with none, the first line is `OBB not found` and no skip line appears. The
  Prediction is amended to "every launch that has an OBB file". Its "No feature flag" stands: the skip is unconditional, and
  `kIsDev` gates the data step, not the skip.
- **ADR 0007, Context:** consistent. It states the `OBB not found` early return and the `GetDataRootDir` fallback that this
  record relies on.
- **ADR 0007, Hooks (the `getMountedObbPath` bullet: "after the skip, return `/storage/emulated/0/readyatdawn`"):** stale. The
  hook returns the root `ResolveRoot` chose, the `Android/media` directory first when it holds `_data`, the legacy root
  otherwise (`src/quest/sentinel/obb_skip_hook.h`). The legacy root is the answer only when the legacy root alone holds the
  game's data; with no OBB file the engine is answered the root `ResolveRoot` chose, through the data-root hook (Design 3).
- **ADR 0009, "Not established": the case where RECORD_AUDIO is granted and storage is not (no request is made at all; not
  handled).** Contradicted: that case is now handled, by the `CheckPermission` hook (Design 6). Its decision (one hook on the
  `RequestPermissions` slot, first call only, 500 ms poll, 30 s cap) is unchanged and the new hook is added beside it.
- **ADR 0011, Status, Decisions and Option A:** superseded as the table above says. Its Context claim that the OBB skip serves
  the root the game reads is true only with an OBB file.

## What the first headset run must show

Written before the run. A release build, sideloaded, no `_data` in the legacy root, Storage granted: a `data_step_start` line, a
`data_check` line with `action=fetch` or `replace`, a `data_step` Error line with `outcome=missing` and no `data_fetch` line,
one `data_present` line for the message, then `android_main_returned` and a nonzero exit at once; no file written under
either data root. The same build with `_data` in place in the legacy root: `data_check action=use` and the game starts.
With `_data` only under the media root, no OBB file and Storage not granted (#866): an `obb_skip_install` line with
`data_root=ok`, a `data_root` line with `source=android_media`, a `data_step_start` line with
`engine_root=/sdcard/Android/media/com.readyatdawn.r15/files` and `engine_root_source=data_root_hook`, `data_check action=use`
with that root, no `no_storage_grant` line, and the game reaches its menu. A development build with `_data`
missing: the `data_fetch` lines of ADR 0011. Whether Horizon OS draws the toast this early (ADR 0011, measurement 1) is not
known; a `data_present` line with `result=requested` says `Toast.show()` returned, not that anything drew.

## Not established

Which presenter a player sees at the microphone check (the one headset run showed no `Toast` from the `android_main`
thread, whose Looper is prepared and never run; a `Toast` posted to the main looper, a dialog and an own-frames display are
untried); whether the toast is drawn and for how long it is readable; whether `/sdcard/readyatdawn` and `/storage/emulated/0/readyatdawn`
are the same folder for every player; that a launch with the data only under `Android/media` reaches the menu (Premise C); where `gOBBPath` is assigned; whether `-dataroot` or `-datadir` exists; whether
`Android/media/<package>` survives an uninstall.

The presenters a development build can probe (`nevr-data-probe`, `data_wiring.h`), and what each needs. `ui-toast`:
`Toast.makeText(activity, Looper.getMainLooper(), text, LENGTH_LONG).show()` through the overload that takes a looper, which
is `@hide` in AOSP (`android-12.0.0_r1`, `core/java/android/widget/Toast.java`: "Make a standard toast to display using the
specified looper ... @hide"), so a lookup from native code may be refused by the hidden-API policy; the probe's
`looper_overload_unavailable` token says the lookup failed, not why. The target SDK is 29, so the toast takes the legacy view
path whose handler needs a running looper (`CHANGE_TEXT_TOASTS_IN_THE_SYSTEM` is enabled for target SDK above 29), which is why a
main-looper handler is the candidate. `ui-post`: showing a dialog or posting any call to the UI thread through JNI alone takes a
`Runnable` or `Handler.Callback` object (`Activity.runOnUiThread`, `Handler.post`; by reading, no exception was found), and the
sentinel ships no dex to define one, so the UI thread's native looper is used instead: the ELF constructor takes
`ALooper_forThread()` (from `libandroid.so` by `dlsym`; the constructor is expected to run on the UI thread, which the
`ui_post state=capture` line records) and adds the read end of a pipe to it; `ShowOnUiThread` writes a queued text to the pipe and
the looper's callback makes the plain Toast on the UI thread, whose `Looper.myLooper()` is the running main looper. The capture
runs in development builds only. `dialog`: has no presenter (it needs the same `Runnable`); the probe logs `no_presenter`.