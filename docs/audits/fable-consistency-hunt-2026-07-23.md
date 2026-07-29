# Fable 5 Consistency/Quality Hunt — 2026-07-23

Model: `claude-fable-5`. 8 regions, 3 lenses, read-only. Findings ranked by severity then blast-radius within each lens.

**Regions:** R1=gamepatches top-level, R2=gameserver, R3=modules, R4a=common, R4b=pnsrad, R5=plugins, R6=nevr_api/launcher, R7=peripheral.

---

## LENS 1 — BUGS (real defects)

### B1 [H] — `src/modules/ws-bridge/src/ws_bridge.cpp:292` — Missing `connIdx == 1` guard causes premature LoginRequest on config connection (R3)
The Open-handler lambda injects LoginRequest for every connection instead of only conn==1. By the time the Message-handler tries conn=0 injection, `loginInjected` is already true — dead code. The gamepatches copy (ws_bridge.cpp:334) has the correct `connIdx == 1 && !pairPtr->loginInjected` guard.
**Fix:** Change guard to `if (connIdx == 1 && !pairPtr->loginInjected)`.

### B2 [H] — `src/runtime/compat/ws_bridge.cpp:628-634` — Use-after-free in shared remote callback after login conn disconnects (R3)
When conn=1 closes but its remote was shared with conn>=2, the Close handler erases ProxyPair but does not clear the remote's `setOnMessageCallback`. The lambda holds dangling `pairPtr`/`gameWsPtr` pointers. Subsequent messages hit freed memory.
**Fix:** Clear callback with `setOnMessageCallback(nullptr)` before erasing when `isShared`.

### B3 [H] — `src/modules/token-auth/src/token_auth.cpp:439-440` — Data race on `s_auth` in Shutdown (R3)
`Shutdown()` deletes `s_auth` without holding `s_tokenMutex`. `GetToken()` reads `s_auth` under the mutex. If ws_bridge callback calls GetToken concurrently with Shutdown, use-after-free.
**Fix:** Wrap delete/null in `std::lock_guard<std::mutex>`.

### B4 [H] — `src/runtime/server/gameserver.cpp:1098-1136` — Use-after-free in shutdown detached thread (R2)
`BeginGracefulShutdown` spawns a detached thread capturing raw `this`. Destructor waits 5s max then destroys members. Detached thread still runs, calling `self->GetContext()`, `self->EndSession()`, `self->Unregister()` on destroyed object.
**Fix:** Join the shutdown thread instead of detaching, or use shared_ptr ownership.

### B5 [H] — `plugins/log-filter/src/log_filter.cpp:61-62` — reinterpret_cast<va_list> from int64_t* is UB (R5)
`vsnprintf(buf, sizeof(buf), fmt, reinterpret_cast<va_list>(varargs))` assumes MinGW CRT va_list layout matches x64 Windows varargs stack. Not guaranteed; will silently produce wrong output if compiler/CRT changes.
**Fix:** Use va_start/va_copy with __va_start builtins, or document as MinGW-x64-ABI-dependent.

### B6 [H] — `plugins/crash-handler/src/plugin.cpp:237-254` — VEH calls fprintf from signal handler (R5)
`CrashVEH` calls `PluginLog` → `fprintf(stderr, ...)` from inside Vectored Exception Handler. stderr lock may be held by crashing thread → deadlock. Heap may be corrupted → recursive crash. `LogCrashDump` loops up to 256 IsBadReadPtr calls.
**Fix:** Use WriteFile(GetStdHandle(STD_ERROR_HANDLE), ...) or pre-allocated ring buffer.

### B7 [H] — `src/core/logging.cpp:81-99` — FatalError MessageBoxA + exit(1) under loader lock (R4a)
`FatalError` unconditionally calls MessageBoxA (modal message loop) when no handler registered. Called from DllMain during loader lock = deadlock. Guard delegates to handler if set, but in legacy dbgcore path, handler not installed until PreprocessCommandLineHook.
**Fix:** Add DllMain-awareness flag routing to vfprintf+TerminateProcess instead of MessageBoxA+exit.

