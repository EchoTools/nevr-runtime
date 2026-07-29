# Recon Results — Owner Bug Batch (19 items) — 2026-07-22

READ-ONLY validation. Next free N-ID: **N39** (highest in BUGS.md: N38).

---

## Item 1 — Boot logs are post-hoc, not pre-action

**CONFIRMED.** All 27 boot-sequence log lines in `initialize.cpp` and `headless_graphics.cpp` confirm completion AFTER each step — none announce intent without having attempted the action first. The complete ordered list:

### `src/runtime/lifecycle/initialize.cpp` — `Initialize()` (lines 211–334)

| # | File:Line | Message | Timing |
|---|-----------|---------|--------|
| 1 | `:215` | `"[NEVR.PATCH] Initializing v%s base=%p\n"` | Announces intent (init starting) |
| 2 | `:219` | `"[NEVR] WARNING: game binary version mismatch — hooks may crash\n"` | Post-check warning |
| 3 | `:223` | `"[NEVR] fn ptrs OK\n"` | Confirms AFTER — function pointers resolved |
| 4 | `:226` | `"[NEVR] FATAL: hooking init failed\n"` | Confirms failure AFTER (early return) |
| 5 | `:229` | `"[NEVR] minhook OK, hooking...\n"` | Hybrid — confirms MinHook init, announces proceeding |
| 6 | `:233` | `"[NEVR] DLL load hooks OK\n"` | Confirms AFTER — `DllLoadHook::Install()` done |
| 7 | `:241` | `"[NEVR] headless graphics hooks registered\n"` | Confirms AFTER — `InstallHeadlessGraphicsHooks()` done |
| 8 | `:249` | `"[NEVR] CSysDLL_GetSymbol hook OK\n"` | Confirms AFTER |
| 9 | `:251` | `"[NEVR] CSysDLL_GetSymbol hook FAILED\n"` | Confirms failure AFTER (alternate) |
| 10 | `:260` | `"[NEVR] CSysDLL_Load hook SKIPPED — prologue mismatch at 0x14105aa70 (binary drift?)\n"` | Post-check warning |
| 11 | `:265` | `"[NEVR] CSysDLL_Load hook OK (pnsradgameserver -> in-process ServerLib)\n"` | Confirms AFTER |
| 12 | `:267` | `"[NEVR] CSysDLL_Load hook FAILED\n"` | Confirms failure AFTER (alternate) |
| 13 | `:273` | `"[NEVR] broadcaster guard OK\n"` | Confirms AFTER |
| 14 | `:278` | `"[NEVR] log filter OK\n"` | Confirms AFTER |
| 15 | `:281` | `"[NEVR] BuildCmdLine target=%p\n"` | Pre-action info (diagnostic) |
| 16 | `:284` | `"[NEVR] BuildCmdLine hook: %s\n"` | Confirms AFTER (OK or FAILED) |
| 17 | `:287` | `"[NEVR] PreprocessCmd hook: %s\n"` | Confirms AFTER (OK or FAILED) |
| 18 | `:295` | `"[NEVR] game hooks OK\n"` | Confirms AFTER — batch of 8 PatchDetour calls |
| 19 | `:301` | `"[NEVR] tls: ws bridge deferred to boot\n"` | Status note (design decision) |
| 20 | `:303` | `"[NEVR] crash OK\n"` | Confirms AFTER |
| 21 | `:305` | `"[NEVR] platform hooks deferred to module\n"` | Status note |
| 22 | `:312` | `"[NEVR] server hooks OK\n"` | Confirms AFTER — batch of 4 hooks |
| 23 | `:315` | `"[NEVR] veh OK\n"` | Confirms AFTER |
| 24 | `:317` | `"[NEVR] console OK\n"` | Confirms AFTER |
| 25 | `:325` | `"[NEVR] patches OK\n"` | Confirms AFTER |
| 26 | `:329` | `"[NEVR] wave0 OK\n"` | Confirms AFTER |
| 27 | `:334` | `"[NEVR.PATCH] All hooks installed"` (via `Log(Info, ...)`) | Confirms AFTER — summary |

### `src/runtime/patch/headless_graphics.cpp`

