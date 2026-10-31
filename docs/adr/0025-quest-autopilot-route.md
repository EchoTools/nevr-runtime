# ADR 0025: a Quest autopilot that drives the local player through a scripted route

Status: Proposed. Tracking issue: #1202.

The route has not yet run on a headset. One fact is measured: on a Quest 2 (Horizon OS, Android 14) with no controller
connected, the system refuses to start the game (`RequiresControllersLaunchInterceptor`; see "Starting and stopping a
run"). Every other statement below about the headset, the controllers, the proximity sensor or OVR Metrics is a
prediction, and the "Predictions" table is what the first headset run checks.

## Context

Quest performance is profiled today by a person wearing the headset. No two runs get the same input, so two builds
cannot be compared like for like. The goal is a run that needs nobody: the same route, of the same duration, every
time. It is started through adb and ends when the route ends. It logs the route's start and end with timestamps, so
the OVR Metrics CSV can be cut to exactly that window.

What the tree and the binary give us (pinned `libr15.so`, build ID `b243509c08ce677aeb95fa348016949b3fc45230`,
`src/quest/sentinel/pinned_targets.h`, `kLibR15StockBuildId`):

- **The game reads controller input through one import.** `readelf -rW` lists `vrapi_GetCurrentInputState` as
  `R_AARCH64_JUMP_SLOT` at 0x36ec0f0. Its PLT stub (0xf29410) has one caller (`disasm_callers 0x1029410`, disassembly address =
  pinned VA + 0x100000), in `NRadEngine::CVR::PollTracking(unsigned int)` (0x1a7142c). Per frame, that function
  enumerates the input devices (`vrapi_EnumerateInputDevices`, call at 0x1a71840) and keeps those of type 4 (`cmp w8, #0x4`
  at 0x1a718e4, `ovrControllerType_TrackedRemote`). It reads each device's capabilities (`vrapi_GetInputDeviceCapabilities`,
  0x1a718fc) and takes the hand from the left-hand bit (`eor w8, w9, w8, lsr #2; and x29, x8, #0x1`, 0x1a71914: left 0,
  otherwise 1). Then it calls `vrapi_GetCurrentInputState(ovr, deviceId, cvr + 0x300 + hand * 0x48)` (0x1a7191c-0x1a71928).
- **The layout.** The stride 0x48 (72) is `sizeof(ovrInputStateTrackedRemote)` in the public Oculus Mobile SDK
  header VrApi_Input.h (SDK API 1.1.15 and later). That layout is a 16-byte header (`ControllerType`,
  `TimeInSeconds`), then `Buttons` at 16, `TrackpadStatus` 20, `TrackpadPosition` 24, battery and recenter bytes 32, `IndexTrigger` 36,
  `GripTrigger` 40, `Touches` 44, `Joystick` 52 and `JoystickNoDeadZone` 60, padded to 72. The game's capabilities buffer
  (sp+0x18 to sp+0x48, 48 bytes) is `sizeof(ovrInputTrackedRemoteCapabilities)`. `ControllerCapabilities` sits at offset 8,
  the word the game reads at sp+0x20. Two independent measurements agree with the header.
- **The game's own input playback is inert in this build.** `CVR::PlaybackInput` (0x1a6f2a0) stores mode 2 and a
  buffer pointer at 0x37a3378 and 0x37a33d8. In the whole disassembly (`llvm-objdump -d`), the only instructions that
  reference 0x37a3378 are the four setters and getters at 0x1a6f274-0x1a6f2c8. Nothing reads the buffer. A debug
  recorder (`CR15NetDebugInputRecorderCS::StartInputPlaybackCB`, 0x21409c8) calls the setter, and nothing consumes what
  it sets.