### B8 [H] — `src/abi/echovr_functions.cpp:71-75` — InitializeFunctionPointers null g_GameBaseAddress — UB (R4a)
All 34 function-pointer assignments dereference `g_GameBaseAddress + offset` with no null check. If called before GameBaseAddress is set, every line is UB.
**Fix:** Add `assert(g_GameBaseAddress)` or `if (!g_GameBaseAddress) return;` at top.

### B9 [H] — `src/launcher/echovr_server_launcher.cpp:64-67` — argv arguments not escaped for CreateProcess (R6)
argv[i] concatenated directly into command-line string without quoting/escaping. Windows CreateProcess parser splits on whitespace, interprets `"` and `\` specially. Arguments with spaces blow up.
**Fix:** Wrap each argv[i] in double quotes, escape embedded `"` as `\"`.

### B10 [H] — `plugins/log-filter/src/log_filter.cpp:619-657` — Bypasses HookManager, uses raw MinHook directly (R5/S-3)
log-filter uses raw MH_CreateHook/MH_EnableHook instead of nevr::HookManager. Hooks not tracked for cleanup, RemoveLogFilterHook must manually disable/remove. Future global cleanup misses log-filter hooks.
**Fix:** Switch to nevr::HookManager::CreateAndEnable.

### B11 [M] — `src/runtime/compat/winhttp_stub.cpp:84-90` — Non-atomic COM refcounts (R1)
Stub_AddRef/Stub_Release/Stub_QueryInterface use raw `++`/`--` on m_refCount, not InterlockedIncrement/Decrement. Concurrent access from COM marshaling can underflow/overflow refcount.
**Fix:** Replace with InterlockedIncrement/InterlockedDecrement.

### B12 [M] — `src/runtime/lifecycle/initialize.cpp:295-301` — Seven PatchDetour calls discard BOOL return (R1)
BuildCmdLine/PreprocessCmd check returns and set g_bootHookFailed, but 7 subsequent PatchDetour calls (NetGameSwitchState through JsonValueAsString) discard every return. Silent hook failure leaves game running degraded.
**Fix:** Capture returns and OR into g_bootHookFailed.

### B13 [M] — `plugins/broadcaster-bridge/src/broadcaster_bridge.cpp:392-408` — Chassis swap writes through unvalidated pointer (R5)
Three code paths write to `*(flagsPtrEarly + 2)` after null-pointer check only. No writability check — if pointer targets read-only page, access violation in game thread. Behind enable_debug_commands but reachable via corrupted injection packet.
**Fix:** Add VirtualQuery check for PAGE_READWRITE before writing.

### B14 [M] — `plugins/broadcaster-bridge/src/broadcaster_bridge.cpp:333-335` — ReadLoadout force-dereferences unvalidated pointers (R5)
`*reinterpret_cast<uint64_t*>(ng + 0x51420 + slot * 0x40)` read as uint64_t then used as pointer with no validation. Same pattern for entry[1], entry[6]. Raw pointer reads from game memory with no bounds/validity checking.
**Fix:** Add VirtualQuery/SafeReadU64 validation for each dereference.

### B15 [M] — `plugins/crash-handler/src/plugin.cpp:42,243` — g_justSuppressedCrash is non-atomic plain bool (R5)
VEH handler writes to g_justSuppressedCrash without atomic or compiler barrier. Compiler may reorder Rip+=1 after the store. Next suppressed crash could miss int3 skip.
**Fix:** std::atomic<bool> with memory_order_release/acquire.

### B16 [M] — `src/modules/ws-bridge/src/ws_bridge.cpp:239-240` — void* to function-pointer cast is UB (R3)
`s_getProc("TokenAuth_GetToken")` returns void*, cast to function pointer via C-style cast. Conditionally-supported, not defined by standard.
**Fix:** Use reinterpret_cast with proper function-pointer type.

