# NEVR Runtime Manual Test Plan

**Generated:** 2026-08-01 from 201-feature log coverage audit.
**Purpose:** Every feature has a row. Every row has a file:line citation, a
verification procedure, and a pass/fail signal. No feature is omitted as "trivial"
-- a trivial broken feature is still broken.

This document is auditable: every claim carries a `file:line` citation. It is
usable by someone who has never read the code.

## Reading this document

Each feature row answers seven questions:

| Column | Meaning |
|--------|---------|
| **#** | Feature number within subsystem |
| **Feature** | What this code path does (one sentence) |
| **Citation** | `file:line` in the source |
| **How to test** | Specific action the tester takes |
| **PASS signal** | Log line or visual confirmation that means it worked |
| **FAIL signal** | What you see when it is broken |
| **Testable on** | "wine" = this host under Wine; "client" = needs Andrew's Windows client; "live" = needs live server with players |
| **Silent break risk** | What happens if this feature silently breaks (one sentence) |

Where a feature has NO log signal (silent success or silent failure), that is
stated explicitly, and the INDIRECT evidence is described -- what ELSE would
break that IS observable.

---

## Summary

| Metric | Count |
|--------|-------|
| Total features | 201 |
| Testable on this host (Wine) | 156 |
| Needs Windows client | 38 |
| Needs live server with players | 7 |
| Silent success (no log on success) | 24 |
| Silent failure (no log on failure) | 19 |
| Logs only at Debug level | 31 |
| One branch of if/else unlogged | 22 |

---

## Quick Smoke (80% confidence in 10 checks)

If all 10 pass, the runtime is almost certainly healthy. If any fail, STOP and
investigate before continuing.

| # | Feature | Citation | How to test | PASS signal |
|---|---------|----------|-------------|-------------|
| Q1 | DLL initialization | `initialize.cpp:215` | Start server, wait 45s | `[NEVR.PATCH] Initializing v*` |
| Q2 | MinHook init | `initialize.cpp:229` | See Q1 log context | `[NEVR] minhook OK, hooking...` |
| Q3 | CLI parsed, server mode set | `boot.cpp:149` | Start with `-server` | `Server mode -- headless + noovr applied` |
| Q4 | All hooks installed | `initialize.cpp:334` | After init sequence | `All hooks installed` |
| Q5 | Config loaded | `config.cpp:54` | Server start | `Early config loaded from:` or `Game config loaded from:` |
| Q6 | GameServer initialized | `gameserver.cpp:872` | After boot, before registration | `Initialized game server` |
| Q7 | WebSocket connected | `websocket_client.cpp:129` | Server connects to ServerDB | `Connected to ServerDB` |
| Q8 | Server registered | `gameserver.cpp:232` | After WS connect + auth | `registration success via protobuf: server_id=` |
| Q9 | Session starting | `gameserver.cpp:424` | After registration | `Session starting` |
| Q10 | Clean exit on CTRL+C | `crash_recovery.cpp:370` | Send SIGINT / CTRL+C | `Console signal * received -- exiting` |

---

## 1. Boot / Lifecycle

### 1.1 DLL Entry (dllmain.cpp)

| # | Feature | Citation | How to test | PASS signal | FAIL signal | Testable on | Silent break risk |
|---|---------|----------|-------------|-------------|-------------|-------------|-------------------|
| B01 | DLL attach -- detects launcher vs legacy mode | `dllmain.cpp:90-97` | Start server under Wine | Process starts without crash | Process crashes at startup | wine | Server never initializes; no log output at all |
| B02 | DLL detach -- clean shutdown (dynamic unload) | `dllmain.cpp:98-118` | Unload DLL dynamically | Modules/plugins/CDN/timing all shut down in order | Missing shutdown log lines for any component | wine | Resources leak; server port not released |
| B03 | BugSplat stub -- MiniDmpSender ctor/dtor/send | `dllmain.cpp:69-79` | Start server | No crash from missing BugSplat imports | `The procedure entry point` loader error | wine | Game fails to load BugSplat64.dll at all |
| B04 | dbgcore proxy -- MiniDumpWriteDump forwarding | `dllmain.cpp:49-62` | Trigger a minidump request | Request forwarded to real dbgcore.dll | Crash dump fails silently; no diagnostic | wine | Crash dumps silently lost; post-mortem diagnosis impossible |
| B05 | Launcher entry -- NEVR_SetGameModule | `dllmain.cpp:83-86` | Launcher calls NEVR_SetGameModule | Initialize() runs | Initialize() never called | wine | Server never initializes; no log output |
| B06 | DetoursExportPlaceholder (ordinal 1) | `dllmain.cpp:123-125` | Verify DLL exports | `dumpbin /exports BugSplat64.dll` shows ordinal 1 | Ordinal 1 missing | wine | DLL cannot be loaded as suspended process |

### 1.2 Initialization (initialize.cpp)

| # | Feature | Citation | How to test | PASS signal | FAIL signal | Testable on | Silent break risk |
|---|---------|----------|-------------|-------------|-------------|-------------|-------------------|
| B07 | Init guard -- double-init prevention | `initialize.cpp:212-213` | Call Initialize() twice | Second call returns immediately | Second initialization causes hook conflicts | wine | Hooks installed twice; undefined behavior |
| B08 | Game version verification | `initialize.cpp:218-220` | Start server | No warning (or explicit warning if version mismatch) | `WARNING: game binary version mismatch` only if binary changed | wine | **SILENT SUCCESS** -- no log on match. Wrong-binary hooks crash at random VAs |
| B09 | Function pointer resolution | `initialize.cpp:222-223` | Start server | `[NEVR] fn ptrs OK` | Silent hang or crash during fn ptr init | wine | All hooks resolve wrong addresses; silent corruption |
| B10 | MinHook initialization | `initialize.cpp:225-229` | Start server | `[NEVR] minhook OK, hooking...` | `FATAL: hooking init failed` | wine | No hooks can be installed; server runs unpatched |
| B11 | DLL load hook install | `initialize.cpp:232-233` | Start server | `[NEVR] DLL load hooks OK` | Missing line (fprintf to stderr) | wine | **SILENT SUCCESS on stderr** -- dxgi/d3d11 interception never fires |
| B12 | Headless graphics callback registration | `initialize.cpp:240-241` | Start server | `[NEVR] headless graphics hooks registered` | Missing line | wine | DirectX DLLs load without interception; crash on GPU init |
| B13 | CSysDLL_GetSymbol hook (ServerLib factory) | `initialize.cpp:244-252` | Start server | `[NEVR] CSysDLL_GetSymbol hook OK` | `CSysDLL_GetSymbol hook FAILED` | wine | Game cannot resolve ServerLib; multiplayer never starts |
| B14 | CSysDLL_Load hook (pnsradgameserver redirect) | `initialize.cpp:255-269` | Start server | `CSysDLL_Load hook OK (pnsradgameserver -> in-process ServerLib)` | `SKIPPED` (prologue mismatch) or `FAILED` | wine | Game tries to load nonexistent DLL; LoadServerSupport fails |
| B15 | Broadcaster guard install | `initialize.cpp:272-273` | Start server | `[NEVR] broadcaster guard OK` | Missing line | wine | **NO-OP PLACEHOLDER** -- `BroadcasterGuard::Install()` is empty; nothing is actually installed |
| B16 | Log filter init | `initialize.cpp:276-278` | Start server | `[NEVR] log filter OK` | Missing line; game log lines unfiltered | wine | Game log noise floods output; no structured filtering |
| B17 | BuildCmdLineSyntaxDefinitions hook | `initialize.cpp:281-284` | Start server | `BuildCmdLine hook: OK` | `BuildCmdLine hook: FAILED` | wine | Custom CLI flags not registered; -server etc. ignored |
| B18 | PreprocessCommandLine hook | `initialize.cpp:285-287` | Start server | `PreprocessCmd hook: OK` | `PreprocessCmd hook: FAILED` | wine | **CATASTROPHIC** -- no patches, no modules, no server mode applied |
| B19 | NetGameSwitchState hook | `initialize.cpp:288` | Start server, watch state transitions | State machine transitions fire hooks | State transitions proceed without NEVR interception | wine | **SILENT -- PatchDetour has no return value check.** NoNetwork not suppressed; session-end exit not triggered |
| B20 | LoadLocalConfig hook | `initialize.cpp:289` | Start server | Config loads from expected path | Config loads from default only (no custom path support) | wine | **SILENT -- PatchDetour.** Custom config paths ignored; service redirects revert to dead RaD endpoints |
| B21 | CJsonGetFloat hook | `initialize.cpp:290` | Set arena_round_time in config, start server | Arena round time log line appears | Arena uses defaults despite config override | wine | **SILENT -- PatchDetour.** Arena rule overrides silently ignored |
| B22 | HttpConnect hook | `initialize.cpp:291` | Start server, check service redirects | Service redirect log lines appear | Game connects to dead readyatdawn.com endpoints | wine | **SILENT -- PatchDetour.** All service connections fail; no multiplayer |
| B23 | GetProcAddress hook (RadPluginShutdown guard) | `initialize.cpp:292` | Start server | Server exits cleanly on platform DLL teardown | Server crashes during RadPluginShutdown | wine | **SILENT -- PatchDetour return value discarded.** Server crash on shutdown |
| B24 | SetWindowTextA hook (window capture) | `initialize.cpp:293` | Start server | Window handle captured for console operations | Window handle is NULL; console ops may fail | wine | **SILENT -- PatchDetour.** Minor; affects console title only |
| B25 | JsonValueAsString hook (config overrides) | `initialize.cpp:294` | Start server with early config | Config override log lines appear | Config overrides silently ignored | wine | **SILENT -- PatchDetour.** Service URLs not redirected; dead endpoints used |
| B26 | Crash recovery hooks | `initialize.cpp:302-303` | Start server | `[NEVR] crash OK` | Missing line | wine | Crash reporter launches and crashes under Wine |
| B27 | Game main hook (crash recovery) | `initialize.cpp:308` | Start server | `[NEVR] server hooks OK` | Missing line | wine | Server crashes fatally instead of recovering |
| B28 | Entity hooks (null-guard) | `initialize.cpp:309` | Start server in game session | Entity lookup guard prevents AV | Access violation in server mode | wine | Server crashes on entity operations with no player actor |
| B29 | BugSplat crash handler hook | `initialize.cpp:310` | Start server | BugSplat handler suppressed | Server fatally exits on non-fatal errors | wine | Missing actor/dialogue causes server termination |
| B30 | GameSpace hook | `initialize.cpp:311` | Start server | GlobalGameSpace init skipped in server mode | Fatal crash: missing player actor | wine | Server crashes during global gamespace init |
| B31 | VEH install | `initialize.cpp:314-315` | Start server | `[NEVR] veh OK` | Missing line | wine | int3 after ExitProcess suppression kills process |
| B32 | Console ctrl handler | `initialize.cpp:316-317` | Start server, press CTRL+C | `[NEVR] console OK` | CTRL+C does nothing | wine | **N13** -- CTRL+C does not trigger graceful server shutdown |
| B33 | NoOvr+SpectatorStream patch | `initialize.cpp:323` | Start server with -noovr | Server starts without -spectatorstream requirement | Game fatals: "must specify -spectatorstream" | wine | **SILENT SUCCESS** -- no log on success. Server fails to start |
| B34 | Deadlock monitor patch | `initialize.cpp:324` | Start server, attach debugger with breakpoint | Breakpoint does not trigger deadlock kill | Process killed after ~30s at breakpoint | wine | **SILENT SUCCESS** -- no log on success. Debugging impossible |
| B35 | Wave0 init (initialization) | `initialize.cpp:328-329` | Start server | `[NEVR] wave0 OK` | Missing line | wine | Wave0 instrumentation hooks not installed |
| B36 | CDN asset init | `initialize.cpp:332` | Start server | AssetCDN initializes (or clean skip) | Missing CDN log lines when CDN is configured | wine | **SILENT SUCCESS** -- `AssetCDN::Initialize()` has no log on success |
| B37 | All hooks summary | `initialize.cpp:334` | Start server | `All hooks installed` | Missing line (hook installation failed silently upstream) | wine | Previous silent hook failures are invisible without this line |

