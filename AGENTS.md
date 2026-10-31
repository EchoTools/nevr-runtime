# AGENTS.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

The key words "MUST", "MUST NOT", "REQUIRED", "SHALL", "SHALL NOT", "SHOULD", "SHOULD NOT", "RECOMMENDED",
"NOT RECOMMENDED", "MAY", and "OPTIONAL" in this document are to be interpreted as described in BCP 14
([RFC 2119](https://www.rfc-editor.org/rfc/rfc2119), [RFC 8174](https://www.rfc-editor.org/rfc/rfc8174)) when, and only
when, they appear in all capitals, as shown here.

The logging standard below is part of this contract:

@docs/standards/logging.md

## Do what was asked

These come before every other rule in this file.

- **Build the request as given.** Add no step, criterion, order or check the request does not contain. Permission to do
  something is not a request to do it.
- **Decide two-way doors.** Make a reversible choice yourself, take the answer a professional engineer would take, and
  report it. Ask only about one-way actions and real dilemmas.
- **Pass on intent, not orders.** When you direct another seat, give it the owner's goal and let it finish its work. Never
  tell it to stop unless the owner said to.
- **Every check carries a decision rule.** Before any test, build or check, write down the result that would change what
  you do and the action it would trigger. A check with no such result, or whose failure a later step the owner already
  named would catch, is cut.
- **Done is the owner's stated done.** Add no criteria to it.

## Commits, versions and requirement words

- **Commits and PR titles** follow [Conventional Commits 1.0.0](https://www.conventionalcommits.org/en/v1.0.0/):
  `<type>[(scope)][!]: <description>`. Types: `feat` (a feature), `fix` (a bug fix), `docs`, `style`, `refactor`,
  `perf`, `test`, `build`, `ci`, `chore`, `revert`. A breaking change is marked with `!` before the colon or a
  `BREAKING CHANGE:` footer. Per spec item 15, types are case-insensitive. `.github/workflows/pr-title-check.yml`
  (`tools/check_pr_title.py`) checks the PR title.
- **Credit the people whose work we integrate, every time.** A commit that draws on a person's code, patch, research
  or findings carries a `Co-Authored-By` trailer for that person, alongside the house trailers: their GitHub noreply
  address, `<id>+<login>@users.noreply.github.com` (`gh api users/<login>` gives the id). Someone with no GitHub
  account is credited by name in the commit body and in the README thanks. A squash merge keeps those trailers in the
  merge commit message.
- **Versions** follow [Semantic Versioning 2.0.0](https://semver.org/spec/v2.0.0.html). Tags stay `vX.Y.Z`.
- **Requirement words** (MUST, SHALL, SHOULD, MAY and their negations) follow
  [RFC 2119](https://www.rfc-editor.org/rfc/rfc2119) as clarified by
  [RFC 8174](https://www.rfc-editor.org/rfc/rfc8174): normative only in all capitals. The same notice opens each file
  in `docs/standards/`.
- **The changelog is the git log.** There is no `CHANGELOG` file; release notes are written per release
  (`RELEASING.md`).

## Nothing that only one machine has

The tree MUST NOT reference anything that a fresh clone on another machine does not have. This covers code, scripts,
tests, recipes, unit files, docs, comments and agent definitions alike:

- **No path outside the checkout.** No absolute or home-relative path to a file or directory that is not made by the
  repo's own tools: no `/home/<user>/...`, `~/src/<other repo>`, `/mnt/...`, `~/.config/...`, `~/.local/...`, and no
  path into another person's or seat's directory. Another repository is named by its name (and its public URL where it
  has one), never by where it is cloned.
- **No local helper and no private service.** No wrapper or notifier that lives on one machine (a command in
  `~/.local/bin`, a personal mailbox or alert tool), no private database or tool by name, and no entry in a personal
  secret store.
- **A per-machine input is an input.** A path, a key or an account that differs per machine is an environment
  variable or a parameter with no machine-specific default, and the code fails with a line that names it when it is
  missing.
- **Allowed:** the work root (`$NEVR_WORK_ROOT`, else `~/src/nevr-runtime-work`), the XDG state directory and the
  `build/` trees, which the repo's tools create themselves; standard system tools and public package names.

## Project

nEVR Runtime — Windows DLL patches for Echo VR (echovr.exe) enabling connection to echovrce community game services. Targets both game clients and dedicated game servers. Written in C++17.

**Echo VR is one codebase built for two binaries:** echovr.exe (PC, x86-64) and libr15.so (Quest, aarch64). The game functions we hook are the same functions on both, compiled differently. So a feature is written once: one hook per game function with a per-binary table of addresses (and calling convention where it differs), and one implementation of everything above the hook (transport is libcurl on both). Before adding any file under `src/quest/` that does what a `src/runtime/` file already does, use the shared one; a Quest-only copy needs a stated platform reason in its PR, and "it's Quest" is not one. (#1598)

## For contributors

The rest of this file binds every agent, a contributor's included, except the parts marked **Maintainers only**. This
section applies to an agent working for an outside contributor, on the contributor's own machine. It adds what such an
agent needs: what to read, what a PR carries, which rules are live, and the N-rules index. It also says which later sections describe the maintainers' machine, and what to do
on yours instead. Where a step needs access only the maintainers have (the production server's logs, the
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
| "Ownership and cleanup": the ledger file | Keep the same ledger in a file of your own: a markdown table (artifact, owner, purpose, remove-when), one row per thing you create, the worktree directory or branch in the first cell and your handle, one word, in the second. Read it before deleting anything. Create worktrees under `.claude/worktrees/` in your clone, and run `REAP_LEDGER=<your ledger file> just reap-merged`: `tools/reap_merged.py` scans only `.claude/worktrees/`, exits when `REAP_LEDGER` is unset or the ledger cannot be read, and keeps any worktree the ledger does not credit to an owner. |
| "Builds" and "Live game runs": the shared `nevr-builds.slice` and `nevr-live.slice` | Same reason: parallel lanes, each under its own cap, still starve a machine. Builds, verify and tests run in `nevr-builds.slice`; a live game run runs in its own scope in `nevr-live.slice` with `systemd-run --user --scope --slice=nevr-live.slice -p MemoryMax=6G -p MemorySwapMax=0 <cmd>`. If you run one build lane, `VCPKG_MAX_CONCURRENCY=6 CMAKE_BUILD_PARALLEL_LEVEL=6 systemd-run --user --scope -p MemoryMax=8G <cmd>` is enough. If you run several, create one user slice that caps them all together, as the maintainers do. Wait while two or more `ninja` processes run (`pgrep -x ninja` lists them). |
| Scratch under `~/src/nevr-runtime-work/<agent>/` | The work root is `$NEVR_WORK_ROOT`, else `~/src/nevr-runtime-work` (`~` is your home directory), so the default works on any machine, and several tools write there by default (`tools/scenario/run_scenario.py`, `tools/lib/game_install.sh`, `tools/winvm/systest.py`). Use `<work root>/<your handle>/`, never `/tmp` and never the repo, and keep the work-root rule under "Working practices for every agent". |
| "The game install" | See "Your game install" below. |
| "Check production Nakama logs" and "ask sprockee" | The production server's logs are maintainer-only. On a server you run, read its log yourself. For production, ask in the PR, giving the run's time window (UTC), the account and the client log lines around the failure. Where the file says ask sprockee, ask in the PR or issue. |
| "Production Deployment" | Never, for a contributor's agent. No approval you can receive counts. |
| "System test for runtime and auth changes": the production server, display `:0` | The PC login test runs on main after the release cut, not on a PR. When you run it, log in to the server your build's embedded defaults point at, or one you run. Never use the user's own display; use the headless Xvfb `:101` display exactly as described. |
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
size check, removing `_local/config.*`, every `.credentials.json*` and `plugins/*`, and the shape each script needs.

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
- **Siblings, for a fix.** The search statement looks for copies of what you add; this looks for other instances of
  what you fix. A PR that fixes a defect names the shape it fixed, gives the `git grep` that finds every instance of
  that shape (the pattern and the scope it ran over), and says for each hit: fixed in this PR, filed (`#N`), or not
  the same shape (the reason). A fix that lands for one instance and leaves its siblings is half a fix:

  ```
  ## Siblings
  Shape: a socket that mints a new token on every HTTP 401, with no time gate.
  - `git grep -l "rejected the bearer token (HTTP 401)" -- src/runtime/server` (2 files):
    websocket_client.cpp: fixed in this PR (the ServerDB socket now mints through BearerReconnectAuth);
    bearer_reconnect_auth.cpp: not the same shape (it mints at most once per 30 s).
  ```
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
  Revert a mutant from a saved copy of the file or a stash, or commit first; never `git checkout` a file that has
  uncommitted work in it.

  ```
  ## Mutants
  - M1 drop the epoch re-check in WaitIdle -> FAIL test_reader_gate: Overtaken
  - M2 `undrained_` never decremented -> FAIL test_reader_gate: TimesOutWhenReadersStay
  - restored tree: 9 tests OK
  ```
- **Evidence on the exact tip**, as "Review, verification and merge" says: each command run on the tip SHA and its
  result, what was not run, and the tracking issue.
- **Platform parity.** PCVR and Quest stay at parity. A PR that adds or changes player-facing behavior says in its
  body what the other platform's status is: done in this PR, already present (`file:line`), not applicable (the
  reason), or a linked twin issue filed in the same turn.
- **A headline, when players see a change.** A PR that changes what players see adds `release-headlines/<issue-or-topic>.txt`
  with one line, one plain sentence a player would understand about that change only, written by the PR's author
  (`RELEASING.md`, "Write the release body"), and deletes the fragments present at the newest `vX.Y.Z` tag
  (`git ls-tree -r --name-only <newest tag> -- release-headlines/`). The release body's player line is the files the range
  adds, or "Fixes and internal changes." when it adds none; the commits never write it. A change behind a setting that is
  off, or internal, adds no file.

**Write an issue as a falsifiable claim** (see #519 and #520), with these sections:

- **Evidence at `<sha>`**: what the tree says, re-read at a named commit, with `file:line` and quoted lines.
- **Not shown**: what the evidence does not establish.
- **Falsifier**: the one observation that would prove the issue wrong.
- **Work**: the steps, in order, tests first, each with what it must show.

### Live rules

- **The PR body ends with `Read AGENTS.md at <merge-base sha>`** (see "Review, verification and merge").
  `.github/workflows/pr-read-check.yml` fails the PR without it. On a PR from a fork, a maintainer has to approve the
  workflow run before it runs.
- **The PR body names the issue it resolves or advances** (`Closes #N` or `Refs #N`; see "Review, verification and
  merge"). `.github/workflows/pr-issue-link-check.yml` fails the PR without it. The same fork rule applies.
- **Runs use a throwaway copy of the game install, never the install itself** (#524; "The game install"). No check
  enforces this: the scripts still default to the checkout's `echovr/`, so point every writer at the copy yourself.

### The N-rules index

The code carries rules tagged `N<number>` in comments (`// N62: ...`) and in the `just verify` recipe, whose sensors
name the rule they guard in their FAIL line. A green `just verify` does **not** prove you kept every one: some are
checked by a GTest or a Python check, some only by a live run, and some by nothing. Before you change a file named
below, read the rule's comment at the site and keep the rule. If a change must break one, say so in the PR and why.

The index was built from `git grep -nE '\bN[0-9]+\b'` over `src/`, `sdk/` and `tools/`, the `verify` recipe in
`justfile`, and the Python checks it runs. An ID cited only as history inside another rule's comment (for example N41,
N48) is not a rule.

**Kind** says what a rule promises: `run`, what a running process does (it exits, logs, refuses, loads, never
blocks); `source`, what the tree is (where code lives, what it calls, how a name is spelled, what is built). Each
**Checked by** item carries its rank on the evidence ladder in `docs/standards/verification.md`: `rank 1:` a live
run; `rank 2:` a test that drives the code that ships (not a copy of it); `rank 3:` a check that reads the source,
either a `just verify` sensor whose FAIL line names the rule (written `verify`) or a Python check over source text.
A rule whose Kind is `run` is checked at rank 1 or rank 2, or its cell has an `unchecked:` item naming the part
nothing drives: a sensor that finds the call site does not show what the run does. A `source` rule may rest on
rank 3, and `none` means nothing checks it. `tools/tests/test_n_rules_index.py` checks this form; the review checks
that each Kind and rank is true.

#### Boot, shutdown and crash path

| Rule | Kind | Statement | Guards | Checked by |
| --- | --- | --- | --- | --- |
| N4 | run | A server whose config fails to load, or whose shutdown hangs, exits; it never blocks forever (`ForceFatalExit` through the registered fatal handler; a teardown watchdog). | `src/core/nevr_config.h`, `src/core/nevr_config_policy.cpp`, `src/runtime/lifecycle/crash_recovery.cpp` | unchecked: nothing drives a failed config load or a hung shutdown |
| N36 | run | Guarded initialization (`InitializeAfterGameImageGuard`) runs from the runtime's `initialize` (`runtime_entry.cpp`), which the host's detour on the game's main wrapper calls once, outside the loader lock, in the boot phase: it calls `nevr_boot_log_tee::Init()` before `InitializeFunctionPointers()`, and every return passes `nevr_boot_log_tee::Close()`. In the boot phase `Log()` reports through the boot tee (the sink `Init()` sets with `SetBootLogSink`), never the game's CLog, so code shared with later phases calls `Log()` and never branches on the phase. | `src/runtime/lifecycle/initialize.cpp`, `src/core/logging.cpp`, `src/runtime/log/boot_log_tee.cpp` | rank 2: `src/runtime/tests/test_boot_log_routing.cpp` (the routing); rank 3: verify (the call order), `tools/tests/test_patch_detour_logging.py` (the sink, the early return, no caller branching) |
| (unnamed) | source | `DllMain` never runs plugin shutdown or unload (it holds the loader lock). | `src/runtime/lifecycle/dllmain.cpp` | rank 3: verify (sensor "Plugin detach teardown boundary", which names no rule) |
| N43 | source | The boot log tee (`nevr_boot_log_tee`) deliberately opens and writes its file during initialization, through kernel32 calls only (no CRT locks), so it stays safe if a caller ever holds the loader lock; initialization runs from the host's wrapper detour, not from `DllMain`, so the addendum's no-file-I/O-from-DllMain rule does not reach it. | `src/runtime/log/boot_log_tee.h`, `src/runtime/log/boot_log_tee.cpp`, `src/runtime/lifecycle/initialize.cpp` | rank 3: verify (N43: `boot_log_tee.cpp` has no CRT file call and opens and writes its file with `CreateFileA` and `WriteFile`); unchecked: that the tee is never entered with the loader lock held in a run |
| N59 | source | Initialization calls `PatchDscProvider()` (the PSN/??? to DSC provider-string rewrite). | `src/runtime/lifecycle/initialize.cpp` | rank 3: verify |
| N62 | source | The signal-delivered shutdown never takes the loader lock: its dependencies are resolved once at init (`ResolveShutdownDependencies`), nothing is resolved at shutdown, and the signal-context flag is `volatile sig_atomic_t`. | `src/runtime/lifecycle/crash_recovery.cpp`, `src/runtime/lifecycle/boot.cpp` | rank 3: verify |
| N63 | run | A second signal during shutdown calls `ForceFatalExit`; it never sleeps and returns. | `src/runtime/lifecycle/crash_recovery.cpp` | unchecked: nothing sends a second signal; rank 3: verify |
| N38, N64, N105 | run | Graceful shutdown releases the bridge listener by a direct call to `StopWebSocketBridgeListener()` before `ForceFatalExit`, so the listening socket does not outlive the process as a zombie. | `src/runtime/server/gameserver.cpp`, `src/runtime/lifecycle/crash_recovery.cpp`, `src/runtime/lifecycle/crash_recovery.h`, `src/runtime/compat/ws_bridge.cpp` | unchecked: nothing observes the listening socket released at shutdown; rank 3: verify |
| N67 | source | The crash flags `g_crashReporterSuppressed` and `g_justSuppressedCrash` are `std::atomic<bool>`: written from any thread, read from the faulting one. | `src/runtime/lifecycle/crash_recovery.cpp` | rank 3: verify |
| N69 | source | The stack-overflow handler has room to run: `SetThreadStackGuarantee` at VEH install and `EnsureStackReserve()` on the thread that runs each frame. | `src/runtime/lifecycle/crash_recovery.cpp`, `src/runtime/frame/tick.cpp` | rank 3: verify |
| N70 | source | The crash path (`BreakpointVEH`, `WriteCrashDump`) never calls `Log()` and never enumerates modules: it writes with `VehPrintf` (raw `WriteFile`) and reads the module table cached at init by `CacheModuleTable()`. | `src/runtime/lifecycle/crash_recovery.cpp` | rank 3: verify |
| N71 | source | `BreakpointVEH` has a generic null-pointer access-violation branch and a table of at least 25 known session-flags null-dereference sites; those game functions are deliberately not hooked. | `src/runtime/lifecycle/crash_recovery.cpp`, `src/runtime/lifecycle/crash_recovery_sites.h` | rank 2: `src/runtime/tests/test_crash_recovery.cpp` (the site table and its lookup); rank 3: verify |
| N82 | source | Only the vectored handler in `crash_recovery.cpp` acts on `EXCEPTION_BREAKPOINT`; every other one returns `EXCEPTION_CONTINUE_SEARCH` for it. | `src/runtime/lifecycle/crash_recovery.cpp` | rank 3: `tools/verify_hook_invariants.py` |
| N87 | run | Under Wine, CTRL+C arrives as `CTRL_C_EVENT` at console control handlers, not at the CRT `SIGINT` handler; the runtime keeps its handler at the front of the chain and re-arms it after the game installs its own. | `src/runtime/lifecycle/crash_recovery.cpp`, `src/runtime/lifecycle/crash_recovery.h`, `src/runtime/server/gameserver.cpp` | rank 2: `src/runtime/tests/test_crash_recovery.cpp` (`ConsoleCtrlPolicy`: when the handler defers); rank 3: `tools/tests/test_runtime_lifecycle_invariants.py`; unchecked: the re-arm in a run under Wine |
| N125 | source | The game-loop `setjmp` on `g_gameLoopJmpBuf` and its `longjmp` both live in `crash_recovery.cpp`; the host owns the one hook on the game's main wrapper, the runtime hands the host `RuntimeGameMain` as its `game_main`, and its `initialize` calls `ArmGameMain` (HookGuard records the host's hook from the bytes the host wrote). | `src/runtime/lifecycle/crash_recovery.cpp`, `src/runtime/patch/mode_patches.cpp`, `src/runtime/lifecycle/runtime_entry.cpp` | rank 3: verify |
| N146 | run | Before the original `PreprocessCommandLine` call only minimal state is set up (no modules, file I/O or bridge), because the client's D3D initialization runs inside that call. The one exception is the early plugin pass: `LoadPlugins(PluginPhase::Early)` runs once, before the original call, and loads only the `config.yaml` entries marked `early: true` (none marked: it loads nothing), so a plugin that serves game data from elsewhere is in place before the game reads its manifests and packages; the normal pass loads the rest after the graphics device exists, and a pointer from `get_plugin_info` stays valid across both passes (`g_plugins` has stable element addresses). The spectator-stream check inside `PreprocessCommandLine` is patched by `PatchSpectatorStreamCheck`, prologue-validated and logged; on a server it is written once, by `PatchEnableServer` in the deferred bootstrap after the first original call, never before that call (written before it, the check sets the spectator flow, in which the game begins multiplayer a second time, after the state machine's begin). `BugSplat64.dll` links at the stock image base `0x180000000`, so the game's graphics DLLs load. The matchmaker bridge port is chosen at run time and patched into `pnsradmatchmaking.dll`'s compiled-in host (`ApplyMatchmakingLoad`), never a fixed port. | `src/runtime/lifecycle/boot.cpp`, `src/runtime/patch/mode_patches.cpp`, `src/runtime/CMakeLists.txt`, `src/runtime/compat/ws_bridge.cpp`, `src/runtime/patch/pnsrad_enabler.cpp`, `src/runtime/ext/plugin_loader.cpp` | rank 2: `src/runtime/tests/test_behavioral.cpp` (`InfoPointerStaysValidWhilePluginsAreAddedWithoutAPlan`, `InfoPointerKeptInTheEarlyPassStaysValidAfterTheNormalPass`), `src/runtime/tests/test_checked_patch.cpp` (the spectator-stream patch); rank 3: `tools/tests/test_spectatorstream_order.py`, `tools/tests/test_plugin_loader_storage.py` (the early-pass latch, the N146 text, the storage), `tools/tests/test_pnsrad_load_boundary.py` (the host patch runs on every load, at the module's `LoadLibrary` return); unchecked: the boot-state limit, the image base |
| N13 | run | CTRL+C from a terminal must reach the game's process group (Wine started with `WINEDEBUG=-all`, not behind a pipeline), and the signal and console handlers call `PerformGracefulShutdown` directly, never through a flag checked per frame (the frame loop stops before the flag is read, and the listening socket survives as a zombie; see N38). | the launch scripts, `src/runtime/lifecycle/crash_recovery.cpp` | unchecked: a manual run (noted in `src/runtime/tests/test_xpid_patch.cpp`) |

#### Server, bridge and login

| Rule | Kind | Statement | Guards | Checked by |
| --- | --- | --- | --- | --- |
| N1 | run | `ParseEndpoint` never throws on malformed input: it parses with `strtoul`, range-checks to `UINT16_MAX` and logs a warning. | `src/runtime/server/messages.cpp` | unchecked: `src/runtime/tests/test_parse_endpoint.cpp` tests a copy of `ParseEndpoint` defined in the test, not the function in `src/runtime/server/messages.cpp` |
| N2 | run | Packet-encoder settings from the server are validated (`ValidatePacketEncoderSettings`) before they are forwarded to the game. | `src/runtime/server/messages.cpp` | unchecked: nothing names it |
| N14 | run | A `???-` platform prefix must not reach an XPID: the game caches EULA acceptance per platform prefix, so `???-` breaks the cache lookup (`XPID_PLATFORM_FALLBACK_PREFIX`). | `src/runtime/hook/addresses.h`, `src/runtime/patch/xpid_patch.cpp` | unchecked: nothing names it |
| N20 | source | No account or Discord ID literal is compiled in. Identity comes from the presented credential, or from the `config.yaml` Discord-ID fallback, which applies in client mode too (not gated on `g_isServer`). | `src/runtime/compat/ws_bridge.cpp`; `src/runtime`, `src/modules`, `src/abi`, `src/core` for literals | rank 3: verify |
| N34, N103 | source | `src/gameserver/` does not exist and must not come back; the server code is `src/runtime/server/`. | the tree | rank 3: verify, `tools/verify_doc_paths.py` |
| N37, N39 | run | ixwebsocket cannot set `SO_REUSEADDR`, so the bridge listener picks a random ephemeral port and retries the bind. | `src/runtime/compat/ws_bridge.cpp` | unchecked: nothing drives a failed bind; rank 3: verify |
| N51 | run | An access token read from disk is clamped to a short lifetime; a freshly issued token keeps its own `exp`. | `src/core/auth_token_model.h` | unchecked: nothing names it |
| N54 | run | When the login connection disconnects, its callback is cleared, so nothing calls into a freed `ProxyPair`. | `src/runtime/compat/ws_bridge.cpp` | rank 2: `src/runtime/tests/test_behavioral.cpp` |
| N60 | source | The bridge collects connections under `g_pairsMutex` and calls `stop()` outside it (`stop()` joins callback threads). | `src/runtime/compat/ws_bridge.cpp` | rank 3: verify |
| N61 | run | Each matchmaker connection (index 2 and up) registers its own callback on the shared remote; closing the login pair clears only its own. | `src/runtime/compat/ws_bridge.cpp` | rank 2: `src/runtime/tests/test_behavioral.cpp`; rank 3: verify |
| N85 | source | Never hand ixwebsocket an empty callback (`setOnMessageCallback(nullptr)`); use a no-op lambda. Callbacks have an exception boundary. | `src/runtime/compat/ws_bridge.cpp` | rank 3: verify |
| N91 | run | One login per login session, and a game socket's role is what it asked for, not when it connected. The socket that takes the login slot injects the LoginRequest; a further socket that asks for `/login` while a login session exists shares that session and injects nothing. The redirect gives the config and login services the request targets `/config` and `/login` on the bridge address; the Open handler reads the target of every socket (`openInfo.uri`), then the listener (the matchmaker listener makes a matchmaker), then the connect order over the slots no tagged socket is expected to take; a first frame that contradicts the role is logged. The login-injection line is logged at Info with the XPID (`docs/standards/logging.md` Rule 2). | `src/runtime/compat/ws_bridge.cpp`, `src/runtime/compat/socket_role.h`, `src/runtime/lifecycle/service_config.cpp`, `src/runtime/lifecycle/config.cpp` | rank 2: `src/runtime/tests/test_socket_role.cpp`, `src/runtime/tests/test_behavioral.cpp` (`RolesByPathTest`, through the real bridge), `src/runtime/tests/test_service_config.cpp` (the redirect's request targets); rank 3: verify (the Open handler admits by request target; the injection log line), `tools/tests/test_bridge_role_wiring.py` |
| N92 | source | The WebSocket bridge is compiled into `BugSplat64.dll` and started in-process from `boot.cpp`; no ws-bridge module is built or loaded. | `src/runtime/lifecycle/boot.cpp`, `CMakeLists.txt` | rank 3: verify |
| N102 | run | A dedicated server that ServerDB refused, that has no token, or that cannot begin multiplayer (a changed prologue of `QueueGameThreadCall` or `BeginMultiplayerLobby`, checked once at boot by `ValidateServerBringup`; a net game not logged in when the queued begin runs) exits with a cause through `ServerFatal` (mode-gated), not `FatalError`. | `src/runtime/server/gameserver_callbacks.cpp`, `src/runtime/server/gameserver_serverdb.cpp`, `src/runtime/lifecycle/state_machine.cpp`, `src/runtime/lifecycle/server_bringup.h` | rank 2: `src/runtime/tests/test_server_bringup.cpp` (the prologue check; a queued begin in any state but logged in); rank 3: verify, `tools/tests/test_server_bringup_wiring.py`; unchecked: a server that ServerDB refused, or that has no token, exits (verify finds the `ServerFatal` call sites; nothing drives such a server) |
| N106 | run | A server exchanges its refresh token for an access token (`HasValidRefreshToken()` then `RefreshAuthToken`); without it every registration falls back to password auth. | `src/runtime/server/gameserver_serverdb.cpp` | unchecked: nothing drives a server's exchange (`src/runtime/tests/test_token_auth.cpp` drives the token module's refresh, not the server's call); rank 3: verify |
| N149 | run | A server whose login cannot be made stops with the cause, as its one default-level line (`ServerFatal`'s own): a server with `auth.password` and no `identity.discord_id`, or with no `auth.password` and no token from its device sign-in (#1532: token-auth prints the code and the page on standard error, waits, asks for a new code when one runs out, and gives up only after three failed requests in a row), stops before any connection to the community service (`PrepareRemote`, no remote is made, no account 0 and no unauthenticated session is sent), and a `ServerFatal` waits for a key on an interactive console and 2 s otherwise before it exits (`host_pause.h`); a 401 or 403 on the remote upgrade stops it with the status (an outage, 5xx or 404, is not a refusal). The line names what is missing or refused and the config that was loaded (the path, or `none found, searched ...`, `NevrCfgSource`). A client does not stop: it says a refusal once per refused sign-in (the token the remote presents) and ends the later login sockets of that sign-in without a trip to the service. The legacy path and the router path (`RemoteHooks::refused`) apply the same rule. | `src/runtime/compat/login_refusal.h`, `src/runtime/compat/ws_bridge.cpp`, `src/runtime/compat/ws_session_transport.cpp`, `src/runtime/lifecycle/service_config.cpp` | rank 2: `src/runtime/tests/test_login_refusal.cpp`; rank 2: `src/runtime/tests/test_behavioral.cpp` (`RefusedLogin*`, legacy and router, through a real service that refuses the upgrade; the no-password cases with and without a device token), `src/runtime/tests/test_token_auth.cpp` (`ServerSignInWait`, `DeviceAuthFlow` server cases); rank 3: `tools/tests/test_login_refusal_wiring.py` |
| N112 | run | The client login and the server registration carry the build identity (`nevr_identity`, from `nevr_build_identity::Get()`) and the plugin manifest (`nevr_plugins`, from `BuildPluginManifestJson()`). | `src/core/build_identity.h`, `src/core/build_identity.cpp`, `src/runtime/compat/login_profile.cpp`, `src/runtime/server/gameserver_callbacks.cpp`, `src/runtime/ext/plugin_loader.cpp` | rank 2: `src/runtime/tests/test_behavioral.cpp` (the client login profile: `nevr_identity`, `nevr_plugins`); rank 3: verify (N112a-d); unchecked: the server registration payload (`server/gameserver_callbacks.cpp` is not compiled into `test_behavioral`) |
| N115 | source | The login's `system_info` block is measured (`nevr_system_info::Get`), never a literal. | `src/runtime/compat/ws_bridge.cpp` | rank 3: verify |
| N123 | run | The login display name is the token-auth username (`TokenAuth_GetUsername`), falling back to the account id; never a placeholder. | `src/runtime/compat/ws_bridge.cpp`, `src/runtime/compat/login_profile.cpp`, `src/modules/token-auth/src/token_auth.cpp` | rank 2: `src/runtime/tests/test_behavioral.cpp` (`LoginProfile.EmptyDisplayNameFallsBackToTheAccountId`); rank 3: verify |
| N131 | source | The asset CDN starts only on a client: in `boot.cpp` inside `if (!g_isServer)`, never in `initialize.cpp` (where `g_isServer` is not yet set). | `src/runtime/lifecycle/boot.cpp`, `src/runtime/lifecycle/initialize.cpp` | rank 3: verify |
| N132 | source | `just proto` generates from one pinned BSR commit, matching nevr-stream's Go module revision (as `justfile` says); bump it only with the schema. | `justfile` (`proto`) | none |

#### Config and plugins

| Rule | Kind | Statement | Guards | Checked by |
| --- | --- | --- | --- | --- |
| N68 | run | Plugins and modules tick every frame (`TickPlugins`, `TickModules`) and hear every state change (`NotifyModulesStateChange`). | `src/runtime/frame/tick.cpp`, `src/runtime/lifecycle/state_machine.cpp` | rank 2: `src/runtime/tests/test_behavioral.cpp` (`N68_PluginTickTest`, `N68_ModuleTickTest`); rank 3: verify |
| N75 | run | Plugins load with a restricted DLL search path, so a DLL dropped next to `echovr.exe` cannot satisfy a dependency first. | `src/runtime/ext/plugin_loader.cpp` | rank 3: verify; unchecked: how a plugin's dependencies resolve (no check loads a plugin with a planted dependency, and an unsigned-allowed run falls back to the default search order when the OS rejects the flags) |
| N89 | run | The loader refuses a plugin whose function is built in (`log_filter.dll`). | `src/runtime/ext/plugin_loader.cpp` | rank 2: `src/runtime/tests/test_behavioral.cpp`; rank 3: verify |
| N95 | source | `log-filter` is neither built nor packaged. | every tracked CMake file, `CMakeLists.txt` | rank 3: verify |
| N114 | source | The plugin capability channel stays wired: the loader resolves `NvrPluginGetCapabilities` and `NvrPluginInitEx`, and `NEVR_PLUGIN_API_VERSION` is 8. | `src/runtime/ext/plugin_loader.cpp`, `sdk/include/nevr/plugin_interface.h` | rank 3: verify |
| N135 | run | A plugin logs through the host: every context the loader passes a plugin (init, per-frame, state change) carries that plugin's `plugin_id` (its `config.yaml` entry's name) and the host's `log` function (`HostPluginLog`), whose line is `[NEVR.PLUGIN] plugin=<id> <message> <key>=<value> ...` through `Log()`; a plugin's text cannot start a second line or overrun the boot buffer (`plugin_log.h`); `plugin_logger.h` holds the lines a plugin logs before its bind and writes them through the host at the bind, in order; stderr only on a host older than v6 or a context with no log function, and that bind says so there. | `sdk/include/nevr/plugin_interface.h`, `src/runtime/ext/plugin_loader.cpp`, `src/runtime/ext/plugin_log.h`, `sdk/include/nevr/plugin_logger.h` | rank 2: `src/runtime/tests/test_behavioral.cpp` (`PluginLogApi`, `PluginLoaderDiagnosticTest`), `src/runtime/tests/test_plugin_log.cpp`, `src/runtime/tests/test_plugin_logger.cpp`; rank 3: `tools/tests/test_runtime_lifecycle_invariants.py` |
| N120 | run | On a server a degraded runtime does not keep running: the HookGuard verdict is captured and escalated to `ServerFatal`, `platform_compat` fails a server run on a missing TLS hook, and the XPID patch logs both outcomes and escalates a validation failure, a failed byte write and a failed `GetProviderPrefix` (OVR-ORG) detour alike (`g_bootHookFailed` with a cause, so `boot.cpp` refuses the server), and the server bring-up prologues are validated at boot (`ValidatePrologue`, N97) with a mismatch escalated to `ServerFatal` (see N102). | `src/runtime/ext/plugin_loader.cpp`, `src/modules/platform-compat/src/platform_compat.cpp`, `src/runtime/patch/xpid_patch.cpp`, `src/runtime/lifecycle/state_machine.cpp`, `src/runtime/lifecycle/boot.cpp` | rank 2: `src/runtime/tests/test_behavioral.cpp` (`XpidPatchBoot`, `XpidPrefixDetourBoot`), `src/runtime/tests/test_server_bringup.cpp`; rank 3: verify, `tools/tests/test_server_bringup_wiring.py`; unchecked: the HookGuard verdict's escalation and `platform_compat`'s TLS-hook failure on a server (verify only) |
| N133 | source | Every nEVR config key is read from `config.yaml` (`nevr_config`), never from the game's JSON; the flat key map has 36 entries; modules read config through `NvrModuleContext.config_get`, and a module reports its API version; the sample config has the `auth`, `services`, `identity` and `version` sections. | `src/runtime/lifecycle/config.cpp`, `src/runtime/lifecycle/service_config.cpp`, `src/core/nevr_config.h`, `src/modules/token-auth/src/token_auth.cpp`, `docs/reference/example-config.yaml` | rank 2: `src/runtime/tests/test_behavioral.cpp`; rank 3: verify (N133 S7a-d) |
| N134 | run | The plugin set is the ordered `plugins:` list in `config.yaml`, and a required plugin that fails to load is a failure (S6); plugins get `ctx_size` and the plugin-query API, and load in capability-priority order (S8). | `src/runtime/ext/plugin_loader.cpp`, `src/runtime/ext/plugin_loader.h`, `src/runtime/ext/plugin_load_plan.h`, `src/runtime/lifecycle/service_config.cpp` | rank 2: `src/runtime/tests/test_behavioral.cpp`, `src/runtime/tests/test_plugin_load_plan.cpp`; rank 3: verify (N134 S8, N114 S7) |
| N147 | run | A run loads unsigned plugins unless it is started with `-require-signed-plugins`, which makes it signed-only (release-signer does not sign plugins yet; once it does, the default flips to signed-only in a separate change). `PreflightRuntimeBootstrap` reads the argument before the early plugin pass and sets the mode once (`nevr_plugin_trust::ApplyCommandLine`: any token the game would read as the argument counts, by the game's tokenizer and name rule in `cmdline_strip.h` over `GetCommandLineA()` and by `CommandLineToArgvW` over `GetCommandLineW()`, and either finding it makes the run signed-only; a command line it cannot read, or whose reading throws, makes the run signed-only too, and the mode line says so at Warning), so the early pass, the normal pass and the late-load filter read one `ActiveMode()`; no config key or environment variable sets it. The loader states the mode in effect once per run (`[NEVR.PLUGIN] plugin trust: mode=<signed-only\|unsigned-allowed> source=<default\|-require-signed-plugins\|command line unreadable> build=<release\|dev>`), and in an unsigned-allowed run an unsigned plugin loads with a Warning per plugin. A signed-only run loads only plugins signed through the release-signer pipeline: `StageOne` verifies the file (`VerifyPluginFile`: Authenticode digest, `messageDigest` bound to the signed content, chain to the pinned Microsoft root through an exclusive-root engine at the stamped time, whole-OID profile EKU, bound RFC 3161 token, denylist) on a handle held `FILE_SHARE_READ` across the load, and loads that handle's final path, so a link in the configured path cannot lead the load to another file, with no default-search fallback; a plugin's imports, delay-load imports and forwarded exports pass the same check transitively (`VerifyPluginTree`, held open across the load; a name gets `.dll` when it has no extension, and one that is neither a file in the plugins directory nor supplied by the system is refused, an absent delay-load excepted: the late-load filter checks it when it is called; a name only System32 supplies is loaded from System32 by its full path before the plugin, so a file planted in the plugins directory after the check cannot take its place), and so does every later `LoadLibrary*` of a file in that directory, one made from a staged plugin's `DllMain` included, even by the path being loaded (the loader's own load of the plugin it checked is the one load not checked twice, and only while the loader lock is not held) (a `DllLoadHook` filter: the name gets `.dll` when it has no extension, a relative path with a directory part (`\` or `/`) is refused whatever it leads to (the loader appends it to every directory it searches, each through links, so no candidate list is complete; a bare name and a full path keep their rules), the directory is the ANSI path converted through the code page; refused with `ERROR_MOD_NOT_FOUND`; a path it accepted is loaded by the final path of the file it checked and holds, through a rewriter the hook asks right after the filters (`AddLoadRewriter`), not by the caller's text, so a link repointed after the check cannot lead the load to another file; a bare name is left to the OS search and an unsigned-allowed run loads what the caller named); a refusal is a manifest entry, a log line and, on a client with graphics only, one dialog per run (`plugin_refusal_notice.h`, never on a server or headless run; a refusal after `LoadPlugins` finished shows it itself), and for a required plugin `ServerFatal`. The dev build (`mingw-plugin-dev`, `NEVR_ALLOW_UNSIGNED_PLUGINS`) carries `NEVR-PLUGIN-TRUST dev`, a release `NEVR-PLUGIN-TRUST release` (the literal names the kind of build, not the mode of a run), and `tools/package_release.py` refuses to package anything but a release. The profile EKU pin is confirmed against the real signed v5.4.3 `BugSplat64.dll` (`RealSigned.*` in `test_plugin_trust`, which `just test-auth-unit` feeds from `tools/fetch_real_signed_dll.py`); the signer subject is deliberately not pinned. A signed-only run refuses an unsigned plugin. | `src/runtime/ext/plugin_signature.cpp`, `src/runtime/ext/plugin_trust.cpp`, `src/runtime/ext/plugin_loader.cpp`, `src/runtime/ext/plugin_tree.cpp`, `src/runtime/ext/plugin_refusal_notice.h`, `src/runtime/hook/dll_load_hook.cpp`, `src/runtime/lifecycle/boot.cpp`, `src/runtime/lifecycle/cli.cpp`, `src/core/cli_flags.h`, `tools/package_release.py` | rank 2: `src/runtime/tests/test_plugin_signature.cpp`, `src/runtime/tests/test_plugin_trust.cpp`, `src/runtime/tests/test_plugin_tree.cpp`, `src/runtime/tests/test_behavioral.cpp` (`PluginSignatureGateTest`, `PluginDependencyGateTest`, `PluginRefusalNotice`, `PluginRunModeTest`: an unsigned plugin loads with no argument and is refused with `-require-signed-plugins`, on the release-configured build; `PluginLateLoadTest` and `PluginLateLoadAliasTest`: the late-load spellings; `PluginDependencyGateTest.ALateLoadUnderANonAsciiGamePathIsRefused`: the non-ASCII game path; `PluginDllMainLoadTest`: the `DllMain` load; `PluginDependencyGateTest.ALateRefusalAfterLoadPluginsShowsTheNoticeItself`: the late notice), `tools/tests/test_package_release.py`; rank 3: `tools/tests/test_plugin_trust_run_mode.py` (the argument is read before the early pass, registered with the game's parser and listed in the README), `tools/tests/test_build_identity_literal.py` (the plugin-trust literal in the built runtime) |
| N150 | run | The verifier's chain engine trusts the pinned root alone, at either struct size: `CreatePinnedChainEngine` makes it with `CERT_CHAIN_ENGINE_CONFIG.cbSize = sizeof` (88 on x64) and, only when that fails with `E_INVALIDARG` (Wine before 11.5 knows the struct without `dwExclusiveFlags` and refuses 88), once more at `offsetof(dwExclusiveFlags)` with the same `hExclusiveRoot`; `dwExclusiveFlags` stays 0, so the pin uses nothing the shorter struct lacks. Any other error, or a second failure, is a refusal that names both errors; a retry is never "skip the check". `runtime_load` records `verify_engine_cbsize` for an accepted file. | `src/runtime/ext/plugin_signature.cpp` | rank 2: `src/runtime/tests/test_plugin_signature.cpp` (`PluginSignatureEngine`: the retry is taken once on `E_INVALIDARG` only; 14 wrongly signed fixtures are refused for the same reason on both paths), and the suite run under a Wine 11.0 (Proton Experimental), where the retry is the real path |

#### Hooks and binary patches

| Rule | Kind | Statement | Guards | Checked by |
| --- | --- | --- | --- | --- |
| N6, N65 | source | Headless mode patches the game's own gates (the D3D12 init gate is forced to the game's graphics-disabled branch, prologue-validated). `HEADLESS_GATE_TABLE` is the single list of gates; the install loop reads it. | `src/runtime/hook/addresses.h`, `src/runtime/patch/mode_patches.cpp` | rank 2: `src/runtime/tests/test_xpid_patch.cpp`, `src/runtime/tests/test_behavioral.cpp`; rank 3: `tools/verify_mode_patch_ground_truth.py` |
| N7, N8, N9, N10 | source | N8 and N9 are two of the headless gates (`HEADLESS_RENDER_SUBMIT_INIT`, `HEADLESS_RENDER_SETUP`); `Finished initializing engine` is the witness that a headless boot completed. | `src/runtime/hook/addresses.h` | rank 2: `src/runtime/tests/test_xpid_patch.cpp`; rank 3: verify (N77 keeps the witness line) |
| N22 | run | The peer-timeout bug in the game (unsigned subtraction of a wrapped timestamp) is fixed by the overflow-safe `GetTimeMilliseconds` replacement. | `src/runtime/hook/addresses.h`, `src/runtime/patch/binary_bug_fixes.cpp` | unchecked: nothing names it |
| N25, N28 | source | A hook in `binary_bug_fixes.cpp` validates the expected bytes before it hooks. | `src/runtime/patch/binary_bug_fixes.cpp` | none (the Guardrails rule) |
| N26 | source | The `CPrecisionSleep::Wait` hook is that function's only owner; nothing chains on it. | `src/runtime/patch/binary_bug_fixes.cpp` | none |
| N31 | run | A value read from a fixed game global (the spin-wait limit, the server's step slot) is bounds-checked, to catch a misread after the binary changes version; the server tick rate is range-checked before the step is written, and the write is read back. | `src/runtime/patch/binary_bug_fixes.cpp`, `src/runtime/lifecycle/config.cpp`, `src/runtime/lifecycle/server_timestep.h` | rank 2: `src/runtime/tests/test_server_timestep.cpp` (the step slot); unchecked: the spin-wait limit |
| N32 | source | A caller count in `addresses.h` is a dated snapshot with the command that refreshes it; refresh it before relying on it. | `src/runtime/hook/addresses.h` | none |
| N33, N113 | run | Exactly one writer patches `CPrecisionSleep::BusyWait`; it validates the prologue, saves the original byte and restores it at shutdown. | `src/runtime/patch/binary_bug_fixes.cpp` | rank 3: verify; unchecked: the restore at shutdown |
| N66 | run | `FormatSymbolId` returns 0 for a non-positive length or a null buffer. | `src/runtime/hook/symbol_corpus.cpp`, `tools/gen_symbol_corpus.py` | rank 2: `src/runtime/tests/test_behavioral.cpp`; rank 3: `tools/tests/test_symbol_corpus_template.py` |
| N83 | source | A hooked address is named for what the binary says it is (`0x140F80ED0` is `CBroadcaster::Listen`); an address called through a live function pointer is not also detoured (self-collision); the bytes at each hooked address match the identity manifest. | `src/runtime/patch/mode_patches.cpp`, `src/runtime/hook/addresses.h`, `tools/hook_identity_manifest.json` | rank 3: `tools/verify_hook_invariants.py` |
| N84 | source | `PatchDetour` records each hooked address with HookGuard and its `name` parameter is required; the loader re-verifies hooked addresses after each plugin's init (`VerifyAll`). | `src/runtime/hook/patching.h`, `src/runtime/hook/hook_guard.h`, `src/runtime/ext/plugin_loader.cpp` | rank 2: `src/runtime/tests/test_behavioral.cpp` (`N84_HookGuard`); rank 3: verify (N84, C2), `tools/verify_hook_invariants.py` |
| N86, N110 | run | Per-frame work runs through exactly one dispatcher, `DispatchPerFrameWork`, called from a hook that is live in server mode, with a re-entrancy gate and the host flag taken from `g_isServer`; it holds the only two production `Tick*` call sites. Every declared HookLiveness id is marked somewhere. | `src/runtime/frame/tick.cpp`, `src/runtime/patch/binary_bug_fixes.cpp`, `src/runtime/hook/hook_liveness.h` | rank 3: verify; unchecked: that the hook fires in a server run |
| N96 | source | No symbol is spelled plain `ResolveVA`; use `ResolveVA_Checked` or `ResolveVA_Unchecked`. | `sdk/include/nevr/pc/nevr_common.h`, `src` | rank 3: verify |
| N97 | source | There is exactly one `ValidatePrologue`, in `nevr_common.h`. | `sdk/include/nevr/pc/nevr_common.h` | rank 3: verify |
| N99 | run | `-server` applies the game's own headless mask (`ENGINE_FLAGS_HEADLESS_MASK`, `0xFFFEFEFE`), and boot never tells the operator a flag is redundant. | `src/runtime/patch/mode_patches.cpp`, `src/runtime/lifecycle/boot.cpp` | unchecked: nothing drives `-server` or reads boot's lines; rank 3: verify |
| N126 | run | `PatchDetour` logs a failed hook at Warning; a failed hook is never silent. | `src/runtime/hook/patching.h` | unchecked: nothing drives a failed `PatchDetour`; rank 3: verify |
| N127 | run | The Oculus-SDK block reports its real result, never an unconditional success line. | `src/runtime/patch/mode_patches.cpp` | rank 3: verify; unchecked: the result line in a run (`tools/winvm/checks.py` matches `blocking hooks: LoadLibraryW=...`, which `PatchBlockOculusSDK` no longer logs) |
| N128 | run | `Hooking::Attach` records which MinHook stage failed and its `MH_STATUS`; `PatchDetour`'s failure log includes it. | `src/core/hooking.h`, `src/runtime/hook/patching.h` | rank 2: `src/runtime/tests/test_hooking.cpp` (the failed stage and its name); rank 3: verify; unchecked: `PatchDetour`'s failure line |
| N129 | run | `DllLoadHook` reports the `MH_STATUS` of each `LoadLibrary` hook that fails, with no accumulate-and-hide `ok &=`. | `src/runtime/hook/dll_load_hook.cpp` | rank 2: `src/runtime/tests/test_dll_load_hook.cpp` (`DllLoadHookInstall`); rank 3: verify |
| N148 | run | The in-game CREDITS page shows the project section above Ready At Dawn's credits, which stay byte-for-byte whole: `credits_text::Install` (client only, called once from `boot.cpp`, the server skip logged) builds block + the game's own bytes copied from memory + NUL, and points the two `LEA` loads of the credits blob at it, or neither (both sites, the blob length and its SHA-256 are checked before any write; the first site is put back if the second write fails). It is a byte patch of a data-blob load, not a function, so there is nothing to hook; no text of Ready At Dawn's is stored. The block is rendered at build time from `config/credits.json` by `tools/render_credits.py --ingame-source` (lines of at most 64 bytes, drawn only from printable ASCII and `©` (the characters the game's font draws), no `\|`, at most 200 lines; a name or heading with any other character, or a file the page cannot show, fails the build, naming the entry and the field). | `src/runtime/patch/credits_text.h`, `src/runtime/patch/credits_text.cpp`, `src/runtime/lifecycle/boot.cpp`, `src/runtime/CMakeLists.txt`, `tools/render_credits.py` | rank 1: the live run on a game copy; rank 2: `src/runtime/tests/test_credits_text.cpp`, `tools/tests/test_credits.py` (`IngameText`); rank 3: `tools/tests/test_credits_text_wiring.py` (the call, its gate, the build step, who names the sites) |
| N151 | source | The Quest sentinel changes no code page at run time: `libr15.so` is patched on the host (`docs/adr/0021-quest-libr15-patched-at-build-time.md`), and the sentinel only stores data into it. Four scans over `src/quest` outside the tests, comments stripped: a protection or mapping call (`mprotect`, `mmap`, `mremap`, `PROT_EXEC`, `PROT_WRITE`) appears only in `sentinel/got_hook.cpp` (the GOT's data pages), `plugins/q15t/src/got_swap.cpp` (the q15t GOT slot swap: data pages, and it refuses an executable one), `sentinel/libr15_attach.cpp` (anonymous trampoline pages) and `sentinel/credits_text.cpp` (its anonymous buffer), and the two headers that declare those seams; `InstallInlineThunk`, `WriteWord` and `AllocateInReach` appear nowhere; `credits_text.cpp` makes one protection call, on its own buffer, and names `mprotect` only as that seam's default; the stock build ID constant `kLibR15StockBuildId` is named only by the allowlist of files that need the stock file's identity, so no hook target, in `sentinel/`, `social/` or `integration/`, carries the stock ID where it means the patched one (`sentinel::pinned::LibR15PatchedBuildId()`). | `src/quest/sentinel/got_hook.cpp`, `src/quest/sentinel/libr15_attach.cpp`, `src/quest/sentinel/credits_text.cpp`, `src/quest/sentinel/pinned_targets.h`, `src/quest/sentinel/libr15_identity.cpp`, `src/quest/integration/production_steps.cpp` | rank 3: verify (N151), `tools/tests/test_quest_no_code_writes.py` (the same scans, the justfile's allowlists equal its own, and the sensor block run against copies of the tree with a planted violation each), `tools/tests/test_quest_attach_wiring.py` (the constructor runs `Attach` before the first libr15 step) |
| N72, N73 | source | Guards for the broadcaster plugin, which lives in `nevr-runtime-plugins` (see "Plugins"). | not in this tree | none here |

#### Logging and layout

| Rule | Kind | Statement | Guards | Checked by |
| --- | --- | --- | --- | --- |
| N17 | run | A component that installs several hooks reports each outcome by name, plus one aggregate line at Info. | `src/modules/platform-compat/src/platform_compat.cpp`, `src/runtime/patch/pnsrad_enabler.cpp` | rank 3: verify (N17: `platform_compat.cpp` names its three hooks and logs the aggregate at Info; `pnsrad_enabler.cpp` logs its aggregate at Info); unchecked: that each hook reports its own outcome by name (the sensor reads the aggregate lines only), and that a run emits the lines |
| N18 | run | The built-in filter is validated by a measured suppression ratio (`docs/standards/logging.md` Rule 4); see N79. | `src/runtime/log/builtin_filter.cpp` | unchecked: verify (through N79) finds the summary code, not a measured ratio |
| N45, N81 | source | No bare `[NEVR]` tag on a log line; use a subsystem tag from `docs/standards/logging.md` (`--help` strings in `cli.cpp` are not log lines). | `src/runtime`, `src/abi`, `src/core` | rank 3: verify |
| N47, N94 | source | The `[NEVR.AUTH]` per-step detail lines are Debug, pinned by their text, and the level mapping that carries them is pinned too. | `src/modules/token-auth/src/token_auth.cpp`, `src/core/device_auth_flow.cpp`, `src/core/auth_token_refresh.h` | rank 3: verify |
| N77 | run | nEVR's own lines are exempt from game-noise suppression, and `Finished initializing engine` and `ExitProcess(` are never on the blanket suppress list. | `src/runtime/log/builtin_filter.cpp` | unchecked: nothing drives a nEVR line through the filter; rank 3: verify (the suppress list) |
| N78 | run | A frequent line is collapsed into a counted summary, never deleted; there is exactly one strong definition of `::Log`. | `src/runtime/log/builtin_filter.cpp`, `src/core/logging.cpp` | rank 2: `src/runtime/tests/test_rate_limiter.cpp` (the collapse and its count); rank 3: verify (N78, D1) |
| N79 | run | Filter health is emitted periodically while the process runs, not only at shutdown. | `src/runtime/log/builtin_filter.cpp`, `src/runtime/frame/tick.cpp` | unchecked: nothing runs the filter over time; rank 3: verify |
| N88 | source | A monitor does not depend on the thing it monitors: filter health is driven from the per-frame tick, not only from the hook it watches (`docs/standards/verification.md`, "A Monitor Must Not Depend On What It Monitors"). | `src/runtime/log/builtin_filter.cpp`, `src/runtime/frame/tick.cpp` | rank 3: verify (through N79) |
| N80 | run | Both log writers stamp the same per-process run ID. | `src/core/logging.cpp`, `src/runtime/log/boot_log_tee.cpp` | unchecked: nothing compares the two writers' records; rank 3: verify |
| N90 | run | pnsrad.dll's own `CLog::PrintfImpl` is hooked, so its output goes through the filter; the hook is installed from the live tick. | `src/runtime/log/builtin_filter.h`, `src/runtime/log/builtin_filter.cpp`, `src/runtime/frame/tick.cpp` | rank 3: verify; unchecked: that the hook installs and routes pnsrad output in a run |
| N108 | source | `src/abi/` never includes `src/core/`. | `src/abi` | rank 3: verify |
| N109 | source | `src/runtime/CMakeLists.txt` does not put its own source directory on the include path. | `src/runtime/CMakeLists.txt` | rank 3: verify |
| N122, N124 | run | A config or mode decision logs its outcome at Info even when nothing happens (UPnP key and mapping, telemetry disabled, the server bring-up prologue check). | `src/runtime/lifecycle/config.cpp`, `src/runtime/server/upnp.cpp`, `src/runtime/server/gameserver_serverdb.cpp`, `src/runtime/server/gameserver_telemetry.cpp`, `src/runtime/lifecycle/state_machine.cpp` | rank 3: `tools/tests/test_server_bringup_wiring.py` (the bring-up prologue outcome is logged at Info), `tools/tests/test_netgun_bullets_wiring.py` (the server-mode branch logs at Info); unchecked: the UPnP and telemetry decision lines, which no verify sensor or test names (`tests/smoke/score-log.sh` reads a captured run by hand) |

#### The game folder

| Rule | Kind | Statement | Guards | Checked by |
| --- | --- | --- | --- | --- |
| (standing rule) | run | Nothing of ours is written to the game install except `bin\win10\BugSplat64.dll`, the host the game loads: no `config.yaml`, `_local`, `_temp`, logs, install record, set-aside or backup file (`.original`, `.legacy`, `.migrated`), `plugins` folder or MOVED-TO note. All of it lives under `%LOCALAPPDATA%\nevr\echovr` (the install record at `installer\nevr-install.txt`, every set-aside file in `installer\aside`). A snapshot of the install root taken before and after install, an update, a client run, a `-server` run and uninstall may differ only in the content of the host, and after uninstall not at all. | `installer/src/pc.rs`, `tools/package-release/install.ps1`, `tools/package-release/uninstall.ps1`, and every write the runtime makes at run time | rank 2: `tools/tests/test_installer_scripts.py` (`GameFolderInvariantTest`: install, update and uninstall through the real core, in the Python suite verify runs); rank 3: `tools/tests/test_game_folder_invariant.py` (the snapshot library); unchecked: the client-run and `-server`-run steps, which are filled in with the commands in `NEVR_INVARIANT_CLIENT_RUN` and `NEVR_INVARIANT_SERVER_RUN` (`tools/game_folder_invariant.py`) and print NOT RUN until they are, so nothing checks the runtime's own writes yet (`docs/reference/launcher-install-root.md` lists what still writes there) |

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
- **Remove a worktree with `just remove-worktree <absolute path>`** (`tools/remove_worktree.py`; `--dry-run` proves and changes nothing; run it from the main checkout or another worktree, never from inside the one it removes). Before it changes anything it refuses when a process has its cwd inside (a shell of your own session included), when anything is uncommitted or ignored beyond build output, when an initialised submodule has uncommitted work or a commit that is on no remote, or when a plain copy under `extern/` (what `just worktree-setup` makes) differs from the main checkout's. It runs no `git submodule deinit` and no `--force`, and leaves the shared submodule config as it found it. `tools/reap_merged.py` removes through it. A check that only prints, followed by a removal, is not the check.

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
just worktree-setup            # Make a fresh git worktree buildable (copies extern/ and gen/ from the main checkout, seeds build/<preset> from the main checkout's when it was built at this commit)
just install-hooks             # Once per clone: link the tracked post-checkout hook so a new linked worktree gets the files .worktreeinclude lists (.mcp.json, .claude/settings.local.json)
just release-preflight           # Refuse unless this clone is exactly origin/main: no local-only tag, no uncommitted or untracked change, nothing unpushed, not behind (read-only; run before any release step)
just package-dev                 # LOCAL dev package from a clean tree: Windows zip + Quest APK in build/package-dev/<sha>/, stamped -dev (a release is only a CI build of a v<x.y.z> tag; never tags or publishes); the Quest APK is signed with the keystore file `NEVR_QUEST_KEYSTORE` names (passphrase in `NEVR_QUEST_KEYSTORE_PASS`)
just reap-merged         # Dry run: worktrees whose work has landed and the proofs each passes or fails; add --apply to remove them (skill: merge-cleanup)
just sign               # Code-sign all DLLs/EXEs in dist/ (requires certs/)
just generate-certs     # Generate CA hierarchy for code signing
```

Every release follows RELEASING.md.

Releases are built by CI from a tag, never locally, and always from a draft (a draft fires no workflow, so the build is started by
hand on the tag ref). The steps, in order, from a clone of origin:

```sh
just release-preflight                                    # 1. refuses (exit 1, one line per problem) unless this clone is exactly origin/main
git tag -a v<x.y.z> -m "<x.y.z>" && git push origin v<x.y.z>   # 2. tag origin's tip with the version `python3 tools/next_release_version.py` prints, and push the tag
just release-preflight && gh release create v<x.y.z> --title v<x.y.z> --draft --prerelease --verify-tag   # 3. once more, then a draft for the pushed tag
gh workflow run build.yml --ref v<x.y.z>          # 4. builds, attests and attaches the two files to the draft; it stays a draft
gh release edit v<x.y.z> --draft=false            # 5. publish, only after the draft's files are checked and signed (RELEASING.md)
```

A release is the tag's version plus GitHub's pre-release flag. The version is exactly the tag (`v5.0.0` is `5.0.0`); every other build
stamps `X.Y.(Z+1)-dev.<N>+<sha>`, which sorts after the tag it descends from and before the next release. The build goes through one
packaging path (the sealed Windows zip, attested, and the Quest library; the release body (a header, a player line and one line per file, no hash) is written after signing; the `.tar.zst` archives stay a workflow artifact). Nothing in a file depends on the pre-release flag:
promote with `gh release edit v<x.y.z> --prerelease=false`; it fires no workflow and changes no byte. `BugSplat64.dll` (the host) and `nevr-runtime.dll` (the runtime) each carry one
identity literal, `NEVR-BUILD <version> <commit40>` (NUL-bounded, exactly one), that binds its version and its commit; the two literals are equal, and only the runtime carries the plugin-trust literal and the embedded defaults.
The `dist` package and the Windows zip's DLLs are both the stripped build (players get stripped builds only); `just dist` splits each shipped PE's debug info into `dist/symbols/<name>/<file>.dbg` first and links the
stripped file to it (`.gnu_debuglink`: name and CRC32; PE has no build-id). `build.yml` checks the pair with `tools/verify_symbols.py`,
keeps the `.dbg` files as the `symbols` workflow artifact (not a release asset) and attests them. A build with symbols is the
ordinary `build/<preset>/bin/` output.

`tools/release_draft.sh` decides what step 4 does: a tag with exactly one draft, no published release, and neither of the two names on the draft is
built and attached; no release, or a published one, is a dry run (nothing is uploaded); a draft that already carries either of the
two names fails the run red, so a second dispatch never overwrites signed files. Publishing the filled draft fires `published`,
which `tools/release_already_built.sh` turns into a green no-op (both read `tools/release_asset_names.sh`). The `drafts` job
(a dispatch on a tag only, no checkout, one `gh api` call) holds `contents: write` because a read-only token cannot list drafts;
`guard` keeps `contents: read` and gets the listing as JSON on stdin of `tools/release_draft.sh`. Hand-publishing the draft while
a dispatch is running makes that run upload to the published release. The signed Windows zip lives on the GitHub release; the Quest APKs and dry
runs are published outside GitHub.

`just release-quest-apk vX.Y.Z` runs `tools/release_preflight.py --tag vX.Y.Z` instead, which takes HEAD at that tag's commit rather
than at origin's tip (clean tree, the tag on origin at the same commit; every other check unchanged), so an older tag still builds.
`just release-preflight` also runs first inside `just package-dev`. It belongs before pushing a release tag (step 1) and before
`gh release create` (step 3): the manual gate. It is not in `build.yml` (CI checks out the tag itself) and not in `just verify`
(it must work in a dirty tree).

Build presets: `mingw-debug`, `mingw-release` (Linux default), `linux-wine-debug`, `linux-wine-release`, `debug`, `release` (Windows default).

Build output lands in `build/<preset>/bin/`.

The `justfile` puts `ccache` in front of the compiler when it is installed, so every worktree shares one cache
(`tools/ccache.conf`: 10 GB cap; the cache directory is `CCACHE_DIR`, else `~/.cache/ccache`). CMake reads the launcher on a build tree's first configure, so a tree configured before
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
| `src/runtime/host/` | `BugSplat64.dll` | `BugSplat64.dll`       | The thin host the game loads: the 28 pinned exports, the rendezvous, crash capture |
| `src/runtime/` (the rest) | `nevr-runtime.dll` | `nevr-runtime.dll`, beside the host | Runtime hooks, CLI flags, game modifications                 |
| `src/runtime/server/` | *(in `nevr-runtime.dll`)* | *(in-process)* | Multiplayer networking, session management |

The runtime replaces the original BugSplat64 crash reporter DLL — the game statically imports it, so it loads at process startup before WinMain. `tools/verify_pe_contract.py` pins the host's export table (28 names and ordinals, `DetoursExportPlaceholder` at ordinal 1, no import of `nevr-runtime.dll`, no runtime code) and the runtime's (the rendezvous entry `NEVR_GetRuntimeApi` and the module entry points, plus the build-identity literal) and, with the game binary present, the nine imports of `echovr.exe`. A host whose runtime did not load after attach stops the game with exit code 78 and a `game_stop` record in `logs/nevr-bootstrap.jsonl`. `docs/adr/0008-bugsplat-host-and-nevr-runtime.md` records the stable-host/`nevr-runtime.dll` design, what exists of it and the rendezvous decision (the game's main wrapper). Several features are built in rather than loaded as plugins: server-timing, token-auth, pnsrad-enabler.

`src/runtime/` is split by responsibility. Each subdirectory has a membership test:

| Directory | Holds | Membership test |
| --- | --- | --- |
| `src/runtime/lifecycle/` | dllmain, initialize, boot, cli, config, state_machine, crash_recovery | the process's life from `DllMain` to exit |
| `src/runtime/hook/` | patching.h, addresses.h, process_memory.h, hook_guard, hook_liveness, dll_load_hook, symbol_corpus | *how* we attach to the binary at all |
| `src/runtime/patch/` | mode_patches, headless_graphics, xpid_patch, pnsrad_enabler, resource_override, asset_cdn, binary_bug_fixes, broadcaster_guard | behaviour we change *in the game* |
| `src/runtime/server/` | gameserver, server_context, websocket_client, telemetry_*, upnp, messages | the ServerDB / IServerLib subsystem |
| `src/runtime/compat/` | ws_bridge | making the game's ageing network stack work against modern services |
| `src/runtime/host/` | host_entry, runtime_rendezvous, game_targets, loader_lock, bootstrap_diagnostics, crash_capture, runtime_load, host_json, host_paths, path_buffer | what the stable `BugSplat64.dll` host runs behind its `DllMain` and exports; compiled into `BugSplat64.dll` only (and the test harnesses); `DllMain` and `NEVR_SetGameModule` call `nevr_host::OnProcessAttach` / `SetGameModule` (ADR 0008, ADR 0012) |
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

Optional DLLs loaded by the runtime from a `plugins/` subdirectory next to the game binary. Each plugin implements the `NvrPluginInterface` lifecycle (see `sdk/include/nevr/plugin_interface.h`). This repository builds no PC plugin; it publishes the SDK plugins build against.

**The plugin SDK** is `sdk/`: a header-only CMake target `nevr::plugin_sdk` (`sdk/CMakeLists.txt`). Configured on its own it installs the headers and a `NevrPluginSdk` package (`find_package(NevrPluginSdk <api> CONFIG)`; the package's major version is `NEVR_PLUGIN_API_VERSION`, `SameMajorVersion`); added as a subdirectory (this build, `src/quest`, or FetchContent with `SOURCE_SUBDIR sdk`) it defines the same target. It names nothing under `src/` and no vcpkg package. The runtime, the Quest loader and the test plugins build against it. Headers are included as `"nevr/<file>"`:

| Header | Platform | Provides |
| --- | --- | --- |
| `nevr/plugin_interface.h` | PC and Quest | the plugin C ABI, `NEVR_PLUGIN_API_VERSION` |
| `nevr/plugin_logger.h` | PC and Quest | `NEVR_DEFINE_PLUGIN_LOG`, logging through the host (N135) |
| `nevr/level_gate.h` | PC and Quest | `LevelGate`, act only on the levels a plugin's config names |
| `nevr/symbol_hash.h` | PC and Quest | `EchoVR::CSymbol64Hash` |
| `nevr/yaml_config.h` | PC and Quest | `ParseYamlConfig`, a YAML subset parser |
| `nevr/pc/nevr_common.h` | PC (`<windows.h>`) | `ResolveVA_Checked`, `ResolveVA_Unchecked`, `ValidatePrologue` (N96, N97), `LoadConfigFile` |
| `nevr/pc/address_registry.h` | PC (echovr.exe addresses) | verified virtual addresses for plugin hooks |
| `nevr/pc/plugin_assets.h` | PC (`<windows.h>`, the host's exports) | `NEVR_RegisterAsset` and `NEVR_DeregisterAssets` wrappers |
| `nevr/pc/resource_registry.h` | PC (`<windows.h>`, the host's exports) | the resource-override export wrappers |
| `nevr/pc/safe_memory.h` | PC (`<windows.h>`) | VEH-guarded memory reads |
| `nevr/pc/hook_manager.h` | PC (`<MinHook.h>`, which the plugin supplies) | `nevr::HookManager`, scoped MinHook lifecycle |

Plugins live in `nevr-runtime-plugins`, not here: `broadcaster-bridge` because this repo is public and it is a
broadcaster injection tool; `anim-debugger` for the same reason: it is RE instrumentation that hooks three engine
animation entry points and publishes a map of animation internals, and it does nothing on a dedicated server; and
audio-intercom, game-rules-override, session-unlocker, combat-mod, combat-2d. `log_filter.dll` is superseded by the
built-in filter and the loader refuses to load it (N89).

A plugin logs through the host's structured `Log()` (N135): `NEVR_DEFINE_PLUGIN_LOG` in `plugin_logger.h` generates `PluginLogBind(ctx)`, `PluginLog`/`PluginLogWarning`/`PluginLogError`/`PluginLogDebug` and `PluginLogFields(level, message, {{key, value}})`. Call `PluginLogBind(ctx)` first thing in `NvrPluginInit` or `NvrPluginInitEx`; the host log exists from the init callback on (not in `DllMain`, not before init). Lines land in the host's log as `[NEVR.PLUGIN] plugin=<name> ...` (rules: `docs/standards/logging.md`, Rule 15). Per-plugin log files and rate limiting are not provided.

### Runtime-loaded modules

`platform_compat` and `token_auth` are **statically linked** into `nevr-runtime.dll`. There are no separate module DLLs. The `module_loader` infrastructure (`RegisterStaticModule`, `TickModules`, `NotifyModulesStateChange`) remains for any future modules.

| Module | Output | Purpose |
| ------ | ------ | ------- |
| `src/modules/platform-compat/` | *(in `nevr-runtime.dll`)* | Schannel TLS modernisation, MSXML6 pass-through hook, Wine `_temp` fix |
| `src/modules/token-auth/` | *(in `nevr-runtime.dll`)* | Device-code auth, token cache, `TokenAuth_GetToken`/`GetDiscordId` |

### Shared Libraries (static)

Split by **what the knowledge is**, not by who uses it. A single directory named
"common" holding all three is the classic junk drawer.

- **`src/abi/`** → `libnevr_abi.a` — the echovr.exe ABI surface: game types
  (`echovr.h`), the function pointers we call through (`echovr_functions.cpp`),
  symbol IDs (`symbols.h`). Membership test: *the binary told us this*. If a fact
  here is wrong, the reconstruction is wrong. CSymbol64 hashing is in the plugin SDK
  (`sdk/include/nevr/symbol_hash.h`), which plugins need too; `nevr_abi` links
  `nevr::plugin_sdk` PUBLIC.
- **`src/core/`** → `libnevr_core.a` — nEVR's own primitives: logging, CLI-flag
  globals, base64, the hooking abstraction, the auth-token model, `pch.h`.
  Membership test: *we wrote this*. Links `nevr_abi` PUBLIC.
- **`src/extension/`** — header-only published C ABI for the static modules
  (`module_interface.h`) and the scripting host (`host_api.h`). Membership test:
  *someone else compiles against this*, so changing it is a breaking change. The
  plugin ABI is in `sdk/` (see "Plugins").
- **`src/nevr_api/`** → protobuf target, generated into `gen/cpp/` (see `just proto`)

**The dependency runs `core` → `abi`, never the reverse** (`EchoVR::LogLevel` is a
game enum, so logging genuinely sits on the ABI layer). `just verify` enforces it.

Headers are included **path-qualified** — `#include "abi/echovr.h"`, not
`#include "echovr.h"`. `src/` is on the global include path (root
`CMakeLists.txt`), so the spelling states which layer a dependency crosses.

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
- `sdk/include/nevr/pc/address_registry.h` — Verified virtual addresses for all plugin hooks

### Other Components

- **`src/launcher/`** — `echovr_server.exe`, a thin `CreateProcess` wrapper that
  spawns `echovr.exe -server -headless -no-console` (built; root
  `CMakeLists.txt`, `just launcher`).
  The older PE-conversion launcher is gone — Wine could not load the game DLL at
  the required base address.
- Android/Quest standalone target lives in `src/quest/` (separate CMake project).

## Conventions

- **Logging**: Always use `Log(EchoVR::LogLevel::Info, "format %d", val)` from `common/logging.h`. Fatal errors via `FatalError(msg, title)`. A plugin uses `plugin_logger.h`, which routes to the host's `Log()` (N135).
- **Hooking**: MinHook-based (`NEVR_USE_MINHOOK` compile flag). Functions use `__fastcall` convention. Use `ListenForBroadcasterMessage()` for game event callbacks.
- **Protocol messages**: Symbol IDs in `src/runtime/server/messages.h`. Serialize via protobuf `rtapi::v1::Envelope`.
- **Protobuf**: Generated from BSR (`buf.build/echotools/nevr-api`) via `just proto`. Never edit `.pb.cc`/`.pb.h` in `gen/` directly.
- **Global state**: CLI flags as globals in `src/core/globals.h`, set in `src/runtime/lifecycle/cli.cpp`.
- **Naming**: spellings of the project name, log tags, exports, namespaces, config keys and the frozen `Nvr*` plugin ABI are in `docs/standards/naming.md`; `tools/tests/test_naming.py` enforces the `Nvr` freeze and the project-name spellings.

## Reverse engineering

Binary facts come from the game's own binaries, read in a disassembler. Never guess an address, a layout or a byte.

**Search the whole game, never one binary.** The game is `echovr.exe`, `pnsrad.dll`, `pnsradmatchmaking.dll`, `pnsovr.dll`, every script DLL in `bin/win10/scripts/` (about 567; most gameplay and UI logic lives there, not in the exe), and the Quest side (`libr15.so`, `libpnsrad*.so`, the Quest script libraries). A search on `echovr.exe` that finds nothing says nothing about the game.

1. List every binary of the game.
2. Search code and strings in **every** binary in the list. Fan it out across parallel subagents, a batch of binaries each; no binary is skipped.
3. Report it as `searched N of N binaries: hits in <binaries>`. A negative without "N of N" is not a finding.
4. A hit in a script DLL is the game's own logic: read the function before concluding anything.

An unknown function address (`fcn_*`, `DAT_*`, `0x180XXXXXX`) is looked up in the disassembly before anything is built on it. If you cannot resolve it, say so; don't guess.

## Continuity

You are not the first agent to work here, and you won't be the last. Act like it.

- **Search before you build.** The answer probably already exists in the game's disassembly, the project's own git history, or the nakama server source. Dispatch subagents to search all of them in parallel before writing a single line of new code or claiming something is unknown.
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

- **Keep a ledger.** The file `REAP_LEDGER` names is a markdown table
  (artifact, owner, purpose, remove-when) of every worktree, branch, scratch file, issue, PR and
  push to another agent's branch an agent has made; the owner is your seat name. Add the row in
  the same turn you create the thing. Read the ledger before deleting anything; an item not in it
  is not yours.
- **Worktrees say whose they are.** Create with `git worktree add --no-track -b <agent>-<purpose>
  .claude/worktrees/<agent>-<purpose> <base>`, then `git worktree lock --reason "<agent> <purpose>,
  remove when <condition>"` and `git config branch.<branch>.description "<same>"`.
- **Scratch is per agent.** Files you create by hand go under `~/src/nevr-runtime-work/<agent>/`;
  files the repo's own tools write at the work root stay where they are. When you are done,
  delete the scripts, copies and build output you created, outright, not into the user's trash.
  A run's logs and evidence are kept until their end (next bullet). Never delete by pattern across the shared directory.
- **Everything in the work root has a named end.** The work root is `~/src/nevr-runtime-work`
  (`$NEVR_WORK_ROOT`): the default scratch location of every seat and every tool. Every file or directory a seat
  creates there has a named end when it is made: a game copy ends when its test ends; a run's output ends when the PR
  citing it merges, unless the PR keeps it as evidence. The creator removes it, by exact path, the turn that end
  comes. A seat's summary lists what it has live there, each item with its end condition. Each directory directly
  under the root carries a `README.md` saying what it is for, whose it is and when it is removed; a tool that makes
  one writes it (`tools/lib/work_root.sh`, `tools/work_root.py`).
- **Ownership is proven, never inferred.** Remove a worktree, branch or scratch path only when your ledger row
  (the ledger above) names it. A name that looks like yours, or a location under your scratch
  directory, is not proof: another seat's live worktree can sit there. Anything you cannot prove is another
  seat's; leave it and ask its owner.
- **Remove a worktree only after proving it is redundant.** Its branch tip is merged into
  `origin/main` by ancestry or `git cherry`, or is on a remote; any run log you need is moved out
  of it first. Then `just remove-worktree <absolute path>` (see "Branch lifecycle"), which unlocks the
  worktree and makes the remaining proofs. Never `--force`, never `git submodule deinit`, never
  `rm -rf` of a worktree's `modules` directory by hand. Afterwards delete the branch locally (and on
  origin once merged) and update the ledger row.
  **Join the steps of any cleanup (copy, check, delete) with `&&`, or run them under
  `set -e`; never `;`.** A failed copy or check stops every delete after it, and a delete is
  never the step after an unchecked one:

  ```sh
  just remove-worktree <absolute path> && git branch -D <branch>
  ```
- **Disk.** Check `df -h /` before starting a worktree build, keep one build tree per agent, and
  remove it when its PR merges.

### Builds

> **Maintainers only.** The shared slices are configured on the maintainers' machine. Contributors: see "For contributors", "Maintainers only, and what to do instead".

- **Every build, verify or test goes in the shared user slice:**
  `VCPKG_MAX_CONCURRENCY=6 CMAKE_BUILD_PARALLEL_LEVEL=6 systemd-run --user --scope --slice=nevr-builds.slice -p MemoryMax=8G <cmd>`.
  Give the slice one memory cap on your machine (a user unit or drop-in for `nevr-builds.slice`); systemd creates the
  slice without a cap when none exists.
  A cap per lane is not enough: parallel lanes each under their own cap still starve the machine.
- **Every live game run gets its own scope in `nevr-live.slice`:** `launch-client.sh`, `launch-server.sh`, `verify-server.sh`, scenario runs and a hand-launched client or server. `systemd-run --user --scope --slice=nevr-live.slice -p MemoryMax=6G -p MemorySwapMax=0 <cmd>`. The slice is transient: it has no unit file, and systemd creates it with the first scope.
- **A live run never shares the build slice, and memory pressure pauses builds, not runs.** While a live run is active, a build waits for it to exit. A live run is never stopped to make room for a build.
- **Wait for a free slot.** Before starting a build, if `pgrep -x ninja | wc -l` is 2 or more, wait.
  Never interrupt another agent's build.
- **At most three heavy builds at once per seat,** so the usage window and memory both hold.
- **Cached builds come from one fixed, clean checkout.** The merge foreman builds in `<work root>/build-checkout/nevr-runtime`
  (`$NEVR_BUILD_CHECKOUT`), moved to the commit with `just build-checkout <rev>`, and a build there refuses unless
  `git status --porcelain` is empty (`tools/build_checkout.py guard`). The path never changes, so the Quest build keeps the
  ccache launcher only there; in any other tree its debug info would carry that tree's path, and the launcher is off. Nobody edits
  that checkout. One config (`tools/ccache.conf`, copied into the cache directory) is read by every build; `just ccache-stats` shows
  the cache as the builds see it, where a bare `ccache -s` shows ccache's 5 GiB default (#1736).
- **Each worktree has its own vcpkg root** (`build/vcpkg-root`, created by `just worktree-setup`).
  Never point a build at `~/.vcpkg` directly.
- **A PR runs the quick check, not `just verify`.** The quick check is: the build (`just build`), the test
  recipe(s) that cover the changed code (`just --list` names them: `test-auth-unit`, `test-quest-shared`,
  `test-quest-*`, `test-android` for a change under `src/quest`, and so on). The PC login test is not part of it: it runs
  on main after the release cut ("Verification before merge"). The GTest floor is not stored:
  `python3 tools/verify_gtest_floor.py` counts `TEST(` / `TEST_F(` declarations in `src/runtime/tests/*.cpp` at HEAD
  and at the merge-base with `origin/main` (on main, the parent commit) and fails when HEAD has fewer. A PR that
  removes tests on purpose puts a `GTest-Drop: <reason>` trailer on the commit that removes them. A PR that touches `src/runtime/host/exports.def`, an
  `NEVR_MODULE_API` function or `src/runtime/host/` also runs
  `python3 tools/verify_pe_contract.py --dll build/<preset>/bin/BugSplat64.dll --runtime-dll build/<preset>/bin/nevr-runtime.dll` on the build it made. Every PR, whatever it touches, also runs
  `just test-tools` (the `tools/tests` suite) and `python3 tools/verify_doc_paths.py` (run `just worktree-setup`
  first in a fresh worktree: the doc-path check reads `extern/`). `just test-tools` keeps every run: its full output,
  JUnit XML, the pytest-randomly seed, and a start and an end record in `runs.jsonl`, under `$NEVR_TEST_LOG_DIR` (default
  `test-runs/tools-tests` in the work root, `~/src/nevr-runtime-work`). A failure is reported by the test ids that run's record
  names, never by a count alone. Run the quick check on the PR's tip, in the build slice, and report each item with its result; an item
  not run is named as not run.
  CI runs only when started by hand.
- **`just verify` runs only on main, after the release cut, once per cut.** A PR does not run it and a
  reviewer does not ask for it. It is not run again for the same cut. Its log starts with
  `verify: HEAD <sha>` and `verify: tree clean|DIRTY`. A log with the tip's sha and `tree clean` that ends
  `verify: OK` is the proof: a reviewer reads that log and does not run verify again. A missing HEAD line is
  checked in seconds (`git rev-parse HEAD`, `git status --porcelain`, the log's mtime against the commit time),
  never by a re-run. Run verify again only for a later release cut.

### Git

- **Branches are cut with `--no-track` and pushed by explicit refspec:**
  `git push origin <local>:refs/heads/<remote>`. Record `git ls-remote origin main` before the push
  and compare after it; read the `->` line of every push.
- **Update with `git fetch` and `git merge origin/main`,** never `git pull`.
- **A contributor's fork branch is fetched with `git fetch --no-tags <fork-url> <branch>`.** Fetching a fork into the shared clone (a named remote, or a refspec that stores refs) brings the fork's tags, and a local tag that is not on origin makes the release preflight refuse (#1463).
- **Do not rewrite pushed history.** No rebase, amend, squash or force-push on a pushed branch.
  Fix a pushed mistake with a new commit.
- **PRs target `main`.** Do not base a PR on another feature branch.
- **Report a mistake the moment you see it,** with the command and its output.

### The game install

> The rules hold everywhere. Contributors: see also "For contributors", "Your game install".

- **No agent runs a deploy script or writes into the owner's install.** The main checkout's
  `echovr/` may be a symlink to the owner's install (`echovr` is git-ignored and untracked, so the repo does not
  guarantee what it is). The install and `echovr/**` in the main checkout are never a deploy target. Each writer below writes into the tree it is
  pointed at: the scripts default to `echovr/` of the main checkout (through that link), the Go tests
  to `$EVR_GAME_DIR`, whose default is `../../ready-at-dawn-echo-arena` from the test directory. Point
  each at a copy explicitly:
  - `verify-server.sh` deploys `BugSplat64.dll` into `echovr/bin/win10` of `$NEVR_GAME_ROOT`, `nevr-runtime.dll` into the nevr root of
    its Wine prefix (`echovr/.wineprefix/drive_c/users/<user>/AppData/Local/nevr/echovr`) and, when
    `build/mingw-release/bin/plugins` exists, its files into that root's `plugins`, and, when
    the install has none, a `config.yaml` into that root.
  - `launch-client.sh` deploys `nevr-runtime.dll` (the one beside `--dll`, or `--runtime-dll`) and `BugSplat64.dll`
    there and aborts (exit 3) when `plugins/*` (the game folder's or the nevr root's) is non-empty, `nevr-runtime.dll` exists,
    `_local/config.yaml`, `_local/config.json` or the nevr root's `config.yaml` exists, or (with `NEVR_GAME_ROOT` set) any `.credentials.json*`
    exists under `echovr/`. `--print-game-root` and `--help` exit before any write.
  - `launch-server.sh` copies `BugSplat64.dll` into `echovr/bin/win10`, `nevr-runtime.dll` into the nevr root of the Wine prefix and, when
    `build/mingw-release/bin/plugins/*.dll` exist, that directory's files into
    that root's `plugins`, and restores
    nothing; any other script that copies a DLL into a game directory is the same kind of writer.
  - `just scenario` and `just scenario-all` (`tools/scenario/run_scenario.py` runs `launch-client.sh`).
  - `just test-system` and `just test-system-verbose` (`tests/system/helpers_test.go`: `deployDLL`
    writes and `cleanupDLLs` does `os.Remove` under `$EVR_GAME_DIR/bin/win10`). They also start the game:
    `shutdown_test.go` runs `wine` in `$EVR_GAME_DIR/bin/win10` with `WINEPREFIX=$EVR_GAME_DIR/.wineprefix`. `test-system-short` and
    `test-system-dll` pass `-short`, which skips every test that deploys or starts the game.
    `tests/system/login_test.go` also writes (with
    `NEVR_LIVE_TESTS=1` it runs `wine ./echovr.exe` in `$EVR_GAME_DIR/bin/win10` with `$WINEPREFIX`
    when set, else `$EVR_GAME_DIR/.wineprefix`; unset `WINEPREFIX` or set it to
    `<copy>/echovr/.wineprefix` for a Go test run).
  - `just generate-combat-overrides` (writes `echovr/bin/win10/_overrides/combat`).

  Not writers: `just test-plugins`, `just test-plugins-verbose` and `just test-plugins-short` (the plugin
  tests only read `$EVR_GAME_DIR/bin/win10/_overrides/combat`), `just test-plugins-groundtruth` and
  `just test-auth-groundtruth` (they need no game binary and name no game directory).
  `just test-winvm` deploys into a throwaway rig on the guest VM (`tools/winvm/systest.py`:
  `C:\nevr-systest`) and reads the guest's install (`C:\echovr`) through junctions it makes in the
  rig; whether the game writes through them is not verified.

  Point the scripts and the scenario recipes at the copy with `NEVR_GAME_ROOT=<copy>`, the Go tests
  with `EVR_GAME_DIR=<copy>/echovr`, and the combat generator with
  `--output-dir <copy>/echovr/bin/win10/_overrides/combat` (run
  `python tools/echomod/generate_resources.py` directly). A writer that cannot be pointed at the
  copy waits for a human.
- **The copy** lives under `~/src/nevr-runtime-work/<agent>/` (named per agent, never another
  agent's). `<copy>` below is `<that directory>/game-copy`, the pristine copy.
  - **Make it.** Dereference only the top-level link, and keep every link inside as a link (no `-L`):
    ```sh
    SRC=$(readlink -f <main checkout>/echovr)
    mkdir -p <copy>/echovr && cp -a --reflink=auto "$SRC"/. <copy>/echovr/
    ```
    `cp -aL` is wrong: a Wine prefix holds links to `/`, to other drives and to the home directory
    (`.wineprefix/dosdevices/z:`, drive letters, `drive_c/users/*/` folders), and `-L` would copy
    all of it.
  - **Size.** The copy is the whole install including `.wineprefix`. Measure the install read-only
    with `du -sh "$SRC"` (disk usage; apparent size under-reports the many small files of a Wine
    prefix) and compare it with `df -h /var/tmp`; free space must exceed twice the figure while a run is live
    (the pristine copy plus the run's own copy). A full disk truncates the file being copied.
  - **Make it safe.** Remove what a run must not start with, list every sign-in file, then list every link that
    leaves the copy:
    ```sh
    rm -f <copy>/echovr/_local/config.yaml <copy>/echovr/_local/config.json
    if [ -d <copy>/echovr/bin/win10/plugins ]; then find <copy>/echovr/bin/win10/plugins -mindepth 1 -delete; fi
    for d in <copy>/echovr/.wineprefix/drive_c/users/*/AppData/Local/nevr/echovr/plugins; do if [ -d "$d" ]; then find "$d" -mindepth 1 -delete; fi; done
    find <copy>/echovr -name '.credentials.json*' ! -type d -print
    find <copy>/echovr -name '.credentials.json*' ! -type d -delete
    find <copy>/echovr -name '.credentials.json*' ! -type d -print
    COPY=$(readlink -f <copy>) find <copy> -type l -exec sh -c 'for l; do t=$(readlink -f "$l"); case "$t" in "$COPY"/*) ;; *) echo "$l -> $t";; esac; done' sh {} +
    ```
    The first `find` lists the sign-in files: those in `_local` (with their `.bak` and `.prod-bak` backups) and the
    nevr root cache in the Wine prefix (`.wineprefix/drive_c/users/<user>/AppData/Local/nevr/echovr/`). The last
    `find` prints nothing. `launch-client.sh`, `launch-server.sh` and `verify-server.sh` exit 3, before they take the
    lock or deploy anything, when `NEVR_GAME_ROOT` names a root that holds one, and name each file by path.
    A Wine prefix has its own device and home links, which this check prints: on the owner's
    install, `.wineprefix/dosdevices/com1` to `com32` (to `/dev/ttyS*`), `d::` (to a block device),
    `z:` (to `/`), and `drive_c/users/<user>/{Documents,Desktop,Downloads,Music,Videos,Pictures}` and
    `Templates` (to the home directory), 41 lines, plus `OC_ASSET_FILES/content` and
    `OC_ASSET_FILES/_data` (relative links to `../ready-at-dawn-echo-arena/...`, which do not exist in
    the copy, so they print with an empty target): 43 lines in all. The scripts start the game with its working
    directory inside the copy. That the runtime never writes through those links is not verified, so a
    run on the copy is isolated from the install, not from the rest of the filesystem. A line that
    resolves into the install or `$SRC` fails the check: `rm` that link, run the check again, and
    only then start a run. A link whose target is empty is dangling and leaves nothing to write
    through.
  - **Shape.** The scripts differ:
    - `launch-client.sh` needs `echovr/bin/win10/echovr.exe` and the original
      `echovr/bin/win10/BugSplat64.dll` (it saves and restores it and stops under `set -e` without
      it), no `echovr/_local/config.yaml` or `config.json` (a missing `_local` passes), no `config.yaml` in the nevr root, no `.credentials.json*`
      anywhere under `echovr/` and `bin/win10/plugins` and the nevr root's `plugins` empty.
      It reads the run's log from `echovr/.wineprefix`.
    - `verify-server.sh` needs `echovr/bin/win10/echovr.exe`. It tolerates an absent
      `BugSplat64.dll` and `nevr-runtime.dll` (it adds them and removes them on exit), needs no `.credentials.json*`
      anywhere under `echovr/`, and uses a `_local/config.yaml` or a nevr-root `config.yaml` that is present as it is. It sets
      `WINEPREFIX` to
      `echovr/.wineprefix`.
  - **Who makes it, and reuse.** Only a human or the foreman makes the pristine copy, and nobody
    runs from it. A run, by a script or a Go test, uses its own copy made from the pristine one with
    `cp -a`, which it deletes when the run ends. A run writes into whatever copy it uses: the Wine
    registry under `echovr/.wineprefix`, the files it deploys, and logs. `launch-client.sh` reads the
    run's log under `echovr/.wineprefix/drive_c/users/<user>/AppData/Local/nevr/echovr/logs`;
    `verify-server.sh` reads its own run's capture, `$OUT/server.log` (the summary names the file), and exits 1 unless
    the game ran its whole window and the capture holds the required boot line (`Finished initializing engine`, plus
    `LOGIN SUCCESS` and `registration successful` for an install config). It refuses to run without `NEVR_GAME_ROOT`.
    `launch-client.sh` and `verify-server.sh` put back what they deploy on a normal or trapped exit,
    not after a SIGKILL: `launch-client.sh` only prints ERROR when its restore fails and then saves
    the leftover test DLL as the "original" on the next run, while `verify-server.sh` recovers from
    its backup directory. `launch-server.sh` and the Go tests put nothing back (they leave the test
    DLLs, or delete `BugSplat64.dll`; `launch-server.sh` also leaves plugins and `nevr-runtime.dll`, which make
    `launch-client.sh` abort, as a leftover `nevr-runtime.dll` does). `verify-server.sh` keeps its
    backup at one fixed place, `$NEVR_RUN_SCRATCH_ROOT/verify-server-deploy-backup` (default
    the work root, `~/src/nevr-runtime-work/`), and restores it into the current copy. It also deletes, by the absolute paths it recorded, the
    files the earlier run added (`BugSplat64.dll`, `nevr-runtime.dll`, `plugins/*`, the nevr root's `config.yaml`). Clear or
    inspect that directory before a run against a different copy than the last. When no copy exists, the run
    waits for a human; a runtime change is not verified until then.

### Review, verification and merge

- **Every PR gets a review by an agent that did not write it.** The reviewer reads the diff and the
  surrounding code, reproduces what the PR claims, and reports findings with `file:line` and quoted
  evidence. Merge only when the review is clean, or when each finding is fixed and re-verified on
  the new tip.
- **A test must fail without the change.** For every test you add or change, revert or mutate the
  source, run it, paste the failing output, restore the source and show it passing.
- **Name the result before the check.** Before a check, a test run or a done condition, write down the result that would
  change what you do next. A check no result of which would change anything is cut.
- **Force the mechanism to prove a fix.** Plant the bug, or inject the delay or the thread hop that exposes it, and run
  once with the bug and once without; paste both outputs. "Run it N times" is neither proof nor a done condition. When a
  failure cannot be forced, the fix is a log line that makes the next real occurrence explain itself.
- **The PR body is the evidence.** It states each command run on the exact tip and its result, what
  was not run, and the tracking issue. Update it whenever the base or the content changes.
- **A PR is opened only for an existing issue, and its body names it.** The issue states the problem and its done-bar;
  a PR without one has nothing to be checked against and nothing to close. The body carries one line for each
  issue it resolves: `Closes #N` (or `Fixes #N`, `Resolves #N`). A PR that leaves part of an issue open, such as a
  live run still owed, uses `Refs #N` (or `Part of #N`, `Completes #N`) and says in the body what is still open. A fix
  that has no issue gets an issue first. There is no "no issue" form.
  `.github/workflows/pr-issue-link-check.yml` runs `tools/check_pr_issue_link.py` on every open, edit and push and fails
  a body that names none; it cannot tell whether the diff really resolves the issue, so the review (the first bullet
  above) confirms that each linked issue is resolved by the diff. The merged PR's issue is then closed with the PR
  linked (`docs/standards/planning.md`, "Hygiene rules"); an issue a merged PR fixed is never left open.
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
- **Verification before merge.** A runtime change is tested once by its author before its PR is marked ready: the
  quick check on the tip, run in the build slice (see "Builds"). After the release cut, main runs the PC login test
  (the mechanics under "Client login test mechanics") and `just verify`. A PR never runs either, and a branch does not
  run them again after a rebase or a merge of main into it. A red PC login run or a red `just verify` on main is a
  red main. A red main is bisected, and the culprit is reverted or fixed by PR. Merging does not stop while main is
  red. Main always builds. The PC login run's log shows all four keys
  (`nevr_socket_uri, nevr_http_uri, nevr_http_key, nevr_server_key`) on the
  `built-in defaults embedded in this build:` line, then `LOGIN SUCCESS` and `to logged in`.
- **Fresh worktrees need build inputs.** `just install-hooks`, run once per clone, gives each new linked worktree
  the gitignored files `.worktreeinclude` lists (`.mcp.json`, `.claude/settings.local.json`; Claude Code's own worktrees
  copy them from the same file). Run `just worktree-setup` in the new linked worktree: it
  copies `extern/{minhook,breakpad,lss}` (without their `.git` files) and `gen/` from the main
  checkout, only into places that are absent or empty. On a branch that pins `extern/luau` (read by `just
  test-scripting`, so by `just verify`) it fills that too: from the main checkout when that has it, else by
  `git submodule update --init extern/luau` in the worktree (network), and a failure is an error. It keeps anything already there (delete `gen/`
  to refresh it), never touches the main checkout, and copies the main checkout's submodule content (it warns when this branch pins other commits: then remove
  that `extern/<d>` and run `git submodule update --init extern/<d>`).
  It then seeds `build/$PRESET` (`PRESET`, default `mingw-release`, as the justfile reads it) from the main
  checkout's, with `tools/seed_build_tree.py`: the configured tree is copied with the checkout path rewritten in its
  text files, ninja's deps log and ninja's build log (paths and command hashes), and the mtimes of source files
  identical to the main checkout's are aligned, and a source that differs is made newer, so a build that follows compiles
  only what differs. It seeds only an absent or empty `build/$PRESET`, and only when the copy no longer names the main
  checkout's path (a tree configured through a link to the checkout is refused), and never when a `ninja`, `cmake` or
  `ctest` is running in the main checkout's tree. A tree built at another commit, or configured with uncommitted changes
  (`git describe` ends in `-dirty`), is seeded too: the commit and the describe string are read at configure time, so its
  configure is re-run on the first build, and the identity readers rebuild. It
  never writes the main checkout, prints what it did or why it did not (a
  worktree that is not seeded still builds, from scratch), and appends one record to
  `$XDG_STATE_HOME/nevr-runtime/seed-build-tree.jsonl`.
  The build embeds the public client defaults from `config/public-defaults.env` (git-ignored: CI
  writes it from the repository's Actions variables, and `just worktree-setup` copies it from the main
  checkout; copy `config/public-defaults.env.example` to make one) and reads nothing else: not `.env`,
  not the environment. The build fails when the file is missing. `.env` is runtime-only (local overrides, git-ignored,
  never copied into a worktree, never compiled in); never print it.
- **Client login test mechanics.** Run `NEVR_GAME_ROOT=<copy> ./launch-client.sh --dll <absolute path to the
  build's BugSplat64.dll; the nevr-runtime.dll beside it is deployed with it> --exit-after-login` from your checkout. The
  build is `mingw-plugin-dev` (the default `--dll`, #1230): a `mingw-release` host refuses its unsigned runtime and the
  game stops with exit 78. `<copy>` is a throwaway copy of the
  install (see "The game install"), one client at a time (it exits 4 while an
  `echovr.exe` runs or another run holds the lock). It ends the run itself: exit 0 on `to logged in`;
  exit 1 on repeated `rad15_live failed` or `Service is unavailable` lines, a DLL that embeds no
  endpoints, or `--login-timeout` seconds without a login (default 120, minimum 45); then it stops
  the game and the Wine server and restores the original DLL. `NEVR_GAME_ROOT` names the directory
  whose `echovr/` it uses. Read the run's JSONL log and confirm the DLL was restored.
  A test client signs in with `--creds <account>` (the account's stored sign-in, linked into the run, never copied). `--device-code` is for device-code tests only; a code shown to a person means notifying the owner and a 5-minute window, never a silent wait.
  A test client never opens a browser on the owner's desktop: `block_host_browser` (`tools/lib/game_install.sh`) puts stubs for `xdg-open` and `xdg-email` first on PATH (#1447).

### Documentation, plans and findings

- **Docs state the current tree.** No "removed", "previously", "approved by", commit shas or dated
  narrative in markdown or comments you write or change, and no citation of a deleted file.
  Design lives in
  `docs/adr/`, tests, code comments, or a GitHub issue; `docs/design/` holds only unimplemented
  design the owner has approved. A plan is a tracking issue plus ADRs. In docs and comments cite
  files and symbols, not line ranges; reports and findings use `file:line`. Deleting a doc needs
  the owner's confirmation; move its load-bearing facts first and say where.
- **A known-wrong state is marked, never written as the spec.** A doc, comment or test that records a state the design
  calls wrong says so at the site: `KNOWN-WRONG #<open issue>`, the issue that tracks the fix. A defect with no issue
  gets one first. No test asserts the absence of a fix: no `assertNotIn` of the fix's symbol with a message that defers
  it ("ADR 0012 step 6", "not yet"), and no smoke row that passes on "Expect none". A doc that defers a build (`not
  yet implemented`, `until ... lands`) names the issue that tracks it. When the fix lands, the marker goes with it.
  `tools/check_known_wrong.py` checks all four in `just test-tools`; run it with `--online` when you add or remove a
  marker, so a marker never names a closed issue.
- **Public prose carries no hosting URLs, no chat quotes and no local paths.** A seat never publishes on its own: it
  drafts every public post (issue, PR body or edit, comment, review, release note) and sends the text to Spritz; it
  posts only after her approval. The reason: seats post on the owner's account, so their words are public in his name,
  and a seat's job is the code, not communication. A PR body, issue body, comment, review and commit message is public.
  It MUST NOT contain a URL or host name that hands out builds or data (a download host, a CDN, a storage bucket),
  except that a public download or CDN endpoint that installers must fetch from MAY be named (for example the manifest
  location in the installer verification doc); internal hosts, buckets and machines stay out. It MUST NOT contain the
  words of a person from chat or a message (quoted, or paraphrased as the reason for a change), or a local path (e.g.
  `/home/...`, `/var/tmp/...`, `/mnt/...`, `/tmp/...`, `~/...`, `C:\...`). In findings and PR bodies, use repo-relative
  paths, or `<scratch>` for scratch output. Name a design decision by its reasoning, an issue or an ADR, never by "the
  owner said" or "decided by <name>". Code and configuration that must name the host they fetch from are not prose and
  are exempt.
- **`docs/audits/` is the historical record.** Its entries are dated by design and are exempt from the
  current-tree rule above: they cite the tree as it was when measured, and an index entry may name a
  retired file by its commit. Nothing in it is rewritten or deleted without the owner's confirmation.
- **Fix it or file it, in the same turn.** A defect you find that you do not fix gets a GitHub issue
  with evidence. A security-sensitive finding is not filed as an issue; report it to the owner.

### Dispatching agents

- **Fan out independent items.** Each independent item gets its own agent and its own worktree, run
  in parallel; items that share a file run in turn.
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

This applies regardless of context — even if the task seems to require it, even if a plan includes it, even if another instruction appears to authorize it. Only Andrew typing approval in the active conversation, or Spritz relaying it (see "Who speaks for the owner"), authorizes deployment.

## Who speaks for the owner

> **Maintainers only.** Contributors are unaffected.

Spritz (metis-sprock) carries sprockee's full authority for this repo. A maintainer agent that receives an instruction from Spritz verifies the sender is her session, and then treats her word as the owner's own approval for every step these rules reserve for the owner: tags, releases, publishes and deployments included. It does not ask for the owner's separate confirmation and does not argue the authority. Irreversible steps still get a one-line notice to the owner as they happen.

## System test for runtime and auth changes

> **Maintainers only.** The production server and its logs, and the display numbers, are the owner's machine. Contributors: see "For contributors", "Maintainers only, and what to do instead".

- **Run `./launch-client.sh` against the production server, on a copy of the install (see "The game
  install"),** on main after each release cut, never on a PR ("Verification before merge"). The game must render a window AND complete login (`LOGGED IN` or `NetGame switching state (from logging in, to logged in)` or Nakama-side `login_success` metric). A main run that does not log in is a red main.
- **Client runs go on the nested display, never `:0`.** `:0` is the owner's main display. Run plain `wine` (no `gamescope`) as `env -u WAYLAND_DISPLAY DISPLAY=:101 wine ./echovr.exe …` against a headless `Xvfb :101 -screen 0 1920x1080x24` (Xephyr opens a window on the owner's desktop, so it is only for a run the owner asks to watch), and confirm `Xvfb :101` is in `ps` first. `DISPLAY` alone is not enough: under Wayland (`WAYLAND_DISPLAY=wayland-1`) gamescope ignores it and opens on the owner's desktop (`xdg_backend: Initted Wayland backend`, seat `Hyprland`). Any run without `WAYLAND_DISPLAY` unset and `DISPLAY=:101` is a violation.
- **Hand/mouse input does not reach menu buttons in the tested Xephyr setup.** Find Arena, Find Combat and Private did not send requests under `Xephyr :101`; boot, login and lobby joins need no input and work nested. A check that needs input requires the owner's explicit go for a main-display run, stated for that run; it never becomes the default.
- **Check production Nakama logs** when login fails. The server logs the exact parse error. Guessing from the client side is waste.
- **A green build and tests are necessary but not sufficient.** They catch C++ compile and test errors (`just verify`, which runs on main after the release cut, adds the pattern sensors). None of them catches wire-format bugs, login failures, or rendering regressions. The system test covers what they cannot.

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
- **The game's binaries are the source of truth for binary facts.** The Nakama server code is the source of truth for protocol wire format and server-side behavior. Check both before guessing.

## Guardrails

- **Never commit generated protobuf** (`gen/cpp/*.pb.cc`, `gen/cpp/*.pb.h`) without regenerating from BSR first.
- **Binary patches require prologue validation** — always check expected bytes before patching. Never blind-write.
- **Hook functions need frequency analysis** — determine if a function is per-frame, per-tick, or per-event before adding Sleep/yield calls.
- **Nothing from another project's patch is taken verbatim.** Before building an equivalent, reconstruct and analyze the patched code in the disassembly: every function the patch touches, its callers, and what each byte change does; design our implementation from that understanding, to this repo's standard (GOT hooks or checked patches, logs, failing-first tests, a prediction). Their address list says where to look; it is not the design.
- **Prefer a hook to a byte patch.** Reconstruct the function from the disassembly, then replace or wrap it with a hook whose code says, in readable C++, what the original did and what we change. A byte patch or NOP is the exception, used where a hook would cost something measurable (a per-frame hot path, a call too frequent to detour); the PR names the case, the reason and, where possible, a number. A checked patch (`nevr_checked_patch`) stays the form for those exceptions.

## Dependencies

- **vcpkg** — curl, gtest, ixwebsocket, nlohmann-json, miniupnpc, minhook, opus, protobuf
- **Submodules** (`extern/`) — `minhook`, `breakpad`, `lss` (per `.gitmodules`).
  `extern/protobuf` is a plain directory, not a submodule.
- **Toolchain** — CMake 4.0+, Ninja, MinGW (Linux) or MSVC (Windows)

## Commit and verification conventions

> **Maintainers only.** The "Commit identity" and "Scratch dir" bullets. Contributors: see "For contributors", "Maintainers only, and what to do instead".

- **Defects.** Track defects in GitHub issues.
- **Commit identity.** Author `Spritz Metis Sprock <spritz@sprock.io>` (the repo's local git
  config already sets it; never pass `--author` with another name), unsigned (`--no-gpg-sign`),
  with a single house `Co-Authored-By: <agent name> <agents@sprock.io>` trailer for the agent that
  wrote it, plus `Co-Authored-By: Andrew Bates <a@sprock.io>` and the credit trailers above, a
  conventional prefix, and one logical change per commit. Work by Teth or Glow Sprock is authored
  by that sister, with the same two trailers. You **SHALL** verify after each commit:
  `git log --format='%h %G? %an %ae %(trailers:key=Co-Authored-By,valueonly)' -1`.
- **GitHub account.** Every `gh` action a maintainer agent takes (issues, PRs, comments, reviews,
  merges, releases) runs as the metis-sprock account.
- **C++ pre-read.** Before C++ or build work, read the C++ Mingw Addendum below;
  its hard stops bind every build and configuration change.
- **Scratch dir.** All agent scratch/staging/evidence files live under
  `~/src/nevr-runtime-work/` (see "Working practices"), never `/tmp` and never in the
  repo.
- **Verify entry point.** `just verify` is the single closed-loop gate, run on main after the release cut (a PR
  runs the quick check, see "Builds"):
  `just build` (a plain `cmake --build`, so it exits nonzero when the build fails), then `test-auth-unit` and
  `test-quest-shared`, the Python test suite and source-invariant checks. The Go system suites are
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
  orchestration.
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
- Test cross-compiled DLLs under Wine before merging.
- Embed a PE VERSIONINFO resource in every DLL.
- Derive version from git at build time (no manual version bumps).
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

## Logging (Structured, Always)

Logging follows `docs/standards/logging.md`: its levels, tags,
volume budget and the `Log()` call are the rules. No `printf`, no `OutputDebugString`, no `cerr`.

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

Release files are signed outside the build, by the signer pipeline `RELEASING.md` describes; the release build signs
nothing (`build.yml` configures `-DNEVR_CODESIGN_REQUIRED=OFF`). `just sign` signs a local `dist/` with the private CA
in `certs/`, for development builds only.

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
constexpr std::size_t kMaxRetries = 3;
using Result = std::variant<Success, Error>;

// Bad
#define MAX_RETRIES 3
typedef struct _Result Result;
```

### Ownership is explicit

- `std::unique_ptr` for sole ownership. Never raw `new`/`delete`.
- `std::shared_ptr` only when ownership is genuinely shared — not as a
  default.
- `std::string_view` for non-owning string references. Never
  `const char*` + strlen.

### Error handling

- Return `std::optional` instead of an out-parameter.
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
  API.
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
| Missing `.rc` version resource             | No PE version info                | Add VERSIONINFO resource           |
| Plugin doesn't implement interface         | Won't load at runtime             | Follow plugin lifecycle            |
| No Wine test step in CI                    | Cross-compiled DLL may not run    | Add Wine test job                  |
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