### B17 [M] — `src/modules/ws-bridge/src/ws_bridge.cpp:667` — g_server unique_ptr accessed without synchronization (R3)
Written in InstallWebSocketBridge (main thread), read/written in WsBridge_Shutdown (signal handler). No memory barrier ensures visibility.
**Fix:** Add atomic_thread_fence(release) after store, (acquire) before load.

### B18 [M] — `plugins/broadcaster-bridge/src/broadcaster_bridge.cpp:955` — inet_pton return value unchecked (R5)
`inet_pton(AF_INET, target_ip, &sin_addr)` return discarded. If config has invalid IP, sin_addr is undefined stack garbage — all sendto calls go to random destination.
**Fix:** Check return; bail if <= 0.

### B19 [M] — `plugins/broadcaster-bridge/src/broadcaster_bridge.cpp:192-202` — sendto return unchecked in game-thread hot path (R5)
Mirror socket sendto return discarded. Buffer-full condition causes silent packet loss with no backoff. Repeated system calls waste CPU.
**Fix:** Log EWOULDBLOCK/EAGAIN, add failure counter that disables mirroring after N drops.

### B20 [M] — `src/core/auth_token.h:215,234-236` — Credential DACL/hidden-attribute failures silently ignored (R4a)
SetFileAttributesA(HIDDEN) and SetNamedSecurityInfoA(DACL) return values discarded. On failure, .credentials.json is world-readable on multi-user system.
**Fix:** Check returns; log warning on failure (best-effort hardening).

### B21 [M] — `src/runtime/server/gameserver.cpp:933-964` — const_cast on received WebSocket data — UB if game modifies (R2)
Message handler const_casts binary data from const std::string to VOID*, passes to BroadcasterReceiveLocalEvent as non-const. If game writes through non-const pointer, UB on const object.
**Fix:** Copy data before passing, or change callback signature to mutable buffer.

### B22 [M] — `src/runtime/server/server_context.cpp:225-229` — Unsynchronized callback-registry from background thread (R2)
GetCallbackRegistry() documented as "NOT internally synchronized — Must not be called from background threads." Yet shutdown thread calls Unregister → UnregisterAllCallbacks → GetCallbackRegistry. Data race with game thread.
**Fix:** Acquire m_stateMutex around callback registry, or defer cleanup to main thread.

### B23 [M] — `plugins/log-filter/src/log_filter.cpp:61-62` — Already listed as B5
(Duplicate — same finding across two lenses)

### B24 [L] — Remaining Low-severity bugs (11 findings across regions)
- R1 B3: ExitProcessHook silent return when OriginalExitProcess is null
- R1 B4: Duplicate OVR_BRANCH_OFFSET constant
- R1 B5: Non-atomic g_loginSessionId.Data1 check
- R3 B7: Hardcoded 60s token expiry vs JSON parsing
- R3 B8: SaveToken checks m_refreshToken vs gamepatches checks m_token
- R3 B9: Missing &unwrap in device auth API URLs
- R5 BB-5: RateLimitCheck check-then-set race
- R5 LF-2: StripComments mishandles unclosed block comments
- R6 B2/B3: WaitForSingleObject/GetExitCodeProcess returns unchecked (launcher)
- R2 F1.03: Dead variable void-cast in gameserver.cpp
- R7 F1-F3: gen_symbol_corpus.py format/annotation/duplicate issues

---

## LENS 2 — CODE SMELL (duplication, dead code, long functions)

