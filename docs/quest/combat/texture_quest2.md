# Quest 2 eye buffer one step down in combat

Design record of the Quest 2 step-down (#1737): on a Quest 2 whose player has not chosen a texture level, the eye buffer is Very Low (1152 x
1264 per eye, the Quest 2 profile's, `src/quest/headset_profile.h`) while a combat map, or the combat lobby, is loaded and settled, and the
player's level everywhere else. It also records the question the step-down depends on: can the texture-quality live apply
(`src/quest/sentinel/texture_menu_hook.h`, a `CEngine::Resize` on the render thread) run while a level load uploads data? ADR 0015
(`docs/adr/0015-quest-texture-quality.md`) lists the live apply in a match as not established, and kotorvr/echoquest-combat's
afk_texres.cpp header reports a hang in that case (a lead only; nothing is taken from it). The words the analysis names are pinned in
`src/quest/sentinel/texture_quality_targets.h` and checked against the store libr15 by `src/quest/tests/texture_quality_pinned_test.cpp`
(`just test-quest-hooks-pinned`).

## Origin

kotorvr/echoquest-combat ships the behaviour as `texresq2` (its afk_texres.cpp; the recipe `qa-1104-aj4-w2z22.json` in that repository's
trials directory lists `texresq2.on` in its top-level `on_files`, read with `python3 -I`). Its behaviour is taken (Very Low on a Quest 2 in combat,
a 10 s settle and a 90 s gap as numbers); its mechanism is not. Here the eye size has one owner, texture_quality
(`src/quest/sentinel/texture_quality_hook.h`, ADR 0039), and combat is an input to it.

## What it does

| Where | Level sent | `reason` |
|---|---|---|
| No combat input registered (release APK, or the key false) | the player's | `off` |
| Not a Quest 2 (`ActiveHeadsetProfile()->id`) | the player's | `not_quest2` |
| The player chose a level | the player's | `player_choice` |
| A combat map, or the combat lobby with `combat.texture_quest2_lobby`, the place for 10 s and no level loading for 10 s | Very Low (`Level::kVeryLow`, menu index 0; never `Level::kStock`) | `quest2_combat` |
| Before that | the player's | `not_in_combat` |
| After it: an Echo Arena level the place for 10 s with no load for 10 s | the player's | `arena` |
| After it: no step-down place seen for 90 s | the player's | `no_combat_level` |
| A look-driven Resize the engine refused, or the upload words not pinned | the player's, for the rest of the run | `look_off` |

"The player chose" (`texture_quality::PlayerChose()`, set once and kept): a `graphics.texture_quality` word other than `stock`
(`InstallVia`; the menu writes only `very_low` to `ultra`, `texture_quality.h`), a change in the menu (`HandleApply`, where the choice is
queued for config.yaml), the reference input's start rule naming a level (`ResolveReference`), or the development property
`debug.nevr.texture_level`. The last one is this unit's choice, not the brief's: a level set by hand is meant to apply.

The rules are `src/quest/sentinel/texture_quality_rules.h` (pure). The look's gate (`LookStep`) runs on the owner's poller thread twice a
second; the per-frame apply (`ApplyPending`, `src/quest/sentinel/texture_menu_hook.cpp`) works towards `EffectiveLevel`. Combat's look comes
from `src/quest/combat/texture_quest2.cpp`: a combat-tick callback, every 500 ms, reads the loaded levels (`level::Scan`, the two loaded
tables, an Echo Arena level winning) and CncaGame's init-fiber count (game + 0x1f08, `combat_level_targets.h` `kFiberCount`). Inferred: the
count is of the gamespaces still loading (the table of in-flight init fibers it indexes is read by `combat_level`, and `meshstep.md` reads the
fiber's insertion into the loaded tables). `UpdateGameSpaceInit` (ELF 0x17ecc7c, the function that inserts the gamespace at 0x17ed480) has no
reconstruction in the r15 set (not established: the function is not in the proven reconstruction tree; tracked in #1737), so when the count returns to 0 is not shown: that it is 0 only after the last
gamespace's resources are built, and not earlier, is not established. A count that is not 0, or that cannot be read, is a load. The tick
also keeps the time it last saw a load (`Look::lastLoadMs`). The combat input registers itself with the owner through
`src/quest/sentinel/texture_quality_input.h`; texture_quality includes no combat module header (`texture_menu_hook.cpp` includes the utility
`quest/combat/pinned_words.h`, to check the upload words; it includes neither `combat_scope.h` nor a module's header).

## The guard: no look-driven Resize in a load window

The two shapes the dispatch named, decided from the code:

- (a) Apply at a moment the game itself resizes. The game resizes on its own only from `CEngine::CheckForResize`, when
  `CCanvasDisplay::NeedsResize` answers true (the proven reconstructions `ce_engine_check_for_resize.cpp`, confidence M, and
  `c_canvas_display_needs_resize.cpp`, confidence H, in the r15 reconstruction set): a changed viewport target size, a display that needs a
  resize, a changed MSAA or split-screen mode, the shadow-map pool, the force word (+0x1d80), the pending word (+0x13a50), or recovery from a
  lost VR display. None of these is tied to a level load in what was read, and when the game sets the pending word is not read. The existing
  apply path makes its own Resize every time (`ApplyPending`); it has no hook on `CEngine::Resize` (a vtable slot the apply reads, with no
  GOT slot), so it cannot wait for the game's. And the game's own Resize has the same exposure as ours (below). So (a) is not supported by the
  existing path, and would not lower the exposure of the Resize it rides on; it would only avoid adding one.
- (b) Never in a level load window. Supported: the combat tick can see a load as it happens (the init-fiber table), and the apply already
  waits, frame by frame, for the engine to be ready. This is what is built.

A look-driven change (a step down, or the way back up while the player has not chosen) is held, and looked at again the next frame,
unless all of these hold at the frame it would be made (`ResizeHold`):

1. the look combat made is at most 1.5 s old (`kLookFreshMs`) and shows no gamespace loading;
2. no gamespace has been seen loading for 10 s (`kSettleMs`), by both measures, and the stricter decides: the gate's quiet stretch (the
   poller's, restarted by every sample that sees a load) and the time of the last load the combat tick itself saw (`Look::lastLoadMs`). The
   tick looks every 500 ms and the poller samples every 500 ms, so a load the next look overwrote before the poller read would not restart
   the poller's stretch; the tick's time keeps it;
3. the upload count (0x379b400) reads 0 and the upload lock (0x379b780) reads 0, read without taking the lock. This is a narrowing, not the
   guard: it sees an `Upload` already between its slot and its `End`, and nothing that starts after the read.

The upload words' addresses are used only when the four words that form them (`kLockPage`, `kLockAdd`, `kCountPage`, `kCountLoad`) hold in
the live libr15 (`texture_menu_hook.cpp` `Install`); otherwise the step-down is off for the run. A change the player makes in the menu keeps
the menu's own path and does not wait. With `combat.texture_quest2_lobby` true, a lobby -> map -> lobby round trip keeps the look on through
its load gaps (90 s), so a combat session adds one look-driven Resize when it first settles in combat and one when it settles in Echo Arena
or leaves combat for 90 s.

## What exposure remains

Inferred, from the analysis below; nothing here was observed on a headset.

- A load shorter than the look spacing: the tick reads the init-fiber count every 500 ms, so a count that is non-zero only between two looks
  is not seen by either measure.
- An `Upload` that is not part of a gamespace load: the init-fiber count does not see it. Which callers upload in a settled level (the
  `CGBuffer` initializers that upload initial data, through 0x182bdf0, created at run time, if any) and on which thread is not shown.
- A load that begins after the look and before the Resize ends: the look can be up to 1.5 s old, and the Resize can take the upload lock up to
  nine times over its whole length. Whether a new init fiber reaches an `Upload` inside that time is not shown.
- An `Upload` taken after the count and lock read: the read narrows, it does not close (the section "What this means" below).
- The menu's own live apply and the game's own Resize are unchanged and keep the whole exposure. The menu's Resize (a change the player makes,
  `ApplyPending` with no look behind it) is made with no hold: no load window, no upload word is read.
- The Resize that undoes a refused one (`ApplyPending`, after a failed Resize it puts the previous level back and resizes again) has no
  guard either: it runs at once, in whatever window the refused one was made, and a look-driven refusal is itself a Resize that passed the
  guard above, so the undo adds one more Resize at the same exposure.
- The 10 s settle is upstream's number, not measured here, and when the init-fiber count returns to 0 against the end of a load's uploads is
  not shown (above).

Given that the deadlock is unobserved and a hang is worse than not stepping down, `combat.texture_quest2_step_down` is FALSE by default
here (upstream ships it on); the headset row decides flipping it.

## Switch and default

- `combat.texture_quest2_step_down`, bool, default **false** (opt-in). With it false nothing is registered and nothing of the step-down runs.
- `combat.texture_quest2_lobby`, bool, default **true**. The combat lobby is `mpl_lobby_b_combat`, a combat-only class
  (`src/quest/combat/combat_scope.h`; `src/quest/combat/combat_level.h`: on a Quest the arena lobby loads no `mpl_lobby_b_combat` game space,
  as far as the code shows). With it false every match adds two look-driven Resizes around its load windows. This follows combat_level.h,
  not the comment beside `refresh_rate_lobby` in `src/quest/sentinel/quest_config.h` ("arena shares the lobby levels").
- Both keys are read only in the combat variants (`combat::Active()`); the row is in `src/quest/combat/combat_install.cpp`.

## Log lines (sentinel log)

- `combat_texture_quest2_install` Info (`result` installed|off, `reason`, `lobby`), Error on a refusal (`inactive`, `tick_not_installed`,
  `tick_table_full`).
- `combat_texture_quest2_look` Info, once per change of the class or of loading (a load begins or ends): `where`, `loading`, `fibers` (-1:
  unreadable), `looks`; at most 64 per run.
- `texture_quality_level_rule` Info, from the poller, once per change of the level sent or its reason: `level_asked`, `level_sent`,
  `reason`, `where`, `quest2`, `player_chose`, `unlogged`; at most 32, then one notice (`capped`). After it `texture_quality_level_rule_count`
  (`unlogged`, and the latest `level_sent` and `reason`) is written when the count grew, at most once every 60 s (`kRuleCountEveryMs`), the first
  as soon as a change is not written.
- `texture_quality_level_held` Info, once per hold (not per frame): `level_from`, `level_to`, `hold`
  (stale_look|loading|settling|uploads|lock), `uploads`, `lock`, `quiet_ms`, `holds`.
- `texture_quality_level_rule_off` Warning, once: `why` (resize_failed|upload_words_unavailable).
- `texture_menu_upload_words` Warning at install when a word that forms the two addresses differs: `what`, `va`, `expected`, `actual`. It is
  written on every APK, the release one included, which registers no step-down; its `consequence` says that a step-down, where this build
  registers one, is off for the run and that the player's level is unaffected.
- The existing `texture_quality_applied` line records each change of the applied level, the step-down's included.

## Arena cost

With the key false: none (the per-frame apply reads one null pointer). With it true, in every level of a combat APK, arena included: the tick
callback reads the clock once per frame and, every 500 ms, makes one `Scan` and one 8-byte read; the per-frame apply computes
`EffectiveLevel` from a few atomics, and reads the clock, the look and the two upload words only while a look-driven change is pending.
Nothing is hooked. Not measured on a headset.

## Tests

`src/quest/tests/texture_quality_rules_test.cpp` (the gate, the level, the hold), `src/quest/tests/texture_menu_hook_test.cpp` (the real
apply: Very Low in a quiet window, held on the count, the lock, a load, a stale look, the way back up held too, the player's choice, the
property, not a Quest 2, no input, a refused Resize, unpinned words), `src/quest/tests/texture_quality_hook_test.cpp` (a configured level is a
choice), `src/quest/tests/combat_texture_quest2_test.cpp` (the row and the look), and `src/quest/combat/mutants.d/texture_quest2.sh`.
The guard against the deadlock itself is proven by none of them: it rests on the inference below.

## Headset row

Quest 2 combat APK with `combat.texture_quest2_step_down: true`: lobby -> combat match -> arena match. Expected: `level_sent":"very_low"` with
`reason":"quest2_combat"` in combat and `arena` in arena; the game's `Creating OVR swap chain with size 1152 x 1264`; `texture_quality_level_held`
lines where a load was near; no hang across 5 lobby -> Dyson joins (upstream's repro). A Quest 3 shows `not_quest2`; a menu pick of Low keeps
Low in combat (`player_choice`).

## Platform

PC: not applicable. The PC eye buffer is not a per-headset table (ADR 0015, "Not established", the PC paragraph: the game's own render scale is
the PC control).

## Status of each statement

- Measured: read in the store `libr15.so` (build id `b243509c08ce677aeb95fa348016949b3fc45230`): an instruction word, a symbol, a
  relocation, or a function read whole from its byte-checked reconstruction. The pinned test checks every word, symbol and slot.
- Inferred: derived from measured facts; the step is named. Eight functions below are not in the proven reconstruction tree (tracked in
  #1737); what this page says they do beyond their pinned words is inferred from those words.
- Not shown: listed at the end.

Addresses are ELF virtual addresses; ReVault (project echovr, binary libr15.so) shows each at ELF + 0x100000.

## The two words

Measured. Both lie in `.bss` (`.bss` starts at 0x3761180, size 0x713b8), so the file holds zero and the game writes them at run time.

- 0x379b780, the upload lock: a 32-bit spin lock. A taker spins on `ldar` until it reads 0, then `ldaxr`/`stlxr` 0 to 1; a holder
  gives it back with `stlr wzr`.
- 0x379b400, the upload count: a 64-bit count of the upload ring's taken slots. The ring has 32 slots of 0x38 bytes; a slot's
  submitted word is at ring + 0x8a0 (the same offset in the reaper's load and in both End functions' store, checked by the test).

## The path from CEngine::Resize to the wait

Measured. Each step is a `bl`; a call through the PLT names the stub, and the stub's JUMP_SLOT names the callee (checked by the test).

| Caller | Call site | Through | Callee |
|---|---|---|---|
| `CEngine::Resize` 0x2ac093c | 0x2ac09c0 | stub 0xf3a4d0, slot 0x36f4950 | `CCanvasDisplay::Resize` 0x25edfb4 |
| `CCanvasDisplay::Resize` | 0x25edfd0 | stub 0xf394b0, slot 0x36f4140 | `CCanvas::BeforeResize` 0x25edc0c |
| `CCanvas::BeforeResize` | 0x25edc14 | stub 0xef36c0, slot 0x36d1248 | `CGS::FlushGPU` 0x18e08cc |
| `CCanvasDisplay::Resize` | 0x25edff0 | stub 0xf402a0, slot 0x36f7838 | `CGDisplay::Resize` 0x1834748 |
| `CGDisplay::Resize` | 0x1834884, 0x18348b0, 0x18348d4, 0x18348f8 | direct, each after `mov w0,#0xe` (0x1834880, 0x18348ac, 0x18348d0, 0x18348f4) | deferred-destroy body 0x1888a98 (kind 0xe) |
| `CGDisplay::Resize` | 0x1834878, 0x18348a4, 0x18348c8, 0x18348ec | stub 0xf3c3a0, slot 0x36f58b8 | `CGRawInterfaceVK::DeferredDestroy(SGImage&)` 0x1888c04 (`src/reconstruction/r15/cg_raw_interface_vk_deferred_destroy.cpp`) |
| `DeferredDestroy(SGImage&)` | 0x1888c34, after `mov w0,#0xa` (0x1888c30), on the last reference when the allocation's first word is not 0 | direct | deferred-destroy body 0x1888a98 (kind 0xa) |

`CEngine::Resize` is the function the live apply's engine resize slot must hold (`pinned::kEngineResize`, the same address; the test
checks it). The deferred-destroy body has no `.dynsym` name: it follows `CGRawInterfaceVK::DeferredDestroy(VkBuffer_T*)` (0x1888a8c,
12 bytes), which forwards to it.

`CCanvas::BeforeResize` calls `CGS::FlushGPU` first, before any other step (its byte-checked reconstruction). The game's own resize
also comes this way: `CEngine::CheckForResize` (0x2ac0f9c) calls the engine's vtable slot at +0x30, `CEngine::Resize`, when the
canvas needs a resize (its reconstruction). So one Resize can reach the upload lock up to nine times: once in `FlushGPU`, once in each of the four direct
body calls of `CGDisplay::Resize` (kind 0xe), and once in each of the four `DeferredDestroy(SGImage&)` calls, which reach the body
(kind 0xa) only on the last reference. None of the eight runs when `CCanvas::BeforeResize` answers non-zero:
`CCanvasDisplay::Resize` returns before `CGDisplay::Resize` (`src/reconstruction/r15/ccanvas_display_resize.cpp:79`). Each take sits
behind the body's own tests (a null handle returns first; the take is on the immediate path only), so nine is a bound, not a count.
Each can be skipped: `FlushGPU` returns with no lock and no `currframe` store when gctx + 0xb50 is 0
(`ldr x8,[x19,#0xb50]` at 0x18e08dc, `cbz x8` at 0x18e08e0 to the epilogue at 0x18e098c), and the destroy body returns on a null handle before it
reads `currframe` (`cbz x1` at 0x1888aa8 to the epilogue at 0x1888be4). The test pins both branches and their targets.

### The wait in the deferred-destroy body (0x1888a98)

Measured, read whole.

- 0x1888aa8: a null handle (x1) returns at once, before any lock.
- 0x1888aac..0x1888ad0: `CGS::currframe` is compared with the GPU frame (gctx + 0xb70); when they differ the body takes the queue
  path (below). Both come through pointer words: the gctx from 0x370f360 (`adrp` 0x1888ab0, `ldr` 0x1888ab8) and `currframe` from
  0x3729558 (`adrp` 0x1888aac, `ldr` 0x1888ab4, then `ldr x9,[x9]` at 0x1888ac4).
- Equal: 0x1888ad4/0x1888ad8 (`adrp`/`add`) form the upload lock's address; 0x1888ae0..0x1888af4 take it.
- 0x1888b34/0x1888b38 read the count; while it is not 0, 0x1888b40..0x1888b4c call the reaper 0x18ebb48 with 32 and read it again.
- 0x1888b50 gives the lock back, and the body then branches to 0x1884fa4 with the kind and the handle (that function is not read
  here).

The lock is held for the whole wait.

### The wait in FlushGPU (0x18e08cc)

Inferred from its pinned words (not established: the function is not in the proven reconstruction tree; tracked in #1737). 0x18e08d4/0x18e08d8 load the gctx from the same pointer
word 0x370f360 the body reads, and 0x18e08e0 returns with no lock when gctx + 0xb50 is 0. 0x18e08f4/0x18e08f8 form the upload lock's
address and 0x18e0910 takes it; 0x18e0920/0x18e0924 read the count, and the loop to 0x18e0938 calls the reaper with 32 until it is 0;
0x18e093c gives the lock back (`stlr wzr,[x20]`, pinned). Five calls follow with no lock held: four `bl` to the PLT stub 0xf2b750
(0x18e0948, 0x18e0954, 0x18e0960, 0x18e096c; its JUMP_SLOT 0x36ed290 is
`CGRawInterfaceVK::SubmitPendingTemporaryCommandBuffers`, checked by the test, and what it does is not read) and a `blr` through
gctx + 0xf8 (0x18e0978). 0x18e0988 stores `currframe` (the same pointer word 0x3729558 the body reads, `adrp` 0x18e097c, `ldr`
0x18e0980, deref 0x18e0984) into gctx + 0xb70, the word the deferred-destroy body compares, so the four destroys that follow in
`CGDisplay::Resize` take the immediate path and wait again.

### Other waits on the same count

Measured. `CGRawInterfaceVK::DeferredFree` (0x18888b0) has the same shape: when `currframe` equals its resource frame it takes the
upload lock, calls the reaper with 32 until the count is 0 and gives the lock back
(`src/reconstruction/r15/cg_raw_interface_vk_deferred_free.cpp`); otherwise it appends to its own queue under its own lock.
ReVault's cross-references to the count also list reads in a function at 0x1888a28 (`Flush`), in `CGS::EndFrame` (0x1890b80, which
calls the reaper with 0, so it does not wait) and in `WaitForUploads` (0x18e0d50); those are not read here.

## Who decrements the count

Inferred from pinned words (the reaper, the allocator, both Uploads and both Ends have no reconstruction yet).

- Only the reaper 0x18ebb48 decrements it: `sub x9,x9,#1` at 0x18ebcb0, `str x9,[x21]` at 0x18ebcb4, where x21 is 0x379b400
  (0x18ebb64/0x18ebb68). ReVault's cross-references to 0x379b400 (VA 0x389b400) list two writes: 0x18ebcb4 and 0x18ebe28.
- Only the slot allocator 0x18ebd38 increments it: it reads the count at 0x18ebd48, adds one at 0x18ebe04 and stores it at 0x18ebe28.
- The reaper returns without decrementing when the oldest taken slot's submitted word is 0: `ldr x8,[x8,#0x8a0]` at 0x18ebbc4, then
  `cbz x8` to the epilogue at 0x18ebbc8.
- `CGUploader::Upload` (VkBuffer 0x18ebef4, VkImage 0x18e7134) takes the upload lock, calls the allocator (0x18ebf80, 0x18e71c0)
  and gives the lock back (0x18ebf90 for the buffer). The caller then records its copy.
- `SGUploadBufferContext::End` (0x18eb89c) takes the lock again (0x18eba8c..0x18ebaac), submits, stores 1 into the slot's submitted
  word (0x18ebb20) and gives the lock back (0x18ebb24). `SGUploadImageContext::End` (0x18e7260) does the same (lock at 0x18e750c,
  store at 0x18e759c).

## The deadlock

Inferred. Between an `Upload` and its `End` a slot is taken (count at least 1) and its submitted word is 0, and that thread holds no
lock. If a Resize reaches `FlushGPU` or the deferred-destroy body in that window, it takes the upload lock and calls the reaper, which
stops at the unsubmitted slot and decrements nothing; the count never reaches 0, so the Resize spins with the lock held. The uploading
thread's `End` then spins on the same lock. Neither returns. The step is the reaper's early return (0x18ebbc8) together with `End`
taking the lock before it marks the slot submitted (0x18ebaa0 before 0x18ebb20). That a submitted slot never hangs the wait (the reaper
waiting on its fence) is not established: it needs the reaper read whole.

This needs the upload to run on another thread than the Resize. The live apply runs on the render thread
(`src/quest/sentinel/texture_menu_hook.h`).

## The second lock path (0x1888b04)

Measured, read whole. When `currframe` is not the GPU frame, the deferred-destroy body loads a pointer from 0x379b478 (`.bss`;
`adrp` 0x1888b04, `ldr x22,[x9,#0x478]` 0x1888b08), takes the lock it points to (`ldaxr` at 0x1888b18), appends one 0x18-byte entry
(the frame two GPU frames later, the kind, the handle) to the deferred queue, growing it with `CMemBlock::Resize` when it is full,
and gives the lock back (`stlr wzr,[x22]` at 0x1888be0). It never reads the upload count, so it cannot wait on an upload. ReVault
lists two reads of 0x379b478 (0x18848d4, 0x1888b08) and no write: who sets the pointer is not shown.

## What this means for a look-driven Resize

Inferred. A look-driven Resize is safe to start only if no upload slot is taken (count 0 at 0x379b400) and no thread holds the upload
lock (0 at 0x379b780) for the whole Resize, and reading the two words first cannot ensure that. `FlushGPU` gives the lock back at
0x18e093c and makes five calls with none held, and the deferred-destroy body takes it again in each of the four destroys (0x1888ae8)
and gives it back (0x1888b50). An `Upload` that takes a slot between any two takes leaves a slot with its submitted word 0, so the
next take spins on a count the reaper cannot lower until that `End` runs, and the `End` needs the lock the spinner holds. The window
therefore runs from the read to at least the fourth direct destroy's lock take (0x1834884..0x18348f8): most of the Resize, not the
short time to `FlushGPU`'s first take. What `CGDisplay::Resize` runs after 0x18348f8 is not read (the function is not in the proven
reconstruction tree), so the window may be longer. The check narrows the exposure and does not close it. A guard has to be designed
around the whole Resize (what can run an `Upload` while it runs), not around a gap before the lock. Not read: `CGDisplay::Resize`
between the destroys, and the five calls above.
The game's own Resize (a display change) has the same exposure.

## Not shown

- Which thread runs `CGUploader::Upload` during a level load. Its buffer callers are the `CGBuffer` initializers that upload initial
  data (through 0x182bdf0); the thread they run on is not read.
- That the hang happens on a headset; the step is the inference above, and the upstream report is a lead.
- That no `Upload` runs in a settled level outside a gamespace load, and that the 10 s settle covers what a load leaves behind.
- That the game resizes on its own at a level load (choice (a) above): when the pending word +0x13a50 is set is not read.
- The eight unreconstructed functions read whole: `CGS::FlushGPU`, `CGDisplay::Resize`, the reaper 0x18ebb48, the allocator 0x18ebd38,
  both `CGUploader::Upload` and both `End`. Each is tracked in #1737 for a byte-checked reconstruction; until then their behaviour beyond the
  pinned words is inferred.
- When the init-fiber count (game + 0x1f08) returns to 0 against the end of a gamespace's uploads: `UpdateGameSpaceInit` (ELF 0x17ecc7c) has no
  reconstruction yet.
- Who writes the queue lock pointer at 0x379b478.
- What `SubmitPendingTemporaryCommandBuffers` and the call through gctx + 0xf8 do in `FlushGPU`, and what `CGDisplay::Resize` runs
  between its four destroys.
