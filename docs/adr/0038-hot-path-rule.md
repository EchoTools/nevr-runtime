# ADR 0038: a measured cost per frame decides between a hook, a full replacement and a checked byte patch

Status: proposed. Applies to every change this repo makes to a game function on a hot path, on PC and Quest. Amends
AGENTS.md "Guardrails" ("Prefer a hook to a byte patch") and builds on ADR 0021 and ADR 0034.

## Context

AGENTS.md "Guardrails" prefers a hook and makes a byte patch the exception where a hook would cost something measurable
(a per-frame hot path, a call too frequent to detour), and asks for frequency analysis before a hook adds Sleep or yield
calls. It gives no counter, no window and no number, and it names one form for a hot path, the checked patch
(`nevr_checked_patch`). A second form leaves no trampoline on the hot path either: a rewrite of the whole containing
function.

A wrapper (a GOT thunk on Quest, a MinHook trampoline on PC) adds a fixed cost to every call, plus whatever its handler
does; whether that matters depends on how often the game calls the function per frame. On Quest, a function that no
import slot reaches is hooked through an ADR 0021 inline row: a build-time stub branches through the row's slot, so even
a handler that never calls the original keeps the stub's indirect branch, and the row's trampoline is built whether or
not it is called. ADR 0021 section 11's word-patch rows (`kWordPatch`) change instruction words with no hook at all, but
the public table holds none: they come only from a file outside the repository (`src/quest/patch/libr15_patch_table.h`:
"the public table has no word patches"), a mechanism ADR 0035 retires under #1737.

The repo already names a frame-time difference too small to see: ADR 0025's prediction P8 holds the autopilot's filter to
"under 0.2 ms" of median frame time, "no frame-time cost visible in OVR Metrics". P8 is a prediction no run has checked
yet; it is the line this ADR uses until a measurement build gives a better one. The frame budget is 11.1 ms at 90 Hz and
13.9 ms at 72 Hz.

## Decision

1. **The default is reconstruct, then hook**: read the function, write what it does, hook it, and say in readable code
   what changes.
2. **The measurement.** For a function whose call rate is not plainly per event (a login, a level load), a measurement
   build (ADR 0037) counts its calls and the frames in the same window:
   - **Window:** 60 seconds. For gameplay code, a match with a full lobby. For menu or lobby code, 60 seconds with that
     menu open, or the lobby as full as the run can make it; the autopilot route
     (`docs/adr/0025-quest-autopilot-route.md`, a lobby with no other players) is a lower bound and is reported as one.
   - **Counter:** calls per frame = calls in the window divided by frames in the window: on Quest the frames the render
     configuration's `vrapi_SubmitFrame2` handler counts (`frames` in `src/quest/sentinel/render_config_hook.cpp`), on PC
     the frames of the frame-time probe.
   - **Wrapper cost:** the added time of one wrapped call including its handler's own work, measured by the measurement
     build against the direct call, recorded with the build that measured it.
   - **Added time per frame** = calls per frame × wrapper cost.
3. **The thresholds.**
   - **A hook** when its added time per frame is at most 10 µs, and the added time of every hook the build installs
     together stays at most 0.2 ms per frame (P8's line). 10 µs per hook leaves room for twenty hot hooks under that line.
   - **Otherwise a hot-path form:** for a hook above 10 µs, and, when the total is above 0.2 ms, for the hooks with the
     largest added time until the total is at most 0.2 ms. One of two forms:
     - **a full replacement of the containing function**: one handler that never calls the original. On Quest it is
       reached through the function's import slot or its ADR 0021 inline row (the stub's branch remains; the trampoline
       is never called); on PC it is a detour whose handler never calls the trampoline. It is the function's only
       handler: a module that needs to observe or influence it is an input to it (ADR 0039), not a handler in a chain
       (ADR 0034). Chosen when the whole function can be reconstructed and tested in the PR.
     - **a checked byte patch**: `nevr_checked_patch` on PC. On Quest the public table holds no word-patch row, so the
       Quest form is a full replacement unless #1737 makes word-patch rows public (ADR 0021 section 11 changes with it).
       Chosen when the change is a few instruction words inside a function too large to reconstruct in the PR.
4. **The choice is recorded and measured again.** A PR that chooses a hot-path form states the calls per frame, the
   wrapper cost, the window and why it chose that form. A full replacement is measured again in the same window, and its
   added time per frame (the stub's or the detour's branch plus its handler, against the original) is recorded and held
   to the same thresholds.
5. **AGENTS.md amended.** The "Guardrails" bullet names both hot-path forms and the measured numbers, and points here.

## Finding a violation

Scope: hooks and patches added from this ADR on, in `src/runtime` and `src/quest`, and their PRs.

- **T1.** A byte patch, or a full replacement chosen as a hot-path form, with no recorded measurement and reason. A full
  replacement made because the original's behaviour is wrong is not a hot-path choice and not a hit
  (`GetTimeMillisecondsHook` in `src/runtime/patch/binary_bug_fixes.cpp`, N22).
- **T2.** A wrapper whose recorded added time per frame is above 10 µs, or a hook that takes a build's recorded total
  above 0.2 ms per frame.
- **T3.** A function shown to be per-frame, hooked with no measurement at all.

## Consequences

- Hot paths are found by numbers before the design is chosen, not by a slowdown afterwards.

## Not claimed

The wrapper cost on either platform: the first measurement build records it. Whether 0.2 ms is the right line: P8 has
not been run, and a measurement build may move it.