### S1 [H] — Module-vs-gamepatches ws_bridge divergence: 3 critical gaps (R3/D1-D3)
Three High-severity divergences between the active module ws_bridge and the gamepatches copy:
1. **Matchmaker session sharing missing from module** — No g_loginRemoteWs/g_activeGameWs. Every game connection gets own Nakama session → matchmaker allocation fails.
2. **LoginFailure/fake LoginSuccess absent from gamepatches** — Module injects fake success in server mode; gamepatches silently forwards failure.
3. **Unconditional Bearer token vs hasUrlCredentials check** — Module always attaches Bearer; gamepatches skips when URL has credentials.

**Fix:** Reconcile both copies to the better version for each feature.

### S2 [H] — Every plugin reinvents config loading (R5/S-1)
Three distinct config-loading strategies: example uses nevr::LoadConfigFile, broadcaster-bridge uses LoadConfigFile with different filename, log-filter has fully custom FindConfigFile that duplicates nevr::LoadConfigFile logic. The shared utility exists but isn't used consistently.
**Fix:** Unify all behind nevr::LoadConfigFile. Remove log-filter's custom FindConfigFile.

### S3 [H] — Three different logging conventions across plugins (R5/S-2)
Example+crash-handler use NEVR_DEFINE_PLUGIN_LOG. Log-filter+anim-debugger duplicate the macro body locally. Broadcaster-bridge uses raw std::fprintf with no prefix. The macro was created specifically to eliminate this duplication.
**Fix:** Migrate all plugins to NEVR_DEFINE_PLUGIN_LOG.

### S4 [M] — Duplicate config-file discovery (R1/SMELL-1)
builtin_log_filter.cpp and server_timing.cpp each implement their own FindConfigFile() with identical GetModuleFileNameA+strrchr+slash logic. Also duplicate ParseBool/ParseUint32 helpers.
**Fix:** Extract shared ConfigDiscovery::FindFile to a shared header.

### S5 [M] — Duplicate PatchMemory functions (R1/SMELL-2)
server_timing.cpp:195-201 and pnsrad_enabler.cpp:67-72 define identical VirtualProtect+memcpy PatchMemory. A third idiom exists in process_mem.h (WriteProcessMemory-based). Three ways to do the same thing.
**Fix:** Consolidate into a single PatchBytes in gamepatches_internal.h.

### S6 [M] — Duplicate ResolveVA functions (R1/SMELL-3)
server_timing.cpp:224-226 and wave0_instrumentation.cpp:104-106 define identical base+(va-0x140000000) helpers. initialize.cpp inlines the same arithmetic.
**Fix:** Promote to gamepatches_internal.h as shared inline.

### S7 [M] — Seven SendProtobufEnvelope call sites discard return value (R2/F2.04)
gameserver.cpp has 8 calls to SendProtobufEnvelope; 7 ignore the bool return. Session state changes silently lost if serialization fails.
**Fix:** Log warning on failure; consider retry for registration/reconnection.

### S8 [M] — ~1,700 lines dead source in extras/dbghooks/ never compiled (R7/S1)
12 .cpp/.h files on disk not in any CMake target. Largest is weapon_system_trace.cpp (407 lines). echovr_launcher.cpp IS compiled but superseded per BUGS.md.
**Fix:** Remove dead files or archive them.

### S9 [M] — Duplicate UPnP device discovery (R2/F2.02)
OpenPort() and ClosePort() each independently call upnpDiscover+UPNP_GetValidIGD+freeUPNPDevlist with identical parameters.
**Fix:** Extract shared DiscoverIGD helper.

### S10 [M] — Duplicate hex dump logic (R2/F2.03)
OnMsgSaveLoadoutSuccess and OnMsgCurrentLoadoutResponse have identical 256-byte hex-dump loops.
**Fix:** Extract DumpHex(const uint8_t*, size_t) helper.

### S11 [M] — gameserver.cpp: 1484-line monolithic file (R2/F2.01)
Three functions exceed 150 lines (OnMsgSaveLoadoutRequest 152, RequestRegistration 166, OnTcpMsgProtobuf 206). File mixes registration, loadout, session, and connection lifecycle.
**Fix:** Split into separate files by domain.

