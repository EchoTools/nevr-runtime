# launch-server.sh hangs after login — investigation trace

2026-09-14, Claude + Andrew. Not solved. This is a resume point, not a fix.

## Symptom, precisely

`launch-server.sh` boots `echovr.exe -server -headless -noconsole`, logs in
successfully, joins the social lobby group, and then sits there — alive
(main loop ticking, `GetTimeMicroseconds` firing continuously per our own
`hook_liveness` diagnostic), but never progresses to dedicated-server
bring-up. Confirmed dead end-state via `hook_liveness`'s periodic report:
`HTTPListenerBringup entries=0 entered=NO` indefinitely. No crash, no error,
no timeout — it just never happens.

Last known-good reference: `echovr-server-32-2026-07-26T11-16-07.550.jsonl`
(in `~/src/nevr-server-rs-work`), where the same sequence completed in ~12s:
login → lobby join → **connection to the bridge lost** → ~6s later,
**`"Beginning multiplayer"`** (native log line) → broadcaster init →
`GameServerLib::Initialize` → ServerDB registration → `"[NSLOBBY] Registered
lobby (id: ...)"`.

## What was ruled out

**`a692a30` (2026-08-05, "bootstrap server on first preprocess call")** —
git-bisected the ~3-week window since the last commit touching this area.
This commit added `g_isServer` to the condition in `boot.cpp` that forces
the client's windowed/no-VR flag (`0x0100000` at `GAME_WINDOWED_FLAGS_OFFSET`)
onto `g_pGame`. That flag's own comment says it makes the game "reach the
main menu normally, and -mp joins a social lobby" — client behavior, and
suspicious given our exact stuck state. The same commit also relaxed
`tests/system/server_test.go` to stop requiring
`"[NEVR.GAMESERVER] Initialized game server"` as a readiness marker.

**Tested live, reverted the flag-condition half only** (commit `151100b`,
back to `-windowed` only) — kept the other half (bootstrap firing on the
first `PreprocessCommandLine` call, not a second) since that addresses a
separately-real problem per the commit's own comment. **Did not fix it.**
Still hangs at the identical point after the revert. So either this
hypothesis was wrong, or it's a real-but-insufficient part of the picture —
there may be another client-only flag collision in the same family
(windowed/no-VR/spectator-stream) not yet found.

**Confirmed NOT the cause: our own bootstrap.** `"[NEVR.BOOT] runtime
bootstrap complete early_config=1 bridge=1 port=..."` fires every time,
reliably. `ws_bridge`, config loading, plugin loading — all fine. The
blocker is 100% inside the native engine's own progression, after our
injected code has finished its job.

## What's confirmed, with VAs (ReVault, `echovr.exe`)

**`BeginMultiplayer` (`0x1400fda70`) logs `"Beginning multiplayer"`
completely unconditionally as its first action** — no gate before the log
call. Since we never see this line, `BeginMultiplayer` is simply never
*called* — nothing inside it is silently failing a check.

