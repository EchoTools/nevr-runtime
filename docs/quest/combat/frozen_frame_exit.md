# Frozen-frame exit

A combat game whose frames stop for `combat.frozen_frame_exit_s` seconds (default 30) while the headset is in VR is ended, so the player
lands in Home instead of a frozen view (#2377, #1737). The module is `src/quest/combat/frozen_frame_exit.h`; its pins are
`src/quest/combat/frozen_frame_exit_targets.h`, checked by `src/quest/tests/frozen_frame_exit_pinned_test.cpp`; its host test is
`src/quest/tests/frozen_frame_exit_test.cpp`.

## Status of each statement

- Measured: read in this repository's tree, or in the disassembly of `libr15.so`, with the file, line or address given.
- Inferred: derived from measured facts. The step of the inference is named.
- Chosen: a reversible choice of the implementing unit, with its reason, under "Choices" at the end.

## Origin

The hangwd watchdog of kotorvr/echoquest-combat, runtime/native/afkxr/afk_capture.cpp (namespace `hangwd`), read as text and rebuilt on this
tree's owners: the frame submit is the render-configuration hook's, VR mode is the game's own word (no VrApi mode hook), and the end is the
QUIT's activity finish followed by `process_end::EndWithExitCode`. No line of upstream's code is copied.

## Switch and default

- `combat.frozen_frame_exit`, bool, default **true**. Upstream's hangwd is on by default too: its install turns it off only with a `hangwd.off`
  file or the property `debug.echoquestcombat.hangwd` set to 0, and its wave 13 release notes list it as a feature of the build. So the default is
  parity. What differs is the scope, the choice D8 (a) recorded on #2377: the combat APK only. The default stays on only because the headset row
  below, including its pause cases, is the release gate for the first release that carries the module: a release is not cut until that row passes.
- `combat.frozen_frame_exit_s`, integer seconds, 10..900, default 30 (upstream's bound and range). Anything else: the default, with the
  configuration step's Warning naming the key.
- Which APKs: the combat variants, where `combat::Active()` is true; the row runs from the combat install table's normal step, after
  `scope`. Its switch is one of the readers that keep the `scope` row on (`ScopeNormal()` in `src/quest/combat/combat_install.cpp`).

## What it does

1. Each frame submit, the render-configuration hook's `OnSubmitFrame` calls `frozen_frame_exit::NoteSubmit()` before the original. Outside a
   combat map or the combat lobby (`scope::SeenCombatOrLobby()` false) or when the module is not installed it returns at once. Otherwise it
   stamps the submit's `CLOCK_MONOTONIC` time; the first stamp after a disarm arms the watch.
2. A watchdog thread, `nevr-frozenframe`, wakes once a second on a condition variable with a `CLOCK_MONOTONIC` deadline. Out of VR (the in-VR
   word is zero) or out of a combat level it puts the stamp back to zero (disarmed). Otherwise, when more than the limit has passed since the
   stamp, it reads the word and the stamp once more, and when they still say so it notes the gap (its stamp and the time) and goes on. A later
   tick at least `kConfirmMs` (1 s) after that, on the same stamp (no submit since), that still sees the gap past the limit fires, once per
   process. So the watchdog fires at the limit plus one second at the earliest:
   1. the Warning `combat_frozen_frame_exit`;
   2. `quit::FinishActivity()`, the end the menu's QUIT takes, when there is an activity to finish;
   3. a 5 s grace for the game to end itself that way;
   4. the Warning `combat_frozen_frame_exit_end`, then `process_end::EndWithExitCode(1, "frozen_frame")`, which writes the
      `android_main_returned` record and runs each combat module's end line before `_exit(1)`.
3. Once the process is ending the watch is off for good: the watchdog checks at every tick (and once more before it writes the fired record)
   whether a QUIT or `FinishActivity` has begun finishing the activity (`quit::Finishing()`), `android_main` has returned or `EndWithExitCode`
   has begun (`process_end::Ending()`), or the OS has asked the game to close (`android_app::destroyRequested`, at the glue's offset
   `kAppDestroyRequested`, set when the activity is destroyed). Any of them writes `combat_frozen_frame_exit_halted` once, clears the stamp and
   keeps later submits from arming it again. So a QUIT or an OS shutdown that stalls for longer than the limit with the in-VR word still set ends
   with the game's own exit code and no `combat_frozen_frame_exit`, instead of a false record and exit code 1. Nothing is hooked for this; the
   `destroyRequested` word is the glue's (`src/quest/sentinel/android_glue_layout.h`), read from the app `process_end` kept, and it is set on the
   game's thread, so an OS shutdown that stalls that thread before it handles the destroy is not seen (inferred: the glue sets it while the game
   thread handles `APP_CMD_DESTROY`).
4. At the end of the process the row's `end` stops and joins the watchdog, unless the watchdog is the one ending the process, writes
   `combat_frozen_frame_exit_halted` (cause `note_end`) when none was written, and writes the counts line.

The scope is read without its freshness test in the watchdog, because a freeze also stops the combat tick that refreshes it (`kStaleAfterMs`,
`src/quest/combat/combat_scope.h`). The fire path is proven by the host test only (fake clock, finish, grace and end); no headset run has
forced a freeze.

## Log lines

Sentinel log, one JSON record each:

- `combat_frozen_frame_exit_install`: `result` (`installed`, `off`, `refused`), `status` (`ok`, `off_config`, `scope_not_live`,
  `submit_slot_not_held`, `word_unreadable` with `why`, `pinned_word_mismatch` with `pin`, `vaddr`, `found`, `expected`,
  `thread_not_started` with `errno`), `limit_s`, `limit_source` (`config`, `default`), `scope` (`combat_or_lobby`), `msg`. Info for installed
  and off, Error for a refusal.
- `combat_frozen_frame_exit`: Warning, once: `since_submit_ms`, `limit_s`, `scope_bits` (the level answer's bits: 1 combat map, 2 combat
  lobby), `submits`, `armed_ms`, `action` (`finish_activity`, `no_activity`), `msg`.
- `combat_frozen_frame_exit_end`: Warning, once, right before the exit: `grace_ms`, `action` (`end_process`), `msg`.
- `combat_frozen_frame_exit_halted`: Info, once: `cause` (`quit`, `process_end`, `destroy_requested`, `note_end`), `was_armed`, `submits`, `msg`.
- `combat_frozen_frame_exit_counts`: Info at the end of the process, installed only: `arms`, `submits`, `fired`, `halted`, `joined` (0 when the
  watchdog is ending the process), `msg`.

A submit writes no line: `NoteSubmit` runs in the submit handler's live frame (`src/quest/sentinel/callback_thunk.h` rule 1).

## Arena cost

Measured in the code. On the combat APK in an Echo Arena level, in an arena lobby or out of any level, `NoteSubmit` is one relaxed load of
the scope word and a branch per submit; no clock read, no write (the host test counts the clock reads: zero). The watchdog wakes once a
second, reads the in-VR word and the scope word, and disarms. A release APK never runs the row, so its scope word stays empty and the same
load and branch is all a submit pays. Inside a combat level a submit adds one clock read (vDSO), one atomic exchange and one atomic increment.
Not measured on a headset.

## Headset row

Combat APK, in the combat lobby and in a combat map, each with the headset in VR:

1. 30 minutes of normal play including combat-lobby loads and at least one combat match: the `combat_frozen_frame_exit_install` record with
   `installed`, no `combat_frozen_frame_exit` record, and the `combat_frozen_frame_exit_counts` line at the end with `fired` 0.
2. Headset off (taken off, or the proximity sensor covered) for at least 60 s, in the combat lobby and again in a combat map, then put on: no
   `combat_frozen_frame_exit` record, and the game draws again.
3. The system menu open (the Meta button) for at least 60 s, in the combat lobby and again in a combat map, then closed: the same result.
4. The `combat_frozen_frame_exit_counts` line at the end of each run with `fired` 0.

A pause is the false-exit case this row exists for. The in-VR word is cleared only by game code on the main thread (`UpdateTracking`, called from
the per-frame clock update, `frozen_frame_exit_targets.h`), so a pause that blocks that thread, inside `vrapi_SubmitFrame2` too, leaves the word
set and the watchdog counts the gap as a freeze; upstream's own comment records an unclassified 20.9 s gap in VR that it took for a pause, against
its 30 s limit. Whether a headset-off or system-menu hold keeps the thread blocked for 60 s, or leaves the word cleared, is not known: this row
measures it. If any of rows 2 and 3 produces a `combat_frozen_frame_exit` record, the default of `combat.frozen_frame_exit` is turned off before
the release, or a pause signal is added, and no release that carries the module is cut with the row failing. The row is owed before the first
release that carries the module (D8).

## What it hooks

### The upstream rule

Measured. The source is kotorvr/echoquest-combat, the file runtime/native/afkxr/afk_capture.cpp, namespace hangwd.

- `Beat`, called first in the `vrapi_SubmitFrame2` wrapper, stamps CLOCK_MONOTONIC when no stamp is held.
- `Enter`, called from the `vrapi_EnterVrMode` wrapper when it returns a session, sets the in-VR flag and clears the stamp. So
  the first submit after entry arms the timer.
- `Leave`, called from the `vrapi_LeaveVrMode` wrapper, clears the in-VR flag and the stamp.
- The watcher ends the process with `_exit(0)` when the gap since the stamp reaches the bound. The bound is 30 s by default,
  or the value read once from the hangwd.sec file, in the range 10 to 900. The watcher reads the stamp only while the in-VR
  flag is set. Its exit runs on the watcher thread, not in the slot handler.

Upstream's own figures, not re-measured here: its comment above namespace hangwd says a cold lobby load can stall the frame
for 7 to 11 minutes, that the gaps measured on clean legs were at most 12 s, and that three further gaps (20.9 s, 39 s and 46 s) were
unclassified; its closing note on the limit says the 39 s and 46 s ones were one hung run and the 20.9 s one a pause. So a pause of 20.9 s
was seen in VR against a 30 s limit, which is the margin this module's headset row (above) tests with holds of at least 60 s. Upstream
disarms on `LeaveVrMode`, which it wraps; here the game's own word is read, and that word is cleared only by game code on the main thread
(see "The in-VR signal").

### The slot and its owner

Measured.

- Slot: `vrapi_SubmitFrame2`, the libr15 JUMP_SLOT pinned as `LibR15SubmitFrame2()` in `src/quest/sentinel/pinned_targets.h`
  (ELF 0x36e7010, VA 0x37e7010). The import list of `libr15.so` gives the same slot at VA 0x37e7010. The only call to that
  thunk in `libr15.so` is at 0x1b70954, inside `NRadEngine::DoVRSubmission`.
- Owner: the render-configuration hook, `src/quest/sentinel/render_config_hook.cpp`. `RealInstall` case 4, at :148, installs
  the slot. Its handler `OnSubmitFrame`, at :109, adds one to `g_frames` and calls the original.
- Install chain: `Install` (`src/quest/sentinel/render_config_hook.cpp:533`) is reached from `InstallRenderConfigHook`
  (`src/quest/sentinel/entry.cpp:234`), from `InstallRenderConfig` (`src/quest/integration/production_steps.cpp:626`), as step
  `kInstallRenderConfig` (`src/quest/integration/ctor_sequence.cpp:312`). The comment at `ctor_sequence.cpp:310` says the step
  runs on every launch, with no counters.
- No other installer. `git grep -n LibR15SubmitFrame2 -- src/quest` finds the pin, this one installer, and the tests.

### Arena

Measured. The slot is installed on every launch, arena included (`ctor_sequence.cpp:310`). `OnSubmitFrame` runs on every
submit, arena included: it adds one to `g_frames` and then calls the original. The existing handler already does work in arena.

Measured. Combat README rule 9 (`src/quest/combat/README.md`) says a hook on a slot that arena's scripts also call tests
`scope::SeenCombatOrLobby()` or `scope::SeenCombatMap()` first. Outside a combat level it calls the game's function with the
same arguments and does nothing else: no read of the game's data, no write, no counter, no line.

Measured. The existing frame count does not sit behind that test, so on this slot the count already does not meet rule 9.

Inferred. Rule 9 names slots that arena's scripts call. The engine calls this slot from `DoVRSubmission` in arena too, so it
is the same kind of slot. The step is that `DoVRSubmission` runs in arena; that was not measured per level.

Decided here. Any observer added to `OnSubmitFrame` asks combat scope first, and outside a combat level it does nothing else.
The predicate is `scope::SeenCombatOrLobby()` (Choices, item 1); the existing frame count stays where it is, with its reason
(Choices, item 4).

### The route an observer takes

Measured. ADR 0039, Decision 2, gives a module that observes a call two routes. It may give the owner an input, either a setting
the owner reads or "a request through an interface the owner offers". It may also "observe the call through a handler in the
manager's chain (ADR 0034) that changes neither the arguments nor the result". The module takes the first: an input of the owner
(Choices, item 2).

Measured. Upstream calls `Beat` before the original call, in the `vrapi_SubmitFrame2` wrapper. `OnSubmitFrame` calls `NoteSubmit()`
before the original too (`src/quest/sentinel/render_config_hook.cpp`).

Measured. `src/quest/sentinel/callback_thunk.h` rule 1 reads, in full:

> A frame that is LIVE while game code runs under a hook must be personality-free ("zR"): no try/catch, no object with a
> destructor, no cleanup. Live frames are the thunk's Entry, the handler, and any sentinel function the handler calls and that
> is still on the stack when the handler calls `original` (or any other game code).

An observer call made in `OnSubmitFrame` before the original is in that handler's live frame, so the rule binds it.

### The service-only file

Measured in the code; not run. The comment above `kServiceOnlySkipped` in `src/quest/sentinel/libr15_attach.cpp` lists
`render_config` among the hooks that do not install on the pinned service-only libr15, and says each logs `build_id_mismatch`.
The reason is in the code. A non-service-side GOT target accepts only its own build ID or an alias
(`src/quest/sentinel/got_hook.cpp`, `BuildIdAccepted`). The alias returns the patched ID only for the verified file
(`src/quest/integration/libr15_attach_step.cpp`, `BuildIdAliasForPlugins`). On that file nothing holds the slot.

Inferred, by the same reading. A component that took the slot would not be service-side, so it would be refused as well. The
module takes no slot; on that file it refuses `submit_slot_not_held` (Choices, item 6).

### The in-VR signal

Measured, in the disassembly of `libr15.so`. VA = ELF + 0x100000.

- The word is at VA 0x38a33f0 (ELF 0x37a33f0). It is a 64-bit global. It holds the ovrMobile session pointer that
  `vrapi_EnterVrMode` returns. A non-null value means in VR mode.
  - The store at 0x1b71c88, `STR X0, [X24, #0]`, writes the EnterVrMode return into the word.
  - The load at 0x1b71ca8, `LDR X0, [X25, #0x3f0]`, puts the word into x0 for the LeaveVrMode call at 0x1b71cb0. So the word is
    the session that LeaveVrMode is given.
- Set in `NRadEngine::CVR::UpdateTracking` (VA 0x1b71b60). The call to `vrapi_EnterVrMode` (thunk 0xff59d0) is at 0x1b71c84.
  A zero return logs the game's own line, `vrapi_EnterVrMode failed`, and leaves the word zero (decompilation).
- Cleared in `UpdateTracking` after `vrapi_LeaveVrMode` (thunk 0xfdc2b0, call at 0x1b71cb0). The zero store is at 0x1b71cb4. The path is taken
  when `CSysJava::SessionActive()` returns 0 (call at 0x1b71c38, `cbz w0` at 0x1b71c3c) and also when it returns non-zero and
  `CSysJava::NativeWindow()` (call at 0x1b71c40, PLT stub 0xff3120, JUMP_SLOT ELF 0x36d0f78) returns a null window (`cbz x0` at 0x1b71c44):
  both branch to the load of the word at 0x1b71ca8. The pinned test checks the call word, the stub and slot, and both `cbz`.
- Cleared in `NRadEngine::CVR::ShutdownGraphics` (VA 0x1b712b0). The LeaveVrMode call is at 0x1b712cc and the zero store at
  0x1b712d0.
- The disassembly holds 67 references to the word: 3 writes (0x1b712d0, 0x1b71c88, 0x1b71cb4) and 64 reads.

Measured, section. The word is in `.bss`: the store file's section table gives `.bss` at ELF 0x3761180, size 0x713b8 (to 0x37d2538),
`SHT_NOBITS`, which contains 0x37a33f0, and the pinned test checks it.

Status: pinned. `src/quest/combat/frozen_frame_exit_targets.h` pins the word's address and its place in `.bss` (from the section
table: `.bss` 0x3761180, size 0x713b8), the writer (`adrp`, `add`, the EnterVrMode call, `str`) and both clearers (the SessionActive
test, the NativeWindow call and its null test, the LeaveVrMode calls and the zero stores in `UpdateTracking` and `ShutdownGraphics`); `src/quest/tests/frozen_frame_exit_pinned_test.cpp`
checks them against the store file. `ShutdownGraphics` has a byte-checked reconstruction; `UpdateTracking` and `DoVRSubmission` are not in the proven reconstruction tree
(not established), so what this page says they do beyond the pinned words is read from their disassembly.

