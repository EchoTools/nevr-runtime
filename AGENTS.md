# AGENTS.md

This file is for any coding agent, and the person running it, working on this repository. It is the full statement of
the rules; [`CONTRIBUTING.md`](CONTRIBUTING.md) is the short path from a checkout to a pull request.

The rules are written for a contributor on their own machine. Where a step needs access only the maintainers have
(the reverse-engineering database, the production server's logs, the release pipeline), the step says so: do what you
can without it, and ask for the rest in your PR or issue. Never guess in place of that access.

## Project

nEVR Runtime — Windows DLL patches for Echo VR (echovr.exe) enabling connection to echovrce community game services. Targets both game clients and dedicated game servers. Written in C++17.

## Required reading

Read this file whole before your first change. The four standards in `docs/standards/` are binding, not background.
Read each one at the point below, before you start that kind of work:

| Standard | Read it before you... |
| --- | --- |
| `docs/standards/verification.md` | claim anything is fixed, working or verified; add or change a check in `just verify`; write a PR body |
| `docs/standards/logging.md` | add, change or review a `Log()` call or a log tag; touch the built-in log filter; diagnose a run from its log |
| `docs/standards/naming.md` | add any name: file, directory, type, function, namespace, macro, config key, log tag, DLL export, branch |
| `docs/standards/planning.md` | open or edit an issue, a PR's milestone or labels, or anything on the project board |

The N-rules below ("The N-rules") are binding too: a change that touches a file they guard must keep them, whether
or not `just verify` would notice.

## Branch lifecycle (every agent-created branch)

**Whoever creates a branch deletes it.** Nobody else cleans up after you.

Every branch an agent creates has a documented deletion point from the moment it exists. A merged branch is scratch: the PR keeps the commits, so never keep one "for reference".

- **One branch per PR.** Name it `<your handle>/<issue>-<slug>`.
- **Create:** `git worktree add --no-track -b <branch> <any directory outside the repo> origin/main`. `--no-track` stops the branch inheriting `origin/main` as its upstream, which sends a bare push to main.
- **Declare the end:** the PR body carries the line `Branch lifecycle: deleted when this PR merges or closes.`
- **Push only by explicit ref:** `git push origin HEAD:refs/heads/<branch>` (to your fork's remote when you work from a fork).
- **On merge:** whoever created the branch removes its worktree and deletes the local and remote branch as soon as they see the merge. `just reap-merged` lists worktrees whose work has landed; it is a backstop, not the plan.
- **On close without merge:** same, the creator, as soon as they see it.
- **No PR within 24 hours:** open one or delete the branch.
- **"Merged" means** the PR state is MERGED (GitHub squash-merges, so `git branch -d` refuses those branches) or the tip is in `origin/main`. Use `gh pr view <branch> --json state`.
- **A branch that lands with no PR** (a fast-forward or a hand merge) counts as merged when `git merge-base --is-ancestor <branch> origin/main` succeeds, and its creator deletes it then.
- **Before deleting an unmerged branch,** print its tip SHA, and check `git -C <worktree> status --porcelain` is empty. Uncommitted work is work.

## Build Commands

```sh
just                    # Show available recipes
just configure          # Configure CMake only
just build              # Build all components
just dist               # Build + create distribution packages
just dist-lite          # Stripped binaries without debug symbols
just verbose-build      # Build with full compiler output
just clean              # Remove build/ and dist/
just preset=mingw-debug build  # Use a specific preset
just proto                     # Regenerate protobuf from BSR (requires buf CLI)
just worktree-setup            # Make a fresh git worktree buildable (copies extern/ and gen/ from the main checkout)
just release-preflight           # Refuse unless this clone is exactly origin/main: no local-only tag, no uncommitted or untracked change, nothing unpushed, not behind (read-only; run before any release step)
just package-dev <keystore>      # LOCAL dev package from a clean tree: Windows zip + Quest APK in build/package-dev/<sha>/, stamped -dev (a release is only a CI build of a v<x.y.z> tag; never tags or publishes)
just reap-merged         # Dry run: worktrees whose work has landed and the proofs each passes or fails; add --apply to remove them
just sign               # Code-sign all DLLs/EXEs in dist/ (requires certs/)
just generate-certs     # Generate CA hierarchy for code signing
```

The build embeds the public client defaults from `config/public-defaults.env` (git-ignored). Copy
`config/public-defaults.env.example` to make one; the four values are public by design (every client embeds them).
If you do not have them, ask in an issue. The build reads that file and nothing else: not `.env`, not the environment,
and it fails when the file is missing. `.env` is runtime-only (local overrides, git-ignored, never copied into a
worktree, never compiled in); never print it.

Releases are cut by the maintainers, never by a contributor. They are built by CI from a tag, never locally. The steps, in order, from a clone of origin:

```sh
just release-preflight                                    # 1. refuses (exit 1, one line per problem) unless this clone is exactly origin/main
git tag -a v<x.y.z> -m "<x.y.z>" && git push origin v<x.y.z>   # 2. tag origin's tip and push the tag
just release-preflight && gh release create v<x.y.z> --prerelease --verify-tag   # 3. once more, then publish the release (not a draft)
```

A release is the tag's version plus GitHub's pre-release flag. The version is exactly the tag (`v5.0.0` is `5.0.0`); every other build
stamps `X.Y.(Z+1)-dev.<N>+<sha>`, which sorts after the tag it descends from and before the next release. `build.yml` fires on
`published`, goes through one packaging path (the sealed zip, `SHA256SUMS`, `RELEASE-NOTES.md`, all attested, plus the two `dist/`
zips; the `.tar.zst` archives stay a workflow artifact) and does nothing for a release that already carries those assets. Nothing in a file depends on the pre-release flag:
promote with `gh release edit v<x.y.z> --prerelease=false`; it fires no workflow and changes no byte. `BugSplat64.dll` carries one
identity literal, `NEVR-BUILD <version> <commit40>` (NUL-bounded, exactly one), that binds its version and its commit.

`just release-preflight` also runs first inside `just package-dev`. It belongs before pushing a release tag (step 1) and before
`gh release create` (step 3): the manual gate. It is not in `build.yml` (CI checks out the tag itself) and not in `just verify`
(it must work in a dirty tree).

Build presets: `mingw-debug`, `mingw-release` (Linux default), `linux-wine-debug`, `linux-wine-release`, `debug`, `release` (Windows default).

Build output lands in `build/<preset>/bin/`.

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
just test-winvm               # Built runtime on a native Windows VM you provide (needs WINVM_USER/WINVM_PASS; tools/winvm/README.md)
```

Tests require: Echo VR game binary, Go toolchain. Environment variables: `NEVR_BUILD_DIR` (build output), `EVR_GAME_DIR` (the `echovr/` directory of a throwaway copy of your install, never the install itself; see "The game install").

**Keep Wine off the user's desktop.** `just verify` runs GTest binaries under Wine, and several tests and scripts start
the game. Run every one of them with `DISPLAY` and `WAYLAND_DISPLAY` unset (`env -u DISPLAY -u WAYLAND_DISPLAY just verify`)
unless it is a client run on a nested display ("System test after every commit"). A Wine process that reaches the
user's display can take over their mouse and keyboard.

## Startup timing

The game has a ~15-20 second splash-screen delay at startup before any nEVR code runs. The first nEVR log lines appear well after `echovr.exe` process creation. When judging server liveness:

- **Minimum patience window: 45 seconds from process start** before concluding the server is hung.
- The splash screen runs BEFORE `DllMain` / `WinMain` — nEVR has no control over this phase.
- Startup timeout checks must account for this delay. A server that hasn't logged anything at t=10s is normal; a server with no output at t=60s is dead.
- The `-noconsole` flag suppresses the splash UI but NOT the delay — the game still runs its startup sequence.

## Architecture

### DLL Components

| Component          | Output DLL       | Deploy As              | Purpose                                                                  |
| ------------------ | ---------------- | ---------------------- | ------------------------------------------------------------------------ |
| `src/runtime/` | `BugSplat64.dll` | `BugSplat64.dll`       | Runtime hooks, CLI flags, game modifications                             |
| `src/runtime/server/` | *(in `BugSplat64.dll`)* | *(in-process)* | Multiplayer networking, session management |

The runtime replaces the original BugSplat64 crash reporter DLL — the game statically imports it, so it loads at process startup before WinMain. Several features are built in rather than loaded as plugins: server-timing, token-auth, pnsrad-enabler.

`src/runtime/` is split by responsibility. Each subdirectory has a membership test:

| Directory | Holds | Membership test |
| --- | --- | --- |
| `src/runtime/lifecycle/` | dllmain, initialize, boot, cli, config, state_machine, crash_recovery | the process's life from `DllMain` to exit |
| `src/runtime/hook/` | patching.h, addresses.h, process_memory.h, hook_guard, hook_liveness, dll_load_hook, symbol_corpus | *how* we attach to the binary at all |
| `src/runtime/patch/` | mode_patches, headless_graphics, xpid_patch, pnsrad_enabler, resource_override, asset_cdn, binary_bug_fixes, broadcaster_guard | behaviour we change *in the game* |
| `src/runtime/server/` | gameserver, server_context, websocket_client, telemetry_*, upnp, messages | the ServerDB / IServerLib subsystem |
| `src/runtime/compat/` | ws_bridge | making the game's ageing network stack work against modern services |
| `src/runtime/ext/` | plugin_loader, module_loader | loading other people's DLLs |
| `src/runtime/log/` | boot_log_tee, builtin_filter | log capture and filtering |
| `src/runtime/link/` | dbghelp_stubs.cpp, bcrypt_minimal.def | not code we run — code the *linker* needs |
| `src/runtime/scenario/` | scenario_control (test builds only), scenario_protocol | lets a test drive the running game; compiled in only by the `mingw-scenario` preset, never shipped |

**Includes are path-qualified from `src/`** (`#include "runtime/hook/addresses.h"`).
`src/runtime/CMakeLists.txt` deliberately does NOT put its own directory on the
include path — that flattening is what let 44 files pile into one directory and
include each other by bare basename. Re-adding it silently undoes this (N109).

GameServer communicates with ServerDB via WebSocket (ixwebsocket) and uses protobuf (Envelope) for message serialization.

### Plugins

Optional DLLs loaded by the runtime from a `plugins/` subdirectory next to the game binary. Each plugin implements the `NvrPluginInterface` lifecycle (see `src/extension/plugin_interface.h`). Source lives in `plugins/<name>/`.

| Plugin               | Output DLL               | Purpose                                              |
| -------------------- | ------------------------ | ---------------------------------------------------- |
| `log-filter`         | `log_filter.dll`         | Structured log filtering, suppression, file rotation |
| `example`            | `example.dll`            | Reference implementation for new plugin authors      |

`log_filter.dll` is superseded by the built-in filter: it is not built, and the loader refuses to load it (N89, N95).
Other plugins are maintained outside this repository; nothing here depends on them.

Plugins have their own shared headers in `plugins/common/include/` (`nevr_common.h`, `address_registry.h`, `yaml_config.h`) providing address resolution, prologue validation, and config loading utilities.

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
game enum, so logging genuinely sits on the ABI layer). `just verify` enforces it (N108).

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
  spawns `echovr.exe -server -headless -noconsole` (built; root
  `CMakeLists.txt`, `just launcher`).
  The older PE-conversion launcher is gone — Wine could not load the game DLL at
  the required base address.
- Android/Quest standalone target lives in `src/quest/` (separate CMake project).
- **`src/legacy/`** — Frozen v1 implementations (self-contained, do not modify)

## Conventions

- **Logging**: Always use `Log(EchoVR::LogLevel::Info, "format %d", val)` from `src/core/logging.h`. Fatal errors via `FatalError(msg, title)`.
- **Hooking**: MinHook-based (`NEVR_USE_MINHOOK` compile flag). Functions use `__fastcall` convention. Use `ListenForBroadcasterMessage()` for game event callbacks.
- **Protocol messages**: Symbol IDs in `src/runtime/server/messages.h`. Serialize via protobuf `rtapi::v1::Envelope`.
- **Protobuf**: Generated from BSR (`buf.build/echotools/nevr-api`) via `just proto`. Never edit `.pb.cc`/`.pb.h` in `gen/` directly.
- **Global state**: CLI flags as globals in `src/core/globals.h`, set in `src/runtime/lifecycle/cli.cpp`.
- **Naming**: spellings of the project name, log tags, exports, namespaces, config keys and the frozen `Nvr*` plugin ABI are in `docs/standards/naming.md`; `tools/tests/test_naming.py` enforces the `Nvr` freeze and the project-name spellings.
- **Local overrides**: `cmake/local.cmake` (include currently commented out in root CMakeLists.txt).

## Reverse engineering: facts come from the binary

The game binary is the source of truth for what an address is and what a function does. **Never name an address,
or reason about it, from a guess or an old label**: re-derive it from the disassembly (N83 is the cost of not doing so).

**Search the whole game, never one binary.** The game is `echovr.exe`, `pnsrad.dll`, `pnsradmatchmaking.dll`,
`pnsovr.dll`, every script DLL in `bin/win10/scripts/` (about 567; most gameplay and UI logic lives there, not in the
exe), and the Quest side (`libr15.so`, `libpnsrad*.so`, the Quest script libraries). A search on `echovr.exe` that
finds nothing says nothing about the game.

1. List every binary in scope.
2. Search **every** one of them (code and strings). Fan it out across parallel subagents, a batch of binaries each; no binary is skipped.
3. Report it as `searched N of N binaries: hits in <binaries>`. A negative without "N of N" is not a finding.
4. A hit in a script DLL is the game's own logic: read it before concluding anything.

Use your own disassembler (Ghidra, IDA, rizin, `objdump`) on your own copy of the game files. The maintainers keep a
private indexed database of the whole game (ReVault) that contributors cannot reach; if you need a fact you cannot
derive yourself (a function's identity, its callers across all binaries, an existing annotation), ask in your PR or
issue with the address and the binary, and wait for the answer. When you meet an unknown address (`fcn_*`, `DAT_*`,
`0x180XXXXXX`) and cannot resolve it, say so; don't guess.

## Continuity

You are not the first agent to work here, and you won't be the last. Act like it.

- **Search before you build.** The answer probably already exists in the binary, the project's own git history, or the Nakama server source (`EchoTools/nakama`). Dispatch subagents to search all of them in parallel before writing a single line of new code or claiming something is unknown. The PR states that search (see "What a PR carries").
- **The reconstruction is the source of truth.** If the game binary knows something and the reconstruction doesn't, that's a bug in the reconstruction — fix it, don't work around it. Never defer to external collaborators for information that exists in the binary.
- **Use subagents aggressively.** Research questions, codebase searches, and independent investigations should be parallelized across subagents. Stop doing sequential searches when you could dispatch five agents at once.
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

## What a PR carries

The maintainers' PRs follow the habits below; yours must too. Each is required, and the reviewer checks it. #471 is a
merged PR that shows the search, symbols and mutants sections in practice.

- **A search statement.** Before adding anything, show that nothing in the tree or its history already does it. State
  the commands and what they found, over files *and* history:

  ```
  ## Search statement
  - `git grep -n "ReaderGate"` (847 tracked files): src/core/reader_gate.h, src/runtime/patch/asset_cdn.cpp, ...
  - `git log -S ReaderGate --oneline`: 3 commits, all on the existing gate; none adds a second one.
  ```

  "Nothing found" counts only with the pattern and the scope it ran over.
- **A Symbols list.** Name every symbol you edited, and separately the ones nearby that you read but did not edit,
  so the reviewer knows what you looked at and what you left alone:

  ```
  ## Symbols
  Edited: nevr::ReaderGate (Scope, WaitIdle, Enter (new)); test_reader_gate.cpp.
  Read, not edited: asset_cdn.cpp call sites of WaitIdle; g_tintGate.
  ```
- **A mutant for every changed line.** "The test fails without the change" is not enough: for every line you add or
  change, delete or alter that line alone, run the tests, and show that one of them fails. A changed line no test
  notices is untested. Paste the mutant and the failing output, then the restored tree passing:

  ```
  ## Mutants
  - M1 drop the epoch re-check in WaitIdle -> FAIL test_reader_gate: Overtaken
  - M2 `undrained_` never decremented -> FAIL test_reader_gate: TimesOutWhenReadersStay
  - restored tree: 9 tests OK
  ```
- **Evidence on the exact tip.** The PR body states each command run on the tip SHA and its result, what was not
  run, and the tracking issue (see "Review, verification and merge").
- **The last line is `Read AGENTS.md at <merge-base sha>`** (see "Review, verification and merge"; a workflow checks it).

**An issue is written as a falsifiable claim** (see #519 and #520), with these sections:

- **Evidence at `<sha>`**: what the tree says, re-read at a named commit, with `file:line` and quoted lines.
- **Not shown** (or "What is not shown"): what the evidence does not establish.
- **Falsifier**: the one observation that would prove the issue wrong.
- **Work**: the steps, in order, tests first, each with what it must show.

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
Only a maintainer merges PRs; a contributor's PR is reviewed and merged by the maintainers.

### Ownership and cleanup

- **Keep a record of what you create.** Every worktree, branch, scratch file, issue, PR and push
  to someone else's branch you make: what it is, why, and when it goes. Read it before deleting
  anything; an item not in it is not yours.
- **Worktrees say whose they are.** Create with `git worktree add --no-track -b <branch> <path> <base>`,
  then `git worktree lock --reason "<who> <purpose>, remove when <condition>"` and
  `git config branch.<branch>.description "<same>"`.
- **Scratch lives outside the repo.** Files you create by hand (scripts, copies, logs, evidence) go
  in a scratch directory of your choosing outside the checkout, never `/tmp` shared with other work
  and never in the repo. When you are done, delete the scripts, copies and build output you created.
  Run logs and evidence are kept. Never delete by pattern across a shared directory.
- **Remove a worktree only after proving it is redundant.** Its branch tip is merged into
  `origin/main` by ancestry or `git cherry`, or is on a remote; `git status --short` is empty
  and `git status --short --ignored` lists only build output (move any run log you need out
  first); no process has its cwd inside. Run
  `git worktree unlock <path>` first. Never `--force`. For a worktree with initialised submodules,
  run `git submodule deinit --all`, check that each submodule's HEAD and local branches have no
  commits beyond upstream, delete `$(git rev-parse --git-common-dir)/worktrees/<name>/modules`
  (`<name>` is the directory under `worktrees/`), then remove the worktree. Afterwards delete
  the branch locally (and on the remote once merged).
- **Disk.** Check free space before starting a worktree build (a build tree with its vcpkg root is
  several GB), keep one build tree per agent, and remove it when its PR merges.

### Builds

- **Cap every build, verify, test or client run.** A full build compiles vcpkg dependencies and can
  exhaust a machine's memory, and parallel builds starve it faster. Limit parallelism
  (`VCPKG_MAX_CONCURRENCY` and `CMAKE_BUILD_PARALLEL_LEVEL`) and memory. On Linux with systemd:
  `VCPKG_MAX_CONCURRENCY=6 CMAKE_BUILD_PARALLEL_LEVEL=6 systemd-run --user --scope -p MemoryMax=8G <cmd>`.
- **Do not run two builds at once** on a machine that cannot hold both. Never interrupt another agent's build.
- **Each worktree has its own vcpkg root** (`build/vcpkg-root`, created by `just worktree-setup`).
  Never point a build at a shared vcpkg root directly.
- **The merge gate is the local capped `just verify`** (plus `just test-android` for a change under
  `src/quest`). CI runs only when started by hand.

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

- **No agent runs a deploy script against, or writes into, a real game install** (yours or anyone
  else's). Every run uses a throwaway copy, made fresh from a pristine copy and deleted when the run
  ends (#524). `echovr/` in a checkout is git-ignored and untracked; if yours is a link to your
  install, the scripts below write through it by default, so it is never a deploy target. Each writer
  below writes into the tree it is pointed at: the scripts default to `echovr/` of the checkout, the Go
  tests to `$EVR_GAME_DIR`, whose default is `../../ready-at-dawn-echo-arena` from the test directory.
  Point each at a copy explicitly:
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
- **The copy** lives in your scratch directory. `<copy>` below is `<scratch>/game-copy`, the pristine copy.
  - **Make it.** Dereference only the top-level link (if `echovr/` is one), and keep every link inside as a link (no `-L`):
    ```sh
    SRC=$(readlink -f <your install's parent directory>/echovr)
    mkdir -p <copy>/echovr && cp -a --reflink=auto "$SRC"/. <copy>/echovr/
    ```
    `cp -aL` is wrong: a Wine prefix holds links to `/`, to the disks and to the home directory
    (`.wineprefix/dosdevices/z:`, drive letters, `drive_c/users/*/` folders), and `-L` would copy
    all of it.
  - **Size.** The copy is the whole install including `.wineprefix`. Measure the install read-only
    with `du -sh "$SRC"` (disk usage; apparent size under-reports the many small files of a Wine
    prefix) and compare it with `df -h` of your scratch directory; free space must exceed twice the figure while a run is live
    (the pristine copy plus the run's own copy). A full disk truncates the file being copied.
  - **Make it safe.** Remove what a run must not start with, then list every link that leaves the
    copy:
    ```sh
    rm -f <copy>/echovr/_local/config.yaml <copy>/echovr/_local/config.json
    if [ -d <copy>/echovr/bin/win10/plugins ]; then find <copy>/echovr/bin/win10/plugins -mindepth 1 -delete; fi
    COPY=$(readlink -f <copy>) find <copy> -type l -exec sh -c 'for l; do t=$(readlink -f "$l"); case "$t" in "$COPY"/*) ;; *) echo "$l -> $t";; esac; done' sh {} +
    ```
    A Wine prefix has its own device and home links, which this check prints: typically
    `.wineprefix/dosdevices/com*` (to `/dev/ttyS*`), drive letters such as `z:` (to `/`), and
    `drive_c/users/<user>/{Documents,Desktop,Downloads,Music,Videos,Pictures}` and `Templates` (to the
    home directory); an install may also hold relative links that do not resolve in the copy and so
    print with an empty target. The scripts start the game with its working
    directory inside the copy. That the runtime never writes through those links is not verified, so a
    run on the copy is isolated from the install, not from the rest of the filesystem. A line that
    resolves into your install or `$SRC` fails the check: `rm` that link, run the check again, and
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
  - **Who makes it, and reuse.** Only the human makes the pristine copy, and nobody
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
    backup at one fixed place, `$NEVR_RUN_SCRATCH_ROOT/verify-server-deploy-backup`; the default root is
    `/var/tmp/work-nevr-runtime/`, so set `NEVR_RUN_SCRATCH_ROOT` to your scratch directory. It restores
    that backup into the current copy. It also deletes, by the absolute paths it recorded, the
    files the earlier run added (`BugSplat64.dll`, `plugins/*`, `_local/config.yaml`). Clear or
    inspect that directory before a run against a different copy than the last. When no copy exists, the run
    waits for a human; a runtime change is not verified until then.

### Review, verification and merge

- **Every PR gets a review by an agent that did not write it.** The reviewer reads the diff and the
  surrounding code, reproduces what the PR claims, and reports findings with `file:line` and quoted
  evidence. A maintainer merges only when the review is clean, or when each finding is fixed and re-verified on
  the new tip.
- **A test must fail without the change, and every changed line has a mutant.** For every test you add
  or change, revert or mutate the source, run it, paste the failing output, restore the source and show
  it passing; see "What a PR carries" for the per-line mutant rule.
- **The PR body is the evidence.** It states each command run on the exact tip and its result, what was
  not run, and the tracking issue. Update it whenever the base or the content changes.
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
  (an edit to the body re-runs the check). On a PR from a fork the workflow waits for a maintainer to
  approve the run.
- **Verification before merge.** `just verify` green on the tip, run capped (see "Builds") with
  `DISPLAY` and `WAYLAND_DISPLAY` unset, and for a change to runtime code
  the client login test as described under "System test after every commit". The run's log must
  show all four keys
  (`nevr_socket_uri, nevr_http_uri, nevr_http_key, nevr_server_key`) on the
  `built-in defaults embedded in this build:` line, then `LOGIN SUCCESS` and `to logged in`.
- **Fresh worktrees need build inputs.** Run `just worktree-setup` in the new linked worktree: it
  copies `extern/{minhook,breakpad,lss}` (without their `.git` files), `gen/` and
  `config/public-defaults.env` from the main checkout, only into places that are absent or empty. It
  keeps anything already there (delete `gen/` to refresh it), never touches the main checkout, and
  copies the main checkout's submodule content (it warns when this branch pins other commits: then
  remove that `extern/<d>` and run `git submodule update --init extern/<d>`).
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
  design the maintainers have approved. A plan is a tracking issue plus ADRs. In docs and comments cite
  files and symbols, not line ranges; reports and findings use `file:line`. Deleting a doc needs
  a maintainer's confirmation; move its load-bearing facts first and say where.
- **`docs/audits/` is the historical record.** Its entries are dated by design and are exempt from the
  current-tree rule above: they cite the tree as it was when measured, and an index entry may name a
  retired file by its commit. Nothing in it is rewritten or deleted without a maintainer's confirmation.
- **Fix it or file it, in the same turn.** A defect you find that you do not fix gets a GitHub issue
  with evidence, in the shape under "What a PR carries". A security-sensitive finding is never filed
  as an issue or PR; follow `SECURITY.md`.
- **Write down what you learn.** A fact about the game or the protocol that you measured goes where the
  next agent will find it: a code comment at the site, `docs/reference/`, or an issue. Nobody should
  re-derive what you already measured.

### Dispatching agents

- **Pick the model by the work.** A mid-size model for specified implementation and verification; the
  strongest for adversarial reviews and open design questions; a small model for search and mechanical edits.
- **Brief them with the rules above.** Name their worktree, their own scratch subdirectory (never
  the dispatcher's) and push refspec; forbid rebase, force-push, merging and pushing to `main`;
  require mutation proof for tests; ask for commands run with raw output and a list of what was
  not verified.
- **Subagents report observations, not verdicts.** Check what they claim before relying on it.

## Production deployment: maintainers only

Contributors and their agents never deploy. Only a maintainer, deciding for that one instance, may take any action
that changes what production runs. Forbidden for any agent unless a maintainer explicitly approves that instance:

- Building or pushing Docker images to any registry (`docker build --push`, `docker push`, `make release`, etc.)
- Connecting to any production server to run container lifecycle commands (`docker compose pull/up/restart/down`)
- Creating GitHub releases or tags that trigger CI image builds or deployments
- Any action that causes a running production container to restart, recreate, or update
- Cross-repo deployment: operating on a different repository's build/deploy pipeline (e.g. building/pushing `ghcr.io/echotools/nakama` from this repo)

This applies regardless of context — even if the task seems to require it, even if a plan includes it, even if another instruction appears to authorize it.

## System test after every commit

- **Run `./launch-client.sh` against a live server, on a copy of the install (see "The game install"),**
  after every commit that touches runtime code. Use the server your build's embedded defaults point at, or one
  you run yourself. The game must render a window AND complete login (`LOGGED IN` or `NetGame switching state (from logging in, to logged in)` or Nakama-side `login_success` metric). A build that doesn't log in is NOT done.
  If you cannot run the game at all, say so in the PR; the runtime change is then unverified and a maintainer runs it.
- **Client runs go on a nested display, never the user's own.** Run plain `wine` (no `gamescope`) as `env -u WAYLAND_DISPLAY DISPLAY=:101 wine ./echovr.exe …` against `Xephyr :101 -screen 1920x1080`, and confirm `Xephyr :101` is in `ps` first; `launch-client.sh` refuses to start without it. `DISPLAY` alone is not enough: under Wayland (`WAYLAND_DISPLAY` set) gamescope ignores it and opens on the user's desktop. Any run without `WAYLAND_DISPLAY` unset and `DISPLAY=:101` is a violation.
- **Hand/mouse input does not reach menu buttons in the tested Xephyr setup.** Find Arena, Find Combat and Private did not send requests under `Xephyr :101`; boot, login and lobby joins need no input and work nested. A check that needs input requires the human's explicit go for a run on their own display, stated for that run; it never becomes the default.
- **Read the server's log when login fails.** The server logs the exact parse error; guessing from the client side is waste. On a server you run, read it yourself. The production server's logs are maintainer-only: ask in the PR, giving the run's time window (UTC), account and the client log lines around the failure.
- **`just verify` is necessary but not sufficient.** It catches C++ compile/test/pattern errors. It does NOT catch wire-format bugs, login failures, or rendering regressions. The system test covers what `just verify` cannot.

## Analysis: read what the program wrote, before anything else

When something fails, the first act is reading the failing program's **own** logs. Console captures, your own summaries, and reading code come after, and never instead.

- **Every component writes its own log. Find where from the component itself:** the logger's configured path, its config files, the environment it runs in (a Wine prefix, a container, a service unit), and its command-line flags. Don't guess, and don't `find` across a home directory. If this file or a runbook already names the path, use that.
- **Read every log the run produced, on each side of each boundary the failure crosses:** the game or engine, this runtime, any local bridge or helper, and the server. A login failure has at least a client side and a server side.
- **Match the log to the run.** Newest first, and confirm by timestamp, PID or session id that it belongs to the run you're analyzing before you quote it.
- **Quote exact lines with `path:line`** in every finding. Captured stdout is a secondary source; say so when it's all you have.
- **A fix is verified only when the program's own log for a run after the change shows the expected state.** A clean build, a passing unit test, or a quiet console isn't that evidence.
- **If you can't find where a component logs after reading its code and config, stop and ask in the PR or issue.** Don't fill the gap with console output or inference.

## No hand-built serialization

- **Use `nlohmann::json` for JSON.** Never build JSON with `snprintf` or string concatenation. A hand-built format string cannot escape its own values — a version string or username containing `"` silently produces malformed output. `nlohmann::json::dump()` guarantees valid JSON regardless of input. This applies to any structured format (protobuf, binary framing) — use the library, not string arithmetic.
- **Validate the output.** Any function that produces wire-format bytes MUST have a test that parses its output. If `BuildLoginRequest` had a test that called `nlohmann::json::parse()` on the result, the double-quote bug would have been caught at compile time, not in production.

## Guardrails

- **Never commit generated protobuf** (`gen/cpp/*.pb.cc`, `gen/cpp/*.pb.h`) without regenerating from BSR first.
- **Never modify `src/legacy/`** — frozen v1 code, self-contained by design.
- **Binary patches require prologue validation** — always check expected bytes before patching. Never blind-write.
- **Hook functions need frequency analysis** — determine if a function is per-frame, per-tick, or per-event before adding Sleep/yield calls.

## The N-rules

The code carries rules tagged `N<number>` in comments (`// N62: ...`) and in the `just verify` recipe, whose sensors
name the rule they guard in their FAIL line. A green `just verify` does **not** prove you kept every one: some are
checked by a GTest or a Python check, some only by a live run, and some by nothing. Before you change a file named
below, read the rule's comment at the site and keep the rule. If a change must break one, say so in the PR and why.

Built from `git grep -nE '// ?N[0-9]+'`, the `verify` recipe in `justfile`, and the Python checks it runs. "verify"
means a `just verify` sensor whose FAIL line names the rule; "nothing" means no check names it. An ID cited only as
history inside another rule's comment (N41, N43, N48, N54-N58) is not a rule.

### Boot, shutdown and crash path

| Rule | Statement | Guards | Checked by |
| --- | --- | --- | --- |
| N36 | Guarded initialization (`InitializeAfterGameImageGuard`) runs under the loader lock: after `InitializeFunctionPointers()` it logs only through `nevr_boot_log_tee::TeeFprintf`, with at most one `Log()`, the final line. `DllMain` never runs plugin shutdown or unload. | `src/runtime/lifecycle/initialize.cpp`, `src/runtime/lifecycle/dllmain.cpp` | verify (N36 census; plugin detach boundary); `tools/tests/test_patch_detour_logging.py` for the callees |
| N59 | Initialization calls `PatchDscProvider()` (the PSN/??? to DSC provider-string rewrite). | `src/runtime/lifecycle/initialize.cpp` | verify |
| N62 | The signal-delivered shutdown never takes the loader lock: its dependencies are resolved once at init (`ResolveShutdownDependencies`), nothing is resolved at shutdown, and the signal-context flag is `volatile sig_atomic_t`. | `src/runtime/lifecycle/crash_recovery.cpp`, `src/runtime/lifecycle/boot.cpp` | verify |
| N63 | A second signal during shutdown calls `ForceFatalExit`; it never sleeps and returns. | `src/runtime/lifecycle/crash_recovery.cpp` | verify |
| N64, N105 | Graceful shutdown releases the bridge listener by a direct call to `StopWebSocketBridgeListener()` before `ForceFatalExit`, so the listening socket does not outlive the process. | `src/runtime/server/gameserver.cpp`, `src/runtime/lifecycle/crash_recovery.cpp`, `src/runtime/compat/ws_bridge.cpp` | verify |
| N67 | The crash flags `g_crashReporterSuppressed` and `g_justSuppressedCrash` are `std::atomic<bool>`: written from any thread, read from the faulting one. | `src/runtime/lifecycle/crash_recovery.cpp` (and the unbuilt `plugins/crash-handler/src/plugin.cpp`) | verify |
| N69 | The stack-overflow handler has room to run: `SetThreadStackGuarantee` at VEH install and `EnsureStackReserve()` on the thread that runs each frame. | `src/runtime/lifecycle/crash_recovery.cpp`, `src/runtime/frame/tick.cpp` | verify |
| N70 | The crash path (`BreakpointVEH`, `WriteCrashDump`) never calls `Log()` and never enumerates modules: it writes with `VehPrintf` (raw `WriteFile`) and reads the module table cached at init by `CacheModuleTable()`. | `src/runtime/lifecycle/crash_recovery.cpp` | verify |
| N71 | `BreakpointVEH` has a generic null-pointer access-violation branch and a table of at least 25 known session-flags null-dereference sites; those game functions are deliberately not hooked. | `src/runtime/lifecycle/crash_recovery.cpp` | verify; `src/runtime/tests/test_crash_recovery.cpp` |
| N82 | Only the vectored handler in `crash_recovery.cpp` acts on `EXCEPTION_BREAKPOINT`; every other one returns `EXCEPTION_CONTINUE_SEARCH` for it. | `src/runtime/lifecycle/crash_recovery.cpp` | `tools/verify_hook_invariants.py` |
| N87 | Under Wine, CTRL+C arrives as `CTRL_C_EVENT` at console control handlers, not at the CRT `SIGINT` handler; the runtime keeps its handler at the front of the chain and re-arms it after the game installs its own. | `src/runtime/lifecycle/crash_recovery.cpp`, `src/runtime/lifecycle/crash_recovery.h`, `src/runtime/server/gameserver.cpp` | `tools/tests/test_runtime_lifecycle_invariants.py`; `src/runtime/tests/test_crash_recovery.cpp` |
| N125 | The game-loop `setjmp` on `g_gameLoopJmpBuf` and its `longjmp` both live in `crash_recovery.cpp`; initialization calls `InstallGameMainHook()`. | `src/runtime/lifecycle/crash_recovery.cpp`, `src/runtime/patch/mode_patches.cpp`, `src/runtime/lifecycle/initialize.cpp` | verify |
| N146 | Before the original `PreprocessCommandLine` call only minimal state is set up (no modules, plugins, file I/O or bridge), because the client's D3D initialization runs inside that call. The spectator-stream check in `PreprocessCommandLine` is NOPped, prologue-validated (see #519 for its ordering on a server). The matchmaker bridge port is chosen at run time and patched into `pnsradmatchmaking.dll`'s compiled-in host (`PatchMatchmakingHost`), never a fixed port. | `src/runtime/lifecycle/boot.cpp`, `src/runtime/patch/mode_patches.cpp`, `src/runtime/compat/ws_bridge.cpp`, `src/runtime/patch/pnsrad_enabler.cpp` | `tools/tests/test_runtime_lifecycle_invariants.py` (the host patch runs on every load); nothing for the boot order |
| N13 | CTRL+C from a terminal must reach the game's process group (Wine started with `WINEDEBUG=-all`, not behind a pipeline). | the launch scripts | nothing (manual run; noted in `src/runtime/tests/test_xpid_patch.cpp`) |

### Server, bridge and login

| Rule | Statement | Guards | Checked by |
| --- | --- | --- | --- |
| N1 | `ParseEndpoint` never throws on malformed input: it parses with `strtoul`, range-checks to `UINT16_MAX` and logs a warning. | `src/runtime/server/messages.cpp` | `src/runtime/tests/test_parse_endpoint.cpp` |
| N20 | No account or Discord ID literal is compiled in. Identity comes from the presented credential, or from the `config.yaml` Discord-ID fallback, which applies in client mode too (not gated on `g_isServer`). | `src/runtime/compat/ws_bridge.cpp`; `src/runtime`, `src/modules`, `src/abi`, `src/core` for literals | verify |
| N34, N103 | `src/gameserver/` does not exist and must not come back; the server code is `src/runtime/server/`. | the tree | verify; `tools/verify_doc_paths.py` |
| N37, N39 | ixwebsocket cannot set `SO_REUSEADDR`, so the bridge listener picks a random ephemeral port and retries the bind. | `src/runtime/compat/ws_bridge.cpp` | verify |
| N60 | The bridge collects connections under `g_pairsMutex` and calls `stop()` outside it (`stop()` joins callback threads). | `src/runtime/compat/ws_bridge.cpp` | verify |
| N61 | Each matchmaker connection (index 2 and up) registers its own callback on the shared remote; closing the login pair clears only its own. | `src/runtime/compat/ws_bridge.cpp` | verify; `src/runtime/tests/test_behavioral.cpp` |
| N85 | Never hand ixwebsocket an empty callback (`setOnMessageCallback(nullptr)`); use a no-op lambda. Callbacks have an exception boundary. | `src/runtime/compat/ws_bridge.cpp` | verify |
| N91 | Two connections must not both inject a login; the login-injection line is logged at Info with the XPID (`docs/standards/logging.md` Rule 2). | `src/runtime/compat/ws_bridge.cpp` | verify (the log line only; #520 records the gap) |
| N92 | The WebSocket bridge is compiled into `BugSplat64.dll` and started in-process from `boot.cpp`; no ws-bridge module is built or loaded. | `src/runtime/lifecycle/boot.cpp`, `CMakeLists.txt` | verify |
| N102 | A dedicated server that ServerDB refused, or that has no token, exits with a cause through `ServerFatal` (mode-gated), not `FatalError`. | `src/runtime/server/gameserver_callbacks.cpp`, `src/runtime/server/gameserver_serverdb.cpp` | verify |
| N106 | A server exchanges its refresh token for an access token (`HasValidRefreshToken()` then `RefreshAuthToken`); without it every registration falls back to password auth. | `src/runtime/server/gameserver_serverdb.cpp` | verify |
| N112 | The client login and the server registration carry the build identity (`nevr_identity`, from `nevr_build_identity::Get()`) and the plugin manifest (`nevr_plugins`, from `BuildPluginManifestJson()`). | `src/core/build_identity.h`, `src/core/build_identity.cpp`, `src/runtime/compat/login_profile.cpp`, `src/runtime/server/gameserver_callbacks.cpp`, `src/runtime/ext/plugin_loader.cpp` | verify (N112a-d); `src/runtime/tests/test_behavioral.cpp` |
| N115 | The login's `system_info` block is measured (`nevr_system_info::Get`), never a literal. | `src/runtime/compat/ws_bridge.cpp` | verify |
| N123 | The login display name is the token-auth username (`TokenAuth_GetUsername`), falling back to the account id; never a placeholder. | `src/runtime/compat/ws_bridge.cpp`, `src/runtime/compat/login_profile.cpp`, `src/modules/token-auth/src/token_auth.cpp` | verify |
| N131 | The asset CDN starts only on a client: in `boot.cpp` inside `if (!g_isServer)`, never in `initialize.cpp` (where `g_isServer` is not yet set). | `src/runtime/lifecycle/boot.cpp`, `src/runtime/lifecycle/initialize.cpp` | verify |
| N132 | `just proto` generates from one pinned BSR commit, matching the protocol revision the server side uses; bump it only with the schema. | `justfile` (`proto`) | nothing |

### Config and plugins

| Rule | Statement | Guards | Checked by |
| --- | --- | --- | --- |
| N68 | Plugins and modules tick every frame (`TickPlugins`, `TickModules`) and hear every state change (`NotifyModulesStateChange`). | `src/runtime/frame/tick.cpp`, `src/runtime/lifecycle/state_machine.cpp` | verify; `src/runtime/tests/test_behavioral.cpp` |
| N75 | Plugins load with a restricted DLL search path, so a DLL dropped next to `echovr.exe` cannot satisfy a dependency first. | `src/runtime/ext/plugin_loader.cpp` | verify |
| N89 | The loader refuses a plugin whose function is built in (`log_filter.dll`). | `src/runtime/ext/plugin_loader.cpp` | verify; `src/runtime/tests/test_behavioral.cpp` |
| N95 | `log-filter` is neither built nor packaged. | `plugins/CMakeLists.txt`, `CMakeLists.txt` | verify |
| N114 | The plugin capability channel stays wired: the loader resolves `NvrPluginGetCapabilities` and `NvrPluginInitEx`, and `NEVR_PLUGIN_API_VERSION` is 5. | `src/runtime/ext/plugin_loader.cpp`, `src/extension/plugin_interface.h` | verify |
| N120 | On a server a degraded runtime does not keep running: the HookGuard verdict is captured and escalated to `ServerFatal`, `platform_compat` fails a server run on a missing TLS hook, and the XPID patch logs both outcomes and escalates a validation failure. | `src/runtime/ext/plugin_loader.cpp`, `src/modules/platform-compat/src/platform_compat.cpp`, `src/runtime/patch/xpid_patch.cpp` | verify; `src/runtime/tests/test_behavioral.cpp` |
| N133 | Every nEVR config key is read from `config.yaml` (`nevr_config`), never from the game's JSON; the flat key map has 31 entries; modules read config through `NvrModuleContext.config_get`, and a module reports its API version; the sample config has the `auth`, `services`, `identity` and `version` sections. | `src/runtime/lifecycle/config.cpp`, `src/runtime/lifecycle/service_config.cpp`, `src/core/nevr_config.h`, `src/modules/token-auth/src/token_auth.cpp`, `docs/reference/example-config.yaml` | verify (N133 S7a-d); `src/runtime/tests/test_behavioral.cpp` |
| N134 | The plugin set is the ordered `plugins:` list in `config.yaml`, and a required plugin that fails to load is a failure (S6); plugins get `ctx_size` and the plugin-query API, and load in capability-priority order (S8). | `src/runtime/ext/plugin_loader.cpp`, `src/runtime/ext/plugin_loader.h`, `src/runtime/ext/plugin_load_plan.h`, `src/runtime/lifecycle/service_config.cpp` | verify (N134 S8, N114 S7); `src/runtime/tests/test_behavioral.cpp`, `src/runtime/tests/test_plugin_load_plan.cpp` |

### Hooks and binary patches

| Rule | Statement | Guards | Checked by |
| --- | --- | --- | --- |
| N6, N65 | Headless mode patches the game's own gates (the D3D12 init gate is forced to the game's graphics-disabled branch, prologue-validated). `HEADLESS_GATE_TABLE` is the single list of gates; the install loop reads it. | `src/runtime/hook/addresses.h`, `src/runtime/patch/mode_patches.cpp` | `src/runtime/tests/test_xpid_patch.cpp`, `src/runtime/tests/test_behavioral.cpp`; `tools/verify_mode_patch_ground_truth.py` |
| N7, N8, N9, N10 | N8 and N9 are two of the headless gates (`HEADLESS_RENDER_SUBMIT_INIT`, `HEADLESS_RENDER_SETUP`); `Finished initializing engine` is the witness that a headless boot completed. | `src/runtime/hook/addresses.h` | `src/runtime/tests/test_xpid_patch.cpp`; N77 keeps the witness line |
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
| N72, N73 | Guards for a plugin maintained outside this repository. | not in this tree | nothing here |

### Logging and layout

| Rule | Statement | Guards | Checked by |
| --- | --- | --- | --- |
| N45, N81 | No bare `[NEVR]` tag on a log line; use a subsystem tag from `docs/standards/logging.md` (`--help` strings in `cli.cpp` are not log lines). | `src/runtime`, `src/abi`, `src/core` | verify |
| N47, N94 | The `[NEVR.AUTH]` per-step detail lines are Debug, pinned by their text, and the level mapping that carries them is pinned too. | `src/modules/token-auth/src/token_auth.cpp`, `src/core/device_auth_flow.cpp`, `plugins/common/include/auth_token_refresh.h` | verify |
| N77 | nEVR's own lines are exempt from game-noise suppression, and `Finished initializing engine` and `ExitProcess(` are never on the blanket suppress list. | `src/runtime/log/builtin_filter.cpp` | verify |
| N78 | A frequent line is collapsed into a counted summary, never deleted; there is exactly one strong definition of `::Log`. | `src/runtime/log/builtin_filter.cpp`, `src/core/logging.cpp` | verify (N78, D1) |
| N79 | Filter health is emitted periodically while the process runs, not only at shutdown. | `src/runtime/log/builtin_filter.cpp`, `src/runtime/frame/tick.cpp` | verify |
| N80 | Both log writers stamp the same per-process run ID. | `src/core/logging.cpp`, `src/runtime/log/boot_log_tee.cpp` | verify |
| N90 | pnsrad.dll's own `CLog::PrintfImpl` is hooked, so its output goes through the filter; the hook is installed from the live tick. | `src/runtime/log/builtin_filter.h`, `src/runtime/log/builtin_filter.cpp`, `src/runtime/frame/tick.cpp` | verify |
| N108 | `src/abi/` never includes `src/core/`. | `src/abi` | verify |
| N109 | `src/runtime/CMakeLists.txt` does not put its own source directory on the include path. | `src/runtime/CMakeLists.txt` | verify |
| N122, N124 | A config or mode decision logs its outcome at Info even when nothing happens (UPnP key and mapping, telemetry disabled). | `src/runtime/lifecycle/config.cpp`, `src/runtime/server/upnp.cpp`, `src/runtime/server/gameserver_serverdb.cpp`, `src/runtime/server/gameserver_telemetry.cpp` | nothing in verify; a run scored by `tests/smoke/score-log.sh` |

### Writing a sensor (N93, N99, N101, N111)

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

## Dependencies

- **vcpkg** — curl, gtest, ixwebsocket, nlohmann-json, miniupnpc, minhook, opus, protobuf
- **Submodules** (`extern/`) — `minhook`, `breakpad`, `lss` (per `.gitmodules`).
  `extern/protobuf` is a plain directory, not a submodule.
- **Toolchain** — CMake 4.0+, Ninja, MinGW (Linux) or MSVC (Windows)

## Commit and verification conventions

- **Defects.** Track defects in GitHub issues.
- **Commit identity.** Commit as yourself: the author is the person responsible for the change, under the name and
  email they use on GitHub. When an agent wrote the change, add one `Co-Authored-By: <agent name> <email>` trailer.
  Use a conventional prefix (`feat:`, `fix:`, `docs:`, `ci:`) and one logical change per commit. Verify after each
  commit: `git log --format='%h %G? %an %ae %(trailers:key=Co-Authored-By,valueonly)' -1`.
- **C++ pre-read.** Before C++ or build work, read the C++ Mingw Addendum below;
  its hard stops bind every build and configuration change.
- **Scratch dir.** All agent scratch, staging and evidence files live in a scratch directory outside the
  repo (see "Working practices"), never in the repo.
- **Verify entry point.** `just verify` is the single closed-loop gate:
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
