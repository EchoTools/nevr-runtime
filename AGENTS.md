# AGENTS.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

The key words "MUST", "MUST NOT", "REQUIRED", "SHALL", "SHALL NOT", "SHOULD", "SHOULD NOT", "RECOMMENDED",
"NOT RECOMMENDED", "MAY", and "OPTIONAL" in this document are to be interpreted as described in BCP 14
([RFC 2119](https://www.rfc-editor.org/rfc/rfc2119), [RFC 8174](https://www.rfc-editor.org/rfc/rfc8174)) when, and only
when, they appear in all capitals, as shown here.

## Commits, versions and requirement words

- **Commits and PR titles** follow [Conventional Commits 1.0.0](https://www.conventionalcommits.org/en/v1.0.0/):
  `<type>[(scope)][!]: <description>`. Types: `feat` (a feature), `fix` (a bug fix), `docs`, `style`, `refactor`,
  `perf`, `test`, `build`, `ci`, `chore`, `revert`. A breaking change is marked with `!` before the colon or a
  `BREAKING CHANGE:` footer. Per spec item 15, types are case-insensitive. `.github/workflows/pr-title-check.yml`
  (`tools/check_pr_title.py`) checks the PR title.
- **Versions** follow [Semantic Versioning 2.0.0](https://semver.org/spec/v2.0.0.html). Tags stay `vX.Y.Z`.
- **Requirement words** (MUST, SHALL, SHOULD, MAY and their negations) follow
  [RFC 2119](https://www.rfc-editor.org/rfc/rfc2119) as clarified by
  [RFC 8174](https://www.rfc-editor.org/rfc/rfc8174): normative only in all capitals. The same notice opens each file
  in `docs/standards/`.
- **The changelog is the git log.** There is no `CHANGELOG` file; release notes are generated per release (git-cliff
  is planned; no tooling yet).

## Project

nEVR Runtime — Windows DLL patches for Echo VR (echovr.exe) enabling connection to echovrce community game services. Targets both game clients and dedicated game servers. Written in C++17.

## For contributors

The rest of this file binds every agent, a contributor's included, except the parts marked **Maintainers only**. This
section applies to an agent working for an outside contributor, on the contributor's own machine. It adds what such an
agent needs: what to read, what a PR carries, which rules are live, and the N-rules index. It also says which later sections describe the maintainers' machine, and what to do
on yours instead. Where a step needs access only the maintainers have (ReVault, the production server's logs, the
release pipeline), do what you can without it and ask in your PR or issue. Never guess in its place.

### Required reading

Read this file whole before your first change. The four standards in `docs/standards/` are binding, not background.
Read each one before you start the kind of work it covers:

| Standard | Read it before you... |
| --- | --- |
| `docs/standards/verification.md` | claim anything is fixed, working or verified; add or change a check in `just verify`; write a PR body |
| `docs/standards/logging.md` | add, change or review a `Log()` call or a log tag; touch the built-in log filter; diagnose a run from its log |
| `docs/standards/naming.md` | add any name: file, directory, type, function, namespace, macro, config key, log tag, DLL export, branch |
| `docs/standards/planning.md` | open or edit an issue, a PR's milestone or labels, or anything on the project board |

The N-rules ("The N-rules index" below) bind too. A change to a file they guard must keep them, whether or not
`just verify` would notice.

### Maintainers only, and what to do instead

The sections and bullets marked **Maintainers only** describe the maintainers' machine and accounts. Keep the reason
each one gives; do the step this way on yours:

| Maintainers-only part | On your machine |
| --- | --- |
| "Commit identity" (author, trailer, signing) | Commit as yourself: the author is the person responsible, under the name and email they use on GitHub. When an agent wrote the change, add one `Co-Authored-By: <agent name> <email>` trailer. Conventional prefix, one logical change per commit, and the same post-commit check. |
| "Ownership and cleanup": the ledger file | Keep the same ledger in a file of your own: a markdown table (artifact, owner, purpose, remove-when), one row per thing you create, the worktree directory or branch in the first cell and your handle, one word, in the second. Read it before deleting anything. Create worktrees under `.claude/worktrees/` in your clone, and run `REAP_LEDGER=<your ledger file> just reap-merged`: `tools/reap_merged.py` scans only `.claude/worktrees/`, exits when it cannot read the ledger (default `~/.local/share/repo-hygiene/nevr-runtime-ledger.md`), and keeps any worktree the ledger does not credit to an owner. |
| "Builds": the shared `nevr-builds.slice` | Same reason: parallel lanes, each under its own cap, still starve a machine. If you run one lane, `VCPKG_MAX_CONCURRENCY=6 CMAKE_BUILD_PARALLEL_LEVEL=6 systemd-run --user --scope -p MemoryMax=8G <cmd>` is enough. If you run several, create one user slice that caps them all together, as the maintainers do. Wait while two or more `ninja` processes run (`pgrep -x ninja` lists them). |
| Scratch under `/var/tmp/work-nevr-runtime/<agent>/` | The same path works on any Linux machine, and several tools write there by default (`tools/scenario/run_scenario.py`, `tools/lib/game_install.sh`, `tools/winvm/systest.py`). Use `/var/tmp/work-nevr-runtime/<your handle>/`, never `/tmp` and never the repo. |
| "The game install": `/mnt/games` and the owner's link | See "Your game install" below. |
| "ReVault" | ReVault is private. Keep its rules (search the whole project, every binary; report `searched N of N`; never guess an address) with your own disassembler on your own copy of the game files. When you need a fact you cannot derive, ask in the PR with the address and the binary. |
| "Check production Nakama logs" and "ask sprockee" | The production server's logs are maintainer-only. On a server you run, read its log yourself. For production, ask in the PR, giving the run's time window (UTC), the account and the client log lines around the failure. Where the file says ask sprockee, ask in the PR or issue. |
| "Production Deployment" | Never, for a contributor's agent. No approval you can receive counts. |
| "System test for runtime and auth changes": the production server, display `:0` | Log in to the server your build's embedded defaults point at, or one you run. Never use the user's own display; use the headless Xvfb `:101` display exactly as described. If you cannot run the game at all, say so in the PR: the runtime change is then unverified, and a maintainer runs it. |
| "Write down what you learn": `.claude/memory/` | That directory is git-ignored and local to a machine. Put a fact about the game or protocol where the next contributor will find it: a code comment at the site, `docs/reference/`, or an issue. |
| Merging, and "Only the agent the owner has told to merge merges PRs" | A contributor's agent never merges. The maintainers review and merge. |
| `config/public-defaults.env` | Copy `config/public-defaults.env.example` and fill in the four public values. They are public by design (every client embeds them); if you don't have them, ask in an issue. `just worktree-setup` copies the main checkout's file into a new worktree. |
| `just test-winvm` | Needs a Windows VM you provide (`tools/winvm/README.md`). |

**Keep Wine off the user's desktop.** `just verify` runs GTest binaries under Wine, and several tests and scripts
start the game. Run each with `DISPLAY` and `WAYLAND_DISPLAY` unset (`env -u DISPLAY -u WAYLAND_DISPLAY just <recipe>`),
except for a client run on the nested display. A Wine process that reaches the user's display can take over their
mouse and keyboard.

### Your game install

The rule in "The game install" holds for you as it is written: no run deploys into, or writes through, a real
install, and every run uses its own copy, made from a pristine copy and deleted when the run ends (#524). The paths
differ, and so does who makes the pristine copy:

- `SRC` is the root of your install: the directory that holds `bin/win10/echovr.exe` (a stock install is named
  `ready-at-dawn-echo-arena`). If your checkout's `echovr/` is a link to it, `SRC=$(readlink -f <checkout>/echovr)`.
- The pristine copy is `<copy>/echovr`, made by you, the human, never by the agent:
  `mkdir -p <copy>/echovr && cp -a --reflink=auto "$SRC"/. <copy>/echovr/`. `<copy>` lives in your scratch directory.
- In the link check, a line that resolves into `SRC` or anywhere else in your install fails the check. Remove that
  link and run the check again.
- Set `NEVR_RUN_SCRATCH_ROOT` to your scratch directory before using `verify-server.sh`.

Everything else in "The game install" applies unchanged: the list of writers, how to point each at the copy, the
size check, removing `_local/config.*` and `plugins/*`, and the shape each script needs.

### What a PR carries

#471 is the model: a merged PR that carries all of the items below. Most merged PRs do not yet carry all of them, but
your PR does:

- **A search statement.** Before adding anything, show that nothing in the tree or its history already does it. State
  the commands and what they found, over files *and* history:

  ```
  ## Search statement
  - `git grep -n "ReaderGate"` (847 tracked files): src/core/reader_gate.h, src/runtime/patch/asset_cdn.cpp, ...
  - `git log -S ReaderGate --oneline`: 3 commits, all on the existing gate; none adds a second one.
  ```

  "Nothing found" counts only with the pattern and the scope it ran over.
- **A Symbols list.** List the symbols you edited, and separately the ones nearby that you read but did not edit, so
  the reviewer knows what you looked at and what you left alone:

  ```
  ## Symbols
  Edited: nevr::ReaderGate (Scope, WaitIdle, Enter (new)); test_reader_gate.cpp.
  Read, not edited: asset_cdn.cpp call sites of WaitIdle; g_tintGate.
  ```
- **A mutant for every changed line of behaviour.** This is stricter than "A test must fail without the change" under
  "Review, verification and merge", which every PR meets; a contributor's PR meets this as well. For every line
  of code, script or check you add or change, delete or alter that line alone, run the tests, and show that one of
  them fails. A changed line that no test notices is untested. Paste each mutant with its failing output, then the
  restored tree passing. A line with no behaviour (prose, a comment) has no mutant; say which lines those are.

  ```
  ## Mutants
  - M1 drop the epoch re-check in WaitIdle -> FAIL test_reader_gate: Overtaken
  - M2 `undrained_` never decremented -> FAIL test_reader_gate: TimesOutWhenReadersStay
  - restored tree: 9 tests OK
  ```
- **Evidence on the exact tip**, as "Review, verification and merge" says: each command run on the tip SHA and its
  result, what was not run, and the tracking issue.

**Write an issue as a falsifiable claim** (see #519 and #520), with these sections:

- **Evidence at `<sha>`**: what the tree says, re-read at a named commit, with `file:line` and quoted lines.
- **Not shown**: what the evidence does not establish.
- **Falsifier**: the one observation that would prove the issue wrong.
- **Work**: the steps, in order, tests first, each with what it must show.

### Live rules

- **The PR body ends with `Read AGENTS.md at <merge-base sha>`** (see "Review, verification and merge").
  `.github/workflows/pr-read-check.yml` fails the PR without it. On a PR from a fork, a maintainer has to approve the
  workflow run before it runs.
- **Runs use a throwaway copy of the game install, never the install itself** (#524; "The game install"). No check
  enforces this: the scripts still default to the checkout's `echovr/`, so point every writer at the copy yourself.

### The N-rules index

The code carries rules tagged `N<number>` in comments (`// N62: ...`) and in the `just verify` recipe, whose sensors
name the rule they guard in their FAIL line. A green `just verify` does **not** prove you kept every one: some are
checked by a GTest or a Python check, some only by a live run, and some by nothing. Before you change a file named
below, read the rule's comment at the site and keep the rule. If a change must break one, say so in the PR and why.

The index was built from `git grep -nE '\bN[0-9]+\b'` over `src/`, `plugins/` and `tools/`, the `verify` recipe in
`justfile`, and the Python checks it runs. In the "Checked by" column, "verify" means a `just verify` sensor whose
FAIL line names the rule, and "nothing" means no check names it. An ID cited only as history inside another rule's
comment (for example N41, N48) is not a rule.

#### Boot, shutdown and crash path

| Rule | Statement | Guards | Checked by |
| --- | --- | --- | --- |
| N4 | A server whose config fails to load, or whose shutdown hangs, exits; it never blocks forever (`ForceFatalExit` through the registered fatal handler; a teardown watchdog). | `src/core/nevr_config.h`, `src/core/nevr_config_policy.cpp`, `src/runtime/lifecycle/crash_recovery.cpp` | nothing |
| N36 | Guarded initialization (`InitializeAfterGameImageGuard`) runs under the loader lock, in the boot phase: it calls `nevr_boot_log_tee::Init()` before `InitializeFunctionPointers()`, and every return passes `nevr_boot_log_tee::Close()`. In the boot phase `Log()` reports through the boot tee (the sink `Init()` sets with `SetBootLogSink`), never the game's CLog, so code shared with later phases calls `Log()` and never branches on the phase. | `src/runtime/lifecycle/initialize.cpp`, `src/core/logging.cpp`, `src/runtime/log/boot_log_tee.cpp` | verify (the call order); `src/runtime/tests/test_boot_log_routing.cpp` (the routing); `tools/tests/test_patch_detour_logging.py` (the sink, the early return, no caller branching) |
| (unnamed) | `DllMain` never runs plugin shutdown or unload (it holds the loader lock). | `src/runtime/lifecycle/dllmain.cpp` | verify (sensor "Plugin detach teardown boundary", which names no rule) |
| N43 | The boot log tee (`nevr_boot_log_tee`) deliberately opens and writes its file during initialization under the loader lock, through kernel32 calls only (no CRT locks); it is the one documented exception to the addendum's no-file-I/O-from-DllMain rule. | `src/runtime/log/boot_log_tee.h`, `src/runtime/log/boot_log_tee.cpp`, `src/runtime/lifecycle/initialize.cpp` | nothing names it |
| N59 | Initialization calls `PatchDscProvider()` (the PSN/??? to DSC provider-string rewrite). | `src/runtime/lifecycle/initialize.cpp` | verify |
| N62 | The signal-delivered shutdown never takes the loader lock: its dependencies are resolved once at init (`ResolveShutdownDependencies`), nothing is resolved at shutdown, and the signal-context flag is `volatile sig_atomic_t`. | `src/runtime/lifecycle/crash_recovery.cpp`, `src/runtime/lifecycle/boot.cpp` | verify |
| N63 | A second signal during shutdown calls `ForceFatalExit`; it never sleeps and returns. | `src/runtime/lifecycle/crash_recovery.cpp` | verify |
| N38, N64, N105 | Graceful shutdown releases the bridge listener by a direct call to `StopWebSocketBridgeListener()` before `ForceFatalExit`, so the listening socket does not outlive the process as a zombie. | `src/runtime/server/gameserver.cpp`, `src/runtime/lifecycle/crash_recovery.cpp`, `src/runtime/lifecycle/crash_recovery.h`, `src/runtime/compat/ws_bridge.cpp` | verify |
| N67 | The crash flags `g_crashReporterSuppressed` and `g_justSuppressedCrash` are `std::atomic<bool>`: written from any thread, read from the faulting one. | `src/runtime/lifecycle/crash_recovery.cpp` (and the unbuilt `plugins/crash-handler/src/plugin.cpp`) | verify |
| N69 | The stack-overflow handler has room to run: `SetThreadStackGuarantee` at VEH install and `EnsureStackReserve()` on the thread that runs each frame. | `src/runtime/lifecycle/crash_recovery.cpp`, `src/runtime/frame/tick.cpp` | verify |
| N70 | The crash path (`BreakpointVEH`, `WriteCrashDump`) never calls `Log()` and never enumerates modules: it writes with `VehPrintf` (raw `WriteFile`) and reads the module table cached at init by `CacheModuleTable()`. | `src/runtime/lifecycle/crash_recovery.cpp` | verify |
| N71 | `BreakpointVEH` has a generic null-pointer access-violation branch and a table of at least 25 known session-flags null-dereference sites; those game functions are deliberately not hooked. | `src/runtime/lifecycle/crash_recovery.cpp`, `src/runtime/lifecycle/crash_recovery_sites.h` | verify; `src/runtime/tests/test_crash_recovery.cpp` |
| N82 | Only the vectored handler in `crash_recovery.cpp` acts on `EXCEPTION_BREAKPOINT`; every other one returns `EXCEPTION_CONTINUE_SEARCH` for it. | `src/runtime/lifecycle/crash_recovery.cpp` | `tools/verify_hook_invariants.py` |
| N87 | Under Wine, CTRL+C arrives as `CTRL_C_EVENT` at console control handlers, not at the CRT `SIGINT` handler; the runtime keeps its handler at the front of the chain and re-arms it after the game installs its own. | `src/runtime/lifecycle/crash_recovery.cpp`, `src/runtime/lifecycle/crash_recovery.h`, `src/runtime/server/gameserver.cpp` | `tools/tests/test_runtime_lifecycle_invariants.py`; `src/runtime/tests/test_crash_recovery.cpp` |
| N125 | The game-loop `setjmp` on `g_gameLoopJmpBuf` and its `longjmp` both live in `crash_recovery.cpp`; initialization calls `InstallGameMainHook()`. | `src/runtime/lifecycle/crash_recovery.cpp`, `src/runtime/patch/mode_patches.cpp`, `src/runtime/lifecycle/initialize.cpp` | verify |
| N146 | Before the original `PreprocessCommandLine` call only minimal state is set up (no modules, file I/O or bridge), because the client's D3D initialization runs inside that call. The one exception is the early plugin pass: `LoadPlugins(PluginPhase::Early)` runs once, before the original call, and loads only the `config.yaml` entries marked `early: true` (none marked: it loads nothing), so a plugin that serves game data from elsewhere is in place before the game reads its manifests and packages; the normal pass loads the rest after the graphics device exists, and a pointer from `get_plugin_info` stays valid across both passes (`g_plugins` has stable element addresses). The spectator-stream check inside `PreprocessCommandLine` is patched by `PatchSpectatorStreamCheck`, prologue-validated and logged; on a server it is written once, by `PatchEnableServer` in the deferred bootstrap after the first original call, never before that call (written before it, the check sets the spectator flow, in which the game begins multiplayer a second time, after the state machine's begin). `BugSplat64.dll` links at the stock image base `0x180000000`, so the game's graphics DLLs load. The matchmaker bridge port is chosen at run time and patched into `pnsradmatchmaking.dll`'s compiled-in host (`ApplyMatchmakingLoad`), never a fixed port. | `src/runtime/lifecycle/boot.cpp`, `src/runtime/patch/mode_patches.cpp`, `src/runtime/CMakeLists.txt`, `src/runtime/compat/ws_bridge.cpp`, `src/runtime/patch/pnsrad_enabler.cpp`, `src/runtime/ext/plugin_loader.cpp` | `tools/tests/test_spectatorstream_order.py`, `tools/tests/test_plugin_loader_storage.py` (the early-pass latch, the N146 text, the storage), `src/runtime/tests/test_behavioral.cpp` (`InfoPointerStaysValidWhilePluginsAreAddedWithoutAPlan`, `InfoPointerKeptInTheEarlyPassStaysValidAfterTheNormalPass`), `src/runtime/tests/test_checked_patch.cpp` (the spectator-stream patch); `tools/tests/test_pnsrad_load_boundary.py` (the host patch runs on every load, at the module's `LoadLibrary` return); nothing for the boot-state limit or the image base |
| N13 | CTRL+C from a terminal must reach the game's process group (Wine started with `WINEDEBUG=-all`, not behind a pipeline), and the signal and console handlers call `PerformGracefulShutdown` directly, never through a flag checked per frame (the frame loop stops before the flag is read, and the listening socket survives as a zombie; see N38). | the launch scripts, `src/runtime/lifecycle/crash_recovery.cpp` | nothing (manual run; noted in `src/runtime/tests/test_xpid_patch.cpp`) |

#### Server, bridge and login

| Rule | Statement | Guards | Checked by |
| --- | --- | --- | --- |
| N1 | `ParseEndpoint` never throws on malformed input: it parses with `strtoul`, range-checks to `UINT16_MAX` and logs a warning. | `src/runtime/server/messages.cpp` | `src/runtime/tests/test_parse_endpoint.cpp` |
| N2 | Packet-encoder settings from the server are validated (`ValidatePacketEncoderSettings`) before they are forwarded to the game. | `src/runtime/server/messages.cpp` | nothing names it |
| N14 | A `???-` platform prefix must not reach an XPID: the game caches EULA acceptance per platform prefix, so `???-` breaks the cache lookup (`XPID_PLATFORM_FALLBACK_PREFIX`). | `src/runtime/hook/addresses.h`, `src/runtime/patch/xpid_patch.cpp` | nothing names it |
| N20 | No account or Discord ID literal is compiled in. Identity comes from the presented credential, or from the `config.yaml` Discord-ID fallback, which applies in client mode too (not gated on `g_isServer`). | `src/runtime/compat/ws_bridge.cpp`; `src/runtime`, `src/modules`, `src/abi`, `src/core` for literals | verify |
| N34, N103 | `src/gameserver/` does not exist and must not come back; the server code is `src/runtime/server/`. | the tree | verify; `tools/verify_doc_paths.py` |
| N37, N39 | ixwebsocket cannot set `SO_REUSEADDR`, so the bridge listener picks a random ephemeral port and retries the bind. | `src/runtime/compat/ws_bridge.cpp` | verify |
| N51 | An access token read from disk is clamped to a short lifetime; a freshly issued token keeps its own `exp`. | `src/core/auth_token_model.h` | nothing names it |
| N54 | When the login connection disconnects, its callback is cleared, so nothing calls into a freed `ProxyPair`. | `src/runtime/compat/ws_bridge.cpp` | `src/runtime/tests/test_behavioral.cpp` |
| N60 | The bridge collects connections under `g_pairsMutex` and calls `stop()` outside it (`stop()` joins callback threads). | `src/runtime/compat/ws_bridge.cpp` | verify |
| N61 | Each matchmaker connection (index 2 and up) registers its own callback on the shared remote; closing the login pair clears only its own. | `src/runtime/compat/ws_bridge.cpp` | verify; `src/runtime/tests/test_behavioral.cpp` |
| N85 | Never hand ixwebsocket an empty callback (`setOnMessageCallback(nullptr)`); use a no-op lambda. Callbacks have an exception boundary. | `src/runtime/compat/ws_bridge.cpp` | verify |
| N91 | One login per login session, and a game socket's role is what it asked for, not when it connected. The socket that takes the login slot injects the LoginRequest; a further socket that asks for `/login` while a login session exists shares that session and injects nothing. The redirect gives the config and login services the request targets `/config` and `/login` on the bridge address; the Open handler reads the target of every socket (`openInfo.uri`), then the listener (the matchmaker listener makes a matchmaker), then the connect order over the slots no tagged socket is expected to take; a first frame that contradicts the role is logged. The login-injection line is logged at Info with the XPID (`docs/standards/logging.md` Rule 2). | `src/runtime/compat/ws_bridge.cpp`, `src/runtime/compat/socket_role.h`, `src/runtime/lifecycle/service_config.cpp`, `src/runtime/lifecycle/config.cpp` | verify (the Open handler admits by request target; the injection log line); `src/runtime/tests/test_socket_role.cpp`, `src/runtime/tests/test_behavioral.cpp` (`RolesByPathTest`, through the real bridge), `src/runtime/tests/test_service_config.cpp` (the redirect's request targets), `tools/tests/test_bridge_role_wiring.py` |
| N92 | The WebSocket bridge is compiled into `BugSplat64.dll` and started in-process from `boot.cpp`; no ws-bridge module is built or loaded. | `src/runtime/lifecycle/boot.cpp`, `CMakeLists.txt` | verify |
| N102 | A dedicated server that ServerDB refused, that has no token, or that cannot begin multiplayer (a changed prologue of `QueueGameThreadCall` or `BeginMultiplayerLobby`, checked once at boot by `ValidateServerBringup`; a net game not logged in when the queued begin runs) exits with a cause through `ServerFatal` (mode-gated), not `FatalError`. | `src/runtime/server/gameserver_callbacks.cpp`, `src/runtime/server/gameserver_serverdb.cpp`, `src/runtime/lifecycle/state_machine.cpp`, `src/runtime/lifecycle/server_bringup.h` | verify; `src/runtime/tests/test_server_bringup.cpp`, `tools/tests/test_server_bringup_wiring.py` |
| N106 | A server exchanges its refresh token for an access token (`HasValidRefreshToken()` then `RefreshAuthToken`); without it every registration falls back to password auth. | `src/runtime/server/gameserver_serverdb.cpp` | verify |
| N112 | The client login and the server registration carry the build identity (`nevr_identity`, from `nevr_build_identity::Get()`) and the plugin manifest (`nevr_plugins`, from `BuildPluginManifestJson()`). | `src/core/build_identity.h`, `src/core/build_identity.cpp`, `src/runtime/compat/login_profile.cpp`, `src/runtime/server/gameserver_callbacks.cpp`, `src/runtime/ext/plugin_loader.cpp` | verify (N112a-d); `src/runtime/tests/test_behavioral.cpp` |
| N115 | The login's `system_info` block is measured (`nevr_system_info::Get`), never a literal. | `src/runtime/compat/ws_bridge.cpp` | verify |
| N123 | The login display name is the token-auth username (`TokenAuth_GetUsername`), falling back to the account id; never a placeholder. | `src/runtime/compat/ws_bridge.cpp`, `src/runtime/compat/login_profile.cpp`, `src/modules/token-auth/src/token_auth.cpp` | verify |
| N131 | The asset CDN starts only on a client: in `boot.cpp` inside `if (!g_isServer)`, never in `initialize.cpp` (where `g_isServer` is not yet set). | `src/runtime/lifecycle/boot.cpp`, `src/runtime/lifecycle/initialize.cpp` | verify |
| N132 | `just proto` generates from one pinned BSR commit, matching nevr-stream's Go module revision (as `justfile` says); bump it only with the schema. | `justfile` (`proto`) | nothing |

#### Config and plugins

| Rule | Statement | Guards | Checked by |
| --- | --- | --- | --- |
| N68 | Plugins and modules tick every frame (`TickPlugins`, `TickModules`) and hear every state change (`NotifyModulesStateChange`). | `src/runtime/frame/tick.cpp`, `src/runtime/lifecycle/state_machine.cpp` | verify; `src/runtime/tests/test_behavioral.cpp` |
| N75 | Plugins load with a restricted DLL search path, so a DLL dropped next to `echovr.exe` cannot satisfy a dependency first. | `src/runtime/ext/plugin_loader.cpp` | verify |
| N89 | The loader refuses a plugin whose function is built in (`log_filter.dll`). | `src/runtime/ext/plugin_loader.cpp` | verify; `src/runtime/tests/test_behavioral.cpp` |
| N95 | `log-filter` is neither built nor packaged. | `plugins/CMakeLists.txt`, `CMakeLists.txt` | verify |
| N114 | The plugin capability channel stays wired: the loader resolves `NvrPluginGetCapabilities` and `NvrPluginInitEx`, and `NEVR_PLUGIN_API_VERSION` is 6. | `src/runtime/ext/plugin_loader.cpp`, `src/extension/plugin_interface.h` | verify |
| N135 | A plugin logs through the host: every context the loader passes a plugin (init, per-frame, state change) carries that plugin's `plugin_id` (its `config.yaml` entry's name) and the host's `log` function (`HostPluginLog`), whose line is `[NEVR.PLUGIN] plugin=<id> <message> <key>=<value> ...` through `Log()`; a plugin's text cannot start a second line or overrun the boot buffer (`plugin_log.h`); `plugin_logger.h` binds to it at init and falls back to stderr only before that or on a host older than v6. | `src/extension/plugin_interface.h`, `src/runtime/ext/plugin_loader.cpp`, `src/runtime/ext/plugin_log.h`, `plugins/common/include/plugin_logger.h` | `tools/tests/test_runtime_lifecycle_invariants.py`; `src/runtime/tests/test_behavioral.cpp` (`PluginLogApi`, `PluginLoaderDiagnosticTest`), `src/runtime/tests/test_plugin_log.cpp`, `src/runtime/tests/test_plugin_logger.cpp` |
| N120 | On a server a degraded runtime does not keep running: the HookGuard verdict is captured and escalated to `ServerFatal`, `platform_compat` fails a server run on a missing TLS hook, and the XPID patch logs both outcomes and escalates a validation failure, a failed byte write and a failed `GetProviderPrefix` (OVR-ORG) detour alike (`g_bootHookFailed` with a cause, so `boot.cpp` refuses the server), and the server bring-up prologues are validated at boot (`ValidatePrologue`, N97) with a mismatch escalated to `ServerFatal` (see N102). | `src/runtime/ext/plugin_loader.cpp`, `src/modules/platform-compat/src/platform_compat.cpp`, `src/runtime/patch/xpid_patch.cpp`, `src/runtime/lifecycle/state_machine.cpp`, `src/runtime/lifecycle/boot.cpp` | verify; `src/runtime/tests/test_behavioral.cpp` (`XpidPatchBoot`, `XpidPrefixDetourBoot`), `src/runtime/tests/test_server_bringup.cpp`, `tools/tests/test_server_bringup_wiring.py` |
| N133 | Every nEVR config key is read from `config.yaml` (`nevr_config`), never from the game's JSON; the flat key map has 33 entries; modules read config through `NvrModuleContext.config_get`, and a module reports its API version; the sample config has the `auth`, `services`, `identity` and `version` sections. | `src/runtime/lifecycle/config.cpp`, `src/runtime/lifecycle/service_config.cpp`, `src/core/nevr_config.h`, `src/modules/token-auth/src/token_auth.cpp`, `docs/reference/example-config.yaml` | verify (N133 S7a-d); `src/runtime/tests/test_behavioral.cpp` |
| N134 | The plugin set is the ordered `plugins:` list in `config.yaml`, and a required plugin that fails to load is a failure (S6); plugins get `ctx_size` and the plugin-query API, and load in capability-priority order (S8). | `src/runtime/ext/plugin_loader.cpp`, `src/runtime/ext/plugin_loader.h`, `src/runtime/ext/plugin_load_plan.h`, `src/runtime/lifecycle/service_config.cpp` | verify (N134 S8, N114 S7); `src/runtime/tests/test_behavioral.cpp`, `src/runtime/tests/test_plugin_load_plan.cpp` |
| N147 | A release build loads only plugins signed through the release-signer pipeline: `StageOne` verifies the file (`VerifyPluginFile`: Authenticode digest, `messageDigest` bound to the signed content, chain to the pinned Microsoft root through an exclusive-root engine at the stamped time, whole-OID profile EKU, bound RFC 3161 token, denylist) on a handle held `FILE_SHARE_READ` across the load, with no default-search fallback; a plugin's imports, delay-load imports and forwarded exports pass the same check transitively (`VerifyPluginTree`, held open across the load; a name gets `.dll` when it has no extension, and one that is neither a file in the plugins directory nor supplied by the system is refused, an absent delay-load excepted: the late-load filter checks it when it is called), and so does every later `LoadLibrary*` of a file in that directory, one made from a staged plugin's `DllMain` included (a `DllLoadHook` filter: the name gets `.dll` when it has no extension, a relative path with a directory part (`\` or `/`) is refused whatever it leads to (the loader appends it to every directory it searches, each through links, so no candidate list is complete; a bare name and a full path keep their rules), the directory is the ANSI path converted through the code page; refused with `ERROR_MOD_NOT_FOUND`); a refusal is a manifest entry, a log line and, on a client with graphics only, one dialog per run (`plugin_refusal_notice.h`, never on a server or headless run; a refusal after `LoadPlugins` finished shows it itself), and for a required plugin `ServerFatal`. The dev build (`mingw-plugin-dev`, `NEVR_ALLOW_UNSIGNED_PLUGINS`) carries `NEVR-PLUGIN-TRUST unsigned-allowed` and `tools/package_release.py` refuses to package it. The profile EKU pin is confirmed against the real signed v5.4.3 `BugSplat64.dll` (`RealSigned.*` in `test_plugin_trust`, which `just test-auth-unit` feeds from `tools/fetch_real_signed_dll.py`); the signer subject is deliberately not pinned. A signed-only build refuses an unsigned plugin. | `src/runtime/ext/plugin_signature.cpp`, `src/runtime/ext/plugin_trust.cpp`, `src/runtime/ext/plugin_loader.cpp`, `src/runtime/ext/plugin_tree.cpp`, `src/runtime/ext/plugin_refusal_notice.h`, `src/runtime/hook/dll_load_hook.cpp`, `tools/package_release.py` | `src/runtime/tests/test_plugin_signature.cpp`, `src/runtime/tests/test_plugin_trust.cpp`, `src/runtime/tests/test_plugin_tree.cpp`, `src/runtime/tests/test_behavioral.cpp` (`PluginSignatureGateTest`, `PluginDependencyGateTest`, `PluginRefusalNotice`), `tools/tests/test_package_release.py`, `tools/tests/test_build_identity_literal.py` (the late-load spellings, the non-ASCII game path, the `DllMain` load and the late notice) |

#### Hooks and binary patches

| Rule | Statement | Guards | Checked by |
| --- | --- | --- | --- |
| N6, N65 | Headless mode patches the game's own gates (the D3D12 init gate is forced to the game's graphics-disabled branch, prologue-validated). `HEADLESS_GATE_TABLE` is the single list of gates; the install loop reads it. | `src/runtime/hook/addresses.h`, `src/runtime/patch/mode_patches.cpp` | `src/runtime/tests/test_xpid_patch.cpp`, `src/runtime/tests/test_behavioral.cpp`; `tools/verify_mode_patch_ground_truth.py` |
| N7, N8, N9, N10 | N8 and N9 are two of the headless gates (`HEADLESS_RENDER_SUBMIT_INIT`, `HEADLESS_RENDER_SETUP`); `Finished initializing engine` is the witness that a headless boot completed. | `src/runtime/hook/addresses.h` | `src/runtime/tests/test_xpid_patch.cpp`; N77 keeps the witness line |
| N22 | The peer-timeout bug in the game (unsigned subtraction of a wrapped timestamp) is fixed by the overflow-safe `GetTimeMilliseconds` replacement. | `src/runtime/hook/addresses.h`, `src/runtime/patch/binary_bug_fixes.cpp` | nothing names it |
| N25, N28 | A hook in `binary_bug_fixes.cpp` validates the expected bytes before it hooks. | `src/runtime/patch/binary_bug_fixes.cpp` | nothing names it (the Guardrails rule) |
| N26 | The `CPrecisionSleep::Wait` hook is that function's only owner; nothing chains on it. | `src/runtime/patch/binary_bug_fixes.cpp` | nothing names it |
| N31 | A value read from a fixed game global (the spin-wait limit, the server's step slot) is bounds-checked, to catch a misread after the binary changes version; the server tick rate is range-checked before the step is written, and the write is read back. | `src/runtime/patch/binary_bug_fixes.cpp`, `src/runtime/lifecycle/config.cpp`, `src/runtime/lifecycle/server_timestep.h` | `src/runtime/tests/test_server_timestep.cpp` (the step slot; the spin-wait limit has none) |
| N32 | A caller count in `addresses.h` is a dated snapshot with the command that refreshes it; refresh it before relying on it. | `src/runtime/hook/addresses.h` | nothing names it |
| N33, N113 | Exactly one writer patches `CPrecisionSleep::BusyWait`; it validates the prologue, saves the original byte and restores it at shutdown. | `src/runtime/patch/binary_bug_fixes.cpp` | verify |
| N66 | `FormatSymbolId` returns 0 for a non-positive length or a null buffer. | `src/runtime/hook/symbol_corpus.cpp`, `tools/gen_symbol_corpus.py` | `src/runtime/tests/test_behavioral.cpp`; `tools/tests/test_symbol_corpus_template.py` |
| N83 | A hooked address is named for what the binary says it is (`0x140F80ED0` is `CBroadcaster::Listen`); an address called through a live function pointer is not also detoured (self-collision); the bytes at each hooked address match the identity manifest. | `src/runtime/patch/mode_patches.cpp`, `src/runtime/hook/addresses.h`, `tools/hook_identity_manifest.json` | `tools/verify_hook_invariants.py` |
| N84 | `PatchDetour` records each hooked address with HookGuard and its `name` parameter is required; the loader re-verifies hooked addresses after each plugin's init (`VerifyAll`). | `src/runtime/hook/patching.h`, `src/runtime/hook/hook_guard.h`, `src/runtime/ext/plugin_loader.cpp` | verify (N84, C2); `tools/verify_hook_invariants.py`; `src/runtime/tests/test_behavioral.cpp` |
| N86, N110 | Per-frame work runs through exactly one dispatcher, `DispatchPerFrameWork`, called from a hook that is live in server mode, with a re-entrancy gate and the host flag taken from `g_isServer`; it holds the only two production `Tick*` call sites. Every declared HookLiveness id is marked somewhere. | `src/runtime/frame/tick.cpp`, `src/runtime/patch/binary_bug_fixes.cpp`, `src/runtime/hook/hook_liveness.h` | verify |
| N96 | No symbol is spelled plain `ResolveVA`; use `ResolveVA_Checked` or `ResolveVA_Unchecked`. | `plugins/common/include/nevr_common.h`, `src` | verify |
| N97 | There is exactly one `ValidatePrologue`, in `nevr_common.h`. | `plugins/common/include/nevr_common.h` | verify |
| N99 | `-server` applies the game's own headless mask (`ENGINE_FLAGS_HEADLESS_MASK`, `0xFFFEFEFE`), and boot never tells the operator a flag is redundant. | `src/runtime/patch/mode_patches.cpp`, `src/runtime/lifecycle/boot.cpp` | verify |
| N126 | `PatchDetour` logs a failed hook at Warning; a failed hook is never silent. | `src/runtime/hook/patching.h` | verify |
| N127 | The Oculus-SDK block reports its real result, never an unconditional success line. | `src/runtime/patch/mode_patches.cpp` | verify; `tools/winvm/checks.py` |
| N128 | `Hooking::Attach` records which MinHook stage failed and its `MH_STATUS`; `PatchDetour`'s failure log includes it. | `src/core/hooking.h`, `src/runtime/hook/patching.h` | verify |
| N129 | `DllLoadHook` reports the `MH_STATUS` of each `LoadLibrary` hook that fails, with no accumulate-and-hide `ok &=`. | `src/runtime/hook/dll_load_hook.cpp` | verify |
| N72, N73 | Guards for the broadcaster plugin, which lives in `nevr-runtime-plugins` (see "Plugins"). | not in this tree | nothing here |

#### Logging and layout

| Rule | Statement | Guards | Checked by |
| --- | --- | --- | --- |
| N17 | A component that installs several hooks reports each outcome by name, plus one aggregate line at Info. | `src/modules/platform-compat/src/platform_compat.cpp`, `src/runtime/patch/pnsrad_enabler.cpp` | nothing names it |
| N18 | The built-in filter is validated by a measured suppression ratio (`docs/standards/logging.md` Rule 4); see N79. | `src/runtime/log/builtin_filter.cpp` | verify (through N79) |
| N45, N81 | No bare `[NEVR]` tag on a log line; use a subsystem tag from `docs/standards/logging.md` (`--help` strings in `cli.cpp` are not log lines). | `src/runtime`, `src/abi`, `src/core` | verify |
| N47, N94 | The `[NEVR.AUTH]` per-step detail lines are Debug, pinned by their text, and the level mapping that carries them is pinned too. | `src/modules/token-auth/src/token_auth.cpp`, `src/core/device_auth_flow.cpp`, `plugins/common/include/auth_token_refresh.h` | verify |
| N77 | nEVR's own lines are exempt from game-noise suppression, and `Finished initializing engine` and `ExitProcess(` are never on the blanket suppress list. | `src/runtime/log/builtin_filter.cpp` | verify |
| N78 | A frequent line is collapsed into a counted summary, never deleted; there is exactly one strong definition of `::Log`. | `src/runtime/log/builtin_filter.cpp`, `src/core/logging.cpp` | verify (N78, D1) |
| N79 | Filter health is emitted periodically while the process runs, not only at shutdown. | `src/runtime/log/builtin_filter.cpp`, `src/runtime/frame/tick.cpp` | verify |
| N88 | A monitor does not depend on the thing it monitors: filter health is driven from the per-frame tick, not only from the hook it watches (`docs/standards/verification.md`, "A Monitor Must Not Depend On What It Monitors"). | `src/runtime/log/builtin_filter.cpp`, `src/runtime/frame/tick.cpp` | verify (through N79) |
| N80 | Both log writers stamp the same per-process run ID. | `src/core/logging.cpp`, `src/runtime/log/boot_log_tee.cpp` | verify |
| N90 | pnsrad.dll's own `CLog::PrintfImpl` is hooked, so its output goes through the filter; the hook is installed from the live tick. | `src/runtime/log/builtin_filter.h`, `src/runtime/log/builtin_filter.cpp`, `src/runtime/frame/tick.cpp` | verify |
| N108 | `src/abi/` never includes `src/core/`. | `src/abi` | verify |
| N109 | `src/runtime/CMakeLists.txt` does not put its own source directory on the include path. | `src/runtime/CMakeLists.txt` | verify |
| N122, N124 | A config or mode decision logs its outcome at Info even when nothing happens (UPnP key and mapping, telemetry disabled, the server bring-up prologue check). | `src/runtime/lifecycle/config.cpp`, `src/runtime/server/upnp.cpp`, `src/runtime/server/gameserver_serverdb.cpp`, `src/runtime/server/gameserver_telemetry.cpp`, `src/runtime/lifecycle/state_machine.cpp` | nothing in verify; a run scored by `tests/smoke/score-log.sh` |

#### Writing a sensor (N93, N99, N101, N111)

A sensor in `just verify` must fail when the thing it watches is gone, and only then:

- **N93.** Run stage 1 alone and capture its exit code; `sensor_stage1` aborts on 2 or more (could not read the
  subject). Check an extracted body with `sensor_nonempty`: an empty extraction is a sensor watching nothing. Do not
  add new single-stage `if grep -q P subject; then FAIL` sensors.
- **N101.** Feed a capture to an early-exit reader with a herestring (`grep -qF "$pat" <<<"$CAPTURE"`), never a
  pipeline: under `pipefail` the SIGPIPE makes a correct tree fail.
- **N99, N111.** Strip comments before matching, so the prose that explains a rule cannot satisfy its check, and use
  the stripper spelling in the recipe (`^[[:space:]]*(//|/\*|\*[[:space:]/]|\*$)`); the obvious `\*` form eats pointer
  dereferences.

The recipe also has sensors named by issue (#21, #41, #43, #361, #451) and by letter (C2, D1); read it before changing
a file it names.

## Branch lifecycle (every agent-created branch)

**Whoever creates a branch deletes it.** Nobody else cleans up after you.

Every branch an agent creates has a documented deletion point from the moment it exists. A merged branch is scratch: the PR keeps the commits, so never keep one "for reference".

- **One branch per PR.** Name it `<seat>/<issue>-<slug>`.
- **Create:** `git worktree add --no-track -b <branch> .claude/worktrees/<branch> origin/main`. `--no-track` stops the branch inheriting `origin/main` as its upstream, which sends a bare push to main.
- **Declare the end:** `git worktree lock --reason "<seat>: delete when PR #<n> merges or closes" .claude/worktrees/<branch>`, and the PR body carries the line `Branch lifecycle: deleted when this PR merges or closes.`
- **Push only by explicit ref:** `git push origin HEAD:refs/heads/<branch>`.
- **On merge:** the seat that created the branch removes its worktree and deletes the local and origin branch in the same turn it sees the merge. nevr-merge's `tools/reap_merged.py` is a backstop, not the plan.
- **On close without merge:** same, the creator, same turn.
- **No PR within 24 hours:** open one or delete the branch.
- **"Merged" means** the PR state is MERGED (GitHub squash-merges, so `git branch -d` refuses those branches) or the tip is in `origin/main`. Use `gh pr view <branch> --json state`.
- **A branch that lands with no PR** (a fast-forward or a hand merge) counts as merged when `git merge-base --is-ancestor <branch> origin/main` succeeds, and its creator deletes it then.
- **Before deleting an unmerged branch,** print its tip SHA, and check `git -C <worktree> status --porcelain` is empty. Uncommitted work is work.
- **Remove a worktree only behind the cwd check** (the "no process has its cwd inside" proof of "Ownership and cleanup"; `tools/reap_merged.py` runs the same check), in one command that removes nothing and fails when any process has its cwd inside, a shell of your own session included:
  `W=$(realpath .claude/worktrees/<branch>); hits=$(for p in /proc/[0-9]*; do c=$(readlink "$p/cwd" 2>/dev/null) && case "$c" in "$W"|"$W"/*) echo "${p#/proc/} $c";; esac; done); if [ -n "$hits" ]; then echo "$hits"; false; else git worktree unlock "$W" 2>/dev/null; git worktree remove "$W"; fi`.
  A check that only prints, followed by a removal, is not the check.

## Build Commands

```sh
just                    # Show available recipes
just configure          # Configure CMake only
just build              # Build all components
just dist               # Build + create the stripped distribution packages
just verbose-build      # Build with full compiler output
just clean              # Remove build/ and dist/
just preset=mingw-debug build  # Use a specific preset
just proto                     # Regenerate protobuf from BSR (requires buf CLI)
just worktree-setup            # Make a fresh git worktree buildable (copies extern/ and gen/ from the main checkout)
just install-hooks             # Once per clone: link the tracked post-checkout hook so a new linked worktree gets the files .worktreeinclude lists (.mcp.json, .claude/settings.local.json)
just release-preflight           # Refuse unless this clone is exactly origin/main: no local-only tag, no uncommitted or untracked change, nothing unpushed, not behind (read-only; run before any release step)
just package-dev                 # LOCAL dev package from a clean tree: Windows zip + Quest APK in build/package-dev/<sha>/, stamped -dev (a release is only a CI build of a v<x.y.z> tag; never tags or publishes); the Quest APK is signed with the key stored in pass by `just import-quest-keystore`
just reap-merged         # Dry run: worktrees whose work has landed and the proofs each passes or fails; add --apply to remove them (skill: merge-cleanup)
just sign               # Code-sign all DLLs/EXEs in dist/ (requires certs/)
just generate-certs     # Generate CA hierarchy for code signing
```

Releases are built by CI from a tag, never locally. The steps, in order, from a clone of origin:

```sh
just release-preflight                                    # 1. refuses (exit 1, one line per problem) unless this clone is exactly origin/main
git tag -a v<x.y.z> -m "<x.y.z>" && git push origin v<x.y.z>   # 2. tag origin's tip and push the tag
just release-preflight && gh release create v<x.y.z> --prerelease --verify-tag   # 3. once more, then publish the release (not a draft)
```

A release is the tag's version plus GitHub's pre-release flag. The version is exactly the tag (`v5.0.0` is `5.0.0`); every other build
stamps `X.Y.(Z+1)-dev.<N>+<sha>`, which sorts after the tag it descends from and before the next release. `build.yml` fires on
`published`, goes through one packaging path (the sealed zip, `SHA256SUMS`, `RELEASE-NOTES.md`, all attested, plus the `dist/`
zip; the `.tar.zst` archives stay a workflow artifact) and does nothing for a release that already carries those assets. Nothing in a file depends on the pre-release flag:
promote with `gh release edit v<x.y.z> --prerelease=false`; it fires no workflow and changes no byte. `BugSplat64.dll` carries one
identity literal, `NEVR-BUILD <version> <commit40>` (NUL-bounded, exactly one), that binds its version and its commit.
The `dist` package and the Windows zip's DLL are both the stripped build (players get stripped builds only); `just dist` splits each shipped PE's debug info into `dist/symbols/<name>/<file>.dbg` first and links the
stripped file to it (`.gnu_debuglink`: name and CRC32; PE has no build-id). `build.yml` checks the pair with `tools/verify_symbols.py`,
keeps the `.dbg` files as the `symbols` workflow artifact (not a release asset) and attests them. A build with symbols is the
ordinary `build/<preset>/bin/` output.

A release whose files must be signed before anything is public goes through a DRAFT: a draft fires no workflow, so the build is
started by hand on the tag ref.

```sh
just release-preflight && gh release create v<x.y.z> --draft --prerelease --verify-tag   # D1. a draft for the pushed tag (instead of step 3)
gh workflow run build.yml --ref v<x.y.z>          # D2. builds, attests and attaches the four files to the draft; it stays a draft
```

`tools/release_draft.sh` decides: a tag with exactly one draft, no published release, and none of the four names on the draft is
built and attached; no release, or a published one, is a dry run (nothing is uploaded); a draft that already carries any of the
four names fails the run red, so a second dispatch never overwrites signed files. Publishing the filled draft fires `published`,
which `tools/release_already_built.sh` turns into a green no-op (both read `tools/release_asset_names.sh`). The `drafts` job
(a dispatch on a tag only, no checkout, one `gh api` call) holds `contents: write` because a read-only token cannot list drafts;
`guard` keeps `contents: read` and gets the listing as JSON on stdin of `tools/release_draft.sh`. Hand-publishing the draft while
a dispatch is running makes that run upload to the published release.

`just release-preflight` also runs first inside `just package-dev`. It belongs before pushing a release tag (step 1) and before
`gh release create` (step 3, or D1): the manual gate. It is not in `build.yml` (CI checks out the tag itself) and not in `just verify`
(it must work in a dirty tree).

Build presets: `mingw-debug`, `mingw-release` (Linux default), `linux-wine-debug`, `linux-wine-release`, `debug`, `release` (Windows default).

Build output lands in `build/<preset>/bin/`.

The `justfile` puts `ccache` in front of the compiler when it is installed, so every worktree shares one cache
(`tools/ccache.conf`: 10 GB cap; the cache directory is `/mnt/games/cache/ccache` when that exists, else
`~/.cache/ccache`). CMake reads the launcher on a build tree's first configure, so a tree configured before
ccache keeps building without it. `NEVR_CCACHE= just build` configures a tree without it. The Quest build never
uses it: its objects embed the worktree path.

## Testing

Go-based system tests in `tests/system/` and `tests/plugins/`:

```sh
just test-system              # All system tests
just test-system-short        # Quick tests only
just test-system-dll          # DLL loading tests only
just test-system-verbose      # No cache, verbose
cd tests/system && go test -v -run TestName ./...  # Single test

just test-plugins-groundtruth # Plugin ground truth tests (no game binary needed)
just test-plugins             # All plugin tests (needs game binary + MCP harness)
just test-plugins-short       # Ground truth only, skip integration

just test-auth                # All auth tests (ground truth + unit)
just test-auth-groundtruth    # Auth ground truth (no game binary, no network)
just test-auth-unit           # C++ GTest under Wine (build with -DBUILD_TESTING=ON)
just test-auth-integration    # Auth integration (needs game binary + MCP harness)

just scenario invite          # One social scenario end to end, unattended (docs/design/2026-10-01-social-scenario-harness.md)
just scenario-all             # Every social scenario in turn, one PASS/FAIL table (tools/scenario/run_all.py)
just test-winvm               # Built runtime on a native Windows VM (needs WINVM_USER/WINVM_PASS; tools/winvm/README.md)
```

Tests require: Echo VR game binary, Go toolchain. Environment variables: `NEVR_BUILD_DIR` (build output), `EVR_GAME_DIR` (the `echovr/` directory of a copy of the install, never the owner's install; see "The game install").

## Startup timing

The game has a ~15-20 second splash-screen delay at startup before any nEVR code runs. The first nEVR log lines appear well after `echovr.exe` process creation. When judging server liveness:

- **Minimum patience window: 45 seconds from process start** before concluding the server is hung.
- The splash screen runs BEFORE `DllMain` / `WinMain` — nEVR has no control over this phase.
- Startup timeout checks must account for this delay. A server that hasn't logged anything at t=10s is normal; a server with no output at t=60s is dead.
- The `-no-console` flag suppresses the splash UI but NOT the delay — the game still runs its startup sequence.

## Architecture

### DLL Components

| Component          | Output DLL       | Deploy As              | Purpose                                                                  |
| ------------------ | ---------------- | ---------------------- | ------------------------------------------------------------------------ |
| `src/runtime/` | `BugSplat64.dll` | `BugSplat64.dll`       | Runtime hooks, CLI flags, game modifications                             |
| `src/runtime/server/` | *(in `BugSplat64.dll`)* | *(in-process)* | Multiplayer networking, session management |

The runtime replaces the original BugSplat64 crash reporter DLL — the game statically imports it, so it loads at process startup before WinMain. `tools/verify_pe_contract.py` pins its export table (26 names and ordinals, `DetoursExportPlaceholder` at ordinal 1, no import of `nevr.dll`) and, with the game binary present, the nine imports of `echovr.exe`. `docs/adr/0008-bugsplat-host-and-nevr-runtime.md` records the stable-host/`nevr.dll` design, what exists of it and the rendezvous decision (the game's main wrapper). Several features are built in rather than loaded as plugins: server-timing, token-auth, pnsrad-enabler.

`src/runtime/` is split by responsibility. Each subdirectory has a membership test:

| Directory | Holds | Membership test |
| --- | --- | --- |
| `src/runtime/lifecycle/` | dllmain, initialize, boot, cli, config, state_machine, crash_recovery | the process's life from `DllMain` to exit |
| `src/runtime/hook/` | patching.h, addresses.h, process_memory.h, hook_guard, hook_liveness, dll_load_hook, symbol_corpus | *how* we attach to the binary at all |
| `src/runtime/patch/` | mode_patches, headless_graphics, xpid_patch, pnsrad_enabler, resource_override, asset_cdn, binary_bug_fixes, broadcaster_guard | behaviour we change *in the game* |
| `src/runtime/server/` | gameserver, server_context, websocket_client, telemetry_*, upnp, messages | the ServerDB / IServerLib subsystem |
| `src/runtime/compat/` | ws_bridge | making the game's ageing network stack work against modern services |
| `src/runtime/host/` | host_entry, runtime_rendezvous, game_targets, loader_lock, bootstrap_diagnostics, crash_capture, runtime_load, host_json, host_paths, path_buffer | what the stable `BugSplat64.dll` host runs behind its `DllMain` and exports; compiled only into the test harnesses until the target split (ADR 0008) |
| `src/runtime/ext/` | plugin_loader, module_loader | loading other people's DLLs |
| `src/runtime/log/` | boot_log_tee, builtin_filter | log capture and filtering |
| `src/runtime/link/` | dbghelp_stubs.cpp, bcrypt_minimal.def | not code we run — code the *linker* needs |
| `src/runtime/scenario/` | scenario_control (test builds only), scenario_protocol | lets a test drive the running game; compiled in only by the `mingw-scenario` preset, never shipped |

**Includes are path-qualified from `src/`** (`#include "runtime/hook/addresses.h"`).
`src/runtime/CMakeLists.txt` deliberately does NOT put its own directory on the
include path — that flattening is what let 44 files pile into one directory and
include each other by bare basename. Re-adding it silently undoes this.

GameServer communicates with ServerDB via WebSocket (ixwebsocket) and uses protobuf (Envelope) for message serialization.

### Plugins

Optional DLLs loaded by the runtime from a `plugins/` subdirectory next to the game binary. Each plugin implements the `NvrPluginInterface` lifecycle (see `src/extension/plugin_interface.h`). Source lives in `plugins/<name>/`.

| Plugin               | Output DLL               | Purpose                                              |
| -------------------- | ------------------------ | ---------------------------------------------------- |
| `log-filter`         | `log_filter.dll`         | Structured log filtering, suppression, file rotation |
| `example`            | `example.dll`            | Reference implementation for new plugin authors      |

`broadcaster-bridge` lives in `nevr-runtime-plugins`, not here — this repo is
public and it is a broadcaster injection tool. `anim-debugger` lives there for the
same reason: it is RE instrumentation that hooks three engine
animation entry points and publishes a map of animation internals, and it does
nothing on a dedicated server. `log_filter.dll` is superseded by the built-in
filter and the loader refuses to load it. Other plugins (audio-intercom,
game-rules-override, session-unlocker, combat-mod, combat-2d) live in
`nevr-runtime-plugins`.

Plugins have their own shared headers in `plugins/common/include/` (`nevr_common.h`, `address_registry.h`, `yaml_config.h`, `plugin_logger.h`) providing address resolution, prologue validation, config loading and logging utilities.

A plugin logs through the host's structured `Log()` (N135): `NEVR_DEFINE_PLUGIN_LOG` in `plugin_logger.h` generates `PluginLogBind(ctx)`, `PluginLog`/`PluginLogWarning`/`PluginLogError`/`PluginLogDebug` and `PluginLogFields(level, message, {{key, value}})`. Call `PluginLogBind(ctx)` first thing in `NvrPluginInit` or `NvrPluginInitEx`; the host log exists from the init callback on (not in `DllMain`, not before init). Lines land in the host's log as `[NEVR.PLUGIN] plugin=<name> ...` (rules: `docs/standards/logging.md`, Rule 15). Per-plugin log files and rate limiting are not provided.

### Runtime-loaded modules

`platform_compat` and `token_auth` are **statically linked** into `BugSplat64.dll`. There are no separate module DLLs — everything ships in one file. The `module_loader` infrastructure (`RegisterStaticModule`, `TickModules`, `NotifyModulesStateChange`) remains for any future modules.

| Module | Output | Purpose |
| ------ | ------ | ------- |
| `src/modules/platform-compat/` | *(in `BugSplat64.dll`)* | Schannel TLS modernisation, MSXML6 pass-through hook, Wine `_temp` fix |
| `src/modules/token-auth/` | *(in `BugSplat64.dll`)* | Device-code auth, token cache, `TokenAuth_GetToken`/`GetDiscordId` |

### Shared Libraries (static)

Split by **what the knowledge is**, not by who uses it. A single directory named
"common" holding all three is the classic junk drawer.

- **`src/abi/`** → `libnevr_abi.a` — the echovr.exe ABI surface: game types
  (`echovr.h`), the function pointers we call through (`echovr_functions.cpp`),
  symbol IDs (`symbols.h`), CSymbol64 hashing (`symbol_hash.h`). Membership test:
  *the binary told us this*. If a fact here is wrong, the reconstruction is wrong.
- **`src/core/`** → `libnevr_core.a` — nEVR's own primitives: logging, CLI-flag
  globals, base64, the hooking abstraction, the auth-token model, `pch.h`.
  Membership test: *we wrote this*. Links `nevr_abi` PUBLIC.
- **`src/extension/`** — header-only published C ABI for third-party DLLs
  (`plugin_interface.h`, `module_interface.h`). Membership test: *someone else
  compiles against this*, so changing it is a breaking change.
- **`src/nevr_api/`** → protobuf target, generated into `gen/cpp/` (see `just proto`)

**The dependency runs `core` → `abi`, never the reverse** (`EchoVR::LogLevel` is a
game enum, so logging genuinely sits on the ABI layer). `just verify` enforces it.

Headers are included **path-qualified** — `#include "abi/echovr.h"`, not
`#include "echovr.h"`. `src/` is on the global include path (root
`CMakeLists.txt`), so the spelling states which layer a dependency crosses.

- **`src/legacy-compat/`** — two forwarding headers, existing solely because
  `src/legacy/gamepatches` is frozen yet includes `common/hooking.h` and
  `common/nevr_plugin_interface.h`, which these headers provide from
  `src/legacy-compat/common/`.
  Scoped to that
  one target. Delete with `src/legacy/`.

### Key Source Files

- `src/runtime/lifecycle/dllmain.cpp` — DLL entry point
- `src/runtime/lifecycle/initialize.cpp` — Initialization sequence after DLL load
- `src/runtime/lifecycle/cli.cpp` — CLI flag parsing and processing
- `src/runtime/lifecycle/boot.cpp` — Game boot sequence hooks
- `src/runtime/patch/mode_patches.cpp` — Server/headless/client mode patches
- `src/runtime/ext/plugin_loader.h` — Plugin discovery and lifecycle management
- `src/runtime/hook/addresses.h` — Virtual addresses for game function hooks
- `src/runtime/server/gameserver.cpp` — IServerLib vtable implementation
- `src/runtime/server/messages.h` — Protocol message symbol IDs (uint64)
- `src/core/globals.h` — Cross-DLL globals (`isServer`, `isHeadless`, `exitOnError`, etc.)
- `src/core/logging.h` — `Log(level, format, ...)` and `FatalError()`
- `plugins/common/include/address_registry.h` — Verified virtual addresses for all plugin hooks

### Other Components

- **`src/launcher/`** — `echovr_server.exe`, a thin `CreateProcess` wrapper that
  spawns `echovr.exe -server -headless -no-console` (built; root
  `CMakeLists.txt`, `just launcher`).
  The older PE-conversion launcher is gone — Wine could not load the game DLL at
  the required base address.
- Android/Quest standalone target lives in `src/quest/` (separate CMake project).
- **`src/legacy/`** — Frozen v1 implementations (self-contained, do not modify)

## Conventions

- **Logging**: Always use `Log(EchoVR::LogLevel::Info, "format %d", val)` from `common/logging.h`. Fatal errors via `FatalError(msg, title)`. A plugin uses `plugin_logger.h`, which routes to the host's `Log()` (N135).
- **Hooking**: MinHook-based (`NEVR_USE_MINHOOK` compile flag). Functions use `__fastcall` convention. Use `ListenForBroadcasterMessage()` for game event callbacks.
- **Protocol messages**: Symbol IDs in `src/runtime/server/messages.h`. Serialize via protobuf `rtapi::v1::Envelope`.
- **Protobuf**: Generated from BSR (`buf.build/echotools/nevr-api`) via `just proto`. Never edit `.pb.cc`/`.pb.h` in `gen/` directly.
- **Global state**: CLI flags as globals in `src/core/globals.h`, set in `src/runtime/lifecycle/cli.cpp`.
- **Naming**: spellings of the project name, log tags, exports, namespaces, config keys and the frozen `Nvr*` plugin ABI are in `docs/standards/naming.md`; `tools/tests/test_naming.py` enforces the `Nvr` freeze and the project-name spellings.
- **Local overrides**: `cmake/local.cmake` (include currently commented out in root CMakeLists.txt).

## ReVault — Reverse Engineering Data Warehouse

> **Maintainers only.** ReVault is private. Contributors: see "For contributors", "Maintainers only, and what to do instead".

ReVault is the single source of truth for binary analysis. It indexes every binary of the game with disassembly, decompilation, xrefs, strings, and annotations. **Use it first, before Ghidra, before guessing.**

**Search the project, never one binary.** The echovr project is the whole game: `echovr.exe`, `pnsrad.dll`, `pnsradmatchmaking.dll`, `pnsovr.dll`, every script DLL in `bin/win10/scripts/` (about 567; most gameplay and UI logic lives there, not in the exe), and the Quest side (`libr15.so`, `libpnsrad*.so`, the Quest script libraries). A search on `echovr.exe` that finds nothing says nothing about the game.

1. `revault_projects`, then `revault_binaries(project)` for the full binary list.
2. Run `revault_search_code` / `revault_search_strings` on **every** binary in the list. Fan it out across parallel subagents, a batch of binaries each; no binary is skipped.
3. Report it as `searched N of N binaries in project <name>: hits in <binaries>`. A negative without "N of N" is not a finding.
4. A hit in a script DLL is the game's own logic: read it (`revault_function`) before concluding anything.

Available as an MCP server (`revault` in `.mcp.json`) and CLI. The CLI takes one `--binary` per call, so a search (`fn search`, `search code`) is repeated for every binary in the project; the commands that act on an address (`fn show`, `fn callers`, `fn callees`, `xref to`, `rename`) belong to the one binary the address is in:

```sh
revault fn show <0xVA> --binary pnsrad.dll    # Decompilation + callers + callees + xrefs
revault fn callers <0xVA> --binary pnsrad.dll # Who calls this function
revault fn callees <0xVA> --binary pnsrad.dll # What does this function call
revault xref to <0xVA> --binary pnsrad.dll    # Cross-references to address
revault rename <0xVA> <new-name> --binary pnsrad.dll  # Annotate
revault fn search <pattern> --binary <binary>      # Search function names + source: once per binary in the project
revault search code <pattern> --binary <binary>    # Search decompiled source: once per binary in the project
```

When you encounter an unknown function address (`fcn_*`, `DAT_*`, `0x180XXXXXX`), **look it up in revault**. If revault doesn't have it, say so — don't guess.

## Continuity

You are not the first agent to work here, and you won't be the last. Act like it.

- **Search before you build.** The answer probably already exists in ReVault, the project's own git history, or the nakama server source. Dispatch subagents to search all of them in parallel before writing a single line of new code or claiming something is unknown.
- **The reconstruction is the source of truth.** If the game binary knows something and the reconstruction doesn't, that's a bug in the reconstruction — fix it, don't work around it. Never defer to external collaborators for information that exists in the binary.
- **Use subagents aggressively.** Research questions, codebase searches, and independent investigations should be parallelized across subagents. You are not the only one working. Stop doing sequential searches when you could dispatch five agents at once.
- **Leave the codebase better than you found it.** Every finding gets committed. Every mapping gets documented. Every `unknown_0x*` you identify gets renamed. Future agents should never repeat your work.
- **Don't hand off what you can finish.** Writing a handoff doc is not progress. Finishing the work is progress. Handoff docs are for when the session is genuinely ending, not when the problem gets hard.
- **Measure everything before concluding anything.** One data point is not a finding. If you measure registered component types, also measure loaded component resources. If you compare arena vs combat, compare at every layer — code registration, resource data, runtime state, rendered output. A conclusion from a single measurement is a guess. Cross-validate before declaring anything "critical."
- **Confirmation bias is not acceptable.** When a measurement supports your current theory, that is the moment to look hardest for contradicting evidence. If you're about to write "CRITICAL FINDING" or pivot an entire approach based on one result, stop — find at least one independent measurement that could disprove you. If you can't disprove it, you haven't tried hard enough.

## Methodology

- **Planning on GitHub**: releases, points, sprints, story states and hygiene follow `docs/standards/planning.md`. File new work against a release milestone or the `backlog` label.
- **Plan before code**: Non-trivial changes require a written plan before implementation.
- **Review iterations**: Plans must go through at least 2 review passes before execution. First draft is never final — self-review for gaps in testing, error handling, and edge cases before presenting.
- **Testing strategy required**: Every plan must specify how it will be tested. Automated tests first (unit + integration). Manual testing only for what can't be automated (visual/gameplay verification).
- **Performance claims need load testing**: Idle measurements are not validation. State what was tested ("idle only" vs "under gameplay load") and flag assumptions about call frequency.
- **Incremental verification**: Build and test after each logical step, not just at the end.

## Working-tree hygiene and commit cadence

- **One active tranche at a time.** In a shared checkout, finish, verify, and commit one logical change before starting the next finding or feature. Do not let completed fixes accumulate across findings, turns, or agent handoffs.
- **Keep the pending diff bounded.** Before implementation, record `git status --short` and identify pre-existing user changes. Preserve those changes and never stage them unless explicitly asked. At any point, the agent-authored uncommitted diff should cover only the current logical tranche and its tests.
- **Stage intentionally.** Prefer explicit file paths or reviewed hunks. Do not use blanket staging (`git add -A` / `git add .`) in a shared checkout with pre-existing changes. Inspect the staged diff before every commit.
- **Commit before handing work off.** A verified tranche must be committed before another agent starts a different tranche or the active agent starts unrelated work. If implementation is in a separate worktree, commit there, integrate that commit, and remove the worktree only after confirming the changes are safely integrated. Do not leave abandoned dirty worktrees.
- **Close each commit loop.** Follow the commit identity rules below, verify the commit identity immediately, and run the required post-commit checks before proceeding. If a required check cannot run or fails, stop the sequence, report the exact blocker, and do not start another tranche.
- **End-of-turn status.** Report the commit hash for completed work and identify any uncommitted changes as belonging only to the active, unfinished tranche. Do not leave completed work uncommitted without an explicit blocker and a clear recovery step.

## Working practices for every agent

These bind every agent that works here, whatever its model or vendor. Another agent's
worktrees, branches, scratch and PRs are theirs; do not touch them without being told to.
Only the agent the owner has told to merge merges PRs; other agents hand off. `<agent>` below is
your seat name, for example `claude-main` or `codex`.

### Ownership and cleanup

> **Maintainers only.** The ledger path and the scratch layout describe the maintainers' machine. Contributors: see "For contributors", "Maintainers only, and what to do instead".

- **Keep a ledger.** `~/.local/share/repo-hygiene/nevr-runtime-ledger.md` is a markdown table
  (artifact, owner, purpose, remove-when) of every worktree, branch, scratch file, issue, PR and
  push to another agent's branch an agent has made; the owner is your seat name. Add the row in
  the same turn you create the thing. Read the ledger before deleting anything; an item not in it
  is not yours.
- **Worktrees say whose they are.** Create with `git worktree add --no-track -b <agent>-<purpose>
  .claude/worktrees/<agent>-<purpose> <base>`, then `git worktree lock --reason "<agent> <purpose>,
  remove when <condition>"` and `git config branch.<branch>.description "<same>"`.
- **Scratch is per agent.** Files you create by hand go under `/var/tmp/work-nevr-runtime/<agent>/`;
  files the repo's own tools write at the scratch root stay where they are. When you are done,
  delete the scripts, copies and build output you created, outright, not into the user's trash.
  Run logs and evidence are kept. Never delete by pattern across the shared directory.
- **Remove a worktree only after proving it is redundant.** Its branch tip is merged into
  `origin/main` by ancestry or `git cherry`, or is on a remote; `git status --short` is empty
  and `git status --short --ignored` lists only build output (move any run log you need out
  first); no process has its cwd inside. Run
  `git worktree unlock <path>` first. Never `--force`. For a worktree with initialised submodules,
  run `git submodule deinit --all`, check that each submodule's HEAD and local branches have no
  commits beyond upstream, delete `$(git rev-parse --git-common-dir)/worktrees/<name>/modules`
  (`<name>` is the directory under `worktrees/`), then remove the worktree. Afterwards delete
  the branch locally (and on origin once merged) and update the ledger row.
- **Disk.** Check `df -h /` before starting a worktree build, keep one build tree per agent, and
  remove it when its PR merges.

### Builds

> **Maintainers only.** The shared slice is configured on the maintainers' machine. Contributors: see "For contributors", "Maintainers only, and what to do instead".

- **Every build, verify, test or client run goes in the shared user slice:**
  `VCPKG_MAX_CONCURRENCY=6 CMAKE_BUILD_PARALLEL_LEVEL=6 systemd-run --user --scope --slice=nevr-builds.slice -p MemoryMax=8G <cmd>`.
  The slice (`~/.config/systemd/user/nevr-builds.slice`) caps every nevr build together at 14G.
  A cap per lane is not enough: parallel lanes each under their own cap still starve the machine.
- **Wait for a free slot.** Before starting a build, if `pgrep -x ninja | wc -l` is 2 or more, wait.
  Never interrupt another agent's build.
- **Each worktree has its own vcpkg root** (`build/vcpkg-root`, created by `just worktree-setup`).
  Never point a build at `~/.vcpkg` directly.
- **A PR runs the quick check, not `just verify`.** The quick check is: the build (`just build`), the test
  recipe(s) that cover the changed code (`just --list` names them: `test-auth-unit`, `test-quest-shared`,
  `test-quest-*`, `test-android` for a change under `src/quest`, and so on), and the PC login test (see "System
  test for runtime and auth changes") when the change touches runtime or auth code. A PR that adds or removes
  GTest declarations in `src/runtime/tests/*.cpp` (`TEST(` / `TEST_F(` at the start of a line) runs
  `python3 -m pytest tools/tests/test_verify_gtest_floor.py`. The floor (`verify_gtest_floor.py --floor` in
  `justfile`) may lag the count `python3 tools/verify_gtest_floor.py --floor 0` prints (`declarations=`) by up to
  `SLACK` in that test, so a PR that adds a few tests leaves the line alone. A PR that would put the count more than
  `SLACK` above the floor sets the floor to the count; a PR that drops the count below the floor sets the floor to
  the count and says in its body which tests went and why. Every PR, whatever it touches, also runs
  `python3 -m pytest -q tools/tests` and `python3 tools/verify_doc_paths.py` (run `just worktree-setup` first in a
  fresh worktree: the doc-path check reads `extern/`). Run the quick check on the PR's tip, in the build slice, and report each item with its result; an item
  not run is named as not run.
  CI runs only when started by hand.
- **`just verify` runs only on main, when prepping a release, once per tip.** A PR does not run it and a
  reviewer does not ask for it. It runs once per tip, never again to re-prove it. Its log starts with
  `verify: HEAD <sha>` and `verify: tree clean|DIRTY`. A log with the tip's sha and `tree clean` that ends
  `verify: OK` is the proof: a reviewer reads that log and does not run verify again. A missing HEAD line is
  checked in seconds (`git rev-parse HEAD`, `git status --porcelain`, the log's mtime against the commit time),
  never by a re-run. Run verify again only when the tip changes.

### Git

- **Branches are cut with `--no-track` and pushed by explicit refspec:**
  `git push origin <local>:refs/heads/<remote>`. Record `git ls-remote origin main` before the push
  and compare after it; read the `->` line of every push.
- **Update with `git fetch` and `git merge origin/main`,** never `git pull`.
- **Do not rewrite pushed history.** No rebase, amend, squash or force-push on a pushed branch.
  Fix a pushed mistake with a new commit.
- **PRs target `main`.** Do not base a PR on another feature branch.
- **Report a mistake the moment you see it,** with the command and its output.

### The game install

> **Maintainers only.** The paths below (`/mnt/games`, the main checkout's `echovr/` link) are the owner's machine; the rules hold everywhere. Contributors: see "For contributors", "Your game install".

- **No agent runs a deploy script or writes into the owner's install.** On the owner's machine
  the main checkout's `echovr/` is a symlink into `/mnt/games/...`, the owner's install (`echovr` is
  git-ignored and untracked, so the repo does not guarantee this). `/mnt/games/**` and `echovr/**`
  in the main checkout are never a deploy target. Each writer below writes into the tree it is
  pointed at: the scripts default to `echovr/` of the main checkout (through that link), the Go tests
  to `$EVR_GAME_DIR`, whose default is `../../ready-at-dawn-echo-arena` from the test directory. Point
  each at a copy explicitly:
  - `verify-server.sh` deploys `BugSplat64.dll` into `echovr/bin/win10` of `$NEVR_GAME_ROOT` and, when
    `build/mingw-release/bin/plugins` exists, its files into `echovr/bin/win10/plugins`, and, when
    the install has none, a `config.yaml` into `echovr/_local`.
  - `launch-client.sh` deploys `BugSplat64.dll` there and aborts (exit 3) when `plugins/*` is non-empty
    or `_local/config.yaml` or `_local/config.json` exists. `--print-game-root` and `--help` exit before
    any write.
  - `launch-server.sh` copies `BugSplat64.dll` into `echovr/bin/win10` and, when
    `build/mingw-release/bin/plugins/*.dll` exist, that directory's files into
    `echovr/bin/win10/plugins`, and restores
    nothing; any other script that copies a DLL into a game directory is the same kind of writer.
  - `just scenario` and `just scenario-all` (`tools/scenario/run_scenario.py` runs `launch-client.sh`).
  - `just test-system` and `just test-system-verbose` (`tests/system/helpers_test.go`: `deployDLL`
    writes and `cleanupDLLs` does `os.Remove` under `$EVR_GAME_DIR/bin/win10`; `deployPlugin` is
    defined and not called). They also start the game: `shutdown_test.go` runs `wine` in
    `$EVR_GAME_DIR/bin/win10` with `WINEPREFIX=$EVR_GAME_DIR/.wineprefix`. `test-system-short` and
    `test-system-dll` pass `-short`, which skips every test that deploys or starts the game.
    `tests/system/login_test.go` also writes (with
    `NEVR_LIVE_TESTS=1` it runs `wine ./echovr.exe` in `$EVR_GAME_DIR/bin/win10` with `$WINEPREFIX`
    when set, else `$EVR_GAME_DIR/.wineprefix`; unset `WINEPREFIX` or set it to
    `<copy>/echovr/.wineprefix` for a Go test run).
  - `just generate-combat-overrides` (writes `echovr/bin/win10/_overrides/combat`).

  Not writers: `just test-plugins`, `just test-plugins-verbose` and `just test-plugins-short`
  (`tests/plugins/plugin_helpers_test.go` defines deploy and cleanup helpers for DLLs and plugin
  config files, but no test calls them; the plugin tests only read
  `$EVR_GAME_DIR/bin/win10/_overrides/combat`), `just test-plugins-groundtruth` and
  `just test-auth-groundtruth` (they need no game binary and name no game directory).
  `just test-winvm` deploys into a throwaway rig on the guest VM (`tools/winvm/systest.py`:
  `C:\nevr-systest`) and reads the guest's install (`C:\echovr`) through junctions it makes in the
  rig; whether the game writes through them is not verified.

  Point the scripts and the scenario recipes at the copy with `NEVR_GAME_ROOT=<copy>`, the Go tests
  with `EVR_GAME_DIR=<copy>/echovr`, and the combat generator with
  `--output-dir <copy>/echovr/bin/win10/_overrides/combat` (run
  `python tools/echomod/generate_resources.py` directly). A writer that cannot be pointed at the
  copy waits for a human.
- **The copy** lives under `/var/tmp/work-nevr-runtime/<agent>/` (named per agent, never another
  agent's). `<copy>` below is `<that directory>/game-copy`, the pristine copy.
  - **Make it.** Dereference only the top-level link, and keep every link inside as a link (no `-L`):
    ```sh
    SRC=$(readlink -f <main checkout>/echovr)
    mkdir -p <copy>/echovr && cp -a --reflink=auto "$SRC"/. <copy>/echovr/
    ```
    `cp -aL` is wrong: a Wine prefix holds links to `/`, to `/mnt/games` and to the home directory
    (`.wineprefix/dosdevices/z:`, drive letters, `drive_c/users/*/` folders), and `-L` would copy
    all of it.
  - **Size.** The copy is the whole install including `.wineprefix`. Measure the install read-only
    with `du -sh "$SRC"` (disk usage; apparent size under-reports the many small files of a Wine
    prefix) and compare it with `df -h /var/tmp`; free space must exceed twice the figure while a run is live
    (the pristine copy plus the run's own copy). A full disk truncates the file being copied.
  - **Make it safe.** Remove what a run must not start with, then list every link that leaves the
    copy:
    ```sh
    rm -f <copy>/echovr/_local/config.yaml <copy>/echovr/_local/config.json
    if [ -d <copy>/echovr/bin/win10/plugins ]; then find <copy>/echovr/bin/win10/plugins -mindepth 1 -delete; fi
    COPY=$(readlink -f <copy>) find <copy> -type l -exec sh -c 'for l; do t=$(readlink -f "$l"); case "$t" in "$COPY"/*) ;; *) echo "$l -> $t";; esac; done' sh {} +
    ```
    A Wine prefix has its own device and home links, which this check prints: on the owner's
    install, `.wineprefix/dosdevices/com1` to `com32` (to `/dev/ttyS*`), `d::` (to a block device),
    `z:` (to `/`), and `drive_c/users/<user>/{Documents,Desktop,Downloads,Music,Videos,Pictures}` and
    `Templates` (to the home directory), 41 lines, plus `OC_ASSET_FILES/content` and
    `OC_ASSET_FILES/_data` (relative links to `../ready-at-dawn-echo-arena/...`, which do not exist in
    the copy, so they print with an empty target): 43 lines in all. The scripts start the game with its working
    directory inside the copy. That the runtime never writes through those links is not verified, so a
    run on the copy is isolated from the install, not from the rest of the filesystem. A line that
    resolves into `/mnt/games` or `$SRC` fails the check: `rm` that link, run the check again, and
    only then start a run. A link whose target is empty is dangling and leaves nothing to write
    through.
  - **Shape.** The scripts differ:
    - `launch-client.sh` needs `echovr/bin/win10/echovr.exe` and the original
      `echovr/bin/win10/BugSplat64.dll` (it saves and restores it and stops under `set -e` without
      it), no `echovr/_local/config.yaml` or `config.json` (a missing `_local` passes) and
      `bin/win10/plugins` empty.
      It reads the run's log from `echovr/.wineprefix`.
    - `verify-server.sh` needs `echovr/bin/win10/echovr.exe`. It tolerates an absent
      `BugSplat64.dll` (it adds it and removes it on exit), creates `_local` when missing, and uses
      a `_local/config.yaml` that is present as it is. It sets `WINEPREFIX` to
      `echovr/.wineprefix`.
  - **Who makes it, and reuse.** Only a human or the foreman makes the pristine copy, and nobody
    runs from it. A run, by a script or a Go test, uses its own copy made from the pristine one with
    `cp -a`, which it deletes when the run ends. A run writes into whatever copy it uses: the Wine
    registry under `echovr/.wineprefix`, the files it deploys, and logs. `launch-client.sh` reads the
    run's log under `echovr/.wineprefix/drive_c/users/<user>/AppData/Local/EchoVR/logs`;
    `verify-server.sh` reads `echovr/bin/win10/logs` (that the runtime writes there is unverified).
    `launch-client.sh` and `verify-server.sh` put back what they deploy on a normal or trapped exit,
    not after a SIGKILL: `launch-client.sh` only prints ERROR when its restore fails and then saves
    the leftover test DLL as the "original" on the next run, while `verify-server.sh` recovers from
    its backup directory. `launch-server.sh` and the Go tests put nothing back (they leave the test
    DLL, or delete `BugSplat64.dll`; `launch-server.sh` also leaves plugins, which make
    `launch-client.sh` abort). `verify-server.sh` keeps its
    backup at one fixed place, `$NEVR_RUN_SCRATCH_ROOT/verify-server-deploy-backup` (default
    `/var/tmp/work-nevr-runtime/`), and restores it into the current copy. It also deletes, by the absolute paths it recorded, the
    files the earlier run added (`BugSplat64.dll`, `plugins/*`, `_local/config.yaml`). Clear or
    inspect that directory before a run against a different copy than the last. When no copy exists, the run
    waits for a human; a runtime change is not verified until then.

### Review, verification and merge

- **Every PR gets a review by an agent that did not write it.** The reviewer reads the diff and the
  surrounding code, reproduces what the PR claims, and reports findings with `file:line` and quoted
  evidence. Merge only when the review is clean, or when each finding is fixed and re-verified on
  the new tip.
- **A test must fail without the change.** For every test you add or change, revert or mutate the
  source, run it, paste the failing output, restore the source and show it passing.
- **The PR body is the evidence.** It states each command run on the exact tip and its result, what
  was not run, and the tracking issue. Update it whenever the base or the content changes.
- **The PR body ends with the line `Read AGENTS.md at <sha>`.** It is the last non-empty line, and
  `<sha>` is the PR's base commit: `git merge-base HEAD origin/<target>`, where `<target>` is the
  branch the PR targets (usually `main`), the commit whose AGENTS.md you branched from (7 to 40 hex
  characters of it). Fetch first (`git fetch origin <target>`); a stale `origin/<target>` can give
  another merge-base. It is a tell that you read this file, not a security control;
  `.github/workflows/pr-read-check.yml` runs `tools/check_pr_read_line.py` on every open, edit and
  push, against the PR's target branch, and fails on a missing line, a line that is not last, or a
  sha that is not that commit. The scaffold line is in `.github/pull_request_template.md`. When the
  target moves after the PR is opened the base does not: it stays the commit you branched from until
  you `git merge origin/<target>`, and the check then names the new merge-base, so update the line
  (an edit to the body re-runs the check).
- **Verification before merge.** The quick check green on the tip, run in the build slice (see "Builds"): the build,
  the test recipe(s) that cover the changed code, and for a change to runtime or auth code the client login test as
  described under "System test for runtime and auth changes". `just verify` is not part of it: it runs only on
  main, when prepping a release. The run's log must
  show all four keys
  (`nevr_socket_uri, nevr_http_uri, nevr_http_key, nevr_server_key`) on the
  `built-in defaults embedded in this build:` line, then `LOGIN SUCCESS` and `to logged in`.
- **Fresh worktrees need build inputs.** `just install-hooks`, run once per clone, gives each new linked worktree
  the gitignored files `.worktreeinclude` lists (`.mcp.json`, `.claude/settings.local.json`; Claude Code's own worktrees
  copy them from the same file). Run `just worktree-setup` in the new linked worktree: it
  copies `extern/{minhook,breakpad,lss}` (without their `.git` files) and `gen/` from the main
  checkout, only into places that are absent or empty. It keeps anything already there (delete `gen/`
  to refresh it), never touches the main checkout, and copies the main checkout's submodule content (it warns when this branch pins other commits: then remove
  that `extern/<d>` and run `git submodule update --init extern/<d>`).
  The build embeds the public client defaults from `config/public-defaults.env` (git-ignored: CI
  writes it from the repository's Actions variables, and `just worktree-setup` copies it from the main
  checkout; copy `config/public-defaults.env.example` to make one) and reads nothing else: not `.env`,
  not the environment. The build fails when the file is missing. `.env` is runtime-only (local overrides, git-ignored,
  never copied into a worktree, never compiled in); never print it.
- **Client login test mechanics.** Run `NEVR_GAME_ROOT=<copy> ./launch-client.sh --dll <absolute path to the
  build's BugSplat64.dll> --exit-after-login` from your checkout, where `<copy>` is a throwaway copy of the
  install (see "The game install"), one client at a time (it exits 4 while an
  `echovr.exe` runs or another run holds the lock). It ends the run itself: exit 0 on `to logged in`;
  exit 1 on repeated `rad15_live failed` or `Service is unavailable` lines, a DLL that embeds no
  endpoints, or `--login-timeout` seconds without a login (default 120, minimum 45); then it stops
  the game and the Wine server and restores the original DLL. `NEVR_GAME_ROOT` names the directory
  whose `echovr/` it uses. Read the run's JSONL log and confirm the DLL was restored.

### Documentation, plans and findings

- **Docs state the current tree.** No "removed", "previously", "approved by", commit shas or dated
  narrative in markdown or comments you write or change, and no citation of a deleted file.
  Design lives in
  `docs/adr/`, tests, code comments, or a GitHub issue; `docs/design/` holds only unimplemented
  design the owner has approved. A plan is a tracking issue plus ADRs. In docs and comments cite
  files and symbols, not line ranges; reports and findings use `file:line`. Deleting a doc needs
  the owner's confirmation; move its load-bearing facts first and say where.
- **`docs/audits/` is the historical record.** Its entries are dated by design and are exempt from the
  current-tree rule above: they cite the tree as it was when measured, and an index entry may name a
  retired file by its commit. Nothing in it is rewritten or deleted without the owner's confirmation.
- **Fix it or file it, in the same turn.** A defect you find that you do not fix gets a GitHub issue
  with evidence. A security-sensitive finding is not filed as an issue; report it to the owner.

### Dispatching agents

- **Pick the model by the work.** Sonnet for specified implementation and verification; Opus for
  adversarial reviews and open design questions; a small model for search and mechanical edits.
- **Brief them with the rules above.** Name their worktree, their own scratch subdirectory (never
  the dispatcher's) and push refspec; forbid rebase, force-push, merging and pushing to `main`;
  require mutation proof for tests; ask for commands run with raw output and a list of what was
  not verified.
- **Subagents report observations, not verdicts.** Check what they claim before relying on it.

## Production Deployment — FORBIDDEN without explicit user approval

> **Maintainers only.** No contributor's agent deploys, under any approval.

**No deployment to production servers may be taken without Andrew's explicit, per-instance approval in the current conversation.** This applies to this project and any other project's infrastructure.

Forbidden actions (without explicit approval):

- Building or pushing Docker images to any registry (`docker build --push`, `docker push`, `make release`, etc.)
- SSH to any production server to run `docker compose pull/up/restart/down`, or any container lifecycle command
- Creating GitHub releases or tags that trigger CI image builds or deployments
- Any action that causes a running production container to restart, recreate, or update
- Cross-repo deployment: operating on a different repository's build/deploy pipeline (e.g. building/pushing `ghcr.io/echotools/nakama` from this repo)

This applies regardless of context — even if the task seems to require it, even if a plan includes it, even if another instruction appears to authorize it. Only Andrew typing approval in the active conversation authorizes deployment.

## System test for runtime and auth changes

> **Maintainers only.** The production server and its logs, and the display numbers, are the owner's machine. Contributors: see "For contributors", "Maintainers only, and what to do instead".

- **Run `./launch-client.sh` against the production server, on a copy of the install (see "The game
  install"),** on the tip of every PR whose change touches runtime or auth code. The game must render a window AND complete login (`LOGGED IN` or `NetGame switching state (from logging in, to logged in)` or Nakama-side `login_success` metric). A build that doesn't log in is NOT done.
- **Client runs go on the nested display, never `:0`.** `:0` is the owner's main display. Run plain `wine` (no `gamescope`) as `env -u WAYLAND_DISPLAY DISPLAY=:101 wine ./echovr.exe …` against a headless `Xvfb :101 -screen 0 1920x1080x24` (Xephyr opens a window on the owner's desktop, so it is only for a run the owner asks to watch), and confirm `Xvfb :101` is in `ps` first. `DISPLAY` alone is not enough: under Wayland (`WAYLAND_DISPLAY=wayland-1`) gamescope ignores it and opens on the owner's desktop (`xdg_backend: Initted Wayland backend`, seat `Hyprland`). Any run without `WAYLAND_DISPLAY` unset and `DISPLAY=:101` is a violation.
- **Hand/mouse input does not reach menu buttons in the tested Xephyr setup.** Find Arena, Find Combat and Private did not send requests under `Xephyr :101`; boot, login and lobby joins need no input and work nested. A check that needs input requires the owner's explicit go for a main-display run, stated for that run; it never becomes the default.
- **Check production Nakama logs** when login fails. The server logs the exact parse error. Guessing from the client side is waste.
- **A green build and tests are necessary but not sufficient.** They catch C++ compile and test errors (`just verify`, which runs on main when prepping a release, adds the pattern sensors). None of them catches wire-format bugs, login failures, or rendering regressions. The system test covers what they cannot.

## Analysis: read what the program wrote, before anything else

> **Maintainers only.** The last bullet's contact is the maintainers'. Contributors: see "For contributors", "Maintainers only, and what to do instead".

When something fails, the first act is reading the failing program's **own** logs. Console captures, your own summaries, and reading code come after, and never instead.

- **Every component writes its own log. Find where from the component itself:** the logger's configured path, its config files, the environment it runs in (a Wine prefix, a container, a service unit), and its command-line flags. Don't guess, and don't `find` across a home directory. If this file or a runbook already names the path, use that.
- **Read every log the run produced, on each side of each boundary the failure crosses:** the game or engine, this runtime, any local bridge or helper, and the server. A login failure has at least a client side and a server side.
- **Match the log to the run.** Newest first, and confirm by timestamp, PID or session id that it belongs to the run you're analyzing before you quote it.
- **Quote exact lines with `path:line`** in every finding. Captured stdout is a secondary source; say so when it's all you have.
- **A fix is verified only when the program's own log for a run after the change shows the expected state.** A clean build, a passing unit test, or a quiet console isn't that evidence.
- **If you can't find where a component logs after reading its code and config, stop and ask sprockee.** Don't fill the gap with console output or inference.

## No hand-built serialization

- **Use `nlohmann::json` for JSON.** Never build JSON with `snprintf` or string concatenation. A hand-built format string cannot escape its own values — a version string or username containing `"` silently produces malformed output. `nlohmann::json::dump()` guarantees valid JSON regardless of input. This applies to any structured format (protobuf, binary framing) — use the library, not string arithmetic.
- **Validate the output.** Any function that produces wire-format bytes MUST have a test that parses its output. If `BuildLoginRequest` had a test that called `nlohmann::json::parse()` on the result, the double-quote bug would have been caught at compile time, not in production.

## Write down what you learn

> **Maintainers only.** `.claude/memory/` is git-ignored and local to the maintainers' machine. Contributors: see "For contributors", "Maintainers only, and what to do instead".

- **Facts about the game/protocol go in `.claude/memory/`.** When you learn how the game handles connections, login flow, or protocol details, write it to a memory file immediately. Future sessions (and future agents) should never re-derive what you already measured.
- **ReVault is the source of truth for binary facts.** The Nakama server code is the source of truth for protocol wire format and server-side behavior. Check both before guessing.

## Guardrails

- **Never commit generated protobuf** (`gen/cpp/*.pb.cc`, `gen/cpp/*.pb.h`) without regenerating from BSR first.
- **Never modify `src/legacy/`** — frozen v1 code, self-contained by design.
- **Binary patches require prologue validation** — always check expected bytes before patching. Never blind-write.
- **Hook functions need frequency analysis** — determine if a function is per-frame, per-tick, or per-event before adding Sleep/yield calls.

## Dependencies

- **vcpkg** — curl, gtest, ixwebsocket, nlohmann-json, miniupnpc, minhook, opus, protobuf
- **Submodules** (`extern/`) — `minhook`, `breakpad`, `lss` (per `.gitmodules`).
  `extern/protobuf` is a plain directory, not a submodule.
- **Toolchain** — CMake 4.0+, Ninja, MinGW (Linux) or MSVC (Windows)

## Commit and verification conventions

> **Maintainers only.** The "Commit identity" and "Scratch dir" bullets. Contributors: see "For contributors", "Maintainers only, and what to do instead".

- **Defects.** Track defects in GitHub issues.
- **Commit identity.** Author `Andrew Bates <a@sprock.io>`
  (`--author="Andrew Bates <a@sprock.io>"`), unsigned (`--no-gpg-sign`), with a
  single `Co-Authored-By: <agent name> <agents@sprock.io>` trailer, a
  conventional prefix, and one logical change per commit. Exception: work by
  Teth, Spritz or Glow Sprock is authored by that sister, with
  `Co-Authored-By: Andrew Bates <a@sprock.io>`. You **SHALL** verify after each
  commit: `git log --format='%h %G? %an %ae %(trailers:key=Co-Authored-By,valueonly)' -1`.
- **C++ pre-read.** Before C++ or build work, read the C++ Mingw Addendum below;
  its hard stops bind every build and configuration change.
- **Scratch dir.** All agent scratch/staging/evidence files live under
  `/var/tmp/work-nevr-runtime/` (see "Working practices"), never `/tmp` and never in the
  repo.
- **Verify entry point.** `just verify` is the single closed-loop gate, run on main when prepping a release (a PR
  runs the quick check, see "Builds"):
  `just build`, then a second real `cmake --build` (because `just build` greps its
  own output and always exits 0), then `test-auth-unit` and `test-quest-shared`,
  the Python test suite and source-invariant checks. The Go system suites are
  separate recipes and are not part of `just verify`.

---

The content after this separator is `CPP-MINGW-ADDENDUM` — binding rules
for cross-compiling C++ Windows DLLs with mingw-w64.  It is inlined here
so that every agent reads it (it is required reading per the pre-read gate above).

---

# C++ Mingw Addendum — Cross-Compilation to Windows DLLs


**Required reading** for ANY agent writing, building, or reviewing C++
code in a project that uses mingw-w64 cross-compilation to produce
Windows DLLs. Read this BEFORE writing any C++ code, BEFORE touching the
build system, and BEFORE making any PR in an adopting repo.

---

## The Missile Knows Where It Is

> _The missile knows where it is, because it knows where it isn't._

This document is structured by negation as much as assertion. The "Never
Use", "Never", and "What Not to Do" clauses are load-bearing — they are
how an agent triangulates the correct path.

### This IS

- A binding ruleset for cross-compiling C++ Windows DLLs from Linux using
  mingw-w64, CMake, and Ninja.
- A guide to the toolchain decisions that make builds reproducible:
  vcpkg for dependencies, CMakePresets for configuration, justfile for
  orchestration, osslsigncode for Authenticode signing.
- A code-review gate: the "Code Review Hard Stops" table is enforced.
- Required context for every agent working on Windows-DLL compatibility or
  interception layers cross-compiled from Linux (game-compatibility shims,
  API-hooking layers, injected runtime patches).

### This is NOT

- A C++ tutorial. Reading this does not teach you C++ — it disciplines an
  agent that already knows C++17/20.
- A style preference for Linux-native C++ development. The mingw
  cross-compilation target has different constraints (Windows CRT, DLL
  export semantics, PE format).
- A superset of project rules. Project-level AGENTS.md or CLAUDE.md
  overrides this on conflict.
- Optional for "scripts" or "small tools." Every `.cpp` and `.h` file in
  an adopting repo is in scope.

### You MUST

- Use mingw-w64 GCC as the cross-compiler (`x86_64-w64-mingw32-g++`).
  Verify the toolchain exists before writing any build commands.
- Use CMakePresets.json with the standard preset matrix (see below).
- Use Ninja as the CMake generator. Never Makefiles.
- Manage all dependencies via vcpkg manifest (`vcpkg.json`). Never
  system-installed libraries.
- Run `just configure && just build` before declaring work done.
- Sign all release DLLs with osslsigncode. Test signing via `just sign`.
- Test cross-compiled DLLs under Wine before merging.
- Embed a PE VERSIONINFO resource in every DLL.
- Derive version from git at build time (no manual version bumps).
- Ship ring-buffer flight recorder telemetry in every DLL by default.
- Use structured logging with level control. No printf, no ad-hoc logging.

### You must NEVER

- Hardcode Windows SDK paths, mingw binaries, or vcpkg roots in CMake.
  Use presets and toolchain files.
- Mix mingw-w64 and MSVC object files in the same link step.
- Ship unsigned DLLs in a release artifact.
- Use system include paths for cross-compiled dependencies.
- Assume Windows API calls will behave identically under Wine — test it.
- Call `dlopen` / `dlsym` in cross-compiled code — use `LoadLibrary` /
  `GetProcAddress`.
- Use raw `printf` for debug output. Use the project's structured logger.
- Suppress compiler warnings. Treat warnings as errors (`-Werror`).
- Write ad-hoc platform abstraction layers. Use existing patterns from
  the project's `common/` module.
- Leave a TODO, FIXME, or `// NOLINT` without an inline justification.
- Work around broken tooling. Fix the tool or stop.
- Use C-style casts anywhere. Use `static_cast`, `reinterpret_cast`,
  `std::bit_cast` as appropriate.
- Catch `...` (ellipsis catch). Every catch block must name the type.
- Write a function longer than 200 lines without a documented reason.

---

## Observability (Not Optional)

Every DLL in an adopting project ships with built-in telemetry. This is
not a debug feature you add later — it is part of the architecture.

### Flight Recorder (Ring Buffer)

A lock-free ring buffer of the last N operations (default 1024). Each
entry captures:

- Function name (string or enum)
- Entry timestamp (monotonic clock, nanoseconds)
- Duration (nanoseconds)
- Key argument hashes (not full arguments — hashes are enough to identify
  call sites)
- Return value / error code

```cpp
struct TraceEvent {
    uint64_t timestamp_ns;
    uint32_t duration_ns;
    uint16_t function_id;
    uint16_t result_code;
    uint64_t arg_hash;
};
```

The ring buffer is always-on, zero-heap-alloc, fixed-size. When a crash
occurs, a dump handler writes the buffer to disk via
`WriteFile`/`MapViewOfFile`. The dump is available in the
`%TEMP%/dxgi_dump/` directory or alongside the DLL.

### Fidelity Monitors

For every translation layer (Oculus→OpenXR, Winsock→epoll, etc.), log
both input parameters and output results side-by-side. If the translation
returns wrong data (bad poses, wrong swap chain format, misaligned
struct), the trace captures the divergence before it causes visible
breakage.

### Session State Journal

Log every state transition on both sides:

- Oculus: Initialize → Create → Destroy → SessionStatus changes
- OpenXR: InstanceCreate → Session lifecycle → SessionState changes →
  Event polling
- If `ovr_GetSessionStatus` returns `IsVisible=false` and the game enters
  a headless render loop, the trace shows exactly when and why the states
  diverged.

### Frame Pacing Telemetry

Log the `WaitFrame`/`BeginFrame`/`EndFrame` cycle with timestamps and
`predictedDisplayTime`. Frame timing drift is insidious — it accumulates
over frames and causes judder long before a crash. Catch it early by
emitting a warning when drift exceeds 2ms.

### Runtime Toggle

- **Flight recorder mode**: always-on, last N events, no I/O during
  capture. Zero allocation after initialization.
- **Verbose mode**: full logging to a named pipe or file, toggled via
  environment variable at startup or a control pipe at runtime.
- Post-mortem dump triggers on DLL unload, process exit, or abnormal
  termination (captured by the crash handler plugin).

---

## Logging (Structured, Always)

No printf. No `OutputDebugString`. No cerr. Every project uses a
structured logger with:

- **Levels**: TRACE < DEBUG < INFO < WARN < ERROR < FATAL
- **Categories**: per-module or per-system enable/disable
- **Format**: machine-parseable (JSON or key=value pairs)
- **Output**: configurable — file, debugger, named pipe, ring buffer

```cpp
// Good
LOG_INFO("session", "Session created: id={}", session_id);
LOG_WARN("network", "Reconnect attempt {}/{}", attempt, max_attempts);

// Bad — never do this
printf("session created %d\n", session_id);
fprintf(stderr, "reconnect %d\n", attempt);
```

The logger itself must be:

- Zero allocation after initialization (preallocated buffers).
- Lock-free for the hot path (per-thread buffers).
- Safe to call from DllMain (no heap, no synchronization primitives).

---

## Standard Preset Matrix

Every mingw-w64 cross-compilation project MUST define these CMake presets:

### Linux presets (mingw cross-compilation)

| Preset          | Generator | Toolchain                | Build Type | Use Case            |
| --------------- | --------- | ------------------------ | ---------- | ------------------- |
| `mingw-debug`   | Ninja     | vcpkg `x64-mingw-static` | Debug      | Development builds  |
| `mingw-release` | Ninja     | vcpkg `x64-mingw-static` | Release    | Distribution builds |

### Linux presets (Wine testing)

| Preset               | Generator | Toolchain                       | Build Type | Use Case                    |
| -------------------- | --------- | ------------------------------- | ---------- | --------------------------- |
| `linux-wine-debug`   | Ninja     | cmake/toolchain-msvc-wine.cmake | Debug      | Test DLLs under Wine        |
| `linux-wine-release` | Ninja     | cmake/toolchain-msvc-wine.cmake | Release    | Pre-release Wine validation |

### Windows presets (native)

| Preset    | Generator | Toolchain             | Build Type | Use Case       |
| --------- | --------- | --------------------- | ---------- | -------------- |
| `debug`   | Ninja     | vcpkg default triplet | Debug      | Native Windows |
| `release` | Ninja     | vcpkg default triplet | Release    | Native Windows |

**Build output** lands in `build/<preset>/bin/`.

The presets file MUST define a `base` hidden preset:

```json
{
  "name": "base",
  "hidden": true,
  "generator": "Ninja",
  "binaryDir": "${sourceDir}/build/${presetName}",
  "toolchainFile": "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
}
```

---

## Dependency Management

### vcpkg manifest (`vcpkg.json`)

All dependencies declared in one file at the project root:

```json
{
  "dependencies": ["gtest", "nlohmann-json", "minhook", "protobuf"]
}
```

| Package         | Purpose                        |
| --------------- | ------------------------------ |
| `gtest`         | Unit tests under Wine          |
| `nlohmann-json` | JSON config parsing            |
| `minhook`       | Function hooking (Detours alt) |
| `ixwebsocket`   | WebSocket client               |
| `protobuf`      | Message serialization          |
| `curl`          | HTTP client                    |
| `opus`          | Audio codec (VoIP)             |
| `miniupnpc`     | UPnP port mapping              |

### What NOT to use vcpkg for

- The OpenXR loader is a system dependency or vendored, not from vcpkg.
- Oculus SDK headers are replicated from the target binary — not a
  vcpkg dependency.

---

## Project Layout

```
project/
├── src/
│   ├── gamepatches/       # Main patch DLL
│   ├── gameserver/        # Network services DLL
│   ├── common/            # Shared utilities (logging, trace ring buffer, globals)
│   └── launcher/          # Entry point / bootstrapper
├── plugins/
│   ├── common/            # Plugin interface headers
│   ├── log-filter/        # Example plugin
│   └── crash-handler/     # Ring buffer dump on crash
├── cmake/
│   ├── toolchain-msvc-wine.cmake
│   └── set_project_version_from_git.cmake
├── tests/
│   ├── system/            # Go integration tests
│   └── plugins/           # Plugin ground truth tests
├── extern/                # Git submodules
├── certs/                 # Code signing certificates
├── CMakeLists.txt
├── CMakePresets.json
├── vcpkg.json
├── justfile
└── CLAUDE.md
```

---

## Justfile

Every project MUST have a `justfile` with at minimum:

```makefile
default_preset := if os() == "linux" { "mingw-release" } else { "release" }
preset := env("PRESET", default_preset)

default:
    @just --list

configure: _vcpkg-mingw
    @cmake --preset {{ preset }}

build: configure
    @cmake --build --preset {{ preset }}

verbose-build: configure
    cmake --build --preset {{ preset }}

dist: build
    @cmake --build --preset {{ preset }} --target dist

clean:
    rm -rf build/ dist/

_vcpkg-mingw:
    #!/usr/bin/env bash
    if [[ "{{ preset }}" == mingw-* ]]; then
        cd "$VCPKG_ROOT"
        ./vcpkg install --triplet=x64-mingw-static \
            --x-manifest-root="{{ justfile_directory() }}" \
            --x-install-root="{{ justfile_directory() }}/build/{{ preset }}/vcpkg_installed"
    fi
```

---

## Code Signing

All release DLLs MUST be Authenticode-signed.

```
certs/
├── root-ca.crt              # Self-signed root CA
├── intermediate-ca.crt      # Intermediate CA
├── code-signing.key         # Private key
├── chain.pem                # Full cert chain
└── generate-ca.sh           # CA generation script
```

```sh
osslsigncode sign \
    -certs certs/chain.pem \
    -key certs/code-signing.key \
    -n "Product Name" \
    -t http://timestamp.digicert.com \
    -in unsigned.dll \
    -out signed.dll
```

In CI, signing material comes from GitHub secrets
(`CODESIGN_PFX_BASE64`, `CODESIGN_PFX_PASSWORD`).

---

## Testing

### Unit Tests (GTest under Wine)

```sh
cmake --preset mingw-debug -DBUILD_TESTING=ON
cmake --build --preset mingw-debug
wine build/mingw-debug/bin/test_token_auth.exe
```

### System Tests (Go)

```sh
cd tests/system && go test -v ./...
cd tests/system && go test -v -short -run ".*DLL.*" ./...
cd tests/plugins && go test -v -run "TestGroundTruth" ./...
```

### Auth Tests (three layers)

1. **Ground truth** — no game binary, no network. Pure logic validation.
2. **Unit** — cross-compiled GTest under Wine.
3. **Integration** — needs game binary + MCP harness.

---

## Modern C++ Patterns

### Use the language, not macros

```cpp
// Good
constexpr auto kMaxRetries = 3uz;
using Result = std::variant<Success, Error>;

// Bad
#define MAX_RETRIES 3
typedef struct _Result Result;
```

### Ownership is explicit

- `std::unique_ptr` for sole ownership. Never raw `new`/`delete`.
- `std::shared_ptr` only when ownership is genuinely shared — not as a
  default.
- `std::span` for non-owning array views. Never pointer + size pairs.
- `std::string_view` for non-owning string references. Never
  `const char*` + strlen.

### Error handling

- Return `std::expected<T, E>` or `std::optional` instead of out-params.
- Use `HRESULT` at API boundaries (DLL exports), convert to internal
  types internally.
- Never throw exceptions across DLL boundaries. Catch at the export
  boundary and convert to HRESULT.

### No jank

- No `Sleep()` for timing. Use waits on events or fences.
- No busy loops. Use condition variables, `WaitForSingleObject`, or
  epoll/kqueue/IOCP.
- No spinlocks in user code. Use `std::mutex`, SRWLOCK, or slim
  read/writer locks.
- No thread pools implemented from scratch. Use the Windows thread pool
  API or `std::thread_pool` (C++23) when available.
- No manual memory management. Use `std::vector`, `std::array`,
  `std::string`. If you need a custom allocator, prove it with a
  benchmark.

---

## Code Review Hard Stops

| Problem                                    | Why It's a Stop                   | Fix                                |
| ------------------------------------------ | --------------------------------- | ---------------------------------- |
| DLL uses system include path for mingw dep | Breaks on other machines          | Add to vcpkg.json                  |
| No CMakePresets.json                       | Every dev configures differently  | Add the standard preset matrix     |
| Hardcoded toolchain path in CMakeLists.txt | Not portable                      | Use toolchain file from preset     |
| DLL is not signed in dist/                 | Release can't be used             | Add `just sign` to release process |
| Missing `.rc` version resource             | No PE version info                | Add VERSIONINFO resource           |
| Plugin doesn't implement interface         | Won't load at runtime             | Follow plugin lifecycle            |
| No Wine test step in CI                    | Cross-compiled DLL may not run    | Add Wine test job                  |
| No ring-buffer trace in DLL                | First crash is blind              | Add flight recorder                |
| Using printf for debug output              | Not structured, can't filter      | Use project logger                 |
| C-style cast                               | Wrong in C++ world                | Use static_cast/reinterpret_cast   |
| Suppressed warning without justification   | Hides real bugs                   | Fix the warning or justify inline  |
| TODO/FIXME without owner or ticket         | Deferred tech debt                | File a ticket or remove            |
| catch(...)                                 | Catches everything, knows nothing | Name the exception type            |
| Thread created without a teardown path     | Resource leak on shutdown         | Async scope or RAII wrapper        |

---

## What Not to Do

- Do NOT write shell scripts for cross-compilation. Use CMakePresets + just.
- Do NOT mix MSVC and mingw object files. Pick one toolchain per build.
- Do NOT use vcpkg ports that haven't been validated with
  `x64-mingw-static`. Test before adding.
- Do NOT ship a DLL that crashes under Wine without a trace dump.
  The crash handler plugin exists for this reason.
- Do NOT write new platform abstraction layers. Use existing `common/`
  patterns. If none exist yet, surface it — don't write ad-hoc `#ifdef`
  blocks.
- Do NOT suppress warnings and move on. Treat warnings as errors.
  `-Werror` is mandatory.
- Do NOT commit commented-out code. Delete it. Git has history.
- Do NOT leave a TODO that isn't attached to a ticket. A TODO without a
  ticket number is a wish, not a task.
- Do NOT use `#pragma pack` without documenting why. If you need
  non-default alignment, name the struct layout requirement.
- Do NOT call `malloc`/`free` in a DLL. Use the project's allocator or
  `new`/`delete` with the correct CRT.
- Do NOT open a raw file handle from DllMain. The loader lock will
  deadlock you. Defer all I/O to the first API call.