| # | File:Line | Message | Timing |
|---|-----------|---------|--------|
| 28 | `:570-571` | `"[NEVR.HEADLESS] registered dxgi/d3d11/d3d12 hooks g_isHeadless=%d\n"` | Confirms AFTER (registration done; uses fprintf per N36) |
| 29 | `:451-453` | `"[NEVR.HEADLESS] hooked name=CreateDXGIFactory1 va=0x%llX"` (Log) | Confirms AFTER (fires later when DLL loads) |
| 30 | `:457-461` | `"[NEVR.HEADLESS] hook failed name=CreateDXGIFactory1 ..."` (Log, Warning) | Confirms failure AFTER (alternate) |
| 31 | `:472-474` | `"[NEVR.HEADLESS] hooked name=CreateDXGIFactory va=0x%llX"` (Log) | Confirms AFTER |
| 32 | `:478-482` | `"[NEVR.HEADLESS] hook failed name=CreateDXGIFactory ..."` (Log, Warning) | Confirms failure AFTER |
| 33 | `:494-496` | `"[NEVR.HEADLESS] hooked name=D3D11CreateDevice va=0x%llX"` (Log) | Confirms AFTER |
| 34 | `:500-504` | `"[NEVR.HEADLESS] hook failed name=D3D11CreateDevice ..."` (Log, Warning) | Confirms failure AFTER |
| 35 | `:540-542` | `"[NEVR.HEADLESS] hooked name=D3D12CreateDevice va=0x%llX"` (Log) | Confirms AFTER |
| 36 | `:546-550` | `"[NEVR.HEADLESS] hook failed name=D3D12CreateDevice ..."` (Log, Warning) | Confirms failure AFTER |

Runtime intercept log lines (when `g_isHeadless` is true at call time):
- `:361-362`: `"[NEVR.HEADLESS] CreateDXGIFactory1 intercepted — returning stub factory (no GPU)"`
- `:379-380`: `"[NEVR.HEADLESS] CreateDXGIFactory intercepted — returning stub factory (no GPU)"`
- `:428-429`: `"[NEVR.HEADLESS] D3D11CreateDevice intercepted — returning null device (no GPU)"`
- `:526-527`: `"[NEVR.HEADLESS] D3D12CreateDevice intercepted — returning null device (no GPU)"`

**Existing N-entry:** None directly. N17 (fprintf→Log conversion) touched these lines but didn't change timing.

---

## Item 2 — Bare `[NEVR]` prefix with no submodule

**CONFIRMED.** Counts across all of `src/`:

| Tag | Count |
|-----|-------|
| `[NEVR.PATCH]` | 248 |
| `[NEVR.GAMESERVER]` | 226 |
| `[NEVR.AUTH]` | 77 |
| `[NEVR.WS]` | 71 |
| `[NEVR.TELEMETRY]` | 44 |
| `[NEVR.CDN]` | 35 |
| `[NEVR.PLUGIN]` | 34 |
| `[NEVR.API]` | 26 |
| `[NEVR.HTTP]` | 24 |
| `[NEVR.HEADLESS]` | 23 |
| `[NEVR.SOCIAL]` | 17 |
| `[NEVR.UPNP]` | 14 |
| `[NEVR.RESOURCE]` | 12 |
| `[NEVR.MODULE]` | 8 |
| `[NEVR.XPID]` | 5 |
| `[NEVR.PROFILE]` | 4 |
| `[NEVR.DLLHOOK]` | 3 |
| `[NEVR.OVR-STUB]` (via STUB_LOG macro) | 2 |
| `[NEVR.SHIM]` | 1 |
| `[NEVR.CONFIG]` | 1 |
| **`[NEVR]` (bare, no submodule)** | **36** |

### Bare `[NEVR]` log/message uses (25 instances, all in active code):

