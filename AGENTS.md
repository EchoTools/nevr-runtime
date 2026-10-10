# AGENTS.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

nEVR Runtime — Windows DLL patches for Echo VR (echovr.exe) enabling connection to echovrce community game services. Targets both game clients and dedicated game servers. Written in C++17.

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
just worktree-setup            # Make a fresh git worktree buildable (copies extern/, gen/, .env from the main checkout)
just reap-merged         # Dry run: worktrees whose work has landed and the proofs each passes or fails; add --apply to remove them (skill: merge-cleanup)
just sign               # Code-sign all DLLs/EXEs in dist/ (requires certs/)
just generate-certs     # Generate CA hierarchy for code signing
```

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
just test-winvm               # Built runtime on a native Windows VM (needs WINVM_USER/WINVM_PASS; tools/winvm/README.md)
```

Tests require: Echo VR game binary, Go toolchain. Environment variables: `NEVR_BUILD_DIR` (build output), `EVR_GAME_DIR` (game installation).

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
  spawns `echovr.exe -server -headless -noconsole` (built; root
  `CMakeLists.txt`, `just launcher`).
  The older PE-conversion launcher is gone — Wine could not load the game DLL at
  the required base address.
- Android/Quest standalone target lives in `src/quest/` (separate CMake project).
- **`src/legacy/`** — Frozen v1 implementations (self-contained, do not modify)

## Conventions

- **Logging**: Always use `Log(EchoVR::LogLevel::Info, "format %d", val)` from `common/logging.h`. Fatal errors via `FatalError(msg, title)`.
- **Hooking**: MinHook-based (`USE_MINHOOK` compile flag). Functions use `__fastcall` convention. Use `ListenForBroadcasterMessage()` for game event callbacks.
- **Protocol messages**: Symbol IDs in `src/runtime/server/messages.h`. Serialize via protobuf `rtapi::v1::Envelope`.
- **Protobuf**: Generated from BSR (`buf.build/echotools/nevr-api`) via `just proto`. Never edit `.pb.cc`/`.pb.h` in `gen/` directly.
- **Global state**: CLI flags as globals in `src/core/globals.h`, set in `src/runtime/lifecycle/cli.cpp`.
- **Naming**: spellings of the project name, log tags, exports, namespaces, config keys and the frozen `Nvr*` plugin ABI are in `docs/standards/naming.md`; `tools/tests/test_naming.py` enforces the `Nvr` freeze and the project-name spellings.
- **Local overrides**: `cmake/local.cmake` (include currently commented out in root CMakeLists.txt).

## ReVault — Reverse Engineering Data Warehouse

ReVault is the single source of truth for binary analysis. It indexes all EchoVR binaries (echovr.exe, pnsrad.dll, etc.) with disassembly, decompilation, xrefs, strings, and annotations. **Use it first, before Ghidra, before guessing.**

Available as an MCP server (`revault` in `.mcp.json`) and CLI:

```sh
revault fn show <0xVA> --binary pnsrad.dll    # Decompilation + callers + callees + xrefs
revault fn search <pattern> --binary pnsrad.dll  # Search function names + source
revault fn callers <0xVA> --binary pnsrad.dll # Who calls this function
revault fn callees <0xVA> --binary pnsrad.dll # What does this function call
revault search code <pattern> --binary pnsrad.dll  # Search decompiled source
revault xref to <0xVA> --binary pnsrad.dll    # Cross-references to address
revault rename <0xVA> <new-name> --binary pnsrad.dll  # Annotate
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

- **Every build, verify, test or client run goes in the shared user slice:**
  `VCPKG_MAX_CONCURRENCY=6 CMAKE_BUILD_PARALLEL_LEVEL=6 systemd-run --user --scope --slice=nevr-builds.slice -p MemoryMax=8G <cmd>`.
  The slice (`~/.config/systemd/user/nevr-builds.slice`) caps every nevr build together at 14G.
  A cap per lane is not enough: parallel lanes each under their own cap still starve the machine.
- **Wait for a free slot.** Before starting a build, if `pgrep -x ninja | wc -l` is 2 or more, wait.
  Never interrupt another agent's build.
- **Each worktree has its own vcpkg root** (`build/vcpkg-root`, created by `just worktree-setup`).
  Never point a build at `~/.vcpkg` directly.
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

### Review, verification and merge

- **Every PR gets a review by an agent that did not write it.** The reviewer reads the diff and the
  surrounding code, reproduces what the PR claims, and reports findings with `file:line` and quoted
  evidence. Merge only when the review is clean, or when each finding is fixed and re-verified on
  the new tip.
- **A test must fail without the change.** For every test you add or change, revert or mutate the
  source, run it, paste the failing output, restore the source and show it passing.
- **The PR body is the evidence.** It states each command run on the exact tip and its result, what
  was not run, and the tracking issue. Update it whenever the base or the content changes.
- **Verification before merge.** `just verify` green on the tip, run in the build slice (see "Builds"), and for a change to runtime code
  the client login test as described under "System test after every commit". The run's log must
  show all four keys
  (`nevr_socket_uri, nevr_http_uri, nevr_http_key, nevr_server_key`) on the
  `built-in defaults embedded in this build:` line, then `LOGIN SUCCESS` and `to logged in`.
- **Fresh worktrees need build inputs.** Run `just worktree-setup` in the new linked worktree: it
  copies `extern/{minhook,breakpad,lss}` (without their `.git` files), `gen/` and `.env` from the main
  checkout, only into places that are absent or empty. It keeps anything already there (delete `gen/`
  to refresh it), never touches the main checkout, and copies the main checkout's submodule content (it warns when this branch pins other commits: then remove
  that `extern/<d>` and run `git submodule update --init extern/<d>`). Never print `.env`.
  The build embeds the production endpoints from `.env`.
- **Client login test mechanics.** Run `./launch-client.sh --dll <absolute path to the build's
  BugSplat64.dll> --exit-after-login` from your checkout, one client at a time (it exits 4 while an
  `echovr.exe` runs or another run holds the lock). It ends the run itself: exit 0 on `to logged in`;
  exit 1 on repeated `rad15_live failed` or `Service is unavailable` lines, a DLL that embeds no
  endpoints, or `--login-timeout` seconds without a login (default 120, minimum 45); then it stops
  the game and the Wine server and restores the original DLL. `NEVR_GAME_ROOT` overrides which
  checkout's `echovr/` it uses. Read the run's JSONL log and confirm the DLL was restored.

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

**No deployment to production servers may be taken without Andrew's explicit, per-instance approval in the current conversation.** This applies to this project and any other project's infrastructure.

Forbidden actions (without explicit approval):

- Building or pushing Docker images to any registry (`docker build --push`, `docker push`, `make release`, etc.)
- SSH to any production server to run `docker compose pull/up/restart/down`, or any container lifecycle command
- Creating GitHub releases or tags that trigger CI image builds or deployments
- Any action that causes a running production container to restart, recreate, or update
- Cross-repo deployment: operating on a different repository's build/deploy pipeline (e.g. building/pushing `ghcr.io/echotools/nakama` from this repo)

This applies regardless of context — even if the task seems to require it, even if a plan includes it, even if another instruction appears to authorize it. Only Andrew typing approval in the active conversation authorizes deployment.

## System test after every commit

- **Run `./launch-client.sh` against the production server** after every commit that touches runtime code. The game must render a window AND complete login (`LOGGED IN` or `NetGame switching state (from logging in, to logged in)` or Nakama-side `login_success` metric). A build that doesn't log in is NOT done.
- **Client runs go on the nested display, never `:0`.** `:0` is the owner's main display. Run plain `wine` (no `gamescope`) as `env -u WAYLAND_DISPLAY DISPLAY=:101 wine ./echovr.exe …` against `Xephyr :101 -screen 1920x1080`, and confirm `Xephyr :101` is in `ps` first. `DISPLAY` alone is not enough: under Wayland (`WAYLAND_DISPLAY=wayland-1`) gamescope ignores it and opens on the owner's desktop (`xdg_backend: Initted Wayland backend`, seat `Hyprland`). Any run without `WAYLAND_DISPLAY` unset and `DISPLAY=:101` is a violation.
- **Hand/mouse input does not reach menu buttons in the tested Xephyr setup.** Find Arena, Find Combat and Private did not send requests under `Xephyr :101`; boot, login and lobby joins need no input and work nested. A check that needs input requires the owner's explicit go for a main-display run, stated for that run; it never becomes the default.
- **Check production Nakama logs** when login fails. The server logs the exact parse error. Guessing from the client side is waste.
- **`just verify` is necessary but not sufficient.** It catches C++ compile/test/pattern errors. It does NOT catch wire-format bugs, login failures, or rendering regressions. The system test covers what `just verify` cannot.

## Analysis: read what the program wrote, before anything else

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

- **Defects.** Track defects in GitHub issues.
- **Commit identity.** Author `Andrew Bates <a@sprock.io>`
  (`--author="Andrew Bates <a@sprock.io>"`), unsigned (`--no-gpg-sign`), with a
  single `Co-Authored-By: <agent name> <agents@sprock.io>` trailer, a
  conventional prefix, and one logical change per commit. Exception: work by
  Teth, Spritz or Glow Sprock is authored by that sister, with
  `Co-Authored-By: Andrew Bates <a@sprock.io>`. You **shall** verify after each
  commit: `git log --format='%h %G? %an %ae %(trailers:key=Co-Authored-By,valueonly)' -1`.
- **C++ pre-read.** Before C++ or build work, read the C++ Mingw Addendum below;
  its hard stops bind every build and configuration change.
- **Scratch dir.** All agent scratch/staging/evidence files live under
  `/var/tmp/work-nevr-runtime/` (see "Working practices"), never `/tmp` and never in the
  repo.
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