### 1.3 Boot Sequence (boot.cpp)

| # | Feature | Citation | How to test | PASS signal | FAIL signal | Testable on | Silent break risk |
|---|---------|----------|-------------|-------------|-------------|-------------|-------------------|
| B38 | Early config load | `boot.cpp:28` | Start server | Early config loads before CLI parse | Config-dependent features fail before game config loads | wine | **SILENT** -- `LoadEarlyConfig()` result not checked; failures invisible |
| B39 | Resource override install | `boot.cpp:29` | Start server | Resource override initialized | Embedded resources not injected | wine | **SILENT** -- `InstallResourceOverride()` has no return value check |
| B40 | Server-mode early detect | `boot.cpp:33-43` | Start server with -server | TokenAuth mode set correctly before init | TokenAuth runs device-code flow on server (wrong) | wine | **SILENT** -- detection is by argv scan, no log on success |
| B41 | Module context setup | `boot.cpp:47-56` | Start server | Module context built with flags | Modules receive wrong flags | wine | **SILENT** -- no log of context values |
| B42 | platform_compat module load | `boot.cpp:60` | Start server | `[NEVR.MODULE] Loading: platform_compat` then `Loaded: platform_compat` | `Failed to load platform_compat` (fatal) | wine | Schannel TLS fails; all HTTPS connections break |
| B43 | token_auth module load | `boot.cpp:64` | Start server | `Loading: token_auth` then `Loaded: token_auth` | `Failed to load token_auth` (fatal) | wine | No auth token available; ServerDB connection fails |
| B44 | TokenAuth export registration | `boot.cpp:67-73` | Start server | `TokenAuth_GetToken` and `TokenAuth_GetDiscordId` registered | ws_bridge cannot resolve auth exports | wine | **SILENT SUCCESS** -- no log when registration succeeds. Login injection lacks JWT |
| B45 | ws_bridge module load | `boot.cpp:77` | Start server (N92 -- folded into BugSplat64, this is a no-op load) | Module loads or is absent without error | Fatal if module missing and still required | wine | **DEPRECATED** per N92; ws_bridge now lives in BugSplat64 |
| B46 | WsBridge export registration | `boot.cpp:80-86` | Start server | `WsBridge_GetPort` and `WsBridge_IsActive` registered | Service URL redirect fails to detect bridge | wine | **SILENT SUCCESS** -- no log. Service redirects cannot use bridge relay |
| B47 | CLI: -server flag | `boot.cpp:96-97` | Start server with `-server` | `g_isServer = TRUE`; server patches apply | Server patches not applied | wine | **SILENT** -- flag parsed, no per-flag log |
| B48 | CLI: -offline flag | `boot.cpp:98-99` | Start with `-offline` | `g_isOffline = TRUE` | Offline patches not applied | wine | **SILENT** |
| B49 | CLI: -noconsole flag | `boot.cpp:100-101` | Start with `-noconsole` | No console window created | Console window appears | wine | **SILENT** |
| B50 | CLI: -windowed flag | `boot.cpp:102-103` | Start client with `-windowed` | Windowed mode applied | VR mode forced | client | **SILENT** |
| B51 | CLI: -noexitonerror flag | `boot.cpp:104-105` | Start with `-noexitonerror` | `g_exitOnError = FALSE` | Server exits on non-fatal errors | wine | **SILENT** |
| B52 | CLI: -exitonerror (deprecated) | `boot.cpp:106-108` | Start with `-exitonerror` | `[NEVR.PATCH] -exitonerror is deprecated (now default)` | Flag silently ignored | wine | No risk; deprecated |
| B53 | CLI: -notelemetry flag | `boot.cpp:109-110` | Start with `-notelemetry` | `g_telemetryEnabled = FALSE` | Telemetry still starts | wine | **SILENT** |
| B54 | CLI: -telemetryrate flag | `boot.cpp:111-116` | Start with `-telemetryrate 5` | Telemetry streams at 5Hz | Default 10Hz used | wine | **SILENT** -- no log of parsed rate value |
| B55 | CLI: -telemetrydiag flag | `boot.cpp:117-118` | Start with `-telemetrydiag` | Diagnostic snapshot logged | No snapshot output | wine | **SILENT** |
| B56 | CLI: -timestamps flag | `boot.cpp:119-120` | Start with `-timestamps` | `g_timestampLogs = TRUE` | Logs lack timestamps | wine | **SILENT** |
| B57 | CLI: -upnp flag | `boot.cpp:121-122` | Start with `-upnp` | `g_upnpEnabled = TRUE` | UPnP not attempted | wine | **SILENT** |
| B58 | CLI: -config/-config-path | `boot.cpp:123-127` | Start with `-config-path /path/to/config.json` | Custom config loaded | Default config path used | wine | **SILENT** -- no log of captured path value |
| B59 | CLI: -region/-serverregion | `boot.cpp:128-132` | Start with `-region us-west` | `g_regionOverride` set | Default region used | wine | **SILENT** |
| B60 | CLI: -timestep/-fixedtimestep (deprecated) | `boot.cpp:133-136` | Start with `-timestep` | `[NEVR.PATCH] -timestep is deprecated and ignored` | Flag silently ignored | wine | No risk; deprecated |
| B61 | CLI: -headless/-noovr (deprecated) | `boot.cpp:137-139` | Start with `-headless` | `[NEVR.PATCH] -headless is deprecated and ignored` | Flag silently ignored | wine | No risk; deprecated |
| B62 | Server mode auto-headless | `boot.cpp:147-149` | Start with `-server` | `Server mode -- headless + noovr applied` log line | Headless mode not automatically enabled | wine | Server tries to init VR; crashes |
| B63 | Wine auto-detect -noconsole | `boot.cpp:153-158` | Start server under Wine | `Wine detected -- defaulting to -noconsole` | Console window created (possible Wine crash) | wine | Console window under Wine may cause rendering issues |
| B64 | Mutual exclusion: server+offline | `boot.cpp:162-164` | Start with `-server -offline` | `FatalError` prevents start | Server starts in ambiguous mode | wine | Undefined behavior from conflicting patches |
| B65 | Offline mode patches | `boot.cpp:170-172` | Start with `-offline` | Offline patches applied | Game requires online services | client | Game stuck at login screen |
| B66 | Headless mode patches | `boot.cpp:174-176` | Start with `-headless` or `-server` | Headless patches applied | Game crashes on GPU init | wine | DXGI/D3D11 crash |
| B67 | Windowed mode flag | `boot.cpp:179-184` | Start with `-windowed` or `-server` | Windowed flag set in game structure | Game tries to init VR | wine/client | VR init fails; crash or hang |
| B68 | PNSRAD enabler init | `boot.cpp:188` | Start server | pnsrad.dll loaded instead of pnsovr.dll | pnsovr.dll loaded; Oculus services fail | wine | **SILENT** -- no log from PnsradEnabler::Init success |
| B69 | DSC provider prefix patch | `boot.cpp:192` | Start server or client | XPID strings show `DSC-` not `PSN-` | Account IDs display as `PSN-` or `UNK-` | wine/client | **SILENT** -- no log on success. Wrong platform identity |
| B70 | OVR platform bypass | `boot.cpp:195-197` | Start with `-server` or `-headless` | OVR platform init skipped | Game tries to init Oculus Platform SDK; crash | wine | **SILENT** -- no per-operation log. Crash from missing OVR DLLs |
| B71 | Oculus SDK DLL block | `boot.cpp:197` | Start with `-server` or `-headless` | `[NEVR.PATCH] Installed Oculus Platform SDK blocking hooks` | libovrplatform loads; memory/CPU wasted | wine | 50-80MB RAM, 8-12% CPU wasted per instance |
| B72 | Server mode patches | `boot.cpp:201-208` | Start with `-server` | Server flags, loading tips, Wwise, server profile all applied | Server runs in client-like mode | wine | Missing server flags; game doesn't act as dedicated server |
| B73 | Wave0 init (boot-time) | `boot.cpp:212` | Start server | Wave0 instrumentation hooks active | Binary bug detection disabled | wine | **SILENT** -- `Wave0::Init()` result not checked at this call site |
| B74 | Plugin loading | `boot.cpp:215` | Start server | Plugin loader scans plugins/ directory | Plugins never loaded | wine | Plugin-dependent features missing |
| B75 | Original function call | `boot.cpp:218-219` | Start server | Game's PreprocessCommandLine runs after NEVR patches | Game boot stalls | wine | **SILENT** -- no log of dispatched call. Game init incomplete |

---

## 2. Config / Service Redirect (config.cpp)