| File:Line | Message |
|-----------|---------|
| `initialize.cpp:91` | `CSysDLL_GetSymbol('ServerLib') -> gamepatches factory` |
| `initialize.cpp:219` | `WARNING: game binary version mismatch — hooks may crash` |
| `initialize.cpp:223` | `fn ptrs OK` |
| `initialize.cpp:226` | `FATAL: hooking init failed` |
| `initialize.cpp:229` | `minhook OK, hooking...` |
| `initialize.cpp:233` | `DLL load hooks OK` |
| `initialize.cpp:241` | `headless graphics hooks registered` |
| `initialize.cpp:249` | `CSysDLL_GetSymbol hook OK` |
| `initialize.cpp:251` | `CSysDLL_GetSymbol hook FAILED` |
| `initialize.cpp:260` | `CSysDLL_Load hook SKIPPED — prologue mismatch at 0x14105aa70 (binary drift?)` |
| `initialize.cpp:265` | `CSysDLL_Load hook OK (pnsradgameserver -> in-process ServerLib)` |
| `initialize.cpp:267` | `CSysDLL_Load hook FAILED` |
| `initialize.cpp:273` | `broadcaster guard OK` |
| `initialize.cpp:278` | `log filter OK` |
| `initialize.cpp:281` | `BuildCmdLine target=%p` |
| `initialize.cpp:284` | `BuildCmdLine hook: %s` |
| `initialize.cpp:287` | `PreprocessCmd hook: %s` |
| `initialize.cpp:295` | `game hooks OK` |
| `initialize.cpp:301` | `tls: ws bridge deferred to boot` |
| `initialize.cpp:303` | `crash OK` |
| `initialize.cpp:305` | `platform hooks deferred to module` |
| `initialize.cpp:312` | `server hooks OK` |
| `initialize.cpp:315` | `veh OK` |
| `initialize.cpp:317` | `console OK` |
| `initialize.cpp:325` | `patches OK` |
| `initialize.cpp:329` | `wave0 OK` |
| `state_machine.cpp:67` | `Session ended. Server exiting.` |

### Bare `[NEVR]` CLI help-text uses (11 instances):

| File:Line | Message |
|-----------|---------|
| `cli.cpp:17` | `Run as a dedicated game server (implies headless)` |
| `cli.cpp:20` | `Run the game in offline mode` |
| `cli.cpp:23` | `Run the game with no headset, in a window` |
| `cli.cpp:26` | `Disable console window creation` |
| `cli.cpp:29` | `Specify a custom path to config.yaml` |
| `cli.cpp:32` | `Set the matchmaking region` |
| `cli.cpp:35` | `Set the server fleet region` |
| `cli.cpp:39` | `Keep server running after serverdb disconnect (default: exit)` |
| `cli.cpp:43` | `Deprecated — exit-on-error is now the default` |
| `cli.cpp:46` | `Disable telemetry streaming` |
| `cli.cpp:49` | `Set telemetry rate in Hz (default 10)` |
| `cli.cpp:52` | `Log telemetry diagnostics every second` |
| `cli.cpp:55` | `Prefix log lines with timestamps` |
| `cli.cpp:58` | `Enable UPnP port forwarding` |

**Existing N-entry:** None. This is a logging-convention finding.

---

## Item 3 — Default-loaded modules/plugins

**CONFIRMED.** Three modules loaded unconditionally in `src/runtime/lifecycle/boot.cpp`, inside `PreprocessCommandLineHook()`:

| # | Module name | File:Line | Reason |
|---|---|---|---|
| 1 | `platform_compat` | `boot.cpp:70` | Schannel TLS hooks, CreateDirectory fixes, WinHTTP bridge. "Must load before any network-using code." |
| 2 | `token_auth` | `boot.cpp:74` | Device code authentication, JWT refresh. "Must load before ws_bridge." |
| 3 | `ws_bridge` | `boot.cpp:87` | WebSocket TLS proxy. "Must load after token_auth." |

All three are **runtime-loaded via `LoadLibrary`** from a `modules/` subdirectory next to echovr.exe. `LoadModule()` in `module_loader.cpp:42-83` constructs `<modules dir>\modules\<name>.dll` and calls `LoadLibraryA`.

**`builtin_log_filter.cpp` is compiled into BugSplat64.dll**, not loaded as a module:
- `src/runtime/CMakeLists.txt:25` — `"builtin_log_filter.cpp"` in `PATCHES_SOURCES`
- `initialize.cpp:277` — `BuiltinLogFilter::Init(...)` called during `Initialize()`

**Plugins** are discovered at runtime from `plugins/` subdir via glob (`*.dll`). `plugin_loader.cpp:23-131` (`LoadPlugins()`) calls `FindFirstFileA`/`FindNextFileA`. No plugins ship by default; only what's on disk loads. Called from `boot.cpp:227`.

**Existing N-entry:** None.

---

## Item 4 — Example plugin / Plugin API v2

**NOT-FOUND.** No example/reference plugin exists. `plugins/` contains only production plugins:

```
plugins/anim-debugger/
plugins/broadcaster-bridge/
plugins/common/
plugins/crash-handler/
plugins/log-filter/
```

