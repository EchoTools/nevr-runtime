---
name: nevr-work
description: Use when you start any task in the nevr-runtime repo, before the first change. The repo is one Echo VR codebase for two binaries (echovr.exe on PC, libr15.so on Quest) plus everything around it: the PC host and runtime DLLs and the dedicated game server, the plugin SDK, loader and signed-plugin trust, the Luau scripting host, the Quest sentinel and its plugins, the game-service protocol and its generated code, content packs and game data, the core installer, the Python, Go and Rust tools and tests, and the release, signing and Quest-APK pipeline. It names the AGENTS.md sections, standards, recipes and proof that cover the area you touch.
---

# /nevr-work: find the rules and the proof for the area you touch

The rules are in `AGENTS.md` (read it whole, as it is on `origin/main`: `git show origin/main:AGENTS.md`), in the
four standards under `docs/standards/`, and in the repo's `justfile`. This skill never restates them and never
overrides them; where it disagrees with `AGENTS.md`, `AGENTS.md` wins and this file is wrong. The first section of
`AGENTS.md`, "Do what was asked", comes before everything here: build the request as given, add no step or criterion it
does not contain.

Parts of `AGENTS.md` are marked **Maintainers only**; "For contributors" says what to do on your own machine instead.

## What the repo is

One game, two binaries, one codebase: a feature is written once, with a per-binary address table (`AGENTS.md`,
"Project"; parity is #1598). Measure which of these you are in before you plan.

| Area | Where | What it is |
| --- | --- | --- |
| PC host | `src/runtime/host/` | `BugSplat64.dll`: the thin host the game imports (28 pinned exports, one hook, crash capture); exit 78 when the runtime cannot load |
| PC runtime | `src/runtime/` (not `host/`), `src/core/`, `src/abi/`, `src/modules/`, `src/extension/` | `nevr-runtime.dll`: hooks, modes, crash recovery, config, bridge, log filter, plugin and module loaders |
| Dedicated server | `src/runtime/server/`, `src/launcher/`, `launch-server.sh`, `verify-server.sh` | `echovr.exe -server -headless`: ServerDB registration, sessions, telemetry, UPnP |
| Plugin SDK and trust | `sdk/`, `src/runtime/ext/` | the plugin C ABI, loader, signed-only mode (ADR 0010); gameplay plugins live in a separate repo |
| Scripting | `src/scripting/`, `extern/luau`, `docs/lua-scripting.md` | the Luau host (prototype, ADR 0016) |
| Game service and protocol | `src/nevr_api/`, `gen/`, `src/runtime/compat/`, `docs/reference/social-nakama-protocol.md` | protobuf from BSR (`just proto`), the WebSocket bridge, the session router, codecs |
| Quest sentinel | `src/quest/` | a separate CMake project (NDK, arm64); `libr15.so` is patched on the host at build time (ADR 0021, N151) |
| Quest plugins | `src/quest/plugins/q15t`, `src/quest/plugins/autopilot`, `src/quest/plugin/` | the Quest plugin loader (ADR 0019), q15t modules, the autopilot route (ADR 0025); in no release or package-dev APK |
| Content packs and game data | `docs/reference/content-packs.md`, `content-pack-hosting.md`, `tools/echomod/`, `src/runtime/patch/{asset_cdn,resource_override,evrp_package}.cpp`, `src/quest/data/` | game data served by plugins, hosted on the CDN, named by the signed manifest (ADR 0028, 0029) |
| Core installer | `installer/` (Rust crate; the binary is `install-nevr-runtime`), `tools/package-release/` | one install method for PC and Quest (ADR 0023, 0024) |
| Python tools | `tools/`, `tools/tests/` | gate checks (`verify_*.py`), release and Quest-APK tooling, scenario harness (`tools/scenario/`), Windows VM rig (`tools/winvm/`) |
| Go suites | `tests/system`, `tests/plugins`, `tests/quest`, `tests/crash-ingest` | ground-truth and system tests; some write into a game install |
| Release and signing | `RELEASING.md`, `.github/workflows/`, `tools/release_*.py`, `release-headlines/` | tag, draft, CI build, signer, publish; the Quest APK is built and published outside CI |
| Docs | `docs/adr/`, `docs/standards/`, `docs/reference/`, `docs/engine/` | `ls docs/adr` is the ADR list |

## Gates

Each gate has a proof to produce. A gate you did not pass is named as not passed in your report.

### 1. Orient

Read `AGENTS.md`, the global `CLAUDE.md` if you are a maintainer seat, `git log --oneline -20`, `git status`, and the
issue the unit tracks (a PR is opened only for an existing issue: "Review, verification and merge"). Read the standard for the work you will
do (`AGENTS.md` "For contributors", "Required reading": verification, logging, naming, planning). For C++ or build work
read the C++ Mingw Addendum at the end of `AGENTS.md`; a Quest change is built with the NDK, not MinGW
(`just configure-android`), so check which of the addendum's hard stops apply to it. If a file you will change is
guarded by an N-rule, read the rule in "The N-rules index" and the comment at the site first.

Proof: your plan names the area from the table above, the issue, the N-rules in play and the standards read.

### 2. Measure before you assert

`AGENTS.md` "Reverse engineering" (search the whole project, `searched N of N`), "Continuity", and "Analysis: read what the program
wrote". Build facts come from the CMake, the recipe and the process's own config and log, not from a document. Binary
facts come from the disassembly or the file itself (`readelf`, `nm`, `objdump`).

Proof: each address, offset, flag or behavior claim carries its source, or is written `[UNVERIFIED]`.

### 3. Decide: what needs the owner's word

Ask only about one-way actions and real dilemmas ("Do what was asked": "Decide two-way doors"). The one-way ones are: a
deploy, a tag, a release or publish, a signing step ("Production Deployment", "Who speaks for the owner",
`RELEASING.md`), and a write into the owner's game install ("The game install"). The "Guardrails" are not decisions: a
binary patch without prologue validation and committed generated protobuf are refused, not asked about. A change that
must break an N-rule says so in the PR and why ("The N-rules index"). Everything else is a two-way door: decide it and
report it.

### 4. Plan and write the test first

"Methodology" (plan, two review passes, testing strategy) and "What a PR carries" (search statement, siblings, symbols,
mutants, platform parity, headline). A new flag or config key has a consumer, shown by a grep and a test. Logging is part
of the work (the logging standard `AGENTS.md` opens with, `docs/standards/logging.md`).

### 5. Implement inside the area's constraints

| Area | Constraints to carry (section of `AGENTS.md` unless stated) |
| --- | --- |
| PC host and runtime | "DLL Components", the boot, shutdown and crash N-rules (N36, N43, N62, N70, N82, N125, N146), "No hand-built serialization" |
| Dedicated server | "Startup timing" (45 s patience), N102, N120, N149; a server never blocks on a prompt |
| Plugins and trust | "Plugins", N114, N134, N135, N147, N150; `sdk/include/nevr/plugin_interface.h` is a frozen ABI (`docs/standards/naming.md`) |
| Hooks and patches | "Guardrails" (prologue validation, hook over byte patch), the hooks N-rules (N83, N84, N96, N97, N126) |
| Protocol | "Guardrails" and "Conventions" on generated protobuf; N91, N112, N132; the wire format is checked against the game service, not guessed |
| Quest | N151 (no code page written at run time), "Project" on sharing code with `src/runtime/`, ADR 0003/0004/0021 |
| Content packs | `RELEASING.md` "Game-derived packages": no game-derived byte in the repo, a release asset or the Windows zip |
| Installer | N-table row "(standing rule)": only `BugSplat64.dll` goes in the game folder; ADR 0012, 0023 |
| Tools | `tools/README.md`; a sensor follows "Writing a sensor (N93, N99, N101, N111)" |
| Release | `RELEASING.md` whole; no tag that is not on origin (`just release-preflight` refuses one); build CI runs only when started by hand ("Builds") |

### 6. Prove it: the quick check, by area

The PR gate is "A PR runs the quick check, not `just verify`" in `AGENTS.md` "Builds"; `just verify` runs only on main
after the release cut. `just --list` is the authority for recipe names. Run what covers the code you changed, in the
build slice on the PR's tip, and report each item with its result; an item not run is named as not run.

| You changed | Run (in addition to `just build`) |
| --- | --- |
| `src/runtime`, `src/core`, `src/abi`, `src/modules` | `just test-auth-unit`; `python3 tools/verify_gtest_floor.py`; with `src/runtime/host/`, `exports.def` or an `NEVR_MODULE_API` function also `tools/verify_pe_contract.py` as "Builds" gives it |
| `src/runtime/server` | the above; a server outcome is read from the run's own log, on a game copy (`verify-server.sh`, "The game install") |
| `sdk/`, `src/runtime/ext` | `just test-auth-unit`; `just test-plugins-groundtruth` |
| `src/scripting` | `just test-scripting` (needs `extern/luau`) |
| `src/quest` | `just test-android` and the `test-quest-*` recipe for the subdirectory (`shared`, `redirect`, `social`, `integration`, `data`, `invite`, `router`, `plugins`, `q15t`, `autopilot`, `patcher`, `hooks`); the `*-pinned` variants need the store APK; `just check-android-static-init` |
| `installer/` | `just test-installer` |
| `tools/`, any doc | `just test-tools`; `python3 tools/verify_doc_paths.py` (run `just worktree-setup` first in a fresh worktree) |
| `tests/` Go modules | `go vet ./...` in the module; the system suites write into `$EVR_GAME_DIR`, so only on a game copy |
| Release or packaging | `just release-preflight` before a tag; `tools/package_release.py` and the `test_release_*` tests via `just test-tools` |

Every PR also runs `just test-tools` and `python3 tools/verify_doc_paths.py`. A runtime or auth change's PC login runs on
main after the release cut ("Verification before merge", "Client login test mechanics"). Behavior only a headset or a
live game shows is named as not run unless you ran it (the evidence ladder is `docs/standards/verification.md`).

### 7. Land

The PR body: the issue (`Closes`/`Refs #N`), each command run on the exact tip with its result, what was not run, and
last the line `Read AGENTS.md at <merge-base sha>` ("Review, verification and merge"). Commit identity and trailers:
"Commit identity" and "Commits, versions and requirement words". Branch and worktree: "Branch lifecycle"; scratch:
"Ownership and cleanup". A defect you do not fix in the same turn gets an issue ("Fix it or file it, in the same turn"),
shaped as "What a PR carries" says; a security-sensitive one goes to the owner. Do not merge unless the owner told you to
("Only the agent the owner has told to merge merges PRs").

## Mistakes that recur

- Reading `$?` after `just build | tail`: it is the last stage's. Write the output to a file under the work root and read
  the recipe's exit code.
- Treating `just verify`, or a `ninja -t inputs` proof, as a substitute for the PC login or for the quick check.
- Reading a plugin as loaded because it built or sits in `plugins\`: the loader reads the `plugins:` list in
  `config.yaml` (README "Plugins").
- Taking a Quest-only copy of something `src/runtime/` already does ("Project").
- Running a game, a Go system suite or a deploy script against the owner's install instead of a copy ("The game
  install").