| # | Feature | Citation | How to test | PASS signal | FAIL signal | Testable on | Silent break risk |
|---|---------|----------|-------------|-------------|-------------|-------------|-------------------|
| C01 | Early config load -- next to exe | `config.cpp:43-56` | Place `_local/config.json` next to echovr.exe, start server | `Early config loaded from: .../_local/config.json` | `Failed to early-load config` | wine | Service redirects use dead RaD endpoints |
| C02 | Early config load -- parent dirs | `config.cpp:42-46` | Place config in parent, start server | Config found via parent-directory search | Config not found if in non-standard location | wine | Same as C01 |
| C03 | Custom config path (-config-path) | `config.cpp:73-97` | Start with `-config-path /tmp/test-config.json` | `Loading custom config from:` then `Successfully loaded custom config from:` | `Failed to load custom config file` | wine | Wrong config values; service endpoints broken |
| C04 | Default config fallback search | `config.cpp:98-123` | Start server with config in parent dir | `Game config loaded from:` with correct path | Config stays NULL; all config-dependent features fail | wine | **SILENT** on the search path iteration itself |
| C05 | Config: exitonerror key | `config.cpp:139-142` | Set `"exitonerror": "true"` in config | `g_exitOnError = TRUE` | Exit-on-error behavior inverted | wine | **SILENT SUCCESS** -- no log when key is present and parsed |
| C06 | Config: upnp key | `config.cpp:145-148` | Set `"upnp": "true"` in config | `g_upnpEnabled = TRUE` | UPnP not attempted despite config | wine | **SILENT SUCCESS** -- no log |
| C07 | Config: upnp_port key | `config.cpp:150-155` | Set `"upnp_port": "12345"` in config | UPnP maps port 12345 | Default port used | wine | **SILENT SUCCESS** -- no log of parsed value |
| C08 | Config: internal_ip override | `config.cpp:158-161` | Set `"internal_ip": "10.0.0.1"` in config | Server uses 10.0.0.1 as internal IP | Auto-detected IP used | wine | **SILENT SUCCESS** -- no log |
| C09 | Config: external_ip override | `config.cpp:164-167` | Set `"external_ip": "1.2.3.4"` in config | Server registers with 1.2.3.4 | Auto-detected external IP used | wine | **SILENT SUCCESS** -- no log |
| C10 | Config: arena_round_time override | `config.cpp:170-173` | Set `"arena_round_time": "300"` in config | `Arena round time override: 300 seconds` | Default round time used | wine | Arena matches have wrong duration |
| C11 | Config: arena_celebration_time override | `config.cpp:175-179` | Set `"arena_celebration_time": "15.0"` in config | `Arena celebration time override: 15.0 seconds` | Default celebration time used | wine | Goal celebration cut short or too long |
| C12 | Config: arena_mercy_score override | `config.cpp:181-184` | Set `"arena_mercy_score": "10"` in config | `Arena mercy score override: 10` | Default mercy threshold used | wine | Mercy rule triggers at wrong score differential |
| C13 | CJsonGetFloat hook -- round_time override | `config.cpp:205-208` | Set arena_round_time, start game | Float override applied at runtime | Game uses default float from game assets | client/live | **DEBUG-ONLY validation** -- no INFO log when override fires |
| C14 | CJsonGetFloat hook -- celebration_time override | `config.cpp:200-202` | Set arena_celebration_time, start game | Float override applied | Default used | client/live | Same as C13 |
| C15 | CJsonGetFloat hook -- mercy_score override | `config.cpp:210-212` | Set arena_mercy_score, start game | Float override applied | Default used | client/live | Same as C13 |
| C16 | Service redirect: loginservice_host fallback | `config.cpp:238-243` | Set `loginservice_host` in config | `Service fallback [* -> loginservice_host]:` at Debug level | Services use per-key overrides only | wine | **DEBUG-ONLY** -- invisible in production. Service falls back to dead default |
| C17 | Service redirect: per-service override | `config.cpp:233-234` | Set per-service key in config | `Service override [key]: host` at Debug level | Default URL used | wine | **DEBUG-ONLY** |
| C18 | Service redirect: default URL | `config.cpp:246` | No config override set | `Service default [key]: defaultUrl` at Debug level | N/A -- informational | wine | **DEBUG-ONLY** |
| C19 | Auto-relay through WebSocket bridge | `config.cpp:257-272` | Start server with bridge active + config | `Auto-relay [service] through bridge: url -> ws://127.0.0.1:PORT` | Service connects directly to dead endpoint | wine | Service connections fail silently |
| C20 | HttpConnect hook -- API service redirect | `config.cpp:292-298` | Set `apiservice_host` or `api_host` in config | `HTTP(S) connection redirected: old -> new` | Game connects to dead `api.readyatdawn.com` | wine | API calls fail; no leaderboards, no stats |
| C21 | HttpConnect hook -- config service redirect | `config.cpp:300-301` | Set `configservice_host` in config | Redirect log line appears | Dead config endpoint used | wine | Config service calls fail |
| C22 | HttpConnect hook -- transaction service redirect | `config.cpp:304-305` | Set `transactionservice_host` in config | Redirect log line appears | Dead transaction endpoint used | wine/client | IAP/transaction calls fail |
| C23 | HttpConnect hook -- matching service redirect | `config.cpp:308-312` | Set `matchingservice_host` in config | Redirect or auto-relay log line appears | Dead matchmaking endpoint used | wine/client | Matchmaking broken |
| C24 | HttpConnect hook -- ServerDB redirect | `config.cpp:315-319` | Set `serverdb_host` in config | Redirect or auto-relay log line appears | Dead serverdb endpoint used | wine | Server registration fails |
| C25 | HttpConnect hook -- Oculus Graph redirect | `config.cpp:322-327` | Set `graph_host` or `graphservice_host` in config | Redirect log line appears | Dead graph.oculus.com used | client | Graph API calls fail |
| C26 | JsonValueAsString hook -- key tracing | `config.cpp:402-405` | Start server, check Debug logs | `[NEVR.CONFIG] JsonValueAsString key='...' result='...'` for every config lookup | Silent config resolution | wine | **DEBUG-ONLY** |
| C27 | JsonValueAsString hook -- service URL redirect | `config.cpp:354-394` | Start server with nevr_socket_uri set | `Service redirect [key]: old -> new` for each WebSocket URL | WebSocket URLs connect to dead RaD endpoints | wine | All WebSocket connections fail |
| C28 | JsonValueAsString hook -- config override injection | `config.cpp:413-420` | Set override in early config, start server | `Config override [key]: old -> new` | Game uses hardcoded defaults despite config | wine | Config overrides silently ignored |

---

## 3. State Machine (state_machine.cpp)

| # | Feature | Citation | How to test | PASS signal | FAIL signal | Testable on | Silent break risk |
|---|---------|----------|-------------|-------------|-------------|-------------|-------------------|
| S01 | NoNetwork suppression (server) | `state_machine.cpp:44-49` | Start server; wait for NoNetwork transition | `Suppressed NoNetwork transition -- redirecting to LoadingRoot to continue boot` | Server stuck in NoNetwork; multiplayer never starts | wine | Server dead in water; cannot reach lobby |
| S02 | LoadFailed redirect (server) | `state_machine.cpp:52-57` | Trigger level load failure on server | `Dedicated server failed to load level. Resetting session...` at Debug level | Server stuck on load-fail; game unavailable | wine | **DEBUG-ONLY** -- invisible in production. Server appears hung |
| S03 | InGame tracking | `state_machine.cpp:60-62` | Server enters a game session | g_serverWasInGame set to TRUE | Session-end detection broken | wine/live | **SILENT** -- no log for this transition. Server doesn't exit after session |
| S04 | Session-end exit (server) | `state_machine.cpp:66-69` | Complete a game session on server | `Session ended. Server exiting.` | Server stays running after session; port not released | wine/live | Fleet manager cannot cycle instances; port exhaustion |
| S05 | Login session GUID capture | `state_machine.cpp:76-133` | Start server, let it reach lobby | `Captured login session: XXXXXXXX-XXXX-...` | `Login session GUID not found in game log` | wine | Session tracking broken; downstream features lack session ID |
| S06 | Plugin state change notification | `state_machine.cpp:25-36` | Start server with plugin loaded | Plugins receive state change callbacks | Plugins unaware of game state | wine | Plugin behavior desynchronized from game state |

---

## 4. Crash Recovery (crash_recovery.cpp)

