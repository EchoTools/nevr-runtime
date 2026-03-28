# Guardrails: Performance Patches on Game Binaries

Rules learned from the thread pool dispatch disaster of 2026-03-26.

## The Mistake

We hooked `ThreadPoolDispatch` (a per-work-item function called thousands of times per frame) and added `Sleep(1)` to every call. This made idle CPU look great in perf but destroyed gameplay — 999ms pings, unresponsive servers, broken game.

## Rules

### 1. Know the call frequency before adding latency

Before hooking ANY function with a sleep/yield:
- Count how many times it's called per second during **active gameplay**, not just idle
- If it's called more than ~100 times/sec, adding even 1ms per call will compound
- `1ms * 1000 calls/frame = 1 second of added latency per frame`

### 2. Distinguish "hot because idle-spinning" from "hot because high-throughput"

A function showing 60% in perf can mean:
- **(A)** It's spinning in a tight loop doing nothing — safe to throttle
- **(B)** It's processing thousands of legitimate work items — DO NOT throttle

The thread pool dispatch was (B). The perf profile looked identical to (A). The only way to tell them apart: **trace the call count** during gameplay, not just profile at idle.

### 3. Test with gameplay, not just idle

CPU measurements at idle are necessary but not sufficient. Every performance patch MUST be tested with:
- Active gameplay (players connected, moving, scoring)
- Ping measurement (is latency affected?)
- Game responsiveness (do actions feel delayed?)

"It looks good at idle" is not validation.

### 4. Sleep belongs in IDLE LOOPS, not HOT PATHS

The correct pattern for reducing idle CPU:
```
// WRONG: sleep after every dispatch
void DispatchHook(work) {
    original_dispatch(work);
    Sleep(1);  // Adds 1ms to EVERY work item
}

// RIGHT: sleep only when there's no work
void DrainQueueHook(queue) {
    int items = original_drain(queue);
    if (items == 0) {
        Sleep(1);  // Only sleep when idle
    }
}
```

### 5. Validate the function signature before hooking

We hooked `fcn_1401d64f0` based on perf address proximity. We assumed it was a loop function. It was actually a dispatch function called per-item. Before hooking:
- Decompile the function (Ghidra, reconstruction)
- Understand its arguments and return value
- Determine if it's called once-per-loop or once-per-item
- Check the call sites — who calls it and how often?

### 6. Confidence from tools is not confidence in understanding

We ran `perf`, validated `Sleep(1)` latency, measured CPU before/after, and committed with a detailed message. Every step looked rigorous. But we never understood what the function *does* — only that it was hot. Tools validate measurements, not assumptions.

### 7. Revert fast

If a performance "fix" hasn't been tested under load, ship it with a kill switch (config flag, environment variable). Don't commit as if it's proven. The commit message said "perf validated locally" — it was validated for the wrong workload.