Plugin API v2 header: `src/extension/plugin_interface.h:95` — `NEVR_PLUGIN_API_VERSION = 2`. Shared plugin-author headers in `plugins/common/include/`: `nevr_common.h`, `nevr_curl.h`, `yaml_config.h`, `plugin_logger.h`, `hook_manager.h`, `address_registry.h`, `resource_registry.h`, `safe_memory.h`, `auth_token_refresh.h`.

**Existing N-entry:** None.

---

## Item 5 — Log-level distribution

**CONFIRMED.** Counts across `src/` (excluding `src/legacy/`):

| Call site category | Count |
|--------------------|-------|
| `Log(..., LogLevel::Info, ...)` | 474 |
| `Log(..., LogLevel::Warning, ...)` | 243 |
| `Log(..., LogLevel::Error, ...)` | 71 |
| `Log(..., LogLevel::Debug, ...)` | 55 |
| `Log(..., LogLevel::Trace, ...)` | 0 |
| `fprintf(stderr, ...)` | 49 (30 in `initialize.cpp`) |
| `fflush(stderr)` | 41 (29 in `initialize.cpp`) |
| `BlfLog(...)` | 10 (all in `builtin_log_filter.cpp`) |

Including legacy:
| Call site category | Count |
|--------------------|-------|
| `Log(..., LogLevel::Info, ...)` | 624 |
| `Log(..., LogLevel::Warning, ...)` | 296 |
| `Log(..., LogLevel::Error, ...)` | 102 |
| `Log(..., LogLevel::Debug, ...)` | 74 |
| `Log(..., LogLevel::Trace, ...)` | 0 |

Key observations:
- **No `LogLevel::Trace` usage** anywhere in the codebase.
- **`fprintf(stderr)` is concentrated in `initialize.cpp`** (30 of 49). By design — boot messages must use fprintf because `EchoVR::WriteLog` crashes under the loader lock (N36).
- Info:Warning ratio ~2:1.
- Top Info files: `gameserver/gameserver.cpp` (52), `telemetry_streamer.cpp` (40), `token_auth.cpp` (27), `ws_bridge.cpp` (26).

**Existing N-entry:** None. N19 resolved the logging standard's existence but didn't audit volume distribution.

---

## Item 6 — dbgcore.dll handling

**CONFIRMED — implicit detection, no explicit logging.**

Detection at `dllmain.cpp:90-96`:
```cpp
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
  switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
      if (GetModuleHandleA("echovr_game.dll") == NULL) {
        EchoVR::g_GameBaseAddress = (CHAR*)GetModuleHandle(NULL);
        Initialize();
      }
      break;
```

The check is `GetModuleHandleA("echovr_game.dll") == NULL` — if the launcher's helper DLL isn't present, assume legacy dbgcore.dll hijack path. No explicit log line distinguishes the two paths. The `Initialize()` call at `:215` fires either way with `"[NEVR.PATCH] Initializing v%s base=%p"`.

**No `-allow-*` flags exist** anywhere in the codebase. Search of `src/` for `allow-` returned zero results (except `--allow-multiple-definition` which is a CMake linker flag).

**Existing N-entry:** None.

---

## Item 7 — echovr_server.exe wrapper

**NOT-FOUND.** No wrapper exe, no build target, no launcher directory.

- `src/launcher/` — does not exist.
- `CMakeLists.txt` — no `add_executable` targets. References to `echovr_launcher.exe`/`echovr_server.exe` are in `dist-prepare` copy commands (`|| true` guarded — optional).
- `justfile` — no exe build target.
- `extras/dbghooks/echovr_launcher.cpp` — 106-line dormant file, loads `echovr.dll` (not `echovr.exe`), refers to legacy `GamePatches.dll`/`dbgcore.dll`. Not wired into any build target.

**`launch-server.sh`** deploys from `build/mingw-release/bin/`, then runs:
```sh
cd echovr/bin/win10 && wine ./echovr.exe -server -noconsole
```
It runs the original `echovr.exe` directly under Wine. The DLL hijack (deploying as `BugSplat64.dll`) provides the entry point.

**Existing N-entry:** None.

---

## Item 8 — `[NSUSER] Creating user ???-1` platform prefix

**CONFIRMED — game binary, not our code.**