Its callers (5, from `revault_callers`): `FUN_1400fddd0`, `fcn.1400fe0a0`,
`FUN_1400fe0e0`, `~CTaskTarget<SPhUpdateTriTask> @ 0x1400fe180`,
`fcn.1400fde60`. The destructor in that list is almost certainly a
decompiler/linker-folding artifact (identical-code-folding across template
instantiations), not a real semantic caller — treat it as noise. The others
are trivial wrapper functions (e.g. `FUN_1400fddd0` just packs a struct and
tail-calls `BeginMultiplayer`) with **zero statically-resolvable callers of
their own** (`revault_callers` returns "No callers of FUN_1400fddd0"). This
is the classic indirect-dispatch dead end: `CTaskTarget<...>` naming
strongly suggests these are enqueued onto the engine's task scheduler
(matches the boot log's own `"---> Starting 10 worker threads in the
scheduler."`), not called directly, so nothing in the static call graph
points at whoever enqueues the task.

**Confirmed exact gate for loading `pnsradgameserver`, verified via RAW
DISASSEMBLY (not decompiled text — see correction note below)**, in the
giant function `fcn.140157fb0 @ 0x140157fb0` (also the caller of
`HTTPListenerBringup`). At `0x1401599b6`-`0x1401599e5`:

```
0x1401599b6:  MOV RAX, [RSI+0x2da0]        ; RSI = netgame "this"
0x1401599bd:  MOV RDX, [RAX]               ; flags qword (double-deref)
0x1401599c0:  SHR AL,1; TEST AL,1          ; bit1
0x1401599c7:  JNZ 0x1401599db              ; bit1 SET -> proceed
0x1401599c9:  SHR AL,2; TEST AL,1          ; bit2
0x1401599d1:  JNZ 0x140159a47              ; bit2 SET -> SKIP (don't load)
0x1401599d3:  SHR DL,6; TEST DL,1          ; bit6
0x1401599d9:  JZ 0x140159a47               ; bit6 CLEAR -> SKIP (don't load)
0x1401599db:  CALL 0x140614b00             ; extra check
0x1401599e3:  TEST EAX,EAX; JNZ 0x140159a47 ; nonzero -> SKIP
0x1401599e7..: reads "server_plugin" (default "pnsradgameserver"), loads it
```

Equivalent to: **load `pnsradgameserver` iff `(bit1==1 OR (bit2==0 AND
bit6==1)) AND FUN_140614b00()==0`**. Given bit1 alone is sufficient
(independent of bit2/bit6), this is the simplest, most actionable target:
**does bit1 of the flags qword at `*(netgame+0x2da0)` get set for our
`-server` run?** Not yet determined.

**Cross-verified this is the exact same "host authority" predicate used
elsewhere** — confirmed via direct disassembly of `CR15NetGame::Update @
0x1401bf610` at THREE separate sites (`0x1401bf9ad`, `0x1401bfb7d`,
`0x1401bfd09`), all doing the identical `bit1==1 OR (bit2==0 AND bit6==1)`
test against `*(this+0x2da0)`. This matches the earlier `u-engine-cadence`
receipt's (`~/src/nevr-server-rs-work/promoted-20260901/work-nevr-server-rs
/receipts/u-engine-cadence-20260826T013206Z.md`) finding of the same
predicate gating stat-emission — confirmed correct, same bits, same
"am I the game server/host" meaning.

**Correction to an earlier version of this doc:** I initially transcribed
this offset as `netgame+0xb68` (from Ghidra's decompiled C text) and lost
significant time (~1hr) chasing that offset through unrelated code. Direct
`revault_disassemble` on the actual instructions proves the real offset is
**`+0x2da0`** throughout — `+0xb68` was a transcription error on my part,
not a Ghidra artifact. The *bit numbers* (1, 2, 6) I'd noted alongside it
were, by luck, still correct. Lesson: for load-bearing offsets, disassemble
directly rather than trusting a copied decompiled-text fragment.

**Dead end, recorded so it isn't retried:** searching
`revault_search_code("+ 0x2da0) =")` for where this flags-qword *pointer*
gets initialized (constructor-time allocation) returns 20 hits, but ~18 of
them are `revault_search_code`'s well-documented reconstruction-node noise
problem (same generic "loading map flag, bit 37" boilerplate text attached
to completely unrelated functions — materials, physics, VOIP, random-number
generation). This is the exact failure mode the `u-engine-cadence` receipt's
§0 warns about ("reconstruction noise swamped the result set entirely" for
generic patterns) — confirmed independently here. One hit
(`RegistrationFailureCB @ 0x1401b6d30`, tagged `matchmaking-session`) looked
promising by name but turned out to be the same noise — the function is
actually just a 35-state enum-to-string stringifier (already correctly
documented in ReVault as "THE AUTHORITATIVE STATE MACHINE ENUM"), unrelated
to setting any flag bit. **Use `revault_disassemble` or check the
decompilation tier header (`ghidra ... | raw`) before trusting any
`revault_search_code` hit on a generic offset pattern — the reconstruction
tier will lie by omission.**

**`-headless` is a real, recognized native flag** (unlike `-server`, which
has no standalone string in `echovr.exe` at all — it's a pure
nevr-runtime-invented token consumed only by our own `boot.cpp`). Traced
`-headless`'s consumer in `CR15Game::ApplyCommandLineEngineFlags @
0x140503ab0` via disassembly (`0x140504520`-`0x140504566`): when present, it
does `AND dword ptr [RBX+0x1d4], 0xfffefefe` — clears bits 0/8/16 of a
**`CR15Game`** (not `CR15NetGame`) flags dword. This is a different
object/offset than the `CR15NetGame+0xb68` bit1/bit2/bit6 we actually need.
**Not yet checked: whether `CR15NetGame`'s bit1/bit2/bit6 get derived from
`CR15Game`'s flags at netgame-construction time** — that translation site,
if it exists, was not located. This is the most promising unexplored lead.

## Update 2026-09-14, later same day: live diagnostic confirms the block is upstream of both functions

Implemented the diagnostic from step 1 below (`NetGameHostCheckHook` on
`fcn.140157fb0`, commits `b2790d3`/`837efb3` — the second commit fixes a bug
in the first: I initially gated the hook *install* on `g_isServer`, which
isn't set yet at that point in boot, so it never fired at all on the first
attempt — same ordering-bug shape as the rest of this investigation, caught
and fixed the same day).

**Result: the hook installs successfully every time, but never fires** —
confirmed after 90+ seconds sitting at "Social lobby group info received"
with no further progress. This is a second, independent confirmation (on a
*different* function than `BeginMultiplayer` itself) that the block is
genuinely upstream of this whole call chain — not "reached with bit1
wrong," but never reached at all. Whatever enqueues the
`CTaskTarget<SPhUpdateTriTask>`-wrapped call to `BeginMultiplayer` never
does so for this run.

## Update 2026-09-14, later still: STcpConnectionUnrequireEvent experiment — REFUTED

Implemented and tested the disconnect-causation hypothesis directly
(commits `d0190c4`, `dd1e9e7` — the second fixes a real bug in the first:
`rsym` only reflects the first message in a WS frame, and Nakama batches
`LoginSuccess`+`STcpConnectionUnrequireEvent` into one frame, so the first
version of this check was silently dead code — confirmed nakama.log showed
the event sent while our DIAG line never printed. Fixed by scanning every
message in the frame).

**Result: the DIAG line fires correctly (confirmed live, 3 times — once per
batched frame instance across the two bridge connections), `remoteWs->close()`
executes, but nothing downstream changes.** No `"connection ... lost"`, no
`"game disconnected"` — the game never appears to notice the remote closed
at all, and the hang is identical to every other run (`HTTPListenerBringup
entries=0` after 3+ minutes).

**This refutes the disconnect-causation hypothesis as tested** — or at
least, this specific way of forcing a disconnect does not reproduce whatever
happened organically in the July 26 capture. Two live possibilities, not
distinguished yet:
1. The July 26 disconnect was a red herring — an unrelated network blip
   that happened to precede `BeginMultiplayer` by coincidence, not a trigger
   for it.
2. Our `remoteWs->close()` isn't actually producing the same observable
   effect the original disconnect did — e.g. the game's own WS client
   might only "detect" a closed remote when *it* tries to write and the
   OS-level socket write fails, and if this server-mode session never
   sends anything else on this connection at all (unlike a client, which
   keeps chattering), that passive detection may simply never get a chance
   to fire. Untested: forcibly closing `gameWsPtr` instead (the
   game-facing side) — flagged as deadlock-risky elsewhere in this file
   under the loader lock, but the earlier failure mode is different
   (SIGTERM/graceful-shutdown paths), not necessarily this one; would need
   its own careful safety review before trying.

## Concrete next steps, in priority order

*(Superseded — kept for the record.)* Steps 1 and the disconnect-causation
idea below were both executed live today, not just planned. Status:

- ~~Live diagnostic hook on `fcn.140157fb0`~~ — **done** (`b2790d3`,
  `837efb3`). Installs correctly (fixed a real g_isServer-timing bug in the
  same session), but **never fires** — confirms the block is upstream of
  this function entirely, so the bit1/bit2/bit6 question never got answered
  because the code path that would read it is never reached at all.
- ~~STcpConnectionUnrequireEvent disconnect experiment~~ — **done and
  refuted** (`d0190c4`, `dd1e9e7`). Fires correctly, calls `remoteWs->close()`,
  the game never visibly reacts, hang is unchanged. See the write-up above.

**What's actually still open, in priority order:**

1. **Find what's supposed to enqueue the `BeginMultiplayer` task.** This is
   now the single most direct open question — not "is a flag wrong" (ruled
   out, the whole call chain is unreached) but "what native trigger is
   missing." `BeginMultiplayer`'s callers are a `CTaskTarget<SPhUpdateTriTask>`
   family with zero resolvable static callers (indirect task-scheduler
   dispatch — `revault_callers` returns nothing). Two angles, neither tried:
   (a) find the task scheduler's own enqueue/dispatch function and see what
   conditions gate a `SPhUpdateTriTask`-shaped task being queued at all;
   (b) live-hook something in that scheduler's dispatch loop and log every
   task type that DOES get enqueued during a hung `-server` run, to see by
   omission what's missing compared to a working client run.
2. **Try closing `gameWsPtr` instead of `remoteWs`** in the
   `STcpConnectionUnrequireEvent` handler — the *game-facing* side, not the
   Nakama-facing side. `remoteWs->close()` produced zero observable game
   reaction; it's possible the game's own detection genuinely requires a
   failed *local* read/write, which only happens if `gameWsPtr` itself goes
   away. This is flagged elsewhere in `ws_bridge.cpp` as deadlock-prone
   under the loader lock in a *different* context (`DLL_PROCESS_DETACH`) —
   needs its own safety review before trying, this is not a drop-in change.
3. **Find the `CR15Game` → `CR15NetGame` flag translation site** (if one
   exists) — lower priority now that (1) shows the flag question is moot
   until the task actually gets enqueued at all.
4. **`-mp`**: nothing in nevr-runtime's own source references it despite
   `launch-client.sh` passing it, and no standalone `-mp` string exists in
   `echovr.exe` either — consistent with it being a harmless no-op token,
   but not confirmed. Low priority given (1) is a more direct line of
   attack, but cheap to rule out by just running `launch-client.sh` without
   it and confirming client mode is unaffected.

## Environment note, unrelated to the root cause

Xephyr (`:101`) crashed repeatedly during this session
(`XIO: fatal IO error 110`) — auto-restarts on its own, not investigated,
probably worth a look if it keeps happening but is a separate issue from
this hang (client-mode testing continued fine across those crashes).