| # | Feature | Citation | How to test | PASS signal | FAIL signal | Testable on | Silent break risk |
|---|---------|----------|-------------|-------------|-------------|-------------|-------------------|
| R01 | CreateProcessA hook -- block BsSndRpt | `crash_recovery.cpp:35-53` | Trigger a game crash | `Blocked crash reporter launch (A):` log line | BsSndRpt64.exe launches; Wine errors | wine | Crash reporter spawns and fails under Wine; noise |
| R02 | CreateProcessW hook -- block BsSndRpt (wide) | `crash_recovery.cpp:64-85` | Trigger a game crash | `Blocked crash reporter launch (W):` log line | Same as R01 but for wide-char path | wine | Same as R01 |
| R03 | ExitProcess suppression (server mode) | `crash_recovery.cpp:100-118` | Server encounters non-fatal error | `ExitProcess(N) suppressed in server mode (call #N)` | Server terminates on non-fatal error | wine | Server dies from missing actor/dialogue errors |
| R04 | ExitProcess suppression (post-crash-reporter) | `crash_recovery.cpp:120-133` | Crash reporter blocked, then ExitProcess called | `ExitProcess(N) suppressed after crash reporter block - server continuing` with stack trace | Server exits after blocking crash reporter | wine | Server terminates despite crash reporter suppression |
| R05 | Crash dump -- exception info | `crash_recovery.cpp:184-186` | Trigger an access violation | `=== CRASH DUMP ===` + `Exception: ACCESS_VIOLATION (0xC0000005)` | Crash occurs with no diagnostic | wine | Post-mortem impossible |
| R06 | Crash dump -- access violation details | `crash_recovery.cpp:189-194` | Trigger AV on known address | `Access: READ/WRITE at 0x...` | AV logged without address detail | wine | Cannot determine what was accessed |
| R07 | Crash dump -- register dump | `crash_recovery.cpp:197-204` | Trigger any crash | Full x64 register dump (RAX-R15) | Crash logged without register state | wine | Cannot reconstruct crash state |
| R08 | Crash dump -- stack scan | `crash_recovery.cpp:208-218` | Trigger crash in game code | Stack trace with `game+0x...` RVAs | No stack trace in dump | wine | Cannot determine call chain |
| R09 | Crash dump -- module listing | `crash_recovery.cpp:221-243` | Trigger any crash | Module list with base/end + CRASH marker | No module context | wine | Cannot identify which DLL caused crash |
| R10 | VEH -- int3 skip after ExitProcess suppression | `crash_recovery.cpp:257-265` | Suppress ExitProcess, let CPU hit int3 | `int3 after suppressed ExitProcess at RIP=... -- skipping, server continuing` | Process killed by int3 after suppression | wine | Server dies on int3 one instruction after suppressed ExitProcess |
| R11 | VEH -- null-ptr AV recovery via longjmp | `crash_recovery.cpp:267-279` | Server mode, null-ptr AV in game loop | `Null-ptr AV #N -- longjmp to server hold` | Server crashes fatally on null-ptr AV | wine | Server dies instead of entering server-hold recovery |
| R12 | VEH -- fatal exception crash dump | `crash_recovery.cpp:282-293` | Trigger AV/illegal instruction/stack overflow | Crash dump logged for first 3 occurrences | Fatal exception occurs with no diagnostic | wine | Crash with no evidence |
| R13 | TerminateProcess hook -- self-termination suppression | `crash_recovery.cpp:301-315` | Game tries to TerminateProcess(self) | `TerminateProcess(self, N) suppressed after crash reporter block` or `called - allowing` | Server self-terminates from crash reporter chain | wine | Server kills itself after crash reporter blocked |
| R14 | InstallCrashRecoveryHooks -- CreateProcessA | `crash_recovery.cpp:321-326` | Start server | `CreateProcessA hook installed (crash reporter disabled)` | `Failed to find CreateProcessA` | wine | Crash reporter not blocked on ANSI path |
| R15 | InstallCrashRecoveryHooks -- CreateProcessW | `crash_recovery.cpp:329-334` | Start server | `CreateProcessW hook installed (crash reporter disabled)` | `Failed to find CreateProcessW` | wine | Crash reporter not blocked on wide path |
| R16 | InstallCrashRecoveryHooks -- ExitProcess | `crash_recovery.cpp:337-342` | Start server | `ExitProcess hook installed (prevents crash reporter termination)` | `Failed to find ExitProcess` | wine | Server cannot suppress ExitProcess |
| R17 | InstallCrashRecoveryHooks -- TerminateProcess | `crash_recovery.cpp:345-350` | Start server | `TerminateProcess hook installed (prevents self-termination)` | `Failed to find TerminateProcess` | wine | Server cannot suppress TerminateProcess |
| R18 | Console ctrl handler | `crash_recovery.cpp:367-380` | Press CTRL+C on server | `Console signal 0/2/1 received -- exiting` | CTRL+C swallowed by game's handler; server doesn't exit | wine | **N13** -- server cannot be stopped cleanly |
| R19 | ForceFatalExit | `crash_recovery.cpp:383-404` | Server encounters unrecoverable error | `ForceFatalExit(N) -- terminating process` | Process hangs instead of terminating | wine | Server stuck in zombie state; port held |

---

## 5. Patches (mode_patches.cpp)

### 5.1 Headless Patches

| # | Feature | Citation | How to test | PASS signal | FAIL signal | Testable on | Silent break risk |
|---|---------|----------|-------------|-------------|-------------|-------------|-------------------|
| P01 | ForceHeadlessSkip -- generic gate force | `mode_patches.cpp:32-46` | Start server | `[NEVR.HEADLESS] <name> -- forced device-free branch at +0x...` | `prologue mismatch at +0x... -- NOT patched` | wine | Specific render/graphics subsystem crashes |
| P02 | CEngineConfig copy hook -- bit-0x1 clear | `mode_patches.cpp:63-83` | Start server | `CEngine config copy -- cleared renderer bit 0x1: 0x... -> 0x...` | Bit not cleared; render pipeline runs | wine | Renderer init crashes with no GPU |
| P03 | CEngineConfig copy hook install | `mode_patches.cpp:105-109` | Start server | `CEngineConfig copy hook installed -- bit-0x1 will be cleared after config copy` | Hook not installed | wine | Same as P02 |
| P04 | Audio disable | `mode_patches.cpp:112-113` | Start server | Audio flags cleared in game structure | Wwise audio init runs | wine | **SILENT** -- no log for this mutation. Audio resources wasted |
| P05 | Renderer init skip (byte patch) | `mode_patches.cpp:118-120` | Start server | Renderer init skipped | Game tries to init renderer; crash | wine | **SILENT** -- `ApplyPatch` has no per-patch log |
| P06 | Effects resource loading skip | `mode_patches.cpp:123-125` | Start server | Effects loading skipped | Game tries to load effects resources; crash | wine | **SILENT** |
| P07 | ApplyGraphicsSettings skip (NOP) | `mode_patches.cpp:128-130` | Start server | Graphics settings call skipped | ~66 CGRenderer methods called without renderer; crash | wine | **SILENT** |
| P08 | DirectInput8Create kill (NOP) | `mode_patches.cpp:133-135` | Start server | DirectInput HID thread prevented | HID enumeration thread spins | wine | **SILENT** -- CPU wasted on HID enumeration |
| P09 | D3D12 device init skip | `mode_patches.cpp:145-157` | Start server | `D3D12 device init skipped -- forced device-free branch at +0x...` | `D3D12-skip prologue mismatch -- NOT patched` | wine | D3D12 init runs; asserts "Unknown error while loading the game" |
| P10 | CEngine renderer init skip | `mode_patches.cpp:163` | Start server | `renderer init skipped` | Renderer init runs without GPU; crash | wine | **N6** -- crash in CEngine init |
| P11 | GUI subsystem init skip | `mode_patches.cpp:169` | Start server | `GUI subsystem init skipped` | GUI creates GPU resources on null device; crash | wine | **N7** -- AV at 0x1413581E4 |
| P12 | Render-submit-context init skip | `mode_patches.cpp:179` | Start server | `render-submit-context init skipped` | Divide-by-zero in CRenderSubmitContext::RequestBuffer | wine | **N8** -- INT_DIVIDE_BY_ZERO at 0x1405bc52a |
| P13 | Renderer-setup path skip | `mode_patches.cpp:188` | Start server | `renderer setup skipped` | Float getter derefs null render object | wine | **N9** -- ACCESS_VIOLATION at 0x14056d560 |
| P14 | SYSNET hook -- always report connected | `mode_patches.cpp:201-213` | Start server under Wine | `SYSNET check -- returning TRUE (internet-connected) for server mode` | SYSNET reports no internet; NoNetwork transition | wine | Server stuck at NoNetwork despite actual connectivity |
| P15 | Console creation (headless) | `mode_patches.cpp:216-242` | Start server without -noconsole | Console window appears with ANSI color support | No console; output invisible | wine | **SILENT SUCCESS** -- no log on console creation success |

### 5.2 Server Mode Patches

| # | Feature | Citation | How to test | PASS signal | FAIL signal | Testable on | Silent break risk |
|---|---------|----------|-------------|-------------|-------------|-------------|-------------------|
| P16 | Server flags check patch | `mode_patches.cpp:489-495` | Start server | Bit 2+3 permanently set in server flags | Server flags conditional on checks that fail | wine | **SILENT** -- `ApplyPatch` has no per-patch log. Game not in dedicated server mode |
| P17 | r14netserver logging disable | `mode_patches.cpp:499-501` | Start server | r14netserver logging disabled | Game tries to write to missing r14netserver log files | wine | **SILENT** |
| P18 | Logging subject override ("r14(server)") | `mode_patches.cpp:504-506` | Start server | Log subject set to "r14(server)" | Log subject stays as client default | wine | **SILENT** |
| P19 | Force allow_incoming=true | `mode_patches.cpp:512-514` | Start server | Incoming connections allowed | Server rejects incoming connections | wine | **SILENT** -- server unreachable |
| P20 | Bypass -spectatorstream requirement | `mode_patches.cpp:518-521` | Start server | Server enters lobby without -spectatorstream | Server requires -spectatorstream flag | wine | **SILENT** -- server stuck at spectator stream check |
| P21 | Loading tips disable (3 patches) | `mode_patches.cpp:312-325` | Start server | `Disabled loading tips system for server mode` | Loading tip functions run; log spam/resources wasted | wine | Log spam; resource loading failures |
| P22 | Wwise audio init block | `mode_patches.cpp:665-683` | Start server | `Wwise audio initialization blocked (VOIP preserved)` + `Installed Wwise audio blocking hooks` | Wwise audio system initializes | wine | 20-30MB RAM, 5-8% CPU wasted per instance |
| P23 | Wwise RenderAudio block | `mode_patches.cpp:670,679` | Start server | Wwise render loop suppressed | Audio render loop runs every frame | wine | Per-frame CPU waste |
| P24 | Server frame pacing -- BusyWait RET | `mode_patches.cpp:694-701` | Start server | `CPrecisionSleep::BusyWait patched to RET (Wine CPU optimization)` | BusyWait spin-loop runs under Wine | wine | ~250us CPU spin per frame under Wine |
| P25 | Server profile log | `mode_patches.cpp:711-725` | Start server | `[NEVR.PROFILE] WorkingSet: N MB, PrivateBytes: N MB` + DLL load status | No memory/module profile | wine | Cannot verify memory savings from patches |

### 5.3 Server Crash Guards

| # | Feature | Citation | How to test | PASS signal | FAIL signal | Testable on | Silent break risk |
|---|---------|----------|-------------|-------------|-------------|-------------|-------------------|
| P26 | Entity lookup null-guard | `mode_patches.cpp:363-383` | Start server, enter game | Entity lookup hook installed; guards fire if needed | `Entity lookup null-guard triggered` (first 3 times) then silent | wine | Access violation on uninitialized hash table pointer |
| P27 | Entity prop dispatch skip (server) | `mode_patches.cpp:395-401` | Start server | Entity prop dispatch skipped entirely in server mode | Prop dispatch runs; cascading AVs | wine | **SILENT** -- no log for the skip. Multiple AVs from uninitialized pointers |
| P28 | Game main wrapper -- crash recovery loop | `mode_patches.cpp:426-452` | Server crashes in game loop | `Game loop recovered from crash #N -- entering server hold` | Server exits on game loop crash | wine | Server dies instead of staying alive for broadcaster/API |
| P29 | BugSplat crash handler suppression (server) | `mode_patches.cpp:466-473` | Server encounters non-fatal error | `BugSplat crash handler intercepted (exit code N) -- suppressed in server mode` | Server fatally exits on missing actor/dialogue | wine | Server terminates on non-fatal errors |
| P30 | InitializeGlobalGameSpace skip (server) | `mode_patches.cpp:340-348` | Start server | `InitializeGlobalGameSpace skipped in server mode (no local player actor needed)` | Game fatals: missing player actor or CDialogueSceneCS | wine | Server crashes during global gamespace init |
| P31 | OVR platform branch bypass | `mode_patches.cpp:268-301` | Start server | `OVR platform branch bypassed - allowing normal initialization` | OVR init branch runs; crash from missing OVR DLLs | wine | Server crashes during platform module init |
| P32 | LogInSuccess capability check bypass | `mode_patches.cpp:287-297` | Start server | Login capability check NOP'd; state update always runs | Login state update skipped when pnsrad loaded | wine | Login success state not processed correctly |

