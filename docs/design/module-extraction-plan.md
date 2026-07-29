# Module Extraction Plan

**Date:** 2026-04-10
**Base commit:** `f69e24f` (known-good, monolithic loader)
**Goal:** Extract self-contained subsystems from BugSplat64.dll into loadable module DLLs.

## Why

The monolithic loader (src/runtime/) contains ~3500 lines mixing unrelated concerns: TLS hooks, WinHTTP bridging, WebSocket proxy, auth flow, directory fixes, log filtering, server timing. Extracting into modules enables:

- Independent testing of each subsystem
- Conditional loading (skip server-timing on clients)
- Faster iteration (rebuild one module, not the entire loader)

## Approach

**Incremental extraction with launch verification after each step.**

Each step: extract code → build → deploy → launch → verify the specific feature works → commit.

## Extraction Order

Based on dependency analysis of boot.cpp. Extract in reverse dependency order (least dependencies first):

1. **Foundation: Module interface + loader** — Add NvrModuleInterface and LoadModule() to the loader. No extraction yet, just the infrastructure.

2. **platform-compat** — TLS hook + CreateDirectory hooks. Zero cross-module dependencies. Self-contained.
   - Verify: launch, check for `[NEVR.PATCH] SSL/TLS modernization hook installed`

3. **winhttp-proxy** — CoCreateInstance hook + libcurl WinHTTP stub. Depends on game base address only.
   - Verify: launch, check for `[NEVR.PATCH] WinHTTP to libcurl hook installed`

4. **log-filter builtin → module** — Already exists as both a plugin and a builtin. Extract the builtin to a module.
   - Verify: launch, check for `[log_filter] hook installed on CLog::PrintfImpl`

5. **server-timing builtin → module** — Same pattern as log-filter.
   - Verify: launch, check for `[server_timing]` messages (only on -server)

6. **token-auth** — Device code auth. Depends on config (reads HTTP URI). No other module dependencies.
   - Verify: launch, check for `[NEVR.AUTH] Configured:` and device code prompt

7. **ws-bridge** — WebSocket TLS proxy. Depends on token-auth (reads JWT for injection). Must load after token-auth.
   - Verify: launch, check for `[NEVR.WS] Proxy listening on ws://127.0.0.1:`

8. **pnsrad-enabler** — Social platform DLL forcing. Already a plugin; convert to module for earlier loading.
   - Verify: launch, check for `[pnsrad_enabler] patched`

## Lessons from the Failed Attempt

Every module MUST:

1. Call `Hooking::Initialize()` before any PatchDetour/MH_CreateHook call
2. Use `LoadLibraryA()` as fallback when `GetModuleHandleA()` might fail (Secur32.dll, ole32.dll)
3. Check PatchDetour return values and log+propagate failures
4. Have accurate comments (no stale "does not call MH_Initialize" lies)

The `PatchDetour` function in patching.h must return BOOL, not void.

## What NOT to Extract

- boot.cpp — the orchestrator stays in the loader
- config.cpp — the config system stays in the loader (modules read config via context)
- mode_patches.cpp — game-mode patches stay in the loader
- crash_recovery.cpp — process-level crash handling stays in the loader
- cli.cpp — command line parsing stays in the loader
- plugin_loader.cpp — plugin loading stays in the loader

## Testing Strategy

After each extraction:

1. `just verbose-build` — must compile with zero errors
2. `./launch-client.sh` — game must launch
3. Grep output for the module's log messages — feature must be active
4. Compare output to the known-good baseline (captured in this session)

## Known-Good Baseline Output

From launch test at f69e24f:

```
[NEVR.PATCH] Initializing v3.2.0+235.f69e24f
[NEVR] patches OK
[NEVR.AUTH] Configured: url=https://g.echovrce.com:7350
[NEVR.WS] Proxy listening on ws://127.0.0.1:6821 -> wss://g.echovrce.com:7350/ws
[NEVR.PLUGIN] 6 plugin(s) loaded
[log_filter] hook installed on CLog::PrintfImpl @ 0x1400ebe70
[pnsrad_enabler] init complete (2 echovr.exe patch(es))
```

Every module extraction must preserve this output.
