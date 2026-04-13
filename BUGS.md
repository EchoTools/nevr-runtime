# EchoVR Binary Bug Audit

Bugs in the original Ready at Dawn binary (`echovr.exe`). Not introduced by NEVR patches.

All addresses are virtual addresses (ImageBase `0x140000000`). Analysis via ReVault + Ghidra. Quest ARM64 binary cross-referenced for symbol names.

## Summary

| Severity  | Count  | Fixed |
| --------- | ------ | ----- |
| Critical  | 2      | 2     |
| High      | 12     | 7     |
| Medium    | 30     | 0     |
| Low       | 17     | 0     |
| **Total** | **61** | **9** |

## Critical

### 1. GetTimeMicroseconds — int64 overflow after extended uptime

|             |                                      |
| ----------- | ------------------------------------ |
| **Address** | `0x1400D00C0` (68 bytes, 12 callers) |
| **Status**  | **FIXED** (wave0 hook)               |

```c
return (perfCount * 1000000) / perfFreq;
```

`perfCount * 1000000` overflows `INT64_MAX` after ~10.7 days at 10 MHz QPC, ~4.3 days at 25 MHz (Ryzen). Wraps negative, producing garbage timestamps across physics, networking, animation, rendering simultaneously.

**Fix:** Overflow-safe replacement using quotient+remainder split: `(pc/pf)*1000000 + ((pc%pf)*1000000)/pf`. Also caches QPC frequency (was bug #37). Does not call original.

### 2. Network peer timeout — no sanity check on elapsed time

|             |                                          |
| ----------- | ---------------------------------------- |
| **Address** | `0x140F76500` (CleanupPeers, 1939 bytes) |
| **Status**  | **FIXED** (by bug #1 fix)                |

Unsigned subtraction of overflowed timestamp wraps to massive value. Every peer exceeds timeout threshold — all disconnected at once.

**Fix:** Root cause eliminated — GetTimeMicroseconds no longer overflows, so CleanupPeers never sees wrapped timestamps.

## High

### 3. No minimum packet length check before header parsing

|             |                                  |
| ----------- | -------------------------------- |
| **Address** | `0x140F8E310` (Update_UDPSocket) |
| **Status**  | Unpatched                        |

UDP packets are dispatched to decrypt/decode without verifying minimum header size (0x14 bytes). A packet shorter than the flag field causes reads from uninitialized memory before the length check fires.

### 4. Custom TLS — CBC-only, stream/AEAD unimplemented

|             |                                                             |
| ----------- | ----------------------------------------------------------- |
| **Address** | `0x14155A160` (CTlsCodec::ReceiveRecord)                    |
| **Source**  | `d:\projects\rad\dev\src\engine\libs\network\ctlscodec.cpp` |
| **Status**  | Unpatched                                                   |

Custom TLS implementation. Stream cipher and AEAD (GCM) paths are stubbed with fatal errors. Only CBC mode works. Multiple distinct error paths for padding vs MAC failures suggest padding oracle vulnerability.

### 5. No inbound UDP packet rate limiting

|             |                              |
| ----------- | ---------------------------- |
| **Address** | `0x140F8E310` (receive loop) |
| **Status**  | Unpatched                    |

Receive loop processes packets until time budget expires. No packet count limit, no per-source rate limit. A UDP flood from any address consumes the entire receive budget, starving sends. Other players see the game freeze.

### 6. Session flags double-pointer deref without null check (20+ functions)

|               |                                                                              |
| ------------- | ---------------------------------------------------------------------------- |
| **Pattern**   | `*(*(uint32_t**)(arg1 + 0x2DA0))`                                            |
| **Functions** | EndMultiplayer (`0x140162450`), IsGameModeActive (`0x140113A50`), +17 others |
| **Status**    | **Partial fix** (EndMultiplayer hooked, 19 others unpatched)                 |

Neither pointer level has a null check. EndMultiplayer is called during disconnect cleanup — if session never initialized, crash.

**Fix:** Wave0 hook on EndMultiplayer checks `*(arg1+0x2DA0)` for NULL and returns early. Other 19 functions remain unpatched.

### 7. All DX errors treated as fatal — no recovery from transient DEVICE_HUNG

|               |                                           |
| ------------- | ----------------------------------------- |
| **Addresses** | `0x14059CAD0`, `0x14055CFE0`, 75+ callers |
| **Status**    | **FIXED** (wave0 hook)                    |

Every DXGI HRESULT failure calls `NRadEngine_LogError(8, ...)` which does not return. `DXGI_ERROR_DEVICE_HUNG` (transient, TDR recoverable) and `DXGI_ERROR_WAS_STILL_DRAWING` (GPU busy) are treated identically to `DEVICE_REMOVED` (unrecoverable).

**Fix:** Wave0 hook on HandleDXError (`0x140551070`) intercepts transient errors (DEVICE_HUNG, WAS_STILL_DRAWING) — logs and returns instead of calling the fatal original. Other DXGI errors still pass through.

### 8. DLL hijacking — LoadLibraryExW with flags=0

|               |                                                     |
| ------------- | --------------------------------------------------- |
| **Addresses** | 10+ call sites (`0x140022970`, `0x140022DF0`, etc.) |
| **Status**    | Unpatched                                           |

`LoadLibraryExW` called with `dwFlags=0`, including CWD in DLL search order. `LOAD_LIBRARY_SEARCH_SYSTEM32` not used. Significant attack surface on community servers with shared game directories.

### 9. Crash handler cannot report stack overflow

|             |                                      |
| ----------- | ------------------------------------ |
| **Address** | `0x1401CEF00` (CrashExceptionFilter) |
| **Status**  | Unpatched                            |

Explicitly skips `STATUS_STACK_OVERFLOW`. The crash handler (`HandleCrashDump`, 5957 bytes) itself calls `__chkstk` and uses large stack buffers — it would re-trigger the overflow. No `SetThreadStackGuarantee` for alternate stack.

### 10. Crash handler allocates memory and does I/O during crash

|             |                                                          |
| ----------- | -------------------------------------------------------- |
| **Address** | `0x1401CEFE0` (HandleCrashDump, 5957 bytes, 183 callees) |
| **Status**  | Unpatched                                                |

Calls `CreateFileA`, `WriteFile`, `snprintf`, `GetModuleFileName`, `getenv`, and constructs BugSplat `MiniDmpSender`. If the crash was caused by heap corruption, memory allocation deadlocks. If caused by file system issues, I/O hangs.

### 11. Frame pacer uses standard waitable timer — no high-res support

|             |                                                  |
| ----------- | ------------------------------------------------ |
| **Address** | `0x1401CE0B0` (CPrecisionSleep::Wait, 361 bytes) |
| **Status**  | **FIXED** (wave0 hook, all modes)                |

`CreateWaitableTimerA` cannot accept `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`. Timer precision depends on global system timer interrupt rate (default 15.625ms). `timeBeginPeriod` imported but no xrefs to call site found.

**Fix:** Wave0 hook replaces with persistent `CreateWaitableTimerExW` using `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` flag (Windows 10 1803+). Falls back to standard timer on older Windows. On servers, server_timing chains on top with WSAPoll-based recv.

### 12. Timer handle created/destroyed every frame

|             |                                   |
| ----------- | --------------------------------- |
| **Address** | `0x1401CE0B0`                     |
| **Status**  | **FIXED** (wave0 hook, all modes) |

Kernel timer object allocated and freed every frame. At 90fps: 180 kernel-mode transitions/sec.

**Fix:** Wave0 creates a single persistent timer handle at init. Same hook as bug #11.

### 13. BusyWait Sleep(0) spin with no \_mm_pause()

|             |                                                      |
| ----------- | ---------------------------------------------------- |
| **Address** | `0x1401CE4C0` (CPrecisionSleep::BusyWait, 112 bytes) |
| **Status**  | **FIXED** (wave0 RET patch, all modes)               |

Tight QPC loop with `Sleep(0)`. No PAUSE instruction — starves HT sibling core's pipeline.

**Fix:** Wave0 patches first byte to RET (0xC3) unconditionally. WaitableTimer phase in Wait handles the bulk of the sleep; only ~250us of busy-wait precision lost.

### 14. Inverted backoff in CSpinWait::WaitForValue

|             |                                      |
| ----------- | ------------------------------------ |
| **Address** | `0x141500ED8` (115 bytes, 3 callers) |
| **Status**  | **FIXED** (wave0 hook)               |

Starts with `Sleep(1)`, degrades to `Sleep(0)` after 10 iterations. Maximum CPU consumption at peak contention.

**Fix:** Wave0 hook replaces with proper increasing backoff (0→10ms) and adds `YieldProcessor()` (PAUSE instruction) for HT-friendly spinning.

## Medium

### 15. Render spinlocks have no timeout (7 call sites)

|               |                                                                                                         |
| ------------- | ------------------------------------------------------------------------------------------------------- |
| **Addresses** | `0x1401D5E20`, `0x1405253E0`, `0x140538370`, `0x140538850`, `0x1405A4790`, `0x1405A93F0`, `0x1405A9520` |

Busy-wait spinlock with no timeout, no backoff, no `_mm_pause()`. If lock holder crashes or stalls (common on Wine), waiter spins forever at 100% CPU.

### 16. \_\_debugbreak() in release build resource creation paths (20+ functions)

GPU resource creation failures (`g_renderSystem == nullptr`, buffer size exceeded) trigger `int 3`. Without a debugger, this raises `EXCEPTION_BREAKPOINT` — crash on VRAM exhaustion.

### 17. LibOVRRT partial symbol resolution leaves half-initialized table

|             |                                         |
| ----------- | --------------------------------------- |
| **Address** | `0x141360790` (CVR loader, 21221 bytes) |

If LibOVRRT has partial exports (wrong version), some function pointers are set, others are NULL. Error code set but callers may check individual pointers, not the error code.

### 18. Deterministic PRNG for crypto sliding window

|             |                                               |
| ----------- | --------------------------------------------- |
| **Address** | `0x140F7F800` (CPacketEncoder::AdvanceWindow) |

Custom bit-rotation scheme, not a CSPRNG. If initial state is predictable, entire IV sequence is predictable, breaking CBC confidentiality.

### 19. Socket recreation loop with no backoff

|             |                               |
| ----------- | ----------------------------- |
| **Address** | `0x140F87EC0` (RecoverSocket) |

On send failure: close socket → `getaddrinfo` → `socket()` → `bind()` immediately. Persistent failures create tight loop burning CPU.

### 20. WSAEMSGSIZE treated as generic error

|             |               |
| ----------- | ------------- |
| **Address** | `0x1401F6D20` |

Oversized UDP datagrams truncated by OS. Truncated data may still reach decoder as generic error doesn't prevent dispatch.

### 21. Reliable send retry timer int32 overflow on frame stalls

|             |                              |
| ----------- | ---------------------------- |
| **Address** | `0x140F8B5D0` (SendReliable) |

Frame delta read as int64, truncated to int32. Single frame >2.1s wraps negative. Packet stuck in permanent retry.

### 22. O(n) linear peer search per packet

|             |               |
| ----------- | ------------- |
| **Address** | `0x1405EC330` |

Every incoming packet scans all peers. Combined with no rate limiting (#5), flood + full lobby amplifies CPU cost.

### 23. Max packet size not validated on receive

|             |               |
| ----------- | ------------- |
| **Address** | `0x140F79DE0` |

Send enforces 0x4F0 (1264 byte) max. Receive path does not validate length before decrypting into 0x4F0-byte stack buffer. Potential buffer overflow.

### 24. Synchronous getaddrinfo on network thread

|               |                                             |
| ------------- | ------------------------------------------- |
| **Addresses** | `0x1401F7AC0`, `0x1401F7C10`, `0x1401F7D60` |

DNS resolution blocks entire network layer. No DNS caching visible.

### 25. No certificate pinning on HTTP client

|             |                                  |
| ----------- | -------------------------------- |
| **Address** | `0x1401F57E0` (IXMLHTTPRequest2) |

Accepts any CA in system trust store. Corporate proxies can MITM login/matchmaking traffic.

### 26. 16-bit XOR checksum — 11 effective bits

|             |               |
| ----------- | ------------- |
| **Address** | `0x140F8B5D0` |

XOR checksum fails to detect byte swaps, double-bit errors. ~1-in-2048 chance of undetected corruption per packet.

### 27. CThread::Fork m_finished without memory barriers

|             |               |
| ----------- | ------------- |
| **Address** | `0x1401D31B0` |

Plain `int32_t`, not volatile/atomic. Compiler can hoist read out of loop. Works on x86_64 in practice; risk on Wine with aggressive optimization.

### 28. Ghost connections — retransmit timer int32 truncation

|             |               |
| ----------- | ------------- |
| **Address** | `0x140F8B5D0` |

Same as #21 but from the perspective of the peer table: the peer appears connected, sends no data. Other players see a frozen player.

### 29. INVALID_HANDLE_VALUE propagated to async resource loader

|             |               |
| ----------- | ------------- |
| **Address** | `0x1400E00B0` |

File open fails (not sharing violation) → INVALID_HANDLE_VALUE stored → resource loader tries to read through it.

### 30. VirtualAlloc NULL silently ignored in expanding allocator

|             |               |
| ----------- | ------------- |
| **Address** | `0x1400D6F20` |

Allocator struct left with `+0x38 = NULL` but other fields populated. Next use dereferences NULL.

### 31. Crash handler reentrancy drops second thread's crash

|             |               |
| ----------- | ------------- |
| **Address** | `0x1401CEF00` |

`LOCK CMPXCHG` guard with no timeout. Second crashing thread's exception silently lost. If first crash is symptom and second is cause, diagnostic is misleading.

### 32. DirectInput device not re-acquired after disconnect

|             |               |
| ----------- | ------------- |
| **Address** | `0x141055D90` |

No `IDirectInputDevice8::Acquire` call or `DIERR_INPUTLOST` handling visible. Controller stops working after unplug/replug until restart.

### 33. Wwise init failure not propagated — APIs called anyway

|             |                                       |
| ----------- | ------------------------------------- |
| **Address** | `0x140209920` (64 callees after init) |

If audio device unavailable at startup, init fails but game continues calling Wwise APIs. `RenderAudio` runs every tick doing nothing.

### 34. 8KB stack allocation per log call — 3019 call sites

|             |                                  |
| ----------- | -------------------------------- |
| **Address** | `0x1400EBE70` (CLog::PrintfImpl) |

0x2000-byte format buffer on stack per call. In deep call chains (physics → callback → network → log), can overflow stack, triggering bug #9.

### 35. BugSplat64.dll delay-load crashes crash handler when missing

|             |               |
| ----------- | ------------- |
| **Address** | `0x1401CEFE0` |

Community installations don't have BugSplat64.dll. Crash handler's `MiniDmpSender` instantiation triggers `STATUS_DLL_NOT_FOUND` inside exception handler. Double-fault.

### 36. NaN propagation in velocity normalization before replication

|             |               |
| ----------- | ------------- |
| **Address** | `0x1400DADD0` |

`1.0f / speed` with no zero check. NaN propagates through network replication to all clients.

### 37. QueryPerformanceFrequency never cached

|               |                                             |
| ------------- | ------------------------------------------- |
| **Addresses** | `0x1400D00C0`, `0x1401CE0B0`, `0x1401CE4C0` |

QPF is constant per process. Called per-invocation in 12+ hot paths. ~20ns on Windows, ~1-5µs on Wine.

### 38. DrainWorkQueue unbounded stall

|             |                           |
| ----------- | ------------------------- |
| **Address** | `0x1401D66B0` (389 bytes) |

Re-enqueues work during drain. No timeout or progress check. Multi-second freeze during scene transitions on slow I/O.

### 39. Fixed physics substep count — not frame-adaptive

|             |                                       |
| ----------- | ------------------------------------- |
| **Address** | `0x14069D820` (UpdateConstraintState) |

Substep count from config, not frame delta. Physics runs in slow-motion during frame drops. Likely intentional but produces visible artifacts.

### 40. VR-to-wallclock timing discontinuity

|             |               |
| ----------- | ------------- |
| **Address** | `0x14072D110` |

Switching from VR-derived delta (stable 11–14ms) to QPC wall time (variable) produces one-frame physics jolt.

## Low

### 41. Thread handle leak on same-thread fork

|             |               |
| ----------- | ------------- |
| **Address** | `0x1401D31B0` |

`CloseHandle` skipped when Fork called from same thread. Old handle leaked.

### 42. Sequence number off-by-one (% 1023)

|             |               |
| ----------- | ------------- |
| **Address** | `0x140F8B5D0` |

Hand-rolled `% 1023` on 10-bit space. Seq 1023 unreachable. Extra RTT every ~34 seconds.

### 43. Hardcoded 15-second network timeout

|             |               |
| ----------- | ------------- |
| **Address** | `0x140F717A0` |

Not configurable. Too short for high-latency community servers, too long for LAN.

### 44. Crash report JSON truncation (2048-byte buffer)

|             |               |
| ----------- | ------------- |
| **Address** | `0x1400D0890` |

Long install paths cause silent truncation → malformed JSON → lost crash reports.

### 45. Sleep(1) spin instead of Event wait in CThread::Fork

|             |               |
| ----------- | ------------- |
| **Address** | `0x1401D31B0` |

Adds 1–15ms per thread creation. Accumulates during startup.

### 46. 10-bit sequence window — max 1023 outstanding reliable packets

|             |               |
| ----------- | ------------- |
| **Address** | `0x140F8E310` |

Under extreme packet loss, window fills and all reliable sends stall for 15 seconds.

### 47. Replay window wraps at 499

|             |               |
| ----------- | ------------- |
| **Address** | `0x140F7F800` |

Theoretical replay attack after 500 packets. HMAC still validates, limiting practical impact.

### 48. No hard cap on peer count

|             |               |
| ----------- | ------------- |
| **Address** | `0x140F85AF0` |

Configurable stale timeout, no maximum. Connection flood exhausts peer pool.

### 49. Hardcoded dead Oculus hostname

|             |               |
| ----------- | ------------- |
| **Address** | `0x1401F7AC0` |

DNS queries to shutdown Oculus services. Wasted time during connection setup.

### 50. Embedded libevent2 — frozen version

|             |               |
| ----------- | ------------- |
| **Address** | `0x141342BD0` |

Statically linked, cannot be updated independently. Inherits any CVEs in that version.

### 51. No DPI awareness declared

Companion window wrong size on high-DPI displays. Does not affect VR headset rendering.

### 52. Fullscreen transition loses original window style

|             |               |
| ----------- | ------------- |
| **Address** | `0x14054CCC0` |

Sets `WS_POPUP | WS_VISIBLE` without saving previous style. Window chrome glitches on mode switch.

### 53. Audio device notification — two incompatible paths

|             |               |
| ----------- | ------------- |
| **Address** | `0x14138B1E0` |

COM (WASAPI) path vs Win32 (`RegisterDeviceNotificationW`) fallback. Audio hot-swap unreliable on Wine.

### 54. MXCSR save/restore in occlusion culling vulnerable to injected DLLs

|               |                                             |
| ------------- | ------------------------------------------- |
| **Addresses** | `0x1414DE950`, `0x1414CEEC0`, `0x1414D05E0` |

If injected DLL modifies MXCSR between save/restore, rounding errors cause occlusion pop-in. Theoretical.

### 55. Synchronous CreateFileW for soundbank loading

|             |               |
| ----------- | ------------- |
| **Address** | `0x1400E00B0` |

Wwise low-level I/O uses synchronous file open for some soundbank loads during gameplay. Brief audio stutter on area transitions.

### 56. COM IMMDeviceEnumerator conditional Release

|             |                                                   |
| ----------- | ------------------------------------------------- |
| **Address** | `0x14138B1E0` (audio device notification handler) |
| **Status**  | Unpatched                                         |

`CoCreateInstance` creates `IMMDeviceEnumerator` but `Release()` is only called on the success path. If downstream code assumes the COM object exists after the failure path, potential use-after-free. Related to bug #53 (two notification paths).

### 57. GetUserName player index no bounds check

|             |               |
| ----------- | ------------- |
| **Address** | `0x1401C98C0` |
| **Status**  | Unpatched     |

Player index multiplied by stride `0x250` without validating against player table bounds. Network-reachable if player index comes from a packet. Out-of-bounds read from netgame context.

### 58. Stun duration validation masks sign bit

|             |                      |
| ----------- | -------------------- |
| **Address** | `0x140D534F0` (Stun) |
| **Status**  | Unpatched            |

Validation uses `(uint32_t)arg & 0x7FFFFFFF` (abs value) before comparing to epsilon. Negative stun durations pass validation and are treated as positive.

### 59. Allocator failure vtable call before non-returning error

|             |                                              |
| ----------- | -------------------------------------------- |
| **Address** | `0x1400D4EF0` (linear allocator DirectAlloc) |
| **Status**  | Unpatched                                    |

When allocation exceeds capacity, calls a vtable function and then `NRadEngine_LogError` (non-returning). The vtable result is not checked — if it throws or corrupts state, the subsequent fatal log operates on bad data.

### 60. Crash metadata StrLen cap mismatch

|             |                               |
| ----------- | ----------------------------- |
| **Address** | `0x1401D0870` (WriteCrashLog) |
| **Status**  | Unpatched                     |

Crash metadata buffer is `0x800` bytes but `StrLen` is capped at `0x400`. If JSON content exceeds 0x400 bytes, written length is truncated, potentially leaking uninitialized stack data or producing malformed output.

### 61. AdvanceBodies frame counter int32 wraparound

|             |                                            |
| ----------- | ------------------------------------------ |
| **Address** | `0x14068A150` (AdvanceBodies physics loop) |
| **Status**  | Unpatched                                  |

Physics frame counter is `int32_t`, wraps after ~331 days at 72fps. Reset-to-1 guard only catches wrap to zero, not INT_MAX→INT_MIN transition. Causes frame numbering discontinuity in physics state.