Inferred. The word is cleared only by game code: `UpdateTracking` runs on the main thread, from the per-frame clock update (the call at ELF 0x11fd744 in
`CR15Game::UpdateClock`, ELF 0x11fd724, through a PLT stub; that the stub reaches `UpdateTracking` is read from the disassembly and not pinned), so if that thread is blocked (inside `vrapi_SubmitFrame2` too) while the headset is off or the system menu is
open, the word stays set. `UpdateTracking` has no byte-checked reconstruction (not established: the function is not in the proven reconstruction tree), so this rests on its disassembly and on
the pinned words, not on a whole reconstruction. That is the false-exit path the second look and the headset row's pause cases address.

Inferred, against upstream. Upstream's flag is set when EnterVrMode returns a session and cleared at LeaveVrMode. The word
follows the same two events: `libr15.so` has one call to `vrapi_EnterVrMode` and two calls to `vrapi_LeaveVrMode`, the ones
above. The word is written inside `UpdateTracking`, not in a wrapper, so the timing of the write differs from upstream's.

### The frame-submit gate

Measured.

- `NRadEngine::DoVRSubmission` (VA 0x1b707bc) calls `vrapi_SubmitFrame2` (call at 0x1b70954) only when the in-VR word and
  `DAT_038a4160` are both non-zero. The decompilation tests both before the call.
