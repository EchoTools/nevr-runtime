# EchoVR Bug Fix Plan

Systematic plan for runtime-patching verified binary bugs. Each fix is a MinHook function hook or direct memory patch applied by gamepatches.dll or a plugin at process load time.

## Principles

1. **One fix per build, one test per fix.** Each patch lands as its own commit, gets its own test session, and can be reverted independently.
2. **Observation before intervention.** Before patching a bug, add logging/telemetry to confirm it fires in the wild. Don't fix what you can't measure.
3. **Trampoline preservation.** Every hook must call the original function unless the replacement is total. Never silently drop the original behavior.
4. **Server and client are separate test tracks.** Server-only fixes ship first (lower blast radius). Client fixes require player testing.
5. **Bisectable history.** If a fix causes a regression, `git bisect` must be able to isolate it to one commit.

## Verification Status

Bugs verified against decompiled binary (2026-04-12):

| Bug | Claim                                     | Verdict                    | Correction                                                                     |
| --- | ----------------------------------------- | -------------------------- | ------------------------------------------------------------------------------ |
| #1  | GetTimeMicroseconds overflow at 10.7 days | **CONFIRMED**              | Microsecond variant: ~10.7 days. Millisecond variant (CleanupPeers): ~106 days |
| #2  | CleanupPeers no sanity check              | **PARTIALLY CONFIRMED**    | Uses CTimer_GetMilliSeconds (×1000), not ×1000000. Overflow at ~106 days       |
| #3  | No min packet length check                | **PARTIALLY CONFIRMED**    | Zero-length checked. Sub-header (1–0x13 bytes) dispatched unchecked            |
| #5  | No inbound rate limiting                  | **FALSE — REMOVED**        | Rate limiting counters exist at socket +0x40–0x4E                              |
| #6  | Session +0x2DA0 null deref                | **CONFIRMED, understated** | 50+ functions, not 20+. Zero null checks found                                 |
| #7  | DX errors all fatal                       | **CONFIRMED**              | Ghidra marks log call as "does not return"                                     |
| #8  | LoadLibraryExW flags=0                    | **TRUE but LOW risk**      | All 10 sites are CRT delay-load helpers, not user paths                        |
| #9  | Crash handler skips stack overflow        | **CONFIRMED**              | Explicit != 0xC00000FD check. No SetThreadStackGuarantee                       |
| #10 | Crash handler does I/O                    | **CONFIRMED**              | 183 callees including pool allocators, file I/O                                |
| #14 | Inverted spinwait backoff                 | **UNVERIFIABLE**           | Function exists, uses Sleep, direction not confirmed                           |
| #23 | Receive buffer overflow                   | **PARTIALLY CONFIRMED**    | Asymmetry real, uses CStackAllocator not raw stack                             |
| #35 | BugSplat delay-load crashes handler       | **CONFIRMED**              | Delay-loaded, no SEH around calls                                              |
| #36 | NaN velocity normalization                | **FALSE — REMOVED**        | 0x1400DADD0 is a memcpy helper, not velocity math                              |

Bugs #5, #14, and #36 removed from fix plan. #8 downgraded to Low.

## Fix Sequence

Fixes are grouped into **waves**. Each wave is a logical unit that can be tested together because the fixes are independent (no interactions). Within a wave, each fix is still a separate commit.

### Wave 0 — Instrumentation (no behavior changes)

**Purpose:** Confirm bugs fire in the wild before patching them. Low risk — logging only, but hooks modify function prologues.

