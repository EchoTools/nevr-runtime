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

## Context

The game reads its data from `<root>/_data/<version>/rad15/android/manifests/<id>`. Which root it reads depends on whether an
OBB file exists:

- With an OBB file, `CSysFile::Init` mounts it and the game records the mounted path; ADR 0007 makes the sentinel answer that
  mount with the legacy root (`/storage/emulated/0/readyatdawn`).
- Without one, `CSysFile::Init` returns at `OBB not found` (the `access()` call at `0xf853b8`, ADR 0007, Context, step 1),
  before `AStorageManager_mountObb`. The OBB skip hooks are on `mountObb` and `getMountedObbPath`, so they never run, `gOBBPath`
  stays empty, and `CSysInfo::GetDataRootDir` (`0xf87250`) answers its literal fallback, `/storage/emulated/0/readyatdawn`
  (ADR 0007, Context).

So on a sideloaded headset with no OBB the engine reads the legacy root, and only the legacy root. Reading that root needs the
storage permission (ADR 0009).

**Premise B (the engine never reads `Android/media` with no OBB): established by code reading, plus one headset observation.**
The code reading is ADR 0007's Context (the `OBB not found` return at `0xf853b8` and the `GetDataRootDir` fallback at
`0xf87250`). The observation is the Quest 2 run on #758 with `_data` hidden: the game's failure line named the manifest path
under `/storage/emulated/0/readyatdawn`. Not observed: a launch with the data present only under `Android/media`.

## Design

The chain, each step following from the one before it:

1. **Premise A (stated as the release requirement; not measured in this repository, and no Echo Launcher source or run was
   read for this record):** a player installs the production build once through Echo Launcher,
   and that is what puts `_data` on the headset.
2. **So the runtime does not need to fetch data for players.** Keeping the download out of the runtime keeps the release small
   and avoids a second installer inside the first. This is also the position recorded on #538 before ADR 0011 changed it: no
   player-facing data downloader.
3. **The root the engine reads with no OBB is the legacy root, so development and test tools write there.** A development
   build's data step looks at, and fetches into, the one root the engine reads this launch (`EngineRoot()` in
   `src/quest/sentinel/data_wiring.cpp`: the root the OBB skip served when an OBB exists, the legacy root otherwise). Data
   placed anywhere else is never read, so it is never a success.
4. **The one thing the runtime still owes a player is a visible message when data is missing, instead of a silent exit.**
   A release build, when the data is not there, fetches nothing, writes nothing, recovers nothing, cleans nothing, shows
   `kGameDataMissing` ("Echo VR game data is missing. Install Echo VR with Echo Launcher first, then start the game again."),
   holds it for two periods of five seconds with the message shown again between, and ends the process nonzero with reason
   `game_data_missing` (`src/quest/data/data_step.cpp`, `src/quest/sentinel/data_wiring.cpp`).
5. **Without the storage grant and with the legacy root as the engine root,** nothing is looked at or written: the player is
   shown the Storage message (`kNeedsStoragePermission`) and the process ends nonzero with reason `no_storage_grant`.
6. **When the data step runs.** On the game's thread, once, before the manifest read: from the `RequestPermissions` hook
   after the boot request's wait (ADR 0009), and, when the microphone permission is already granted so that no request follows,
   from a hook on `CheckPermission` (`JUMP_SLOT 0x36c37c8`; `src/quest/sentinel/permission_wait_hook.cpp`, `CheckHandler`).
   `CR15Game::Initialize` calls `CheckPermission(RECORD_AUDIO)` and calls `RequestPermissions` only when that is denied.
7. **Which build fetches** is `nevr_build::kIsDev` (`cmake/nevr_build_info.cmake`: false unless the version contains `-dev.`;
   a release is a GitHub Actions build of a tag at distance 0, whose version is `X.Y.Z`). `entry.cpp` sets
   `Options::allow_fetch` and `Options::probe_enabled` from it; `Options::allow_fetch` is false by default, so a code path that
   does not set it cannot fetch. `tools/tests/test_quest_build_is_dev.py` runs the generator on a tag and on a non-tag and pins
   both results.
8. **Parked, not built:**
   - *A redirect of the engine root* (making a no-OBB launch read another root, by a hook on `GetDataRootDir` or on whatever
     assigns `gOBBPath`, or by an engine flag such as `-dataroot` or `-datadir`). Parked because nothing needs it: the launcher
     puts the data in the legacy root, so the engine already reads it. Where `gOBBPath` is assigned, and whether the engine
     accepts either flag, were not read or measured; a flag, if it exists, might replace an OBB-based redirect entirely.
   - *The media root* (`/sdcard/Android/media/com.readyatdawn.r15/files`). It needs no permission, but the engine never reads
     it in a no-OBB launch, so data fetched there is not data the game can use. Data an older installer left there is not read
     either; its owner sees the missing-data message and installs through the launcher. Whether the folder survives uninstalling
     the store package was not measured.

## What changes in ADR 0011, and what it got wrong

ADR 0011 assumed the OBB path runs on a sideloaded headset. It does not: with no OBB file `CSysFile::Init` returns before the
mount, so the OBB skip never serves a root and the engine reads the legacy root. From that assumption it drew a media-root-first
data root (`data_root::Resolve`) and a fetch into the media root, which the engine would not have read.

| ADR 0011 | This record |
| --- | --- |
| Option A, fetch on first launch, for every player, with no switch ("Decisions" 1 and 3) | Development builds only; a release build never fetches (Design 4, 7) |
| Fetch target: the media root, or whichever root `Resolve` returns | The one root the engine reads this launch (Design 3) |
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
- **ADR 0009, "Not established": the case where RECORD_AUDIO is granted and storage is not (no request is made at all; not
  handled).** Contradicted: that case is now handled, by the `CheckPermission` hook (Design 6). Its decision (one hook on the
  `RequestPermissions` slot, first call only, 500 ms poll, 30 s cap) is unchanged and the new hook is added beside it.
- **ADR 0011, Status, Decisions and Option A:** superseded as the table above says. Its Context claim that the OBB skip serves
  the root the game reads is true only with an OBB file.

## What the first headset run must show

Written before the run. A release build, sideloaded, no `_data` in the legacy root, Storage granted: a `data_step_start` line, a
`data_check` line with `action=fetch` or `replace`, a `data_step` Error line with `outcome=missing` and no `data_fetch` line,
two `data_present` lines for the message, then `android_main_returned` and a nonzero exit; no file written under either data
root. The same build with `_data` in place: `data_check action=use` and the game starts. A development build with `_data`
missing: the `data_fetch` lines of ADR 0011. Whether Horizon OS draws the toast this early (ADR 0011, measurement 1) is not
known; a `data_present` line with `result=requested` says `Toast.show()` returned, not that anything drew.

## Not established

Whether the toast is drawn and for how long it is readable; whether `/sdcard/readyatdawn` and `/storage/emulated/0/readyatdawn`
are the same folder for every player; where `gOBBPath` is assigned; whether `-dataroot` or `-datadir` exists; whether
`Android/media/<package>` survives an uninstall.
