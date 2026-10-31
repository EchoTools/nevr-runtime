# ADR 0009: the game waits for the storage permission it just asked for

Status: accepted, implemented (`src/quest/sentinel/permission_wait_hook.{h,cpp}`, #640). On in every Quest build, with no feature flag.
Not yet observed on a headset: the "Prediction" section is what the first fresh-install run must show.

## Context

On the first launch of a fresh install the game asks for `RECORD_AUDIO`, `READ_EXTERNAL_STORAGE` and `WRITE_EXTERNAL_STORAGE` and
does not wait for the answer. `CR15Game::Initialize` (`0x11f9b40`) calls `CSysInfo::RequestPermissions` (`0xf873ec`), a fire and
forget JNI `requestPermissions` on the NativeActivity, and goes straight into `CncaGame::Initialize`, whose package-manifest read
(`ReadManifestFile`, `0x2541404`) needs the storage permissions. The read fails 260 ms later, while the grant dialog has not
started; `ShowInitializationError` reports "Unknown error while loading the game." and `android_main` finishes the activity. The
process-end hook (#621, ADR 0020) makes the relaunch after the grant start clean; this decision is the first launch itself.

## Decision

One GOT hook on libr15's `RequestPermissions` slot (`JUMP_SLOT 0x36e4320`). The handler makes the request (see "The request
code" below); on the first call only
(the boot request: later callers are script nodes that must not block) it then waits, on the same thread, until each storage
permission in the request is granted. The check is the game's own `CSysInfo::CheckPermission` (`0xf87298`), reached as libr15 base
plus that address after the module's build ID and the function's first four instructions match the pinned build; a mismatch logs
`check_unavailable` and leaves the game's call alone. The poll is 500 ms; the wait ends at the grant or after 30 s, and a timeout
leaves the game exactly where it is without the hook. The wait does not pump the looper. A denial cannot be told from "not answered yet" here: `CheckPermission` returns the same "not granted" for both, and the answer reaches only Java (`onRequestPermissionsResult`), so telling them apart needs a JNI call from the blocked thread. The cap is therefore 30 s, about what a player waits: a player who taps Deny sees a blank screen for the cap and then today's initialization error, with a `timeout` line carrying `waited_ms`.

## The request code (#947)

The game's request is not made by the game's function. `CSysInfo::RequestPermissions` calls
`CJavaObject::CallVoidMethod("requestPermissions", "([Ljava/lang/String;I)V", array)` (`0xf7d6e4`), and that wrapper calls
JNI's variadic `CallVoidMethod` with one argument after the method id: `0xf7d84c-0xf7d858` load the activity into x1, the
method id into x2 and the `String[]` into x3, and nothing sets x4. The signature's `int`, the request code, is therefore
whatever x4 holds when the call is made. `Activity.requestPermissions` throws `IllegalArgumentException` for a negative
code, and the uncaught exception aborts the process. The community patch repo's fix is to skip the request altogether (its
permission patch, `ret` at `0xf873ec`), which also removes the grant this decision waits for.

The handler makes the same call itself instead (`permission_request.cpp`): the activity and the VM from the glue (the
`android_app` that `android_main` was given, kept by the process-end hook), a `String[]` of the game's names, and
`requestPermissions(names, 1)`. Every JNI call is checked for a pending exception, which is described and cleared. When
that call cannot be made (no `android_app`, no VM, a JNI step that throws) the game's own function runs, as before, and the
line says why. Each request logs `{"event":"permission_request","result":...,"path":"ours"|"game","permissions":N}`, with
`"request_code":1` on our path, for the first eight requests. The game no longer logs `Requesting %llu android permissions`
for a request we make. The script-node requests take the same path. The names are the game's: the microphone is still
asked for, since voice needs it.

## Prediction (written before the code)

ELF addresses of the pinned libr15 (build id b243509c08ce677aeb95fa348016949b3fc45230); disassembly address = ELF + 0x100000. Static analysis only; nothing was run on a device.

**What happens today (14:06:52.056-.379 in the issue's logcat).** Everything is on the android_main thread, one call chain: `CSysOS::Init` (0xf78738) -> `CSysFile::Init` (OBB mount, skipped by #319) -> `NRadEngine::Main` (0xf78794) -> `Main_` (0xfb021c) -> `CncaGame::Run` (0x17fad54) -> `CR15Game::Initialize` (0x11f9b40). That function checks RECORD_AUDIO (`CSysInfo::CheckPermission` 0xf87298, logged "Checking android permission ... RECORD_AUDIO (Result: -1)" at .057); on denial it calls `CSysInfo::RequestPermissions` (0xf873ec, 128 bytes) with {RECORD_AUDIO, READ_EXTERNAL_STORAGE, WRITE_EXTERNAL_STORAGE} (logged "Requesting 3 android permissions" at .057) and then calls `CncaGame::Initialize` (0x17e9984) immediately, at 0x11f9cf0. `RequestPermissions` is fire and forget: it builds a Java string array and does one JNI `CallVoidMethod("requestPermissions", "([Ljava/lang/String;I)V")` on the NativeActivity, then returns. It keeps no state and waits for nothing; nothing in libr15 or the APK's classes.dex has an `onRequestPermissionsResult`. `CncaGame::Initialize` -> `ConfigureEngine` -> `NR15Logger::CreateLogFile` (0x1209ab0) checks WRITE_EXTERNAL_STORAGE (the .300 line, Result -1) and carries on; then `InitEngine` -> `CEngine::InitGlobals` -> `CPackageManifest::AddPackage` -> `ReadManifestFile` (0x2541404): `CSysFile::FileExists` is false, "Failed to find package manifest file ..." error -> `CncaGame::Run` -> `ShowInitializationError` (0x17ec13c, "Initialization Error: Unknown error while loading the game." at .316) -> `android_main` finishes the activity (.329). The GrantPermissionsActivity only starts at .379, so the manifest read runs 260 ms before the dialog exists. That the missing grant is what makes `FileExists` fail is not proven by code; it rests on the issue's second launch (grants in place, same path, runs).

**The hook that runs before `CSysFile::Init`'s successor reads the manifest.** None of ours: the OBB skip (#319) and the process-end hook (#621) are GOT hooks on libr15 slots (`AStorageManager_mountObb`, `android_main`); neither sits between the request and the manifest read. The new hook is a GOT hook on libr15's `RequestPermissions` slot (JUMP_SLOT 0x36e4320; PLT 0xf19870; all three call sites go through it). The handler calls the original, then, on the FIRST call only (the boot request in `CR15Game::Initialize` at 0x11f9ce8; later calls come from script nodes `CRequestPermissionNode::Enter` 0x2722788 and must not block), checks the storage permissions in the array with the game's own `CheckPermission` (reached as libr15 base + 0xf87298 after the build id and first four instructions match) and, while one is denied, sleeps 500 ms and checks again, for at most 30 s. Then it returns, and the game goes on into `CncaGame::Initialize` with the grant in place.

**Predicted logcat on a fresh install, first launch, when it passes.**
1. `RAD: Echo VR`, `Checking android permission: 'android.permission.RECORD_AUDIO' (Result: -1)`, `Requesting 3 android permissions` (unchanged).
2. `NEVR-Sentinel` line `{"event":"permission_wait","state":"waiting","permissions":2}`, then repeated `RAD: Checking android permission: 'android.permission.READ_EXTERNAL_STORAGE' (Result: -1)` every 500 ms (the game's own CheckPermission logs each probe; one probe checks READ first and stops at the first denial), during which `GrantPermissionsActivity` starts (about 100-300 ms after the request) and the user grants.
3. The probes turn to `READ_EXTERNAL_STORAGE (Result: 0)` followed by `WRITE_EXTERNAL_STORAGE (Result: 0)`, then `{"event":"permission_wait","state":"granted","waited_ms":N}` with N about the time the user takes (seconds).
4. Then the normal path: `Checking ... WRITE_EXTERNAL_STORAGE (Result: 0)` from CreateLogFile, no `Failed to find package manifest file`, no `Initialization Error`, no `wm_finish_activity ... app-request`, and `Displayed com.readyatdawn.r15/...MainActivity`, then the main menu.
Install line at start: `{"event":"permission_wait_install","result":"installed"}` and `install_permission_wait` ok in the constructor report (steps_ok 20 where it was 19).

**What would prove it wrong.**
- `permission_wait` `state":"timeout"` (the grant never became visible to `checkSelfPermission` in this process while the thread waited), or the grants logged (14:06:55-56 style) and the probes still `Result: -1`.
- `Initialization Error` / `Failed to find package manifest` after a `granted` line (then the permission is not what blocks the manifest read).
- The process killed while waiting (a `Start proc` for the same package after the grant, no `granted` line): Android restarts a process that is granted a storage permission with a new group id; then the first launch cannot succeed in the same process, and the fix is not this wait (the next launch is clean by #621, and the wait only keeps the game alive until then).
- No GrantPermissionsActivity while the thread waits (the system waits for our activity's onPause, which the native-activity glue may only complete when the app thread pumps its looper; this thread does not pump during the wait). A dialog that never appears, with `waiting` lines running to the 30 s timeout, shows that.
- `permission_wait` lines for a second request (a script node) means the one-shot did not hold.

**Server change needed:** none.
**Not established:** that the grant is visible to `checkSelfPermission` while the app thread is blocked; whether the glue's onPause waits on this thread; whether `FileExists` fails for the permission rather than for another reason; the case where RECORD_AUDIO is granted and storage is not (no request is made at all; handled by the `CheckPermission` hook, ADR 0013). A headset run on a fresh install (uninstall, install, first launch) decides it.

## Reference build (#1737 C2)

The community patch for the reference build (EchoTools/echoApkPatch, `configs/libr15.so.yaml`) replaces the first word of
`CSysInfo::RequestPermissions` at `0xf873ec` with `ret` (`ff0301d1` becomes `c0035fd6`). Its stated reason: the request
carries no request code and aborts the process, and game data needs no storage permission once the Datadir points at the
app-owned media root. That behavior is the storage half of this decision; the request itself is ours (above).

Decision: one hook covers both cases, and the engine's root chooses between them (`data_wiring::StorageNeeded`, true when the
engine root is the legacy shared-storage root):

- Storage needed: every name is asked with our code, and the boot request waits for the storage grant (as above).
- Storage not needed: the storage names are dropped from the request. The microphone (`RECORD_AUDIO`) is still asked when
  it is named. When no name is left, nothing is asked. Nothing waits. This keeps the reference build's "no storage prompt"
  and deliberately keeps its microphone prompt, which the patch drops.

Two departures from the reference build's `ret`, both deliberate:

1. The microphone prompt stays. The `ret` also drops `RECORD_AUDIO`; this hook still asks for it when it is named.
2. The media-root boot request asks no storage permission, so the write grant that `/sdcard/r14logs` would need is not
   requested at boot. The reference build asks nothing at boot either. The difference is that this is decided per launch
   by the engine root: a legacy-root launch still asks for storage and waits for the grant.

The first word of the game's own function decides what its fallback does (`ClassifyOriginalWord`, logged once at install as
`permission_wait_original`). The stock word (`ff0301d1`) is the function that calls Java. The return word (`c0035fd6`) is
the reference build's `ret`: when the requester cannot make the call and the function is that return, nothing is asked and
nothing is waited on. An unknown word is treated as stock, with a WARN that carries the address and both expected words.

| Case | Stock (first word `ff0301d1`) | Reference build (first word `c0035fd6`) |
| --- | --- | --- |
| Storage needed, requester makes the call | Our request with all names, then the wait (30 s cap) | Same |
| Storage needed, requester cannot make the call | The game's request, then the wait | The original is a return: no request, no wait; `path` `game_noop` |
| Storage not needed (media root) | Storage names dropped; the microphone is asked if named; no wait | Same |
| Storage not needed, nothing left to ask | No request; `path` `none`; one probe of the storage | Same |

The probe in the last two rows is the boot request's only check of the grant. It logs `permission_wait` with
`state` `not_requested` and `reason` `storage_not_needed`, `nothing_to_ask` or `original_no_op`.

Headset expectations for the first fresh-install run, not yet observed:

- `permission_wait_original` with `kind` `calls_java` on stock and `returns_at_once` on the reference build.
- `permission_request` with `path` `ours` on both, and `storage_needed` `no` on a media-root launch.
- No `permission_wait` `waiting` line on a media-root launch, and no 30 s stall.
- On a legacy-root launch without the grant: the `waiting` line and the grant as in the prediction above.

## Not claimed

That the wait makes the first launch work on a headset: the closing question of the prediction (whether the grant is visible to the
blocked thread, and whether the dialog appears) is what the run decides.