| Fix | Bug | Method                                      | What it does                                                                                                                                                                                                                                                           |
| --- | --- | ------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 0a  | #1  | Hook `GetTimeMicroseconds` (0x1400D00C0)    | Log when `perfCount * 1000000` would overflow. Count occurrences per session. Do NOT change the return value. `(uint64_t __fastcall(), no params)`                                                                                                                     |
| 0b  | #2  | Hook `CTimer_GetMilliSeconds` (0x1400D0110) | Log when `perfCount * 1000` would overflow. `(uint64_t __fastcall(), no params)`                                                                                                                                                                                       |
| 0c  | #6  | Hook `EndMultiplayer` (0x140162450)         | Log when pointer at `arg1 + 0x2DA0` is NULL (prevents double-deref crash `*(*(arg1+0x2DA0))`). Count occurrences. `(void __fastcall(int64_t arg1, int64_t arg2), arg1=game object)`                                                                                    |
| 0d  | #7  | Hook HandleDXError (0x140551070)            | Log HRESULT value and context string. Distinguish DEVICE_HUNG (0x887A0006) from DEVICE_REMOVED (0x887A0005). Do NOT suppress — log only. `(void __fastcall(uint64_t hr, uint64_t context_fmt, uint64_t detail_str, int64_t extra_info), hr is HRESULT in low 32 bits)` |

**Risk:** Low. Hooks modify function prologues via MinHook. Use atomic counters (InterlockedIncrement) for multi-threaded call sites — GetTimeMicroseconds has 12 callers across physics/render/network threads. Log only on overflow detection, not every call. Install hooks before game threads start.
**Test:** Run server + client for extended session. Check logs for trigger counts.
**Deploy:** Server first (gate on g_isServer). Client after 48h server soak.
**Implementation:** New file `src/gamepatches/wave0_instrumentation.cpp`. Follow the MinHook trampoline pattern from `builtin_server_timing.cpp` (MH_CreateHook → MH_EnableHook, store original in static function pointer). Call from `patches.cpp` initialization after `Hooking::Initialize()`.

### Wave 1 — Crash prevention (null checks, bounds checks)

**Purpose:** Prevent crashes without changing game logic. These are guard-rail fixes — they return safe defaults when invalid state is detected.

| Fix | Bug        | Risk    | Method                                                         | Patch                                                                                                                                                                                                           |
| --- | ---------- | ------- | -------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1a  | #6         | **Low** | Hook functions that deref +0x2DA0                              | Check pointer for NULL before deref. Return 0/false on NULL. Start with EndMultiplayer, then expand.                                                                                                            |
| 1b  | #9/#10/#35 | **Low** | Hook CrashExceptionFilter (0x1401CEF00)                        | Replace crash handler entirely. Write minimal dump using only stack-allocated buffers. No BugSplat, no heap alloc, no file creation beyond the dump. Handle STATUS_STACK_OVERFLOW with SetThreadStackGuarantee. |
| 1c  | #3         | **Low** | Hook packet dispatch (0x140F8CD60)                             | Check received length >= 0x14 (min header). Drop sub-header packets with log.                                                                                                                                   |
| 1d  | #30        | **Low** | Hook VirtualAlloc wrapper in expanding allocator (0x1400D6F20) | Check return value. If NULL, log and return error instead of continuing.                                                                                                                                        |

**Risk:** Low. These only add null/bounds checks before existing code. Worst case: a previously-crashing path now returns a safe default instead, which could mask a logic bug — but a crash is always worse than a safe default.
**Test:** Server: run for 1 hour, verify no new crashes. Client: join/leave sessions repeatedly, disconnect network cable mid-game, verify no crash on disconnect.
**Deploy:** Server first. Client after 48h server soak.

### Wave 2 — Timing fixes (server only)

**Purpose:** Fix frame pacer and time calculation on dedicated servers. These patches already partially exist in server_timing plugin — this wave completes and hardens them.

