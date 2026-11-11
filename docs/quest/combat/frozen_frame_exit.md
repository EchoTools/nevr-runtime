# Frozen-frame exit

Design record for the frame-submit owner, the in-VR signal and the timer source. The component is deferred under #1737.
This page contains no code.

## Status of each statement

- Measured: read in this repository's tree, or in the disassembly of `libr15.so`, with the file, line or address given.
- Inferred: derived from measured facts. The step of the inference is named.
- Open: not decided here. The list is under "Open choices" at the end.

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
for 7 to 11 minutes, and that clean gaps were at most 12 s.

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
Which predicate it asks is open (item 1). Whether the existing frame count moves behind that test or stays with a stated reason
is open (item 4).

### The route an observer takes

Measured. ADR 0039, Decision 2, gives a module that observes a call two routes. It may give the owner an input, either a setting
the owner reads or "a request through an interface the owner offers". It may also "observe the call through a handler in the
manager's chain (ADR 0034) that changes neither the arguments nor the result". This page chooses neither. Which route is open
(item 2).

Measured. Upstream calls `Beat` before the original call, in the `vrapi_SubmitFrame2` wrapper. Where the observer sits relative
to the original call in this tree is open with item 2.

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

Inferred, by the same reading. A component that took the slot would not be service-side, so it would be refused as well. What
the component does on that file is open (item 6).

### The in-VR signal

Measured, in the disassembly of `libr15.so`. VA = ELF + 0x100000.

- The word is at VA 0x38a33f0 (ELF 0x37a33f0). It is a 64-bit global. It holds the ovrMobile session pointer that
  `vrapi_EnterVrMode` returns. A non-null value means in VR mode.
  - The store at 0x1b71c88, `STR X0, [X24, #0]`, writes the EnterVrMode return into the word.
  - The load at 0x1b71ca8, `LDR X0, [X25, #0x3f0]`, puts the word into x0 for the LeaveVrMode call at 0x1b71cb0. So the word is
    the session that LeaveVrMode is given.
- Set in `NRadEngine::CVR::UpdateTracking` (VA 0x1b71b60). The call to `vrapi_EnterVrMode` (thunk 0xff59d0) is at 0x1b71c84.
  A zero return logs the game's own line, `vrapi_EnterVrMode failed`, and leaves the word zero (decompilation).
- Cleared in `UpdateTracking` after `vrapi_LeaveVrMode` (thunk 0xfdc2b0, call at 0x1b71cb0). The zero store is at 0x1b71cb4.
- Cleared in `NRadEngine::CVR::ShutdownGraphics` (VA 0x1b712b0). The LeaveVrMode call is at 0x1b712cc and the zero store at
  0x1b712d0.
- The disassembly holds 67 references to the word: 3 writes (0x1b712d0, 0x1b71c88, 0x1b71cb4) and 64 reads.

Inferred, section. The word is in `.bss`. Measured: a read at VA 0x38a33f0 returns "Requested VA maps beyond the end of the
binary content", so the word has no file bytes. Inferred from that, and from the `.bss` range that
`docs/quest/combat/impactsnd.md` gives for `libr15.so` (ELF 0x3761180 to 0x37d2538, which contains 0x37a33f0). No ELF section
table was read, so the section name is an inference, not a measurement.

Status: a disassembly lead. Nothing pins the word and no pinned test checks it.

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

## Open choices

Not decided here. The implementing unit decides each.

1. The scope predicate an observer asks: `scope::SeenCombatOrLobby()` or `scope::SeenCombatMap()`. Upstream's comment ties its
   stall to a cold lobby load.
2. The route under ADR 0039, Decision 2: a request to the slot's owner through an interface the owner offers, or a handler in
   the manager's chain (ADR 0034). Also where the observer sits relative to the original call.
3. How the in-VR word is read. Either a pinned read of the 64-bit word, which needs its own `*_targets.h` pin and a pinned test,
   and for a word in `.bss` with no file value, a check that its address lies in `.bss` and a pin of the instruction that writes
   it. Or hooks on the two VrApi mode slots, `vrapi_EnterVrMode` (VA 0x37d23d0) and `vrapi_LeaveVrMode` (VA 0x37c5840), each a
   slot in `hook_budget.h`.
4. Whether the existing frame count in `OnSubmitFrame` moves behind the scope test, or stays with a reason written beside it.
5. The APK scope: the combat APK only, or every APK.
6. What the component does on the service-only file.
7. Whether, and when, the in-VR word is pinned.