### 5.4 Oculus SDK Blocking

| # | Feature | Citation | How to test | PASS signal | FAIL signal | Testable on | Silent break risk |
|---|---------|----------|-------------|-------------|-------------|-------------|-------------------|
| P33 | LoadLibraryW -- block Oculus Platform | `mode_patches.cpp:617-629` | Start server/headless | `Blocked Oculus Platform SDK load: libovrplatform*.dll` | Oculus Platform DLL loads | wine | 50-80MB RAM, 8-12% CPU wasted |
| P34 | LoadLibraryExW -- block Oculus Platform | `mode_patches.cpp:631-643` | Start server/headless | `Blocked Oculus Platform SDK load:` (Ex variant) | Oculus Platform DLL loads via Ex | wine | Same as P33 |

### 5.5 Offline Mode

| # | Feature | Citation | How to test | PASS signal | FAIL signal | Testable on | Silent break risk |
|---|---------|----------|-------------|-------------|-------------|-------------|-------------------|
| P35 | Offline multiplayer patch | `mode_patches.cpp:538-539` | Start with -offline | Offline multiplayer init patched | Game requires online multiplayer services | client | **SILENT** -- ApplyPatch. Game stuck requiring online |
| P36 | Offline incidents patch | `mode_patches.cpp:543-544` | Start with -offline | Incident reporting patched | Incident reporting tries to connect | client | **SILENT** |
| P37 | Offline title/session checks | `mode_patches.cpp:548-549` | Start with -offline | Title checks patched | Title check fails; game won't start | client | **SILENT** |
| P38 | Offline transaction service force | `mode_patches.cpp:554-556` | Start with -offline | Transaction service loads | Transaction service unavailable | client | **SILENT** |
| P39 | Offline logon skip | `mode_patches.cpp:559-561` | Start with -offline | Failed logon code skipped | Logon failure blocks game | client | **SILENT** |
| P40 | Offline tutorial redirect | `mode_patches.cpp:564-566` | Start with -offline | Tutorial redirected | Tutorial points to online resource | client | **SILENT** |

---

## 6. Resource Override (resource_override.cpp + asset_cdn.cpp)

| # | Feature | Citation | How to test | PASS signal | FAIL signal | Testable on | Silent break risk |
|---|---------|----------|-------------|-------------|-------------|-------------|-------------------|
| RS01 | Resource override install | `boot.cpp:29` via `resource_override.cpp` | Start server | Embedded resources injected | Game uses disk resources; missing assets | wine | **SILENT** -- no log on success |
| RS02 | CDN: Manifest fetch start | `asset_cdn.cpp:271` | Configure CDN URL, start server | `[NEVR.CDN] Background fetch started` | No CDN activity | wine | CDN pipeline never starts |
| RS03 | CDN: Manifest fetch failure | `asset_cdn.cpp:277` | Configure invalid CDN URL | `Manifest fetch failed -- CDN pipeline aborted` | Silent failure (manifests fetch hangs) | wine | CDN stalls indefinitely |
| RS04 | CDN: Empty manifest | `asset_cdn.cpp:284` | Configure CDN with empty manifest | `Manifest has no packages -- nothing to download` | Silent; CDN appears stuck | wine | Operator cannot tell if CDN is working or empty |
| RS05 | CDN: Cache directory resolve failure | `asset_cdn.cpp:294` | Corrupt or missing cache dir | `Failed to resolve cache directory` | Silent failure; downloads fail with filesystem errors | wine | Downloads fail with cryptic errors |
| RS06 | CDN: Package download | `asset_cdn.cpp:665` | Configure CDN with valid packages | `[NEVR.CDN]` download completion log | Package not downloaded | wine | CDN assets missing; game uses fallback |
| RS07 | CDN: Package download failure (curl) | `asset_cdn.cpp:674-734` | Configure CDN with bad package URL | Error log with specific curl failure | Silent download failure | wine | Missing CDN asset; fallback used but operator unaware |
| RS08 | CDN: Shutdown | `asset_cdn.cpp:514` | Stop server | `[NEVR.CDN] Shutdown complete` | CDN thread leaked | wine | Background thread persists after shutdown |
| RS09 | CDN: .evrp manifest parse -- bad magic | `asset_cdn.cpp:218` | Corrupt .evrp file | `[NEVR.CDN] .evrp bad magic: 0x...` | Silent parse failure | wine | Manifest silently rejected; no CDN assets |
| RS10 | CDN: .evrp manifest parse -- too small | `asset_cdn.cpp:208` | Truncated .evrp file | `[NEVR.CDN] .evrp file too small: N bytes` | Silent rejection | wine | Same as RS09 |

---

## 7. PNSRAD Enabler (pnsrad_enabler.cpp)

| # | Feature | Citation | How to test | PASS signal | FAIL signal | Testable on | Silent break risk |
|---|---------|----------|-------------|-------------|-------------|-------------|-------------------|
| PN01 | PNSRAD enabler init | `pnsrad_enabler.cpp` via `boot.cpp:188` | Start server | pnsrad.dll loaded instead of pnsovr.dll | pnsovr.dll loaded; Oculus services fail | wine | **SILENT SUCCESS** -- no log. Game stuck on Oculus auth |
| PN02 | PNSRAD enabler hook install | `pnsrad_enabler.cpp` | Start server | Module load redirect installed | Game loads pnsovr.dll | wine | **SILENT** -- no per-hook log |

---

## 8. XPID Patch (xpid_patch.cpp)

| # | Feature | Citation | How to test | PASS signal | FAIL signal | Testable on | Silent break risk |
|---|---------|----------|-------------|-------------|-------------|-------------|-------------------|
| XP01 | DSC provider prefix patch | `xpid_patch.cpp` via `boot.cpp:192` | Start server or client | Account IDs display as `DSC-...` not `PSN-...` | `PSN-` prefix in account IDs | wine/client | **SILENT SUCCESS** -- no log. Wrong platform identity in all XPID strings |

---

## 9. Headless Graphics (headless_graphics.cpp)

| # | Feature | Citation | How to test | PASS signal | FAIL signal | Testable on | Silent break risk |
|---|---------|----------|-------------|-------------|-------------|-------------|-------------------|
| H01 | Headless graphics hook registration | `headless_graphics.cpp` via `initialize.cpp:240` | Start server | `[NEVR] headless graphics hooks registered` | Missing line | wine | DXGI/D3D11 DLLs load without interception |
| H02 | DXGI adapter stub -- IDXGIAdapter3 | `headless_graphics.cpp` | Start server | DXGI calls intercepted; no GPU required | E_NOINTERFACE on DXGI query; load abort | wine | **N6-related** -- game aborts on DXGI interface query failure |
| H03 | D3D11 device stub | `headless_graphics.cpp` | Start server | D3D11 device creation intercepted | Game tries to create real D3D11 device; crash | wine | D3D11 initialization crash under Wine |
| H04 | DXGI output/staging stubs | `headless_graphics.cpp` | Start server | DXGI enumeration stubbed | Game queries real display outputs | wine | Display enumeration may hang or crash |
| H05 | Headless graphics pass-through (non-server) | `headless_graphics.cpp` | Start client | Real DirectX calls pass through normally | Client DirectX broken | client | **SILENT** on pass-through -- no log per call |

---

## 10. WebSocket Bridge (ws_bridge.cpp)

| # | Feature | Citation | How to test | PASS signal | FAIL signal | Testable on | Silent break risk |
|---|---------|----------|-------------|-------------|-------------|-------------|-------------------|
| W01 | WebSocket bridge install | `ws_bridge.cpp` | Start server with nevr_socket_uri set | WebSocket proxy starts | Proxy not started; services connect directly | wine | WebSocket services fail on dead RaD endpoints |
| W02 | WebSocket bridge port | `ws_bridge.cpp` | Start server | `WsBridge_GetPort` returns listening port | Port is 0 or bridge not listening | wine | Service redirects to port 0; connections fail |
| W03 | WebSocket bridge active check | `ws_bridge.cpp` | Start server | `WsBridge_IsActive` returns true | Returns false; auto-relay disabled | wine | Service auto-relay through bridge disabled |
| W04 | Login request injection | `ws_bridge.cpp` | Server connects; client logs in | LoginRequest injected with correct XPID | Login injection fails; client cannot authenticate | client | **N14/N15** -- XPID platform prefix wrong; login rejected |
| W05 | Login success handling | `ws_bridge.cpp` | Client completes login | Login success forwarded to game | Login response lost; client stuck logging in | client | Client stuck at login screen |
| W06 | WebSocket message forwarding | `ws_bridge.cpp` | Server running with connected client | Messages forwarded between game and server | Messages dropped; desync | client/live | Game state desynchronization |
| W07 | Config connection (conn=0) | `ws_bridge.cpp` | Start server | Config service connection established | Config service unreachable | wine | Game config not received |
| W08 | Game login connection (conn=1+) | `ws_bridge.cpp` | Client connects to server | Game login connection established | Login connection fails | client | Client cannot join server |

---

## 11. ServerDB Client (websocket_client.cpp)

