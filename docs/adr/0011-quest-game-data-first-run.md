# ADR 0011: Quest game data: how a headset-only install gets `_data`, and what the player sees when it is missing

Status: accepted by the owner (tracking issue #758; his answers of 2026-10-22, relayed by the Spritz seat: option A, for every player, from the existing R2-hosted `_data.zip`, with the pinned sha256 as the trust). Nothing in this ADR is built yet; the measurements below come first.

The goal, in the owner's words (#nevr-runtime-testing): "i'm literally going for 0 jank". A player who sideloads the APK on the
headset alone (no PC, no installer) must end with a game that starts, or with a message they can read in the headset that says
what to do. Today they get neither: the game returns to Home with nothing shown.

## Context

**What the game needs.** The game reads its data from `<root>/_data/<version>/rad15/android/manifests/<id>`. The sentinel serves
the root for the skipped OBB mount (ADR 0007, `src/quest/sentinel/obb_skip_hook.cpp`): `data_root::Resolve`
(`src/quest/sentinel/data_root.h:58-63`) takes `/sdcard/Android/media/com.readyatdawn.r15/files` or
`/storage/emulated/0/readyatdawn` when either holds `_data`, and otherwise returns the legacy root with `found=false`;
`ResolveRoot` (`obb_skip_hook.cpp:83-101`) logs one error event `data_root result=not_found` and goes on; `Install`
(`obb_skip_hook.cpp:104`) arms the hooks either way.

**Why the OBB is not a source.** Horizon OS refuses keyed OBB mounts (ADR 0007, Context), so the store OBB under
`/sdcard/Android/obb/com.readyatdawn.r15/` cannot be mounted. The owner's statement of the consequence: `/sdcard/readyatdawn/_data`
has had to exist since Meta removed encrypted OBB mounting (#nevr-runtime-testing, 00:09:12, message id
1562694291527049251). Reading the OBB's bytes without mounting it was not measured and is not planned: the OBB is keyed, and the
owner has dropped the option. Whether the OBB survives uninstalling the store app was therefore not measured; it does not matter
to this design.

**The failure, confirmed once.** On a Quest 2 with `_data` hidden (the t5-no-game-data run, findings
`/home/andrew/metis/gopher-spritz/findings/2026-10-22-758-no-game-data-diagnosis.md`), every launch logged, in order,
`RAD: Initialization Error: Unknown error while loading the game.`, then
`Failed to find package manifest file /storage/emulated/0/readyatdawn/_data/5932408047/rad15/android/manifests/48037dc70b0ecab2`,
then the sentinel's `android_main_returned ... exit_code=0` (ADR 0008). The process ended; the player saw Home. The build on the
headset (`c4b37a28`) predates #755; #755 adds the one `data_root` log line and fails the same way.

**The likely cause for players who already had the data.** The data version is not the cause: the `_data.zip` that
Mia's installer extracts has the same version folder and manifest id (`5932408047`, `48037dc70b0ecab2`) as the one this build asks
for. The t5 manifest line above came from our own hidden-data test. What differs is where the data sits: the Quest installer at
quest.echovr.de (Mia / Crafter-1's Quest-EchoVR-Installer) extracts to
`/storage/emulated/0/Android/media/com.readyatdawn.r15/files`, and builds before #755 look only at `/sdcard/readyatdawn`; #755
resolves `Android/media` first (`data_root.h`). Not measured: whether `Android/media/<pkg>` survives uninstalling the store app
(measurement 5). The `adb shell ls` of both roots on an affected headset still separates this cause from a missing `_data`.

## What can run before the game reads its data

The permission wait (ADR 0009, `src/quest/sentinel/permission_wait_hook.cpp`, `WaitForStorage` at `:61`) blocks the
`android_main` thread inside `CSysInfo::RequestPermissions`, before `CncaGame::Initialize` reads the manifest (not yet observed on
a headset, per that ADR). The OBB skip runs earlier still, in `CSysFile::Init`, where the stock mount wait measured 30.225 s on
the same thread (ADR 0007, Context), so a wait of that length there is known to be survivable. So the sentinel has a point on the
game's own thread, before the first manifest read, where it can check, fetch, and block. That a wait much longer than 30 s is
safe there (Android's "not responding" handling for a native activity) is **not measured**; measurement 2 below decides it.

The check is one file: the manifest path the game will ask for. The two values in it (`5932408047`, `48037dc70b0ecab2`) come from
one log; whether they are constants of the pinned `libr15.so` (build ID `b243509c08ce677aeb95fa348016949b3fc45230`) or derived at
run time is **not established**. The check must take them from the game's own path construction or from a pin proved against
`libr15.so`, not from this log.

## Options for a headset-only install

| Option | What it is | Cost / risk |
| --- | --- | --- |
| A. Fetch on first launch | The sentinel downloads `_data.zip` (the file `just quest-install` uses: `https://mia.cdn.echo.taxi/_data.zip`, sha256 `fc2eedeacc50d9ddf751e21914bb4188660cf5e79ce48aab24b84e757f4b543c`, `justfile` `quest-install`), verifies it, extracts it under the resolved root, and lets the game start. | Needs a download of about 937 MB and about 2.2 GiB free (`tools/quest-install.sh` `need_kb`); a zip reader on the Quest side (not in `src/quest/vcpkg.json` today; or `java.util.zip` over JNI); an HTTP range client (`src/quest/auth/curl_http.h` is the existing libcurl wrapper); a long blocking step on the game's thread; and a way to show progress, which is the open problem below. The pin is trust-on-first-use (the `justfile` says so), not a publisher signature. |
| B. Installer step | Keep `just quest-install` / `tools/quest-install.{sh,ps1}` (adb pushes and extracts the data). | No runtime code. Needs a PC and USB debugging, so it does not serve the headset-only route that fails. |
| C. Data inside the APK | The APK carries `_data.zip` as an asset; the runtime extracts it on first launch. | No network at first launch. The APK grows by about 937 MB (the dev APK is 161385468 bytes today), every release re-uploads it, and data and runtime versions are tied together. Still needs the extraction, the space check, and visible progress. |
| D. A companion app | A separate APK with its own screen downloads the data. | A second install and a second signing identity; the player must be told to run it first, which is jank. |
| E. OBB as a source | Dropped by the owner (see Context). | n/a |

**Pick: A (owner, 2026-10-22), with the visible message below as the floor.** It is the only option that serves a headset-only
install with no second artifact and no PC. B stays for testers with a PC. The pick stands only if measurement 1 finds a way to
show the player progress; without that, A is a long blank screen, which is the jank the goal rules out.

**Relation to earlier rulings.** The owner's 2026-10-15 ruling on #538 was "no player-facing data downloader". His 2026-10-22
answer on #758 replaces it for the Quest sentinel: the game data is fetched for every player, with no switch. Both are cited here
so a reader of #538 finds the change. The source is the file `just quest-install` already uses, `https://mia.cdn.echo.taxi/_data.zip`
(sha256 `fc2eedea...543c`), which is in the owner's own Cloudflare R2; no new path is introduced. The sha256 baked into the signed
runtime is the trust: the owner judged it enough (his answer to decision 2), so the zip carries no separate signature.

## The order of fallbacks

1. The game's data is present for this build: the manifest path exists under the resolved root. Start the game; nothing else
   happens.
2. `_data` exists but the manifest does not (version mismatch): do not delete or overwrite the player's `_data`. Extract the matching data into a staging directory beside it; when it verifies, rename the old `_data` to
   `_data.replaced-<utc stamp>` and the staging directory to `_data`, in that order. Keeping both needs space for both.
3. No `_data` at all: fetch, same staging and a single rename into place.
4. The fetch failed (no network, no space, a digest that does not match): show the visible message (the floor), then end the process cleanly (ADR 0008) instead of
   letting the game fail its own load.

The floor message names the missing file and what to do, in these cases: `Echo VR data is missing: <path the game wants>. Copy the
game data to <root>/_data (see <URL>) and start again.` (no `_data`), and `Echo VR data is for another game build: this version needs
<version folder>, found <list>. ...` (mismatch). The wording is for the owner to approve; the information content (which path, which
build, what to do) is not optional.

## Fetch design (option A)

- **Where it runs.** On a worker thread started from the pre-game point; the `android_main` thread waits on it (as
  `WaitForStorage` waits), so the game's own initialization order is untouched. After the storage permission is granted
  (ADR 0009), since the files go to shared storage.
- **Disk space.** `statvfs` on the target volume before the first byte: the zip (about 937 MB) plus the extracted tree plus
  headroom, the same 2.2 GiB figure `tools/quest-install.sh` uses, doubled in the mismatch case where the old `_data` stays. Not
  enough: no download, the floor message names the free and needed MiB.
- **Download, resume, retry.** Into `<root>/_data.zip.part` (a name our code creates), HTTP `Range` from the part's size, a
  bounded number of attempts with backoff, then give up to the floor. The whole-file sha256 is verified at the end only (the
  pin is one digest of the zip); a mismatch deletes that `.part` and counts as a failed attempt, never a retry on the same bytes.
- **Extract.** Into `<root>/_data.staging-<sha prefix>/`, every entry path checked to stay inside that directory (a zip entry
  with `..` or an absolute path is a failure of the whole fetch), then one `rename` to `<root>/_data`. A crash or kill leaves
  only the staging directory and the `.part`; the next launch deletes exactly those two names (they carry our prefix) and starts
  the extract again from the verified zip. `_data` itself exists only when complete: "no partial root left behind" means the game
  can never see a half-extracted `_data`.
- **After success.** Delete the zip we downloaded (about 937 MB), nothing else.
- **Logging** (repo rule: every path logs, queryable): one structured sentinel event per step, with the path, bytes, free space and
  result: `data_check`, `data_fetch` (start, progress at a coarse interval, retry, verified, failed with the reason),
  `data_extract`, `data_swap`. No URL query strings or credentials in them (there are none).

## What the player sees (the open problem)

The sign-in path (`game_error_text`, `login_prompt_hook.cpp:264`) is not available: it needs the game's net game running, and
the game returns from `Run` before that when its data is missing (findings, section 4; an inference from the code and the log,
not tested). `docs/quest/SIGN-IN.md` itself says it is unconfirmed on a headset. What the log does show is that the headset
displays system UI that our process causes during launch: the Meta unofficial-app dialog and the storage `GrantPermissionsActivity`
(findings, launch 1, L5492 and L6317), the latter started by the game's own JNI `requestPermissions` call
(`src/quest/sentinel/pinned_targets.h`, the `RequestPermissions` notes). The sentinel already attaches a thread to the game's
JavaVM and holds the activity object for JNI (`JniSession`, `src/quest/diag/hwdump_android.h:19`). Candidate mechanisms, none of
them measured in a headset:

1. A system-panel activity started through that JNI session (an Intent): for example a page shown by the headset's browser. Text
   and a static page only; no live progress; needs network for the page itself.
2. An Android `Toast` or `AlertDialog` on the activity: whether Horizon OS draws either over an immersive native app is unknown.
3. Our own frames: a minimal VrApi session drawing text and a progress bar before the engine starts. The only candidate that can
   show live progress with no dependency on the OS UI; the largest piece of work, and it must hand the session to the engine
   afterwards.
4. A separate Java activity in the APK: needs a manifest and dex change to the repacked store APK (the repack tooling was not
   read for this ADR).

**Decision for the owner, needed before building:** the mechanism is chosen by measurement 1, in this order of preference: 2, 1, 4,
3 (cheapest first). The plan does not name one as settled, because every one is unmeasured. The floor message needs only one
mechanism that renders in the headset; the progress display of option A needs one that updates (3, or 2 if a toast can be
refreshed).

## Measurements to run first, each with what would falsify the plan

1. On the dev APK, from the sentinel on a headset: show a fixed string through each of the mechanisms 1 to 4 that is cheap to try,
   at the pre-game point, with a debug-only trigger. Falsifier: none renders in the headset, then the floor requires mechanism 3.
2. Block the `android_main` thread at the pre-game point for 120 s, then 10 minutes, then continue. Falsifier: the process is
   killed, or the "not responding" dialog ends the game; then the fetch must not run on that thread, and the game must be held
   some other way.
3. Resolve the version folder and manifest id from `libr15.so` (build ID `b243509c08ce677aeb95fa348016949b3fc45230`) with ReVault
   (searched across the project's binaries, `searched N of N`). Falsifier: they are derived from data the sentinel cannot read
   before the game runs.
4. Falsifier for the cause above: the `adb shell ls` of `/sdcard/readyatdawn` and `/sdcard/Android/media/com.readyatdawn.r15/files` on an affected headset.
5. Uninstall the store package, then `adb shell ls /sdcard/Android/media/com.readyatdawn.r15/files`. Falsifier: the folder is gone, in which case the installer's data does not survive an uninstall either and the first-run fetch is needed on every reinstall.

## Tests

- The pure decision (`data present` / `version mismatch` / `no data` from a set of existing paths and a pinned name): host test
  beside `obb_skip_hook_test`, run in `just test-quest-hooks`; the same function the sentinel calls, with `ExistsFn` injected as
  `data_root.h` already does.
- The fetch state machine against a loopback HTTP server that serves ranges, drops the connection mid-body, serves a wrong digest,
  and serves a zip with a `..` entry: resume continues from the part's size; a wrong digest leaves no `_data`; a traversal entry
  leaves no `_data`; no free space downloads nothing.
- Crash safety: kill the process between extract and rename (a test hook that stops there); the next run deletes only the
  staging and `.part` names and `_data` is either absent or complete.
- A source-shape sensor in `tools/tests` next to `test_quest_data_root_wiring.py`: the pre-game check is called from the install
  path before the hooks are armed, so the game never runs against missing data without the check having run.
- The visible message: a test that the text builder names the path, the build and the action; and one measurement on a headset
  per mechanism (measurement 1), recorded in the PR that builds it.
- `quest-diagnose` (#750) must not call a running game failed because a log line is absent: a separate issue, since its verdict
  says "the game did not reach its data step" when the build does not log `OBB mounted at`.

## Decisions (owner, 2026-10-22)

1. The fetch is for every player. There is no tester switch and no second code path.
2. The sha256 pin baked into the signed runtime is enough; the zip needs no signature of its own. (Trust-on-first-use is the
   `justfile`'s own description of the digest; the runtime that carries it is signed, ADR 0010.)
3. Option A, not C: the first launch needs the network.

## Order of work

Measurements first (1 to 5 above), each with its falsifier; then build A with the visible message as the floor. A measurement
that needs the headset is run on the owner's hardware or reported as blocked; none is assumed.