- "Creating user" format string lives in echovr.exe at `0x141c3b1d8` (ReVault-verified): `[NSUSER] Creating user %s`
- The `???-` prefix is an unpatched fallback string-table entry at `0x1416d0f9c`
- Our `xpid_patch.cpp:8-57` patches 3 entries: `PSN`→`DSC` at `0x1416D0EE0`, `PSN-`→`DSC-` at `0x1416D0F64`, `PSN`→`DSC` at `0x1416D7138`. The `???-` at `0x1416d0f9c` is **not patched** — it's the catch-all fallback.
- Our `PlatformPrefix()` in ws-bridge (both copies) correctly maps platform=2→"DSC":
  - `modules/ws-bridge/src/ws_bridge.cpp:68-75`
  - `gamepatches/ws_bridge.cpp:77-84`

**Discrepancy explanation:** When CNSUser state isn't set yet (early init, login_state=0), the game's platform-name lookup hits the unpatched `???-` fallback and formats `???-1` where `1` is the numeric account ID (not platform code). Once our ws-bridge injects the LoginRequest with platform=2 and CNSUser state advances, the game uses our patched `DSC-` prefix. Our log confirms: `login injected xpid=DSC-1492861453923913968 platform=2`.

**Existing N-entry:** N14 (closed) covered platform prefix hardcoded as OVR_ORG. The `???-` fallback is a residual game-binary limitation not covered by any open entry.

---

## Item 9 — NoVR platform prefix

**CONFIRMED.**

`-noovr` flag exists and is parsed:
- `cli.cpp:63` — registered as recognized argument
- `boot.cpp:148-161` — treated same as `-headless`
- `mode_patches.cpp:570-581` — `PatchNoOvrRequiresSpectatorStream` bypasses the error requiring `-spectatorstream` with `-noovr`

Platform prefix construction sites (all copies of `PlatformPrefix()`):

| File:Line | Codes |
|-----------|-------|
| `modules/ws-bridge/src/ws_bridge.cpp:68-75` | STM(1), DSC(2), XBX(3), OVR-ORG(4), UNK(default) |
| `gamepatches/ws_bridge.cpp:77-84` | Same (duplicate) |

Login JSON at `ws_bridge.cpp:101`: `"headset_type":"No VR"` — this is a system_info field, not a platform prefix. No separate NoVR platform code exists in `PlatformPrefix()`.

Game string-table patches:
- `xpid_patch.cpp:8-57` — 3 PSN→DSC patches
- `patch_addresses.h:399-413` — patch address definitions

**Existing N-entry:** None directly. N14 (platform prefix) is closed.

---

## Item 10 — Multiline log messages

**CONFIRMED — all from CLog::PrintfImpl, through our hook.**

Three real examples from `/var/tmp/work-nevr-runtime/server-run-no-display-v3.log`:

**Example 1 — Game banner (lines 61-63):**
```
2026-07-20T22:50:16.222Z info ==========================================================
 Echo VR
==========================================================
```
One `CLog::PrintfImpl` call with embedded `\n`.

**Example 2 — File error (lines 64-67):**
```
2026-07-20T22:50:16.225Z error 0x1f234cb1527e2544:
    While operating on file 'Z:\home\andrew\src\nevr-runtime\echovr\bin\win10\_local\config.json'
    Windows Error (3): Path not found.
    ...in d:\projects\rad\dev\src\engine\libs\os\csysfile_win_xb1.cpp at line 179
```
One `CLog::PrintfImpl` call from RAD engine's `CSysFile`.

**Example 3 — Scriptnode error (lines 222-229):**
```
2026-07-20T22:50:16.833Z error ~ Scriptnode can't find actor in gamespace ~
	Script Actor:     0x710CD6662636E6B8
	Script:           0x26EBC7ED80EBA931
	Node Type:        EnableComponentNode
	Node UUID:        c9971725
	Target Actor:     0x41D2D7311B3A020C
	Gamespace:        
: raduri://0x26EBC7ED80EBA931?elementname=c9971725&elementtype=scriptnodeelementtype=scriptproperty
```
One `CLog::PrintfImpl` call from game's scripting subsystem with embedded `\n\t`.

**Origin confirmation:** All three appear in the JSONL output file with embedded `\n` escaped as `\\n` — the JSONL is only written by `builtin_log_filter`'s hook on `CLog::PrintfImpl`. Raw Wine stderr (fixme, winediag) appears in console logs WITHOUT timestamps/colors — bypasses the game's logging entirely.

**How builtin_log_filter handles embedded newlines:**
- `builtin_log_filter.cpp:792-805` — `vsnprintf` into `char buf[0x2000]`, no splitting on `\n`
- `:720-774` (`EmitLine`) — console: `fprintf(stderr, "%.*s\n", len, message)` renders literal newlines. JSONL: `JsonEscapeAppend` escapes `\n`→`\\n`, keeping one logical event per JSON line.

