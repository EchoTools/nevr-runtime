# Quest 2 eye buffer in combat: is a live eye-buffer change safe during a level load

Design record for the question the Quest 2 step-down (#1737) depends on: can the texture-quality live apply
(`src/quest/sentinel/texture_menu_hook.h`, a `CEngine::Resize` on the render thread) run while a level load uploads data? ADR 0015
(`docs/adr/0015-quest-texture-quality.md`) lists the live apply in a match as not established. kotorvr/echoquest-combat's
afk_texres.cpp header reports a hang in that case (a lead only; nothing is taken from it). This page contains no code. The words it
names are pinned in `src/quest/sentinel/texture_quality_targets.h` and checked against the store libr15 by
`src/quest/tests/texture_quality_pinned_test.cpp` (`just test-quest-hooks-pinned`).

## Status of each statement

- Measured: read in the store `libr15.so` (build id `b243509c08ce677aeb95fa348016949b3fc45230`): an instruction word, a symbol, a
  relocation, or a function read whole from its byte-checked reconstruction. The pinned test checks every word, symbol and slot.
- Inferred: derived from measured facts; the step is named. Eight functions below have no reconstruction yet (each is queued for
  one); what this page says they do beyond their pinned words is inferred from those words.
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

`CEngine::Resize` is the function the live apply's engine resize slot must hold (`pinned::kEngineResize`, the same address; the test
checks it). The deferred-destroy body has no `.dynsym` name: it follows `CGRawInterfaceVK::DeferredDestroy(VkBuffer_T*)` (0x1888a8c,
12 bytes), which forwards to it.

`CCanvas::BeforeResize` calls `CGS::FlushGPU` first, before any other step (its byte-checked reconstruction). The game's own resize
also comes this way: `CEngine::CheckForResize` (0x2ac0f9c) calls the engine's vtable slot at +0x30, `CEngine::Resize`, when the
canvas needs a resize (its reconstruction). So one Resize reaches the upload lock up to five times: once in `FlushGPU`, then once in each
of the four deferred destroys. Each can be skipped: `FlushGPU` returns with no lock and no `currframe` store when gctx + 0xb50 is 0
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

Inferred from its pinned words (no reconstruction yet; queued in NEEDED.tsv). 0x18e08d4/0x18e08d8 load the gctx from the same pointer
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

## What this means for the Quest 2 step-down

Inferred. A look-driven Resize is safe to start only if no upload slot is taken (count 0 at 0x379b400) and no thread holds the upload
lock (0 at 0x379b780) for the whole Resize, and reading the two words first cannot ensure that. `FlushGPU` gives the lock back at
0x18e093c and makes five calls with none held, and the deferred-destroy body takes it again in each of the four destroys (0x1888ae8)
and gives it back (0x1888b50). An `Upload` that takes a slot between any two takes leaves a slot with its submitted word 0, so the
next take spins on a count the reaper cannot lower until that `End` runs, and the `End` needs the lock the spinner holds. The window
therefore runs from the read to the fourth destroy's lock take: most of the Resize, not the short time to `FlushGPU`'s first take.
The check narrows the exposure and does not close it. A guard has to be designed around the whole Resize (what can run an `Upload`
while it runs), not around a gap before the lock. Not read: `CGDisplay::Resize` between the destroys, and the five calls above.
The game's own Resize (a display change) has the same exposure.

## Not shown

- Which thread runs `CGUploader::Upload` during a level load. Its buffer callers are the `CGBuffer` initializers that upload initial
  data (through 0x182bdf0); the thread they run on is not read.
- That the hang happens on a headset; the step is the inference above, and the upstream report is a lead.
- The eight unreconstructed functions read whole: `CGS::FlushGPU`, `CGDisplay::Resize`, the reaper 0x18ebb48, the allocator 0x18ebd38,
  both `CGUploader::Upload` and both `End`. Each is queued for a byte-checked reconstruction; until then their behaviour beyond the
  pinned words is inferred.
- Who writes the queue lock pointer at 0x379b478.
- What `SubmitPendingTemporaryCommandBuffers` and the call through gctx + 0xf8 do in `FlushGPU`, and what `CGDisplay::Resize` runs
  between its four destroys.