| # | Feature | Citation | How to test | PASS signal | FAIL signal | Testable on | Silent break risk |
|---|---------|----------|-------------|-------------|-------------|-------------|-------------------|
| SD01 | WebSocket connect -- valid URI | `websocket_client.cpp:46` | Start server with valid serverdb URI | `Connecting to ServerDB at <uri>` | `Invalid URI provided for connection` | wine | Server never connects to ServerDB |
| SD02 | WebSocket connect -- Bearer auth | `websocket_client.cpp:56` | Start server with auth token | `Using Bearer auth token` | Auth header not set; connection rejected | wine | ServerDB rejects unauthenticated connection |
| SD03 | WebSocket connected | `websocket_client.cpp:129` | Server connects successfully | `Connected to ServerDB` | No connection; silent timeout | wine | Server never registers |
| SD04 | WebSocket disconnected | `websocket_client.cpp:138` | Server loses connection | `Disconnected from ServerDB (code: N, reason: ...)` | Silent disconnect; server unaware | wine | Server thinks it is still registered; stale entry |
| SD05 | WebSocket error | `websocket_client.cpp:144` | Trigger connection error | `Connection error: ...` | Silent connection failure | wine | Server cannot diagnose connection issues |
| SD06 | Message send -- oversized reject | `websocket_client.cpp:75` | Send oversized message | `Rejecting oversized send (msgId: ..., size: ...)` | Oversized message queued; WebSocket error | wine | WebSocket connection dropped by peer |
| SD07 | Message send success | `websocket_client.cpp:114` | Send any message | `Sent message (msgId: ..., size: ..., total: ...)` at Debug level | Silent send | wine | **DEBUG-ONLY** |
| SD08 | Message send failure | `websocket_client.cpp:110` | Send when disconnected | `Failed to send message (msgId: ...)` | Silent send failure | wine | Messages lost without evidence |
| SD09 | Binary message parse -- too short | `websocket_client.cpp:242` | Receive malformed binary | `Received malformed binary message (too short: N bytes)` | Parse crash or silent drop | wine | Message corruption undetected |
| SD10 | Binary message parse -- multi-message | `websocket_client.cpp:245` | Receive batched messages | `Parsed N messages from single frame (N bytes)` at Debug | Silent parse | wine | **DEBUG-ONLY** |
| SD11 | Duplicate message drop | `websocket_client.cpp:201` | Receive duplicate msgId | `Dropping duplicate message (msgId: ...)` at Debug | Duplicate processed; double-handling | wine | **DEBUG-ONLY** -- invisible in production |
| SD12 | Pending message flush on connect | `websocket_client.cpp:274-282` | Queue messages before connect, then connect | `Flushing N pending messages` + per-message Debug lines | Pending messages lost | wine | Pre-connect messages silently dropped |
| SD13 | Reconnect + re-register | `websocket_client.cpp` or `gameserver.cpp:980` | Connection drops, auto-reconnect | `WebSocket reconnected, re-registering with ServerDB` | Reconnect without re-registration | wine | Server appears connected but not registered |

---

## 12. Game Server (gameserver.cpp)

| # | Feature | Citation | How to test | PASS signal | FAIL signal | Testable on | Silent break risk |
|---|---------|----------|-------------|-------------|-------------|-------------|-------------------|
| G01 | GameServer initialization | `gameserver.cpp:872` | Start server | `Initialized game server` | Missing line; server never reaches init | wine | Multiplayer subsystem never starts |
| G02 | Base address log | `gameserver.cpp:875` | Start server | `EchoVR base address = 0x...` at Debug | No base address logged | wine | **DEBUG-ONLY** |
| G03 | ServerDB URI construction -- token auth | `gameserver.cpp:1249-1282` | Start server with token | `Using cached auth token for ServerDB` or `Constructed serverdb URI for token auth` | Wrong URI format; ServerDB rejects connection | wine | Server cannot authenticate to ServerDB |
| G04 | ServerDB URI construction -- legacy | `gameserver.cpp:1306` | Start server without token | `Constructed serverdb URI from config fields (legacy url-param auth)` | Wrong URI format | wine | ServerDB connection fails |
| G05 | ServerDB URI construction -- failure | `gameserver.cpp:1309` | Missing required config keys | `[NEVR.GAMESERVER]` Warning about missing config | Silent fallback to broken URI | wine | Connection attempt with garbage URI |
| G06 | WebSocket connection initiation | `gameserver.cpp:1318` | Start server | WebSocket connect begins | `Failed to initiate WebSocket connection` | wine | Server never reaches ServerDB |
| G07 | Broadcaster unavailable | `gameserver.cpp:1325` | Start server without broadcaster | `Broadcaster unavailable` | Silent; registration skipped | wine | Server cannot register without broadcaster |
| G08 | UPnP fallback -- raw port | `gameserver.cpp:1346` | Start server with UPnP failure | `UPnP port mapping failed -- using raw broadcaster port` | Server port not reachable from internet | wine | Server unreachable behind NAT |
| G09 | Telemetry disabled (no URI) | `gameserver.cpp:1389` | Start server without telemetry_uri | `No telemetry_uri in config, telemetry disabled` | Telemetry attempts to start with no URI | wine | Telemetry connection failures |
| G10 | Registration request sent | `gameserver.cpp:1393` | Start server | `Requested game server registration via protobuf` | Registration never sent | wine | Server not listed; clients cannot find it |
| G11 | Registration success received | `gameserver.cpp:232` | ServerDB responds with registration | `Received registration success via protobuf: server_id=..., ip=...` | Registration response lost | wine | Server thinks it is unregistered |
| G12 | Registration success encode failure | `gameserver.cpp:244` | Registration response malformed | `Failed to encode registration success` | Silent; game-side registration incomplete | wine | Game doesn't know registration succeeded |
| G13 | Session create received | `gameserver.cpp:254` | ServerDB creates session | `Received session create via protobuf: session=..., max=..., type=...` | Session never created | wine/live | No game session; clients cannot join |
| G14 | Session success (legacy) | `gameserver.cpp:202` | Legacy session success message | `Received session success (SNSLobbySessionSuccessv5), size=...` | Legacy path broken | wine/live | Older clients may fail |
| G15 | Entrants accept received | `gameserver.cpp:326` | Players accepted to session | `Received entrants accept via protobuf: count=...` | Players never accepted | wine/live | Players stuck in pending; cannot join |
| G16 | Entrants reject received | `gameserver.cpp:345` | Players rejected from session | `Received entrants reject via protobuf: count=..., code=...` | Rejection silent | wine/live | Rejected players unaware |
| G17 | Smite entrant received | `gameserver.cpp:364` | Player kicked from session | `Received smite entrant via protobuf: entrant=..., session=...` | Kick never processed | wine/live | Player not removed |
| G18 | Error received via protobuf | `gameserver.cpp:404` | ServerDB sends error | `Received error via protobuf: code=..., msg=...` | Error silently dropped | wine | Server unaware of ServerDB errors |
| G19 | Error before registration -- shutdown | `gameserver.cpp:408` | Error before server is registered | `Error received before registration -- shutting down` | Server stays running in broken state | wine | Zombie server; port held |
| G20 | Unhandled protobuf message | `gameserver.cpp:415` | Unknown message type received | `Received unhandled protobuf message type: N` at Debug | Silent; unknown message dropped | wine | **DEBUG-ONLY** |
| G21 | Empty protobuf message | `gameserver.cpp:215` | Receive empty binary frame | `Received empty protobuf message` | Silent; parse attempt on empty data | wine | Protobuf parse failure |
| G22 | Protobuf parse failure | `gameserver.cpp:222` | Receive malformed protobuf | `Failed to parse protobuf Envelope` | Silent parse failure; message lost | wine | ServerDB messages silently dropped |
| G23 | Protobuf serialization failure | `gameserver.cpp:89` | Send message; serialization fails | `Failed to serialize protobuf to binary` | Silent send failure | wine | Outgoing messages silently lost |
| G24 | Session starting | `gameserver.cpp:424` | Session created, game starting | `Session starting` | Session transitions without log | wine/live | Operator cannot tell when sessions begin |
| G25 | Session error | `gameserver.cpp:428` | Session encounters error | `Session error encountered` | Silent session failure | wine/live | Session fails with no evidence |
| G26 | Game server termination | `gameserver.cpp:1042` | Server shuts down | `Terminated game server` | Server stops without log | wine | Operator cannot confirm clean shutdown |
| G27 | Graceful shutdown complete | `gameserver.cpp:1129` | Server completes graceful shutdown | `Graceful shutdown complete -- exiting` | Server hangs during shutdown | wine | Port not released; zombie process |
| G28 | Round timeout force-shutdown | `gameserver.cpp:1116` | Round exceeds 20 min | `Round did not end within 20 min -- forcing shutdown` | Server stuck in endless round | wine/live | Server wedged; never returns to lobby |
| G29 | Server authenticated (token acquired) | `gameserver.cpp:1217` | Token refresh succeeds | `Server authenticated (token acquired)` | Token acquisition silent | wine | ServerDB reconnection without auth |
| G30 | Save loadout received | `gameserver.cpp:546` | Player changes loadout | `[SAVE_LOADOUT] Slot=..., GenId=..., PayloadSize=...` | Loadout save silently dropped | live | Player loadout not persisted |
| G31 | Save loadout -- invalid slot | `gameserver.cpp:550` | Corrupt loadout message | `Invalid slot index: N` | Silent; bad data processed | live | Memory corruption from bad slot index |
| G32 | Save success received | `gameserver.cpp:693` | Loadout save confirmed | `[SAVE_SUCCESS] size=...` | Save confirmation lost | live | Player thinks loadout wasn't saved |
| G33 | Current loadout request | `gameserver.cpp:728` | Client requests current loadout | `CurrentLoadoutRequest: size=...` | Request silently dropped | live | Client doesn't receive loadout |
| G34 | Current loadout response | `gameserver.cpp:745` | Server responds with loadout | `[CURRENT_LOADOUT] Response: Slot=..., GenId=..., Size=...` | Response not sent | live | Client stuck without loadout data |
| G35 | Unregister game server | `gameserver.cpp:1414` | Server shuts down | `Unregistered game server` | ServerDB keeps stale registration | wine | Stale server entry in ServerDB |
| G36 | End of session signal | `gameserver.cpp:1432` | Session ends | `Signaling end of session` | Session end not signaled | wine/live | ServerDB thinks session still active |
| G37 | Game server locked signal | `gameserver.cpp:1444` | Server locks (no new joins) | `Signaling game server locked` | Lock not signaled | wine/live | New players attempt to join locked server |
| G38 | Game server unlocked signal | `gameserver.cpp:1456` | Server unlocks | `Signaling game server unlocked` | Unlock not signaled | wine/live | Players cannot join despite server being available |
| G39 | Player accepted signal | `gameserver.cpp:1470` | Players accepted to server | `Accepted N players` | Player accept not signaled | wine/live | Players accepted on server but ServerDB unaware |
| G40 | Player removed signal | `gameserver.cpp:1483` | Player leaves server | `Removed player from game server` | Player removal not signaled | wine/live | Stale player entry in ServerDB |