**Existing N-entry:** N18 (log filter suppression) is related but doesn't specifically cover multiline handling.

---

## Item 11 — Service-redirect identity

**CONFIRMED.**

### RedirectServiceUrl (`config.cpp:354-394`)
Uses the config key name as the service label:
```
"[NEVR.PATCH] Service redirect [%s]: %s -> %s", keyName, result, redirected
```
Where `keyName` is e.g. `"loginservice_host"`, `"configservice_host"`.

### HttpConnectHook (`config.cpp:286-319`)
Classifies by URL substring:
- `"https://api."` prefix → `apiservice_host`
- contains `"config"` → `configservice_host`
- contains `"transaction"` or `"iap"` → `transactionservice_host`
- contains `"match"` → `matchingservice_host`
- contains `"serverdb"` or `"registry"` → `serverdb_host`

### Ws-bridge connection labeling (`modules/ws-bridge/src/ws_bridge.cpp`)
Connections carry only:
- Numeric `connIdx` (0=config, 1=login, >=2=matchmaker) — documented at `:52`
- `connState->getId()` — an ixwebsocket internal opaque string (UUID-style)

Key log lines:
- `:244`: `"[NEVR.WS] Remote open (conn=%d): %s", connIdx, g_remoteUri.c_str()`
- `:296-297`: `"[NEVR.WS] login injected xpid=%s platform=%d conn=%d size=%zu", ...`
- `:314-315`: `"[NEVR.WS] server->game [conn=%d]: ... sym=... payloadLen=...", ...`
- `:491-492`: `"[NEVR.WS] Proxy: game connected (conn=%s, ws=%p), bridging to %s", ...`
- `:513-515`: `"[NEVR.WS] game->server [%d]: sym=... len=... (conn=%s)", ...`

**No explicit service tag** (like "login", "config", "match") is carried in connection metadata. Service identity is recoverable heuristically from connection order + message content, but not tagged explicitly.

**Existing N-entry:** None.

---

## Item 12 — Symbol-id → string hash resolution

**NOT-FOUND.** No runtime hash→name reverse lookup table exists.

- `src/abi/symbol_hash.h:66-85` — `CSymbol64Hash()`: custom CRC64, polynomial `0x95ac9329ac4bc9b5`, one-way (forward only, no reverse).
- `src/abi/symbols.h` — ~40 compile-time name→hash constants (forward-only, static).
- `src/runtime/patch/resource_override.cpp` — matches by raw numeric `(type_hash, name_hash)` pairs, no name strings.
- `gameserver/gameserver.cpp:510` — comment: `"Instance name as hex (can be converted via hashes.txt)"` — but `hashes.txt` does not exist in the repository.
- `~/src/revault/` — exists, but managed store is not populated for reverse lookup. ReVault `search_strings` and `search_code` work for forward queries.

**Existing N-entry:** None.

---

## Item 13 — Console vs log file (fprintf sink)

**CONFIRMED.**

### Log() function (`src/core/logging.cpp:64-75`)
- When `WriteLog` is non-null (normal operation): delegates to `EchoVR::WriteLog(level, 0, format, args)` → game's `CLog::PrintfImpl`.
- When `WriteLog` IS null (early DllMain): falls back to `vfprintf(stderr, format, args); fputc('\n', stderr)` — **stderr only, no file.**

### EmitLine (`builtin_log_filter.cpp:720-774`)
Tees to both sinks:
- **Console** (`:729-741`): `fprintf(stderr, ...)` with timestamps + colors.
- **File** (`:745-772`): `fwrite(...)` to `g_log_file` in JSONL format (with `\n` escaped).

**`fprintf(stderr, ...)` messages reach console only — NOT the JSONL log file.** Only messages passing through the hooked `CLog::PrintfImpl` reach the file.

### What it would take for early boot messages to land in the log file
Per N36, earliest boot messages MUST use fprintf (Log() crashes under loader lock). To also land them in the file:
1. Open a file descriptor early in DllMain (before first `Log()`/`fprintf` call) using raw `open()`/`_open()` with a known path
2. Add a `write()`-based fallback in `Log()`'s null-`WriteLog` branch that writes to both stderr and the pre-opened fd
3. Must NOT go through `CLog::PrintfImpl` (not yet safe)
4. Once `builtin_log_filter` takes over, close the boot fd and resume normal JSONL rotation

