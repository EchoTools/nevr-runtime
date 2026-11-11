# Frozen-frame exit

Component `frozen_frame_exit`: the design record for the frame-submit owner, the in-VR signal and the timer source. It
contains no code. No GitHub issue tracks it yet.

## What it hooks

The behaviour. Upstream (`kotorvr/echoquest-combat` at `c5a67e8`, `runtime/native/afkxr/afk_capture.cpp`, namespace `hangwd`)
ends the activity when a headset session is in VR and the game stops submitting frames. The rule measures the gap between the
starts of consecutive `vrapi_SubmitFrame2` calls:

- Armed. `Beat`, called first in the `vrapi_SubmitFrame2` wrapper, stamps `CLOCK_MONOTONIC` when no stamp is held. `Enter`,
  called from the `vrapi_EnterVrMode` wrapper when it returns a session, clears the stamp, so the first submit after entry
  arms the timer.
- Disarmed. `Leave`, called from the `vrapi_LeaveVrMode` wrapper, clears the in-VR flag and the stamp.
- Bound. The watcher ends the process with `_exit(0)` when the gap since the stamp reaches `g_sec`: 30 by default, read from
  `hangwd.sec` in the range 10 to 900. It reads the stamp only while the in-VR flag is set.
- Upstream's comment above `namespace hangwd` says a cold lobby load can stall the frame for 7 to 11 minutes, and that clean
  gaps were at most 12 s. Upstream's own figures; not re-measured here.

### The slot and its owner

- Slot: `vrapi_SubmitFrame2`, the libr15 JUMP_SLOT pinned as `LibR15SubmitFrame2()` in `src/quest/sentinel/pinned_targets.h`
  (ELF `0x36e7010`, VA `0x37e7010`). The `libr15.so` disassembly's import list gives the same slot at VA `0x37e7010`.
- Owner: the render-configuration hook, `src/quest/sentinel/render_config_hook.cpp`. `RealInstall` case 4 (`:148`) installs the
  slot. Its handler `OnSubmitFrame` (`:109`) counts frames and calls the original. The module is installed by `Install`, reached
  from `InstallRenderConfigHook` (`src/quest/sentinel/entry.cpp`), from the production step `InstallRenderConfig`
  (`src/quest/integration/production_steps.cpp`), as step `kInstallRenderConfig` in `src/quest/integration/ctor_sequence.cpp`.
  That step runs on every launch with no counters.
- No other module installs this slot. `git grep -n LibR15SubmitFrame2 -- src/quest` finds the pin, the one installer, and
  the tests.

Decision (ADR 0039, one owner per behaviour): the existing owner gains an observer call. The component adds one call to its
observer inside `OnSubmitFrame`, before the original call, at the place the frame count is taken. The observer changes neither
the arguments nor the result. It adds no slot and no chain entry. It is `noexcept`, allocates nothing and throws nothing
(`callback_thunk.h` rule 1, as `render_config_hook.cpp` states it). The exit belongs to the watchdog thread, not to the handler, so
the handler stays observe-only.

Arena. The slot is installed on every launch, arena included. Combat README rule 9 states the gate for a hook on a slot that
arena's scripts also call: the hook tests combat scope first and, outside a combat level, calls the original and does nothing
else. The observer is the same kind of hook on the same kind of slot, so it takes the same gate. Which predicate it tests
(`scope::SeenCombatOrLobby()` or `scope::SeenCombatMap()`) is not decided here.

Service-only file. The comment above `kServiceOnlySkipped` in `src/quest/sentinel/libr15_attach.cpp` lists `render_config`
among the hooks that do not install on the pinned service-only libr15, and says each logs `build_id_mismatch`. The reason
is in the code: a non-service-side GOT target accepts only its own build ID or an alias (`src/quest/sentinel/got_hook.cpp`,
`BuildIdAccepted`), and the alias returns the patched ID only for the verified file
(`src/quest/integration/libr15_attach_step.cpp`, `BuildIdAliasForPlugins`). Read from the code, not run. On that file nothing
holds the slot. A component that took the slot would, by the same reading, not be service-side and be refused as well. What
the component does on that file is not decided here.

### The in-VR signal

The game's own session word: a global in `libr15.so` that the `NRadEngine::CVR` functions read and write (VA in its
disassembly; VA = ELF + `0x100000`):

- `DAT_038a33f0`, VA `0x38a33f0` (ELF `0x37a33f0`). Non-zero means in VR mode. Not pinned in this tree: neither address
  appears in `src/quest` or `docs`. It is a disassembly lead: nothing pins it and no pinned test checks it.
- Set in `NRadEngine::CVR::UpdateTracking` (VA `0x1b71b60`). The call to `vrapi_EnterVrMode` (thunk `0xff59d0`) is at
  `0x1b71c84`, and its return is stored at `0x1b71c88`. A zero return logs `vrapi_EnterVrMode failed` and leaves the word zero.
- Cleared in `UpdateTracking` after `vrapi_LeaveVrMode` (thunk `0xfdc2b0`, call at `0x1b71cb0`), the store at `0x1b71cb4`.
- Cleared in `NRadEngine::CVR::ShutdownGraphics` (VA `0x1b712b0`), the store at `0x1b712d0`, after the call at `0x1b712cc`.
- The word has 67 references in the disassembly: 3 writes, as above, and 64 reads.

Upstream's flag is set and cleared by the same two VrApi events: `EnterVrMode` returning a session, and `LeaveVrMode`. The game
word tracks those events. Reading the word and observing the calls differ only in where the state comes from.

The frame-submit gate, which the observer sees:

- `NRadEngine::DoVRSubmission` (VA `0x1b707bc`) calls `vrapi_SubmitFrame2` (call at `0x1b70954`, thunk `0x101f250`) only when
  `DAT_038a33f0` and `DAT_038a4160` are both non-zero.
- `DAT_038a4160` is set to `CSysJava::SessionActive()` in `NRadEngine::CVR::EndFrame` (VA `0x1b72de8`), which calls
  `DoVRSubmission`.
- So a frame in VR with `SessionActive` zero makes no `SubmitFrame2` call, and the observer's stamp does not move on it. How
  long that can last in a run is not measured here; it must be measured before the rule's false-exit rate is claimed.

### The timer source

`CLOCK_MONOTONIC`, read with `clock_gettime`, as upstream's `NowNs`. This tree's render-configuration poller
(`src/quest/sentinel/render_config_hook.cpp`, `StartPoller`) already waits on a condition variable whose clock is
`CLOCK_MONOTONIC` (`pthread_condattr_setclock`), with deadlines from `clock_gettime(CLOCK_MONOTONIC)` (`PollerMain`).
Upstream's watcher polls with `sleep(1)`. The C++ addendum forbids `Sleep` for timing, so the component waits on a condition
variable with a deadline.

### Not decided here

For the owner and the implementing unit:

1. The scope predicate for the observer: `SeenCombatOrLobby()` or `SeenCombatMap()`. Upstream's comment ties its stall to a
   cold lobby load.
2. How the in-VR word is read. Either a pinned read of the game word, which needs its own `*_targets.h` pin and a pinned test,
   or new hooks on the two VrApi mode slots: `vrapi_EnterVrMode` (VA `0x37d23d0`) and `vrapi_LeaveVrMode` (VA `0x37c5840`),
   each a slot in `hook_budget.h`.
3. The APK scope: the combat APK only, or every APK.
4. What the component does on the service-only file.
5. Whether the in-VR word is pinned, and when. Until it is, it is a lead.
