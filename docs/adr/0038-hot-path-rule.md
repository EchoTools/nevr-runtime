# ADR 0038: a measured cost per frame decides between a hook, a full replacement and a checked byte patch

Status: proposed. Applies to every change this repo makes to a game function, on PC and Quest. Amends AGENTS.md
"Guardrails" ("Prefer a hook to a byte patch") and builds on ADR 0021 and ADR 0034.

## Context

AGENTS.md "Guardrails" says to prefer a hook, and that "A byte patch or NOP is the exception, used where a hook would cost
something measurable (a per-frame hot path, a call too frequent to detour); the PR names the case, the reason and, where
possible, a number. A checked patch (`nevr_checked_patch`) stays the form for those exceptions." It also says "Hook
functions need frequency analysis — determine if a function is per-frame, per-tick, or per-event before adding
Sleep/yield calls." It gives no counter, no window and no number, and it names one form for a hot path. A second form
leaves no trampoline on the hot path either: a rewrite of the whole containing function.

A wrapper (a GOT thunk on Quest, a MinHook trampoline on PC) adds a fixed cost to every call, plus whatever its handler
does; whether that matters depends on how often the game calls the function per frame. On Quest, a function that no
import slot reaches is hooked through an ADR 0021 inline row: a build-time stub branches through the row's slot, so even
a handler that never calls the original keeps the stub's indirect branch, and the row's trampoline is built whether or
not it is called. ADR 0021 section 11's word-patch rows (`kWordPatch`) change instruction words with no hook at all.

The repo already names a frame-time difference too small to see: ADR 0025's prediction P8 holds the autopilot's filter to
"under 0.2 ms" of median frame time, "no frame-time cost visible in OVR Metrics". The frame budget is 11.1 ms at 90 Hz
and 13.9 ms at 72 Hz.

## Decision

1. **The default is reconstruct, then hook**: read the function, write what it does, hook it, and say in readable code
   what changes.
2. **The measurement.** For a function whose call rate is not plainly per event (a login, a level load), a measurement
   build (ADR 0037) counts its calls and the frames in the same window:
   - **Window:** 60 seconds of the busiest scenario the function runs in. For gameplay code, a match with a full lobby.
     For menu or lobby code, 60 seconds with that menu open, or the autopilot route (`docs/adr/0025-quest-autopilot-route.md`,
     a lobby with no other players).
   - **Counter:** calls per frame = calls in the window divided by frames in the window (the game tick on Quest, the
     frame-time probe on PC).
   - **Wrapper cost:** the added time of one wrapped call including its handler's own work, measured by the measurement
     build against the direct call, recorded with the build that measured it.
   - **Added time per frame** = calls per frame × wrapper cost.
3. **The thresholds.**
   - **A hook** when its added time per frame is at most 10 µs, and the added time of every hook the build installs
     together stays at most 0.2 ms per frame (ADR 0025 P8's line for "not visible"). 10 µs per hook leaves room for
     twenty hot hooks under that line.
   - **Above either, a hot-path form**, one of two:
     - **a full replacement of the containing function**: one handler that never calls the original, through the
       function's import slot or its ADR 0021 inline row (the stub's branch remains; the trampoline is never called). It
       is the function's only handler in the manager's chain (ADR 0034); a second consumer goes through it as an input
       (ADR 0039), not as a chained handler. Chosen when the whole function can be reconstructed and tested in the PR.
     - **a checked byte patch**: `nevr_checked_patch` on PC, a word-patch row (`kWordPatch`, ADR 0021 section 11) on
       Quest. Chosen when the change is a few instruction words inside a function too large to reconstruct in the PR.
4. **The choice is recorded.** A PR that chooses a hot-path form states the calls per frame, the wrapper cost, the window
   and why it chose that form.
5. **AGENTS.md amended.** The "Guardrails" sentence above names both hot-path forms and points here.

## Finding a violation

Scope: hooks and patches added from this ADR on, in `src/runtime` and `src/quest`, and their PRs.

- **T1.** A byte patch or full replacement with no recorded measurement and reason.
- **T2.** A wrapper on a function whose recorded added time per frame is above 10 µs, or a build whose recorded hooks
  together exceed 0.2 ms per frame.
- **T3.** A function shown to be per-frame, hooked with no measurement at all.

## Consequences

- Hot paths are found by numbers before the design is chosen, not by a slowdown afterwards.

## Not claimed

The wrapper cost on either platform: the first measurement build records it. Whether 0.2 ms stays the right line on a
future headset.