**Existing N-entry:** None. N36 is adjacent (documents WHY early boot must use fprintf) but doesn't address the file-sink gap.

---

## Item 14 — Truncated messages (log buffer size)

**CONFIRMED.** Multiple truncation/buffer-size sites:

| File:Line | Buffer/Variable | Size | Purpose | Truncation risk |
|---|---|---|---|---|
| `builtin_log_filter.cpp:792` | `char buf[0x2000]` | 8192 B | `vsnprintf` format buffer | Messages >8191 chars truncated |
| `builtin_log_filter.cpp:67,313` | `max_line_length` | 500 (default) | Emission length clamp | Safe post-format clamp |
| `builtin_log_filter.cpp:416` | `[CONFIGS] ConfigSuccessCB` rule | 80 chars | Specific prefix truncation | Safe clamp — this is what truncated `{"news` in the owner's sample |
| `builtin_log_filter.cpp:413-414` | `[PROFILE] Getting/Updating` rules | 80 chars | Profile message truncation | Safe clamp |
| `builtin_log_filter.cpp:721,468` | `char ts[32]` | 32 B | Timestamp strings | Safe — fixed 24-char ISO8601 format |
| `pnsrad/Plugin/logging.cpp:66` | `char message_buf[0x2000]` | 8192 B | Game native `vsnprintf` buffer | Messages >8191 chars truncated |
| `pnsrad/Plugin/logging.cpp:233` | `char entry_buf[0x100]` | 256 B | Game ring buffer entry | Tightest bottleneck — entries >255 chars truncated |
| `pnsrad/Plugin/logging.cpp:289` | `vsnprintf_wrapper(out_buf, 0x100, ...)` | 256 B | Ring buffer format cap | Same as above |

`ConfigSuccessCB` is NOT in NEVR source code — it's a game-originated message (from the game binary's config system, likely `pnsrad.dll`). The 80-char truncation at `builtin_log_filter.cpp:416` is a safe length clamp on the already-formatted buffer.

**Existing N-entry:** None.

---

## Items 15/16 — Game-originated (RAD) log message hook

**CONFIRMED — CLog::PrintfImpl is hooked. Messages are rewritable.**

Hook installation at `builtin_log_filter.cpp:868-901`:
- `:868-869` — resolves `VA_CLOG_PRINTF_IMPL` via `nevr::ResolveVA()`
- `:870-873` — `MH_CreateHook` on `CLog::PrintfImpl`
- `:886` — `MH_EnableHook`

Hook function at `builtin_log_filter.cpp:785-826` (`hook_PrintfImpl`):
1. Formats into `char buf[0x2000]` via `vsnprintf` (`:792-805`)
2. Checks `ShouldSuppress()` for filtering (`:814`)
3. Applies `ApplyTruncation()` (`:819`)
4. Calls `EmitLine()` for stdout/file output (`:821`)
5. Optionally calls `orig_PrintfImpl()` for passthrough (`:823-824`)

The hook has full access to `level` (uint32_t), `category` (int64_t), `fmt` (const char*), variadic args, and the formatted `buf`. **Messages CAN be re-prefixed, re-leveled, or rewritten** before forwarding to `orig_PrintfImpl`. Currently `passthrough_to_engine = false` (default at `:312`), so game messages are filtered by our hook but NOT forwarded to the original game logger.

**Items 15/16 are implementable.** The architecture is designed for message rewriting.

**Existing N-entry:** None.

---

## Item 17 — Splash screen in server mode (N16)

**CONFIRMED — existing N16 covers it.**

N16 from BUGS.md (lines 1185-1200):
```
### N16. Splash screen delay makes startup timeouts unreliable

| **Where**   | Game process startup (echovr.exe splash → WinMain → boot sequence).   |
| **Reached** | Every server launch. The game unconditionally displays a splash screen |
|             | for ~15-20 seconds before reaching WinMain and the NEVR boot hooks.   |
| **Severity**| Low                                                                   |
| **Status**  | Open — document the known delay.                                       |
```

**Status: Open.** No splash-skip or `-nosplash` code exists anywhere in `src/`. The only splash-related code is `resource_override.cpp:33,209` which replaces the splash texture, not the splash itself. NEVR code does not execute until `DllMain` loads BugSplat64.dll and the game reaches `PreprocessCommandLine` hook — which is after the splash.

---

## Item 18 — EULA dialog