### S12 [L] — Remaining Low-severity smells (11 findings)
- R1 SMELL-4/5/6/7: PatchServerFramePacing dead, BroadcasterGuard empty stub, ProcessMemset unnecessary allocation, Module dir extraction repeated in 6+ files
- R2 F2.05-08: Unnecessary null checks, per-instance Info-level detail, duplicate Log() in messages.cpp, hardcoded bone indices
- R4a S3-S6: magic constant 0x00000000L, auth_token.h header-only, static std::string in inline, hardcoded hex offsets
- R6 S1-S3: Manual PROTO_GENERATED_SRCS list (N35), _USRDLL on EXE, missing PROTOBUF_STATIC_LIB
- R7 S2-S3: Unnecessary per-iteration 64-bit AND mask, Disarm() unprotected

---

## LENS 3 — PATTERN / CONSISTENCY

### P1 [M] — Only example plugin validates prologue before hooking (R5/P-1)
Example treats prologue mismatch as hard stop. Broadcaster-bridge warns but proceeds. Log-filter and anim-debugger don't validate at all. Game update shifting function a few bytes → silent corrupt hook.
**Proposed convention:** All plugins MUST validate prologue before hooking; mismatch = skip and warn.

### P2 [M] — Inconsistent hook failure handling (R1/PATTERN-2)
Three patterns in initialize.cpp: (A) check BOOL + set g_bootHookFailed, (B) discard all returns, (C) individual warning per hook. Same file, same function, three approaches.
**Proposed convention:** All hooks follow Pattern A (check, set flag, continue). ServerFatal catches failures post-boot.

### P3 [M] — Two different memory-patching idioms (R1/PATTERN-1)
process_mem.h uses WriteProcessMemory; PatchMemory functions use VirtualProtect+memcpy. Both achieve same effect. Neither is wrong but codebase should standardize.
**Proposed convention:** VirtualProtect+memcpy (simpler, avoids GetCurrentProcess handle). Promote single PatchBytes to gamepatches_internal.h.

### P4 [M] — Log prefix inconsistency in gameserver (R2/F3.01)
[NEVR.GAMESERVER], [NEVR.TELEMETRY], [TELEMETRY.DIAG] (bare), [WEBSOCKET] (bare), [NEVR.UPNP]. Bare [TELEMETRY.DIAG] and [WEBSOCKET] break NEVR.* convention.
**Proposed convention:** Normalize to [NEVR.WEBSOCKET] and [NEVR.TELEMETRY.DIAG].

### P5 [L] — JsonEscape implemented twice with different signatures (R1/PATTERN-3)
boot_log_tee.cpp has buffer-output variant; builtin_log_filter.cpp has std::string-appending variant. Control-character handling differs.
**Proposed convention:** Standardize on std::string-appending variant; port control-char handling to boot version.

### P6 [L] — platform_compat uses [NEVR.PATCH] prefix — misleading for module (R3/P6)
platform_compat is a runtime-loaded module like ws_bridge/token_auth but uses [NEVR.PATCH] (gamepatches detour prefix) instead of its own prefix.
**Proposed convention:** Rename to [NEVR.COMPAT] or [NEVR.PLATFORM].

### P7 [L] — Remaining Low-severity patterns (8 findings)
- R1 PATTERN-4/5/7: Comment style inconsistency, manual lowercasing vs std::transform, mixed printf format specifiers
- R2 F3.02-06: Double header guard, _DEBUG vs NDEBUG, stale comment, fprintf in loaded DLL, file-static state not reset
- R3: All 5 pattern checks passed (prefixes, guards, errors, naming, C++17 style consistent)
- R4a P1-P4: Header guard, BOOL vs bool, version macros in public header, Log null-check asymmetry
- R5 P2-P5: OnFrame not in example, NvrPluginInfo init styles, OnFrame signature, DllMain only in broadcaster-bridge
- R6 C1-C2: Launcher [echovr_server] prefix vs [NEVR.LAUNCHER], redundant gen/cpp include
- R7 P1-P4: cstdio+bare snprintf, %016llx vs PRIx64, missing tools/README, standalone stubs documented