| Fix | Bug         | Risk       | Method                                                  | Patch                                                                                                                                                                                                       |
| --- | ----------- | ---------- | ------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 2a  | #1          | **Low**    | Hook `GetTimeMicroseconds` (0x1400D00C0)                | Replace `(count * 1000000) / freq` with `(count / freq) * 1000000 + (count % freq) * 1000000 / freq`. Preserves exact semantics, just avoids overflow.                                                      |
| 2b  | #2          | **Low**    | Same pattern for `CTimer_GetMilliSeconds` (0x1400D0110) | Replace `(count * 1000) / freq` with overflow-safe variant.                                                                                                                                                 |
| 2c  | #37         | **Low**    | Cache QPF at hook install time                          | Store QPF result in a static. Both GetTimeMicroseconds and GetMilliSeconds read from cached value instead of calling QPF each time.                                                                         |
| 2d  | #11/#12/#13 | **Medium** | Enhance existing server_timing Wait hook                | Use `CreateWaitableTimerExW` with `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` (Win10 1803+). Cache timer handle across frames. Add `_mm_pause()` in busy-wait. Fall back to current Sleep(ms) on older Windows. |

**Risk:** 2a–2c are Low (same output, different arithmetic). 2d is Medium because it changes frame pacing behavior — must verify tick rate stability under load.
**Test:** Run server for 72+ hours continuously. Monitor tick rate stability. Compare CPU usage before/after. Verify no timing drift by comparing game clock to wall clock over 24h.
**Deploy:** Server only. Never apply 2d to clients.

### Wave 3 — Timing fixes (client)