- `DAT_038a4160` (VA 0x38a4160) has three references, two of them writes:
  - 0x1b72e30 in `NRadEngine::CVR::EndFrame` (VA 0x1b72de8) stores `CSysJava::SessionActive()`, the callee named in the
    decompilation. `EndFrame` calls `DoVRSubmission`.
  - 0x1b7090c in `DoVRSubmission` reads it.
  - 0x1060b9c in `_INIT_213` (VA 0x1060928), a static initialiser, writes zero. It is a 16-byte store of a zeroed register,
    starting at 0x38a4160, so it writes the same word `EndFrame` writes, with zero.

Inferred. A frame in VR whose `SessionActive` is zero makes no `SubmitFrame2` call, so an observer that stamps on the call sees
nothing on that frame. How long that can last in a run is not measured. It must be measured before any false-exit rate is
claimed.

### The timer source

Measured. Upstream uses CLOCK_MONOTONIC through `clock_gettime` (its `NowNs`), and polls with `sleep(1)`. This tree's
render-configuration poller, `src/quest/sentinel/render_config_hook.cpp`, sets `pthread_condattr_setclock(CLOCK_MONOTONIC)`
(:388) and builds its deadlines from `clock_gettime(CLOCK_MONOTONIC)` (:360). The C++ addendum in AGENTS.md forbids `Sleep` for
timing.