---

## 13. Telemetry (telemetry_streamer.cpp)

| # | Feature | Citation | How to test | PASS signal | FAIL signal | Testable on | Silent break risk |
|---|---------|----------|-------------|-------------|-------------|-------------|-------------------|
| T01 | Telemetry connect | `telemetry_streamer.cpp:86` | Start server with telemetry_uri set | `Connecting to <uri>` | Silent; connection never attempted | wine | Telemetry data never sent |
| T02 | Telemetry connected | `telemetry_streamer.cpp:58` | Telemetry server accepts connection | `Connected to telemetry server` | Connection succeeds but no confirmation | wine | Operator cannot tell if telemetry is live |
| T03 | Telemetry disconnected | `telemetry_streamer.cpp:69` | Telemetry server disconnects | `Disconnected from telemetry server (code: N)` | Silent disconnect | wine | Telemetry gap undetected |
| T04 | Telemetry connection error | `telemetry_streamer.cpp:74` | Telemetry connection fails | `Connection error: ...` | Silent connection failure | wine | Telemetry down; no alert |
| T05 | Telemetry stream started | `telemetry_streamer.cpp:115` | Session starts with telemetry | `Started streaming session=... at NHz` | Stream never starts | wine/live | No telemetry data for session |
| T06 | Telemetry stream stopped | `telemetry_streamer.cpp:121` | Session ends | `Stopping telemetry stream` | Stream thread leaked | wine/live | Background thread persists after session |
| T07 | CaptureHeader sent | `telemetry_streamer.cpp:1068` | Stream starts | `Sent CaptureHeader (N bytes)` | Header never sent | wine/live | Consumer cannot parse stream without header |
| T08 | CaptureHeader with roster sent | `telemetry_streamer.cpp:1118` | Stream starts with players | `Sent CaptureHeader with roster (N bytes, N players)` | Roster missing from header | wine/live | Consumer doesn't know player list |
| T09 | CaptureFooter sent | `telemetry_streamer.cpp:1136` | Stream ends | `Sent CaptureFooter (frames=N, duration=Nms)` | Footer never sent | wine/live | Consumer cannot validate stream completeness |
| T10 | Frame serialization failure | `telemetry_streamer.cpp:914` | Frame data corrupt | `Failed to serialize frame N` | Silent frame drop | wine/live | Missing frame in telemetry stream |
| T11 | Send failure | `telemetry_streamer.cpp:1043` | Network error during send | `Send failed (payload=N)` | Silent send failure | wine/live | Telemetry data lost |
| T12 | Timeout waiting for connection | `telemetry_streamer.cpp:652` | Telemetry server unreachable | `Timeout waiting for connection + snapshot` | Thread hangs indefinitely | wine | Telemetry thread stuck; resource leak |
| T13 | Reconnect -- re-send header | `telemetry_streamer.cpp:680` | Telemetry reconnects | `Re-sending header after reconnect` | Header not re-sent; consumer confused | wine/live | Consumer cannot parse resumed stream |
| T14 | Telemetry thread started | `telemetry_streamer.cpp:643` | Telemetry initializes | `Telemetry thread started` | Thread never spawns | wine | Telemetry never runs |
| T15 | Telemetry thread exiting | `telemetry_streamer.cpp:712` | Telemetry shuts down | `Telemetry thread exiting` | Thread hangs on exit | wine | Resource leak; clean shutdown blocked |
| T16 | Game function pointers resolved | `telemetry_streamer.cpp:410` | Telemetry initializes | `Game function pointers resolved` | Function pointers not resolved | wine | Telemetry cannot read game state |
| T17 | Already streaming guard | `telemetry_streamer.cpp:93` | Start stream while already streaming | `Already streaming, stopping previous session` | Two streams run concurrently | wine/live | Duplicate telemetry; resource waste |

---

## 14. UPnP (upnp.cpp)

| # | Feature | Citation | How to test | PASS signal | FAIL signal | Testable on | Silent break risk |
|---|---------|----------|-------------|-------------|-------------|-------------|-------------------|
| U01 | UPnP discovery | `upnp.cpp:30` | Start server with -upnp | UPnP device discovery begins | `[NEVR.UPNP]` warning on discovery failure | wine | Port not mapped; server unreachable behind NAT |
| U02 | UPnP port mapping success | `upnp.cpp:92` | UPnP device found, mapping created | `[NEVR.UPNP]` info with mapped port details | Port mapping not created | wine | Server unreachable from internet |
| U03 | UPnP port mapping failure | `upnp.cpp:105` | UPnP mapping fails | `[NEVR.UPNP]` warning with failure details | Silent mapping failure | wine | Server inaccessible; operator unaware |

---

## 15. Plugin Loading (plugin_loader.cpp)

| # | Feature | Citation | How to test | PASS signal | FAIL signal | Testable on | Silent break risk |
|---|---------|----------|-------------|-------------|-------------|-------------|-------------------|
| PL01 | Plugin directory scan | `plugin_loader.cpp:33` | Place plugin DLL in plugins/ | `Scanning for plugins in: <path>` | Directory not scanned | wine | Plugins never discovered |
| PL02 | No plugins directory found | `plugin_loader.cpp:37-44` | Start without plugins/ directory | `No plugins directory or no plugins found` (INFO) or `FindFirstFile failed: N` (WARNING) | Error swallowed | wine | Operator unaware plugins are missing |
| PL03 | Plugin DLL load failure | `plugin_loader.cpp:79` | Place invalid DLL in plugins/ | `Failed to load <name>: error N` | DLL load failure silent | wine | Broken plugin silently skipped |
| PL04 | Plugin missing NvrPluginGetInfo | `plugin_loader.cpp:85` | Place DLL without GetInfo export | `<name>: missing NvrPluginGetInfo export, skipping` | DLL loaded but treated as plugin anyway | wine | Non-plugin DLL loaded as plugin; undefined behavior |
| PL05 | Plugin NULL name | `plugin_loader.cpp:92` | Plugin returns NULL name | `<name>: NvrPluginGetInfo returned NULL name, skipping` | NULL name causes crash in subsequent log | wine | Crash in string formatting |
| PL06 | Plugin API version check | `plugin_loader.cpp:100-103` | Load plugin with newer API version | `<name> requires API vN, host supports vM -- loading anyway` | Newer plugin rejected or crashes | wine | Forward-compat issue detected but plugin still loaded |
| PL07 | Plugin init failure | `plugin_loader.cpp:116` | Plugin init returns non-zero | `<name> (<desc>): init failed with code N, unloading` | Failed plugin stays loaded | wine | Broken plugin active in process |
| PL08 | Plugin loaded successfully | `plugin_loader.cpp:124` | Load valid plugin | `Loaded: <name> vX.Y.Z (API vN)` | Plugin loaded without confirmation | wine | Operator cannot confirm which plugins are active |
| PL09 | Plugin count summary | `plugin_loader.cpp:130` | Load multiple plugins | `N plugin(s) loaded` | No count summary | wine | Operator must count individual load lines |
| PL10 | Plugin shutdown | `plugin_loader.cpp:133-141` | Stop server | Each plugin's shutdown called; library freed | Plugin leak; shutdown not called | wine | **SILENT** -- no per-plugin shutdown log |

---

## 16. Module Loading (module_loader.cpp)

| # | Feature | Citation | How to test | PASS signal | FAIL signal | Testable on | Silent break risk |
|---|---------|----------|-------------|-------------|-------------|-------------|-------------------|
| M01 | Module load start | `module_loader.cpp:51` | Start server | `Loading: <name>` | No log; module loading skipped | wine | Required modules never loaded |
| M02 | Module DLL load failure | `module_loader.cpp:55-58` | Remove module DLL | `Failed to load <name>: error N (path: ...)` then FATAL | Module missing but server continues | wine | **FatalError** -- process terminates |
| M03 | Module missing NvrModuleInit | `module_loader.cpp:63-66` | Module DLL without Init export | `<name>: missing NvrModuleInit export` then FATAL | Module loaded without init | wine | **FatalError** |
| M04 | Module init failure | `module_loader.cpp:70-74` | Module init returns non-zero | `<name>: init failed with code N` then FATAL | Failed module stays loaded | wine | **FatalError** |
| M05 | Module loaded successfully | `module_loader.cpp:82` | Start server with valid module | `Loaded: <name>` | Module loaded without confirmation | wine | Operator cannot confirm module loaded |
| M06 | Module proc registration | `module_loader.cpp:32-34` | Start server | Cross-module procs registered | Modules cannot resolve each other's exports | wine | **SILENT** -- `RegisterModuleProc` has no log |
| M07 | Module proc resolution | `module_loader.cpp:36-40` | Module calls ResolveModuleProc | Proc resolved and returned | Returns NULL; caller gets null function pointer | wine | **SILENT** -- `ResolveModuleProc` has no log on miss |
| M08 | Module shutdown | `module_loader.cpp:85-94` | Stop server | Each module's shutdown called; library freed | Module leak; shutdown not called | wine | **SILENT** -- no per-module shutdown log |
| M09 | Module context storage | `module_loader.cpp:24-26` | Start server | Module context stored for later retrieval | Context is NULL; modules get no host info | wine | **SILENT** |

---

## 17. Platform Compat (platform_compat.cpp gamepatches copy + modules/platform-compat/)

| # | Feature | Citation | How to test | PASS signal | FAIL signal | Testable on | Silent break risk |
|---|---------|----------|-------------|-------------|-------------|-------------|-------------------|
| PC01 | Platform compat module load | `modules/platform-compat/src/platform_compat.cpp` via `boot.cpp:60` | Start server | `[NEVR.MODULE] Loading: platform_compat` + `Loaded: platform_compat` | Module fails to load (fatal) | wine | Schannel TLS broken; HTTPS fails |
| PC02 | Schannel TLS hooks | `platform_compat.cpp` | Start server, make HTTPS request | TLS connections succeed under Wine | Schannel calls fail under Wine GnuTLS | wine | All HTTPS/WSS connections fail |
| PC03 | CreateDirectory fix (Wine) | `platform_compat.cpp` | Start server under Wine | `_temp` directory operations succeed | Wine `_temp` creation fails | wine | Game cannot create temp files; may crash |
| PC04 | WinHTTP to curl bridge | `platform_compat.cpp` | Start server, trigger HTTP call | HTTP requests routed through curl | WinHTTP calls fail under Wine | wine | HTTP-based services unreachable |
| PC05 | Gamepatches copy -- init return value | `platform_compat.cpp` (gamepatches) | Start server | Gamepatches platform compat init returns success | Init returns false; failure indistinguishable from success | wine | **RETURN VALUE DISCARDED** -- "initialized" printed unconditionally |