**Purpose:** Apply the overflow-safe time calculation to clients. Does NOT change frame pacing (that's server-only).

| Fix | Bug | Risk    | Method                         | Patch                                                                                |
| --- | --- | ------- | ------------------------------ | ------------------------------------------------------------------------------------ |
| 3a  | #1  | **Low** | Same as 2a but for client mode | GetTimeMicroseconds overflow-safe math. Identical code, just also enabled on client. |
| 3b  | #2  | **Low** | Same as 2b but for client mode | CTimer_GetMilliSeconds overflow-safe math.                                           |
| 3c  | #37 | **Low** | Same as 2c but for client mode | QPF cache.                                                                           |

**Risk:** Low. Same arithmetic, different path. The client frame pacer is untouched — only the time conversion math changes.
**Test:** Play 3 full matches. Check for any timing-related weirdness: physics jitter, animation stutters, network desync. Compare replay timestamps.
**Deploy:** After Wave 2 has soaked on servers for 1 week.

### Wave 4 — Network robustness

**Purpose:** Fix network timeout edge cases and packet validation.

| Fix | Bug | Risk       | Method                                         | Patch                                                                                                                            |
| --- | --- | ---------- | ---------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------- |
| 4a  | #2  | **Low**    | Add sanity check in CleanupPeers (0x140F76500) | Hook the function. If `elapsed > 3600000000` (1 hour in µs), clamp to 0 and log. This prevents mass disconnect on time overflow. |
| 4b  | #28 | **Low**    | Hook SendReliable (0x140F8B5D0)                | Clamp frame delta to INT32_MAX before accumulation. Prevents ghost connections from timer wraparound.                            |
| 4c  | #42 | **Low**    | Hook sequence number modular reduction         | Fix `% 1023` to `% 1024` (use `& 0x3FF` bitmask).                                                                                |
| 4d  | #43 | **Medium** | Add configurable timeout                       | Read timeout value from NEVR config instead of hardcoded 15000000. Default to 15s for backward compat.                           |

**Risk:** 4a–4c are Low (boundary clamping, no behavioral change in normal operation). 4d is Medium (configurable value could be set wrong).
**Test:** Server: run with 10 bots for 4 hours. Verify no spurious disconnects. Verify intentional disconnects still trigger within expected time. Client: play with network cable pull/restore, verify reconnect behavior.
**Deploy:** Server first. Client after 1 week server soak.

### Wave 5 — Rendering resilience (client only)

**Purpose:** Prevent crashes from transient GPU errors.

| Fix | Bug | Risk       | Method                                     | Patch                                                                                                                                              |
| --- | --- | ---------- | ------------------------------------------ | -------------------------------------------------------------------------------------------------------------------------------------------------- |
| 5a  | #7  | **High**   | Hook DX error handler chain                | For `DXGI_ERROR_DEVICE_HUNG` and `WAS_STILL_DRAWING`: log, sleep 100ms, retry once. If retry fails, proceed to fatal. All other errors: unchanged. |
| 5b  | #16 | **Medium** | Hook \_\_debugbreak callers in render path | Replace `__debugbreak` with log + return NULL. Caller must handle NULL (most already do for the non-error path).                                   |

**Risk:** 5a is High because it changes the rendering error recovery path. A bad retry could corrupt GPU state. 5b is Medium because returning NULL from resource creation could cause downstream null derefs if callers don't check.
**Test:** Deliberately trigger GPU pressure (run GPU benchmark alongside game). Verify game recovers from transient hangs instead of crashing. Check for visual corruption after recovery.
**Deploy:** Client only. Opt-in via config flag initially.

### Wave 6 — Threading fixes

**Purpose:** Fix spinlock and synchronization issues.

| Fix | Bug | Risk       | Method                            | Patch                                                                                                           |
| --- | --- | ---------- | --------------------------------- | --------------------------------------------------------------------------------------------------------------- |
| 6a  | #15 | **Medium** | Hook render spinlock acquire      | Add timeout (1000ms). If timeout hit, log deadlock warning and break the spin (proceed without lock).           |
| 6b  | #38 | **Medium** | Hook DrainWorkQueue (0x1401D66B0) | Add progress check: if no work items completed in 5 seconds, log and break drain.                               |
| 6c  | #27 | **Low**    | Hook CThread::Fork (0x1401D31B0)  | Replace `m_finished` spin with Event wait. Use `CreateEvent` + `WaitForSingleObject(event, 5000)` with timeout. |

**Risk:** 6a is Medium — breaking a spinlock could corrupt shared state. Only do this with extensive logging. 6b is Medium — breaking a drain could leave work items in-flight. 6c is Low — same semantics with better primitives.
**Test:** Server: run under heavy load (full lobbies) for 24h. Monitor for deadlocks, hangs, or corrupted state. Client: same, plus VR headset on/off transitions.
**Deploy:** Server first. Client after 2 weeks server soak.

## Fixes NOT planned (and why)

| Bug                           | Why not                                                                                                       |
| ----------------------------- | ------------------------------------------------------------------------------------------------------------- |
| #8 (LoadLibraryExW flags)     | CRT delay-load helpers, not user-controlled paths. Low risk.                                                  |
| #14 (inverted backoff)        | Unverifiable. Cannot confirm the bug exists.                                                                  |
| #39 (fixed physics substep)   | Likely intentional design. Changing it risks physics explosions.                                              |
| #40 (VR timing transition)    | Uncommon scenario (headset removal mid-game). Low impact.                                                     |
| #9/#10/#35 crash handler bugs | Handled together in Wave 1b as a complete crash handler replacement.                                          |
| Rendering bugs R2–R8          | Most are low-impact edge cases or require deep D3D state machine knowledge. 5a/5b cover the high-impact ones. |
| Audio bugs 25/26/33/35/53/55  | Low user impact. Audio failures don't crash the game.                                                         |

## Risk ratings

| Risk       | Meaning                                   | Test requirement                      |
| ---------- | ----------------------------------------- | ------------------------------------- |
| **Low**    | Adds safety checks, returns safe defaults | 1 hour server + 3 matches client      |
| **Medium** | Changes behavior in edge cases            | 24h server soak + full match testing  |
| **High**   | Changes core system behavior              | 1 week server soak + player beta test |

## Testing protocol

Each wave follows:

1. **Build** — compile with only the new wave's patches enabled (previous waves stay on)
2. **Server smoke** — start headless server, verify it reaches lobby state
3. **Server soak** — run server for the specified soak duration under load
4. **Client smoke** — connect one client, play one match
5. **Client soak** — play the specified number of matches with 2+ players
6. **Regression check** — verify no new crashes, no timing drift, no network issues
7. **Commit** — if pass, merge wave. If fail, bisect to individual fix.

Each fix within a wave is a separate commit. If a wave fails testing, disable individual fixes until the regression is isolated.