Decided here. The clock is CLOCK_MONOTONIC, the clock the poller already uses. The wait is a condition variable with a deadline,
as the addendum requires.

### The second look

Chosen, and what it buys. The watchdog fires only when the gap is past the limit at one tick and still the same gap (the same stamp, no submit
since) at a tick at least one second later (`rules::Confirmed`, `kConfirmMs`), after the existing double read of the word and the stamp inside each
tick. It buys one thing: a pause that ends in the second after the limit (a headset put on again, a system menu closed) is not a freeze. It does
not tell a long pause from a freeze: a pause that outlasts the limit by a second is still ended. The remaining protection against a long pause
is the headset row's cases above, not this look.

## Choices

The module's choices, each reversible.

1. The scope predicate: `scope::SeenCombatOrLobby()`, unchecked for freshness. Upstream's comment ties its stall to a cold lobby load, so the
   combat lobby is in scope; the freshness test is left out because a freeze stops the tick that keeps the answer fresh.
2. The route under ADR 0039, Decision 2: an input of the slot's owner. `OnSubmitFrame` calls `NoteSubmit()` before the original, upstream's
   place for `Beat`, so a submit that blocks inside VrApi counts as frozen. No second handler joins the slot.
3. The in-VR word is read, not hooked: one aligned 8-byte atomic load at `image.base + 0x37a33f0`, after the install checked the patched
   libr15's build ID, that the word lies in a `PT_LOAD` segment, and the pinned words. No VrApi mode slot is taken (no `hook_budget.h` row).
4. The existing frame count in `OnSubmitFrame` stays where it is: it feeds the `render_config` line's `frames` field on every level, which is
   that line's purpose (ADR 0022), and it is one relaxed increment. The new observer sits behind the scope test.
5. The APK scope: the combat APK only (D8 (a), #2377).
6. On the service-only file nothing holds the submit slot, so the install refuses `submit_slot_not_held` and nothing runs
   (`render_config::SubmitSlotHeld()`).
7. The in-VR word is pinned by this module's targets header.
8. The process-ending halt reads three existing states (QUIT's finish, `process_end`'s end step, `android_app::destroyRequested`) and hooks nothing:
   a new hook on the activity's destroy would be a second owner of a slot the game's shutdown already crosses.