- **A plugin cannot hook today.** The Quest plugin loader (ADR 0019 slice 1, #1112) gives a plugin `base_addr` and
  `game_build_id`, and no hook service (`sdk/include/nevr/plugin_interface.h`). ADR 0019 section 5 keeps the hook machinery
  in the host. Its slice 2, a general GOT install for plugins, is not built.
- **No code page is written at run time** (#1127, #1132: `avc: denied { execmod }` on current Horizon OS). A GOT slot is a
  data word: `.got` (0x36bf480) lies inside `PT_GNU_RELRO` (0x35f9eb0, size 0x132150). The sentinel already writes such
  slots through `GotHook`, for example the `vrapi_GetSystemPropertyInt` slot at 0x36fa728.
- **No hook owns the input slots.** `git grep` for `GetCurrentInputState`, `GetInputDeviceCapabilities`, `0x36ec0f0`
  and `0x36fc9c8` finds nothing.

## Decision

1. **The seam is libr15's `vrapi_GetCurrentInputState` GOT slot.** The host owns it, not the plugin. A recorded
   `CallbackThunk` (`src/quest/sentinel/callback_thunk.h`) is installed through `InstallThunk`, pinned by build ID and
   link-time slot (0x36ec0f0). The handler calls the original and then gives the state to a registered filter.
   Nothing else is hooked: no pose, no head tracking, no code page.
2. **Plugins reach it through one host call, not a general hook API.** A Quest tail field in `NvrGameContext`,
   `const NevrQuestHostApi* quest`, is gated by `ctx_size`. It is additive and needs no version bump (ADR 0019, Q9).
   `NevrQuestHostApi` has its own `struct_size` and one function in v1, `register_input_filter(plugin_id, filter,
   user)`. A filter gets one `NevrQuestControllerState`: hand, buttons, touches, index trigger, grip trigger and stick.
   It is a neutral struct, so a plugin never sees the VrApi layout. A general GOT install stays ADR 0019 slice 2. The
   narrower service also keeps ADR 0019's "one owner per slot, no chaining" rule while leaving the input slot open to
   any plugin, one at a time.
3. **Who may register.** Development builds only: `nevr_build::kIsDev`, false only in a CI build of a release tag
   (`cmake/nevr_build_info.cmake`). A release build refuses with `not_dev_build`. One filter per process: a second
   registration is refused with `owned_by=plugin:<first>`. The hook is installed at the first accepted registration,
   never before, so a run whose config names no such plugin has no hook on the slot.
4. **The translation is the host's and leaves the game's bytes alone.** The host copies the six fields out of the
   VrApi state, calls the filter, and writes them back. A changed stick writes `Joystick` and `JoystickNoDeadZone`
   together; an unchanged one leaves both pairs as VrApi wrote them. A filter that changes nothing leaves the 72 bytes
   exactly as VrApi wrote them.
   The hand comes from the device's capabilities, which the handler reads with the game's own resolved
   `vrapi_GetInputDeviceCapabilities` (libr15's slot 0x36fc9c8, read and never written). It is cached per device ID. A
   device that reports neither hand bit is passed to the filter as `NEVR_QUEST_HAND_UNKNOWN`.
5. **The plugin is `nevr_autopilot` (`libnevr_autopilot.so`), built for Quest only and carried only in an APK built
   for it.** A plugin packaged in an APK loads by default (ADR 0019 R2), so the recipe packages this one
   off (`android-repack-apk`'s `plugin_off` argument): it loads only when `config.yaml` names it in `plugins:`, because
   its init refuses a run with no `args.route`. No recipe that builds a release or `package-dev` APK carries it
   (`android-repack-apk`'s `plugin` argument). `just quest-autopilot-apk` is the one recipe that does. The recipe refuses a plugin whose build-ID note does not list the store APK's own `libr15.so`
   build ID (`tools/quest_repack_libs.py check-plugin-build-ids`), so an APK whose plugin the loader would refuse is
   never signed.
6. **The filter never logs, allocates, locks or calls into the game.** It reads `CLOCK_MONOTONIC`, sets the state,
   and puts events into a fixed ring with their timestamps taken at the frame they happen, then wakes the reporter
   with one non-blocking write to an eventfd. A reporter thread, started in the plugin's init, waits on that eventfd
   and writes the events through `ctx->log`. The thread ends after `route_end`, or at `NvrPluginShutdown`,
   which wakes and joins it. Events the ring cannot hold are counted, and `route_end` reports the count. The host has
   no way to remove a filter, so the run a registered filter points at is never freed: `NvrPluginShutdown` ends it,
   and from then on the filter changes nothing.

### The route file

A small text file, one directive per line, `#` to the end of a line is a comment, blank lines ignored. UTF-8 is not
needed; the parser accepts printable ASCII only and refuses anything else with the line number.

```
autopilot-route 1          # first directive: the format and its version
name lobby_loop            # [a-z0-9_]{1,32}; logged with every event
start_after 5              # seconds from the start point to the first step (0 to 600)
repeat 3                   # play the step list this many times (1 to 1000; default 1)
step 2.0                                      # every control at rest
step 3.0 left.stick=0,1                       # left stick fully forward
step 1.5 right.stick=1,0                      # right stick fully right
step 2.0 left.stick=0,1 right.trigger=1       # two controls at once
step 0.5 right.grip=1 right.buttons=a+b       # grip held, A and B held
```

- `step <seconds> [control=value ...]`: the whole controller state for that long, 0.01 to 600 seconds. A control the
  step does not name is at rest (0, released). At most 256 steps; the whole route (start delay aside) at most 3600
  seconds; the file at most 64 KiB.
- Controls: `left.` or `right.` followed by `stick=x,y` (each -1 to 1), `trigger=v` and `grip=v` (0 to 1), and
  `buttons=` a `+`-joined set. The left hand may hold `x`, `y`, `thumb` and `menu`; the right `a`, `b` and `thumb`
  (the SDK header VrApi_Input.h, `ovrButton`: A 0x1, B 0x2, RThumb 0x4, X 0x100, Y 0x200, LThumb 0x400, Enter
  0x100000, the menu button on the left controller). A button named on the wrong hand is a refusal.
- What the plugin derives, so the state reads like a controller in a hand: `trigger >= 0.5` adds `ovrButton_Trigger`
  (0x20000000), `grip >= 0.5` adds `ovrButton_GripTrigger` (0x04000000), `thumb` adds `ovrButton_Joystick`
  (0x80000000) with the hand's thumb bit. Touches follow the same header (`ovrTouch`): a pressed button touches its
  key, a trigger above 0 touches the trigger, and a stick off centre or a pressed `thumb` touches the stick and the
  hand's thumb. A hand with no thumb touch reports `ThumbUp` (0x100), and one with no trigger touch reports
  `IndexPointing` (0x200), as the header says an untouched controller does. The 0.5 thresholds are this ADR's
  choice. The game's own reading of each control (which one moves, turns, boosts or grabs) is a prediction (P6).
- Any error refuses the whole file: an unknown directive, a missing or misplaced `autopilot-route 1`, a value out of
  range, a malformed number. The plugin's init then returns non-zero, the loader records `init_failed`, and the error
  line names the file line and the reason. A route never runs half parsed.

### Starting and stopping a run

No person is involved after a one-time setup of `config.yaml` on the headset, and with both controllers on and
connected. They don't have to be held. Horizon OS starts this app only when a controller is connected: otherwise
`am start` brings up the system's controller-required dialog instead (logcat:
`ActivityLaunchInterceptorController: RequiresControllersLaunchInterceptor: com.readyatdawn.r15/...`), and the game
process never starts.

```yaml
plugins:
  - name: nevr_autopilot
    args:
      route: /sdcard/Android/data/com.readyatdawn.r15/files/autopilot/route.txt
      start_file: /sdcard/Android/data/com.readyatdawn.r15/files/autopilot/start   # optional
```

- **Start point.** Without `start_file`: the first frame the filter sees. With it: the first frame after the reporter
  thread sees the file exist (it checks every 100 ms while it waits; no event source on that storage is measured, so
  a check is used). `start_after` counts from the start point. The trigger lets the runner start a route at a game
  event it sees in logcat, such as a lobby load, instead of at a fixed time after boot.
- **Clock.** `CLOCK_MONOTONIC`, read in the filter. A step boundary takes effect at the first input frame at or after
  it, so every boundary is late by less than one frame. The route ends at the first frame at or after its planned
  end; from then on the filter changes nothing.
- **The runner** (`tools/quest_autopilot_run.py`, through adb) checks that the headset's `config.yaml` names the
  plugin and the route path. It pushes the route and removes a stale start file. It sends
  `am broadcast -a com.oculus.vrpowermanager.prox_close` (Meta's scriptable-testing guide: the headset acts as worn),
  force-stops the game, clears logcat, and starts `com.readyatdawn.r15/com.oculus.gles3jni.MainActivity`. With
  `--start-on <regex>` it creates the start file when a logcat line matches. It waits for `route_end` (or its timeout,
  or the launch check's line for this app, which ends the run at once as `launch_blocked`),
  force-stops the game, sends `com.oculus.vrpowermanager.automation_disable` to give the sensor back, and saves the
  logcat and `nevr-sentinel.log`. It writes one JSON record per step, plus a summary that carries the `route_start`
  and `route_end` fields, to `runs.jsonl` in its output directory under the work root. It exits 0 only when
  `route_end` arrived and every hand the route drives has a non-zero `frames_driven`. `just quest-autopilot-run <route>`
  runs it. The routes the repo ships are in `tools/quest_autopilot_routes/` (`lobby_loop.txt`: walk, turn, walk back
  and turn, 120 s), and `just test-quest-autopilot` parses each one.

### Logging

Through `ctx->log`, so lines are `[NEVR.PLUGIN] plugin=nevr_autopilot <message> key=value ...` in logcat (tag
`NEVR-Sentinel`) and in `nevr-sentinel.log` (ADR 0019 section 10).

| message | level | when | fields |
| --- | --- | --- | --- |
| `route_loaded` | Info | init | `route` (the path), `name`, `sha256` of the file bytes, `steps`, `repeat`, `planned_s`, `start_after_s`, `start` (`first_frame` or `start_file`), `drives` (`left`, `right`, `left+right` or `none`) |
| `route_refused` | Error | init | `route`, `line` (0 for the args, the file or a whole-file limit), `reason` |
| `input_filter` | Info/Error | init | `result`: `registered`, the host's refusal token (`bad_argument`, `not_dev_build`, `owned`, `hook_refused`), or `no_host_api` (a context without `quest`), `no_wake_fd` or `no_reporter_thread` (each with `errno`) |
| `start_file_seen` | Info | the start file exists | `start_file`, `realtime_ms` |
| `route_start` | Info | first step | `name`, `realtime_ms` (`CLOCK_REALTIME`, Unix ms), `monotonic_ns`, `boottime_ns` |
| `route_step` | Debug | each step | `index`, `pass`, `realtime_ms`, `monotonic_ns` |
| `route_end` | Info | end | `name`, `realtime_ms`, `monotonic_ns`, `boottime_ns`, `planned_s`, `measured_s`, `frames_left`, `frames_right`, `frames_unknown_hand`, `frames_driven_left`, `frames_driven_right`, `max_frame_gap_ms`, `events_dropped` |
| `route_stopped` | Warning | shutdown before the end | `name`, `started`, `realtime_ms`, `frames_driven_left`, `frames_driven_right` |
| `reporter_end` | Debug | the reporter thread returns | `reason`: `route_end` or `stopped` |

`frames_*` count the polls inside the route, from its first step to its end; a hand the route never names is counted
and not driven. `measured_s` runs from `route_start` to `route_end`; `max_frame_gap_ms` is the longest time between two
polls of one hand inside the route.

The host logs one line for each registration, to the sentinel log:
`{"event":"input_seam","result":"installed|refused","reason":"ok|bad_argument|not_dev_build|owned|caps_unresolved|hook_refused","plugin":"<id>","slot":"vrapi_GetCurrentInputState"}`,
with `"status":"<GotStatus>"` when a GOT install ran and `"owned_by":"plugin:<id>"` on `owned`. `caps_unresolved` (the
capabilities slot did not resolve) is reported to the plugin as `hook_refused`.

### Predictions (written before any headset run)

| ID | Prediction | Falsifier |
| --- | --- | --- |
| P1 | The dev APK with the plugin and the config above logs `plugin_load name=nevr_autopilot outcome=loaded`, `route_loaded`, `input_filter result=registered` and the host's `input_seam result=installed status=ok`. This is the only check of the production install and of the capabilities-slot read (the host test injects both). | Any of the four lines is missing, or the install status is not `ok`. |
| P2 | With the proximity sensor overridden and nobody in the headset, the game keeps polling input: `route_end` has `frames_left` and `frames_right` above 0 and `max_frame_gap_ms` below 100. | A hand with 0 frames, or a gap of 100 ms or more (a controller that slept, or a paused game). |
| P3 | `measured_s - planned_s` is at least 0 and less than `max_frame_gap_ms / 1000` plus 0.02. | Outside that bound. |
| P4 | Two runs of the same route on the same build have the same `planned_s` and differ in `measured_s` by less than 0.05 s. | A larger difference. |
| P5 | `route_start.realtime_ms` and `route_end.realtime_ms` fall inside the OVR Metrics CSV's time range, and the CSV rows between them can be selected by its timestamp column. | The CSV has no wall-clock column, or its clock differs from `CLOCK_REALTIME` by more than one sample interval. |
| P6 | In the social lobby the left stick moves the player, the right stick turns, and the triggers and grips do what a player sees them do. Each is recorded on the headset sheet. | A control does nothing, or does something else; the route file is then rewritten, not the plugin. |
| P7 | A release build (kIsDev false) refuses the registration with `not_dev_build`, and installs nothing. Host-tested. | The hook installs on a release build. |
| P8 | The filter adds no frame-time cost visible in OVR Metrics: the same route with `nevr_autopilot` disabled versus enabled changes the median frame time by under 0.2 ms. | A larger difference. |

## Alternatives

- **The game's own playback (`CVR::PlaybackInput`).** Rejected: inert in this build (above). It stores a buffer that
  nothing reads.
- **A general GOT install for plugins (ADR 0019 slice 2) and the hook in the plugin.** Deferred, not rejected. It needs
  the personality check on plugin frames and the registry seeding of ADR 0019 section 9. A plugin-side handler would be
  live across the call into VrApi, so the frame contract would apply to plugin code that no sensor walks. The host
  thunk keeps every live frame inside the sentinel, where `TestHookFramesCarryNoPersonality` walks it, and the plugin's
  filter runs after the call returns.
- **Driving the game's own input layer (`CInputCS::DeviceInputState`, the per-device `InputState` functions).** Rejected:
  those are internal calls with no import slot, so reaching them needs an inline hook or a code write, which #1132
  forbids.
- **Controller and head poses (`vrapi_GetInputTrackingState`, `vrapi_GetPredictedTracking2`).** Not in v1. The route
  moves the player with the sticks and buttons; the poses are those of a headset and controllers lying still. A
  grab therefore grabs only what is within reach of a resting hand. Adding poses is another slot and another row in this
  table.
- **Start by a fixed delay after launch only.** Kept as the default, but boot time includes login and network, so a
  route that must start in the lobby uses the start file.

## Consequences

- The plugin ABI gains `NevrQuestHostApi` and `NevrQuestControllerState`, additive. A PC host's context is zeroed
  before it is filled, so `quest` is null there.
- A development build carries one more hook, installed only when a plugin registers. A release build cannot install it.
- The route drives sticks, triggers and buttons, not poses. A route that needs a reach or a throw needs P6's findings
  and possibly the pose slot.
- Performance runs become repeatable only as far as the game is: matchmaking, other players and the network still
  differ between runs. A lobby with no other players is the like-for-like case.
