# ADR 0038: a measured cost per frame decides between a hook, a full replacement and a checked byte patch

Status: proposed. Applies to every change this repo makes to a game function, on PC and Quest. Builds on AGENTS.md
"Guardrails" (prefer a hook to a byte patch; hook functions need frequency analysis) and ADR 0034 (one hook manager).

## Context

AGENTS.md says to prefer a hook and to use a byte patch where a hook would cost something measurable, and that hooked
functions need frequency analysis, but it gives no counter, no window and no number. ADR 0034's "Not claimed" leaves
open where a chained hook is too expensive. A wrapper (a GOT thunk on Quest, a MinHook trampoline on PC) adds a fixed
cost to every call; whether that matters depends only on how often the game calls the function per frame.

The frame-time record available on Quest is the system's `VrApi FPS=` line, once a second, whose `App=` field is printed
in milliseconds with two decimals, so 10 microseconds is the smallest difference it shows (for example `App=4.97ms` in
a v6.16.4 headset logcat). The frame budget is 11.1 ms at 90 Hz and 13.9 ms at 72 Hz. On PC the frame-time probe
(`diagnostics.frame_stats`, `[NEVR.PERF]`) writes one summary line per 10 s window.

## Decision

1. **The default is reconstruct, then hook**: read the function, write what it does, hook it, and say in readable code
   what changes.
2. **The measurement.** For a function whose call rate is not obviously per event (a login, a level load), a
   measurement build (ADR 0037) counts its calls and the frames in the same window:
   - **Window:** 60 seconds of the busiest scenario the function runs in. For gameplay code, a match with a full lobby
     or the autopilot route (`docs/adr/0025-quest-autopilot-route.md`, built for like-for-like runs). For menu code, 60
     seconds with the menu that uses it open.
   - **Counter:** calls per frame = calls in the window divided by frames in the window. On Quest the frames come from
     the game tick or the sum of the `FPS=` lines; on PC from the frame-time probe.
   - **Wrapper cost:** the added time of one wrapped call, measured once per platform by the measurement build: a thunk
     and an empty handler called a fixed number of times, against the direct call. The number is recorded with the
     build that measured it.
   - **Added time per frame** = calls per frame × wrapper cost.
3. **The thresholds.**
   - **Hook** (a wrapper or a handler in the manager's chain) when the added time per frame is at most 10 µs. That is the
     smallest change the frame-time record can show, and 0.09 % of the budget at 90 Hz.
   - **Full replacement of the containing function** (one handler that never calls the original, no trampoline, no
     chain) when it is above 10 µs. The replacement removes the wrapper's cost.
   - **A checked byte patch** (`nevr_checked_patch` on PC, an ADR 0021 row on Quest) only when no slot reaches the call
     site and the containing function cannot be fully replaced. Typical causes: the site is inlined into a caller too
     large to reconstruct in the PR, or the function is reached only by direct calls.
4. **The exceptions are recorded.** A PR that chooses a full replacement or a byte patch states the measured calls per
   frame, the wrapper cost and the window, or, for a byte patch, why no slot and no replacement can reach it.

## Finding a violation

Scope: hooks and patches added from this ADR on, in `src/runtime` and `src/quest`, and their PRs.

- **T1.** A byte patch or full replacement with no recorded measurement or reason.
- **T2.** A wrapper on a function whose recorded calls per frame put it above 10 µs.
- **T3.** A per-frame function hooked with no measurement at all.

## Consequences

- Hot paths are found by numbers before the design is chosen, not by a slowdown afterwards.

## Not claimed

The wrapper cost on either platform: the first measurement build records it. Whether 10 µs stays the right line on a
future headset with a finer frame-time record.