**NOT-FOUND in NEVR code.** No EULA, first-run, Terms-of-Service, or EULA-acceptance dialog handling code exists anywhere in `src/`. Search of all `.cpp`/`.h` files for EULA, first-run, firstrun, Terms-of-Service, TOS, Accept-EULA returned zero matches.

The EULA is a game-engine UI flow, distinct from N4's `FatalError MessageBoxA`. N14 (closed) noted that the platform prefix fix (OVR_ORG→DSC) resolved the symptom — the game looks for cached EULA acceptance in a per-user directory keyed by platform prefix, and the wrong prefix prevented it from finding the cached acceptance. There is no NEVR code that directly handles or bypasses the EULA dialog.

Server/headless mode does NOT separately bypass the EULA.

**Existing N-entry:** None. N4 (FatalError MessageBoxA) and N14 (platform prefix) are related but don't cover EULA bypass.

---

## Item 19 — Token auth (N20)

**CONFIRMED — existing N20 covers it.**

N20 from BUGS.md (lines 1293-1306):
```
### N20. discord_id hardcoded in login injection (deferred token auth)

| **Where**   | src/modules/ws-bridge/src/ws_bridge.cpp:169, :200, :68-70 |
| **Reached** | Login injection paths (conn=0 and conn>0).                |
| **Severity**| Low                                                       |
| **Status**  | Open — deferred to token-auth integration.                |
```

**Status: Open.** Credentials/tokens are persisted in:

1. **`_local/config.json`** — loaded by `LoadEarlyConfig()` at `config.cpp:32-61`. Contains `nevr_discord_id` (read at `modules/ws-bridge/src/ws_bridge.cpp:181,212`, `gamepatches/ws_bridge.cpp:209`, `gamepatches/gameserver/gameserver.cpp:1156`, `pnsrad/Social/CNSRADUsers.cpp:296`).

2. **`_local/.credentials.json`** — saved by `DeviceAuth::SaveToken()` at `modules/token-auth/src/token_auth.cpp:114-130`. Loaded by `LoadCachedAuthToken()` at `src/core/auth_token.h:110-141`. Contains JWT access token, refresh token, user_id, username. Stored with `FILE_ATTRIBUTE_HIDDEN` + restrictive DACL (Windows) or `umask(0177)` (non-Windows).

3. **`nevr_defaults.h`** — **removed** at `1a91c1d` per N5. Previously contained compile-time default keys.

---

## Summary grid

| Item | Verdict | Existing N-entry? | Key file:line |
|------|---------|-------------------|---------------|
| 1 | CONFIRMED | None | `initialize.cpp:215-334`, `headless_graphics.cpp:570-571` |
| 2 | CONFIRMED | None | 25 bare `[NEVR]` log uses, all in `initialize.cpp` |
| 3 | CONFIRMED | None | `boot.cpp:70,74,87`, `CMakeLists.txt:25`, `initialize.cpp:277` |
| 4 | NOT-FOUND | None | `nevr_plugin_interface.h:95` |
| 5 | CONFIRMED | None | Info:474, Warning:243, Error:71, Debug:55, Trace:0 |
| 6 | CONFIRMED | None | `dllmain.cpp:90-96` (implicit detect) |
| 7 | NOT-FOUND | None | `launch-server.sh` runs `wine ./echovr.exe -server` |
| 8 | CONFIRMED | None (N14 closed) | `???-` at game binary `0x1416d0f9c` |
| 9 | CONFIRMED | None | `cli.cpp:63`, `boot.cpp:148`, ws_bridge `PlatformPrefix()` |
| 10 | CONFIRMED | None | All from `CLog::PrintfImpl`, through our hook |
| 11 | CONFIRMED | None | `conn=N` only; `config.cpp:354-394` for RedirectServiceUrl |
| 12 | NOT-FOUND | None | CRC64 one-way; no reverse table |
| 13 | CONFIRMED | None | `logging.cpp:64-75`, `builtin_log_filter.cpp:720-774` |
| 14 | CONFIRMED | None | `buf[0x2000]`, `max_line_length=500`, `ConfigSuccessCB=80` |
| 15/16 | CONFIRMED | None | `builtin_log_filter.cpp:785-826,868-901` — implementable |
| 17 | N16 covers it | N16 (Open) | `BUGS.md:1185-1200` |
| 18 | NOT-FOUND | None | No EULA code in `src/` |
| 19 | N20 covers it | N20 (Open) | `BUGS.md:1293-1306`, `auth_token.h:110-141` |

Next free N-ID: **N39**.