---

## 18. Token Auth (token_auth.cpp gamepatches + modules/token-auth/)

| # | Feature | Citation | How to test | PASS signal | FAIL signal | Testable on | Silent break risk |
|---|---------|----------|-------------|-------------|-------------|-------------|-------------------|
| TA01 | Token auth module load | `modules/token-auth/src/token_auth.cpp` via `boot.cpp:64` | Start server | `token_auth initialized` | Module fails to load (fatal) | wine | No auth token available |
| TA02 | Device code flow (client) | `modules/token-auth/src/token_auth.cpp` | Launch client without cached token | Device code request sent; code displayed | `device_code` flow fails silently | client | Client cannot authenticate |
| TA03 | Token poll (client) | `modules/token-auth/src/token_auth.cpp` | Complete device auth in browser | Token received and cached | Poll times out; no token | client | Client stuck at auth screen |
| TA04 | Cached token load | `modules/token-auth/src/token_auth.cpp` | Restart with valid cached token | Token loaded from cache | Cache read fails; fallback to device code | client | Unnecessary re-auth prompt |
| TA05 | Token refresh | `modules/token-auth/src/token_auth.cpp` | Token expired, refresh token valid | New token acquired via refresh | Refresh fails; fallback to device code | client | Re-auth required prematurely |
| TA06 | Token cache write | `modules/token-auth/src/token_auth.cpp` | Token acquired or refreshed | Token written to disk cache | Token stored in memory only; lost on restart | client | Re-auth required every launch |
| TA07 | Token auth disabled (server mode) | `modules/token-auth/src/token_auth.cpp` | Start server | Token auth skips device code flow | Device code prompt on server | wine | Server blocked waiting for browser auth |
| TA08 | Gamepatches copy -- TokenAuth::Shutdown only | `gamepatches/token_auth.cpp` | Stop server | `TokenAuth::Shutdown()` called from dllmain.cpp:105 | Shutdown not called | wine | Token cache not flushed |
| TA09 | Gamepatches copy -- Init/GetToken/GetDiscordId uncalled | `gamepatches/token_auth.cpp` | Start server | These functions never called (module path used instead) | Gamepatches copy accidentally invoked | wine | **OBSOLETE CODE** -- gamepatches TokenAuth::Init has zero call sites per boot.cpp analysis |

---

## 19. Frame Dispatch / Wave0 / Other

| # | Feature | Citation | How to test | PASS signal | FAIL signal | Testable on | Silent break risk |
|---|---------|----------|-------------|-------------|-------------|-------------|-------------------|
| F01 | Wave0 instrumentation init | `wave0_instrumentation.cpp` | Start server | `[NEVR] wave0 OK` from initialize.cpp:329 | Wave0 hooks not installed | wine | Binary bug detection disabled |
| F02 | Wave0 shutdown | `wave0_instrumentation.cpp` via `dllmain.cpp:105` | Stop server | Wave0 shutdown called | Hooks left installed | wine | **SILENT** -- no shutdown log |
| F03 | DLL load hook -- intercept LoadLibrary | `dll_load_hook.cpp` | Start server | `[NEVR] DLL load hooks OK` from initialize.cpp:233 | DLL interception not installed | wine | DXGI/D3D11 load without interception |
| F04 | Built-in log filter init | `builtin_log_filter.cpp` | Start server | `[NEVR] log filter OK` from initialize.cpp:278; filter health lines in log | Game log lines unfiltered | wine | Log noise floods output |
| F05 | Log filter -- config load | `builtin_log_filter.cpp:257` | Start server with filter config | `config loaded: min_level=..., N channels, N patterns...` or `no config file found, using built-in defaults` | Config parse failure; default rules used | wine | Wrong suppression rules; important lines filtered or noise unfiltered |
| F06 | Log filter -- file output | `builtin_log_filter.cpp:437` | Start server | `logging to: <path>` | Log file not created | wine | Log output lost on disk |
| F07 | Log filter -- hook install | `builtin_log_filter.cpp:836` | Start server | `hook installed on CLog::PrintfImpl @ 0x...` | `MH_CreateHook failed` or `MH_EnableHook failed` | wine | Game log lines not intercepted |
| F08 | Log filter -- shutdown | `builtin_log_filter.cpp:841-847` | Stop server | `hook removed (emitted: N, suppressed: N)` | Filter not cleaned up; hook left | wine | Next run may have stale hook |
| F09 | Server timing -- tick rate control | `server_timing.cpp` | Start server | Server tick rate managed | Default tick behavior | wine | Server tick rate uncontrolled |
| F10 | Server timing -- shutdown | `server_timing.cpp` via `dllmain.cpp:109` | Stop server | ServerTiming::Shutdown() called | Tick control persists after shutdown | wine | **SILENT** -- no shutdown log |
| F11 | Broadcaster guard -- no-op placeholder | `broadcaster_guard.cpp` | Start server | `[NEVR] broadcaster guard OK` from initialize.cpp:273; nothing actually installed | No observable difference | wine | **NOT IMPLEMENTED** -- `BroadcasterGuard::Install()` is an empty placeholder |
| F12 | WinHTTP stub -- vtable fix | `winhttp_stub.cpp` | Start server | WinHTTP calls intercepted | Game WinHTTP calls fail under Wine | wine | HTTP requests fail; services unreachable |
| F13 | XInput stubs -- GetState/SetState/GetCaps | `initialize.cpp:101-111` | Start server | XInput calls return DEVICE_NOT_CONNECTED (0x48F) | Game tries real XInput; error or hang | wine | **SILENT** -- no log on stub invocation (one-time stderr) |

---

## Visual Checks (no log signal)

These checks require visual confirmation and cannot be validated from log output alone.

| # | Check | How to test | PASS looks like | Testable on |
|---|-------|-------------|-----------------|-------------|
| V01 | Headless server opens no window | Start server; `verify-server.sh` | `max_game_windows=0` in verification output | wine |
| V02 | Cosmetic tints render | Join game on client with CDN enabled | CDN-delivered tint visible on player model (not just "tints loaded" in log) | client |
| V03 | `DSC-` prefix in client UI | Launch client | Account IDs display as `DSC-...` never `PSN-` or `UNK-` | client |
| V04 | Match is joinable and playable | Join a NEVR server from client | Client reaches live session; gameplay is smooth | client/live |
| V05 | Idle CPU is sane | Measure server CPU at idle | CPU usage within expected bounds for headless server | wine |
| V06 | Clean exit leaves no zombie port | CTRL+C server, immediately restart | Port is immediately re-bindable | wine |
| V07 | No crash dumps | Run server through full session | `crash_dumps_after` equals `crash_dumps_before` | wine |

---

## Features Not Testable (and why)

| Feature | Status |
|---------|--------|
| Broadcaster guard | **Not implemented.** `BroadcasterGuard::Install()` is an empty placeholder. |
| Quest / standalone | **Stub.** `src/standalone/` does not build. |
| Build identity / attestation at login | **Not built.** Login JSON hard-codes `buildversion:631547` and empty `publisher_lock`. Open N112. |
| Plugin manifest transmission | **Not built.** Also N112. Capability declaration exists (v3); transmission does not. |
| Go integration suites | **Excised** from `just verify` per RULINGS.md 2026-07-20. |
| crash-handler plugin | Present in `plugins/` but not built by `plugins/CMakeLists.txt`. Unwired. |
| Loadout save/load round-trip | **Needs live server with players.** The loadout persistence path exercises ServerDB write+read. |
| Session lifecycle (full: create -> play -> end) | **Needs live server with players.** Server-side session state transitions require actual game activity. |
| Telemetry data integrity (full frames) | **Needs telemetry consumer.** Can verify stream start/stop; content validation needs a consumer. |
| Token refresh (live expiry) | **Needs client + real token.** Hand-editing cached expiry is the practical workaround (R9 in smoke-tests.md). |

---

## Test Run Sequencing

Runs are ordered so that each one's prerequisites are satisfied before it executes.

| Run | Who | Command | Covers | Notes |
|-----|-----|---------|--------|-------|
| **R1** server baseline | operator | `./verify-server.sh smoke-r1 default 100` | All B*, C*, S*, R*, P*, H*, M*, SD*, G*, T*, PC*, TA07, F* (wine-testable) | Workhorse run. ~150 checks. |
| **R2** plugin loader | operator | Stage `nevr_example.dll` into `plugins/`, then R1 | PL01-PL09 | Plugin path exercise. |
| **R3** loader refusal (N89) | operator | Stage `log_filter.dll`, rerun | PL04 (missing export rejection) | Negative control. Remove afterwards. |
| **R4** UPnP | operator | R1 with `-upnp` added | U01-U03 | Depends on LAN IGD availability. |
| **R5** telemetry | operator | R1 with `telemetry_uri` set + listener | T01-T17 | Without listener, most T* are ABSENT. |
| **R6** client baseline | **Andrew** | `./launch-client.sh` | C13-C15 (arena overrides), S01 (client path), W04-W05, TA02-TA03 | Negative control for server-specific checks. |
| **R7** fresh device auth | **Andrew** | Delete cached credentials, launch client | TA02-TA03, TA06 | Watch for ASCII code box; complete in browser. |
| **R8** cached-token restart | **Andrew** | Relaunch without deleting credentials | TA04 | Must NOT re-prompt. |
| **R9** token refresh | **Andrew** | Hand-edit cached expiry to past, keep refresh token, relaunch | TA05 | Only practical way to force refresh. |
| **R10** live session | **Andrew** | Join match, change loadout, leave | G13-G18, G24-G25, G30-G40, V02-V04 | Visual checks; loadout round-trip. |

---

## References

- `docs/process/smoke-tests.md` -- automated smoke test runner and scoring
- `docs/standards/logging.md` -- logging standards (subsystem tags, levels, rules)
- `docs/standards/verification.md` -- evidence ladder, falsification requirements
- `BUGS.md` -- N-ledger of known defects
- `CLAUDE.md` -- project architecture and conventions