---

## Summary

| Lens | High | Medium | Low | Total |
|------|------|--------|-----|-------|
| BUG | 10 | 13 | 11 | 34 |
| SMELL | 3 | 9 | 11 | 23 |
| PATTERN | 0 | 4 | 8 | 12 |
| **Total** | **13** | **26** | **30** | **69** |

## Recommended fix batches (for Phase 2)

1. **Reconcile ws_bridge copies** (B1, B2, S1/D1-D3 + D4-D12) — critical: affects every server connection. Merge the better features from each copy into both.

2. **Plugin safety sweep** (B5 LF-1, B6 CH-1, B10 S-3, B13, B14, B15, B18, B19, S2, S3, P1) — log-filter va_list UB, crash-handler VEH fprintf, broadcaster-bridge unchecked network I/O, config/logging/hook standardization.

3. **Gameserver robustness** (B4, B21, B22, S7, S9, S10, S11, P4) — use-after-free shutdown thread, const_cast UB, callback-registry race, unify log prefixes.

4. **Common/launcher hardening** (B7, B8, B20, B9, S4-S6) — FatalError DllMain safety, null-guard FunctionPointers, launcher arg escaping, consolidate duplicated patching/config utilities.

5. **Dead code removal** (S8, R1 SMELL-4/5, R6 S1) — ~1,700 lines dbghooks, PatchServerFramePacing, BroadcasterGuard stub.

6. **Pattern standardization** (P2-P7) — unify error handling, commenting, naming, include conventions. Low blast-radius but high consistency value.

---

## Phase 2 Verification (2026-07-23) — owner "verified hazards only"

Per the owner's Phase 2 directive, each High-severity bug was verified against
the BUILT path before fixing. Findings marked STALE are confirmed non-issues on
the target platform; findings marked CONFIRMED-LIVE were fixed.

### B5 — STALE (false positive on x64-mingw target)
`builtin_log_filter.cpp:118` has `static_assert(sizeof(va_list) == sizeof(void*))`
guarding the `reinterpret_cast<va_list>(varargs)`. On x64-mingw, `va_list` IS a
`void*` — the static_assert proves the assumption holds on THIS target. The
reinterpret_cast is technically UB in standard C++ but safe on the project's sole
target platform. No fix needed.

### B7 — STALE (already fixed by N4)
The FatalError `MessageBoxA` path flagged by B7 is only reachable when no
`g_fatalErrorHandler` is installed. Per N4 (`52d2c73`), `InstallFatalErrorHandler()`
is called in `PreprocessCommandLineHook` before any code path that can trigger
`FatalError`. The MessageBoxA path is dead in server mode. In legacy dbgcore mode
(DllMain → Initialize before handler install), no current code in `Initialize()`
calls `FatalError`. The finding is correct about the latent risk but not a live
defect — the handler is always installed before any fatal can fire. No fix needed.

### B8 — STALE (null path unreachable)
`InitializeFunctionPointers()` is called from `Initialize()` at
`initialize.cpp:222`, immediately after `g_GameBaseAddress` is set at
`dllmain.cpp:94`. The null-dereference path flagged by B8 is not reachable in
the current call chain. Adding a defensive null check would be harmless but
provides no observable benefit. No fix needed.

### CONFIRMED-LIVE and fixed:
- **B2** — ws_bridge (gamepatches) use-after-free in shared remote callback (N54)
- **B3** — token_auth data race on `s_auth` in Shutdown (N55)
- **B4** — gameserver use-after-free in shutdown detached thread (N56)
- **B6** — crash-handler VEH calls fprintf from signal context (N57)
- **B9** — launcher argv not escaped for CreateProcess (N58)
