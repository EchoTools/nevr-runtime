---
name: nevr-work
description: Execution harness for any implementation, investigation, build, or server-operation task in the nevr-runtime repo (Windows DLL patches for Echo VR, C++17, MinGW-cross-compiled, run under Wine). Walks the repo's discipline gate by gate — CPP-addendum pre-read, orient, ReVault/measure-before-assert, decision gates on the invariants, spec-before-code with two review passes, the PR quick check (build, covering tests) as the closing gate (the PC login runs on main after the release cut), and findings as GitHub issues plus commit hygiene — with a verification action and an abort condition at every gate. Invoke BEFORE starting any task in this repository.
---

# /nevr-work — the execution harness for this repository

It executes the repo's `AGENTS.md` (as it is on `origin/main`; it inlines the C++ MinGW
Addendum) and the global `~/.claude/CLAUDE.md` (maintainers only). The rules live in those documents; this
skill forces you to walk them in order and produce proof at each gate before passing it.
The former `DRIVER-CHARTER.md` is not in the repo (`git ls-tree origin/main
DRIVER-CHARTER.md` returns nothing); its slots, fleet protocol and constraints are inlined
below.

## This IS

- A gate sequence. Every gate has a **verification action** (something you run
  or read) and an **abort condition** (when you stop and escalate). Passing a
  gate without its verification action is skipping the gate.
- Bound to the PR quick check (AGENTS.md "Builds", "A PR runs the quick check, not
  `just verify`"): the build, the covering test recipes, and on every PR `just test-tools` (kept: output, JUnit XML, `runs.jsonl`) and
  `python3 tools/verify_doc_paths.py`. Every "done/verified" claim resolves against the
  items you ran, each with its result, and an item not run is named as not run — not
  against a proxy. `just verify` runs only on main, after the release cut, once per cut; a PR does not run it and a reviewer does not ask for it.

## This is NOT

- A replacement for `AGENTS.md` (which carries the CPP addendum) or the global
  `CLAUDE.md`. It CITES them; it never overrides them. On conflict, THOSE WIN — note
  the conflict, file it as a GitHub issue, and continue under their reading.
- Optional for "small" tasks. A one-line address constant never validated
  against the binary is how you brick a live patch; a masked `|| echo` is how a
  never-built test recipe passed for months. Small is where discipline decays.

## Repository Settings (the slots from the former `DRIVER-CHARTER.md`, re-pointed at AGENTS.md)

| Slot | Value |
|------|-------|
| `{REPO}` | `~/src/nevr-runtime` (maintainers only; work in a worktree under `.claude/worktrees/`, AGENTS.md "Branch lifecycle") |
| `{PR_GATE_CMD}` | The quick check, on the PR's tip, in the build slice (maintainers only): `just build` (it exits nonzero when the build fails); the covering test recipes (`just --list`: `test-auth-unit`, `test-quest-shared`, `test-quest-*`, `test-android` for a change under `src/quest`, ...); the PC login test runs on main after the release cut, not here; `python3 tools/verify_gtest_floor.py` (the GTest floor: `TEST(`/`TEST_F(` declarations at HEAD and at the merge-base; fails when HEAD has fewer; a deliberate drop carries a `GTest-Drop:` trailer on its commit); on every PR `just test-tools` (kept: output, JUnit XML, `runs.jsonl`) and `python3 tools/verify_doc_paths.py` (run `just worktree-setup` first in a fresh worktree). Each item reported with its result; an item not run named as not run. `just verify` is NOT this gate: it runs only on main after the release cut (AGENTS.md "Builds"). |
| `{FINDINGS}` | GitHub issues, not a ledger file (there is no `BUGS.md`: `git ls-tree origin/main BUGS.md` returns nothing). A defect you do not fix in the same turn gets an issue (AGENTS.md "Documentation, plans and findings", "Fix it or file it, in the same turn") written as a falsifiable claim with Evidence at `<sha>`, Not shown, Falsifier, Work (AGENTS.md "What a PR carries"). A security-sensitive finding is reported to the owner, not filed. The `N<number>` tags are the code's own rule tags, indexed in AGENTS.md "The N-rules index"; the highest in `src`, `tools` and `justfile` is N146 (`git grep -ohE '\bN[0-9]+\b' origin/main -- src tools justfile | sort -V | uniq`). |
| `{INVARIANTS}` | Never commit generated protobuf; prologue-validate before any binary patch; hook frequency analysis before sleeps; production deploy forbidden without per-instance approval; no agent writes into the owner's game install (runs use a throwaway copy); CPP addendum hard-stops (no C-style casts, no `catch(...)`, never throw across a DLL boundary, no I/O from DllMain, Ninja-only, vcpkg-manifest deps only; `-Werror` is on every MinGW target, see the engineer primer's "Compiler warnings"). Source: AGENTS.md "Guardrails", "Production Deployment", "The game install", and the inlined addendum. |
| `{EXPERTISE_PREREADS}` | The C++ MinGW Addendum inlined in AGENTS.md (after its `CPP-MINGW-ADDENDUM` separator) — mandatory pre-read gate before any C++/build work. A maintainer-only copy also exists elsewhere; AGENTS.md wins on conflict. |

## Fleet protocol — binding on every dispatched unit

1. **Verify-first.** Measure the premise before asserting or acting; a stale premise aborts.
2. **Atomic units.** One unit, one worktree, one merge; fix + witnessed red-to-green + the issue reference land together. Whoever creates a branch deletes it (AGENTS.md "Branch lifecycle").
3. **Models named.** Every dispatch states its model; transcripts are the audit. The top model orchestrates and translates, never executes mechanical work.
4. **Citations.** Every decision carries its basis by path+entry, or states "no citable basis — new decision." Inventing a plausible citation is the cardinal fabrication.
5. **CANDIDATE lines.** Any ad-hoc integrity check is reported as one line — the predicate it tests, and yes/no on graduation into a persistent checker. A lesson that does not become a sensor dies with its transcript.
6. **USAGE lines.** Any unit touching a CLI/interface surface notes usage-doc/man gaps as one line. `--help`/`-h` output IS the behavioral acceptance contract for the interface.
7. **Same failure twice = stop and report.** Do not grind.
8. **Scratch in `~/src/nevr-runtime-work/<agent>/`** (maintainers only; a contributor uses `~/src/nevr-runtime-work/<handle>/`, AGENTS.md "For contributors"). Never `/tmp` (RAM-backed on this host), never the repo. Game runs use a throwaway copy of the install, never the owner's.
9. **Commit identity.** Author `Spritz Metis Sprock <spritz@sprock.io>` (the repo's local git config already sets it; never `--author` with another name), `--no-gpg-sign`, the house `Co-Authored-By: <agent name> <agents@sprock.io>` trailer for the agent that wrote the change, and `Co-Authored-By: Andrew Bates <a@sprock.io>` (a sister's work is authored by that sister with the same two trailers; maintainers only). See AGENTS.md "Commit and verification conventions". Verify after each commit: `git log --format='%h %G? %an %ae %(trailers:key=Co-Authored-By,valueonly)' -1`.
10. **CPP pre-read gate.** Before any C++/build work, read `{EXPERTISE_PREREADS}` in full; its Hard-Stops bind the change.
11. **Heartbeat expectation.** A standing unit emits a heartbeat; a silent unit is treated as stuck, not assumed working.

## What this skill never does

- Decide anything reserved to a gate in `{INVARIANTS}` — unsure = gated = escalate as a Decision-line and stop.
- Complete a task by relaxing a constraint "just here." The task is the variable; the constraints are not.
- Trust a cog's number with a story. A correct number wearing a false story re-enters verification like anyone else's.

## Gate 0 — CPP addendum pre-read (before any C++/build work)

**Do:** read the C++ MinGW Addendum inlined in AGENTS.md IN FULL before
touching any `.cpp`/`.h`/`CMakeLists.txt`/`justfile`/preset (AGENTS.md "Commit and
verification conventions", "C++ pre-read"). Its Hard-Stops are enforced, not advisory.

**Verification action:** your plan names the addendum Hard-Stops in scope for
this change (e.g. no C-style casts, no `catch(...)`, no throw across
a DLL boundary, no I/O from DllMain, Ninja-only, vcpkg-manifest deps only,
VERSIONINFO present, release DLLs signed).

**Abort if:** the change cannot be made without violating a Hard-Stop — stop and
hand back a Decision-line; do not "work around" the tool [addendum: "Work around
broken tooling. Fix the tool or stop."].

## Gate 1 — Orient

**Do:** `git fetch origin main`, then read `AGENTS.md` as it is on `origin/main`
(`git show origin/main:AGENTS.md`; a stale checkout's copy is not the authority), the
global `~/.claude/CLAUDE.md` (maintainers only), this skill's Repository Settings + Fleet protocol (below),
`git log --oneline -20`, `git status`, and the GitHub issue the unit tracks (every unit
has an issue before it starts, AGENTS.md "Landing"). Read the
`nevr-runtime-engineer` primer (`.claude/agents/nevr-runtime-engineer.md`) for build graph, module/plugin split, the FatalError path,
deploy manifest, and hazards. There is no `BUGS.md`; the `N<number>` tags are in code
comments and indexed in AGENTS.md "The N-rules index".

**Verification action:** your plan names the files you oriented on and the
one-line current state (what builds, which issue and which N-rules the change touches)
you took from them.

**Abort if:** you cannot establish repo state or the task's subsystem — escalate
rather than explore blind.

## Gate 2 — ReVault / measure before you assert

**Rule:** before asserting ANY fact — a function address, struct offset, vtable
slot, import, prologue bytes, how the game loads a library, what a flag does,
which DLL a target outputs — measure it FIRST. Not to confirm a memory; to find
out. Never assert from training [AGENTS.md "ReVault"].

- Binaries (maintainers only): `revault fn show <0xVA> --binary echovr.exe|pnsrad.dll|...`,
  `revault fn callers/callees`, `revault search code`, `revault xref to`. Search the
  WHOLE project (every binary, report `searched N of N binaries in project <name>`); a
  one-binary negative is not a finding.
  `echovr.exe` ImageBase is `0x140000000`. If ReVault lacks it, SAY SO — don't
  guess. For raw ELF/PE facts, `readelf`/`nm`/`objdump -p` on the real file is
  instant ground truth.
- Build/behavior facts: read the CMake, run the recipe, read the loaded config —
  the process's own command line and the file it actually loaded, not a doc.

**Verification action:** every address/behavior/config claim in your plan or
report carries its source (a `revault` line, `readelf`/`nm` output, an xref, a
`file:line`). An unsourced claim is written `[UNVERIFIED]` or not written.

**Abort if:** a subagent hands you a number with a story — re-verify the story
independently before building on it. A correct number wearing a false story is
the most expensive artifact an agent can hand back.

## Gate 3 — Decision gates on the invariants

**Do:** enumerate what the task touches against the invariants [the
`{INVARIANTS}` slot below]:

- Never commit generated protobuf (`gen/cpp/*.pb.{cc,h}`) without regenerating
  from BSR via `just proto`.
- **Prologue-validate before any binary patch** — read and compare expected
  bytes first; never blind-write.
- **Hook frequency analysis before any sleep/yield** — determine per-frame vs
  per-tick vs per-event first.
- Production deploy is FORBIDDEN without Andrew's explicit per-instance approval
  (registry push, prod SSH, container lifecycle, CI-triggering tag, cross-repo
  pipeline). A local Wine test server is NOT deployment.
- CPP addendum Hard-Stops (Gate 0).
- Never push/merge/rebase from local `main` without the owner's per-instance instruction.
- Never put maintainer-private material in this public repo.

**Verification action:** your plan states, in one line, which invariants the
task touches, or "touches no invariant" with the reasoning.

**Abort if:** unsure whether it's gated — then it is. Write the Decision-line
(closed question + options + Execution line) and STOP. Do not build "pending
approval."

## Gate 4 — Spec the behavior before the code

**Do:**
1. Plan before non-trivial code, with **at least two self-review passes** and a
   stated testing strategy — automated first (unit/GTest + ground-truth), manual
   only for what cannot be automated (visual/gameplay) [AGENTS.md "Methodology"].
2. TDD where testable: the failing test exists before the implementation. For a
   binary-constant or address change, the ground-truth test that pins the
   expected bytes/range comes first (cf. `test_xpid_patch`).
3. **The wiring rule:** every CLI flag and config field you add must have a
   consumer, proven — grep shows the parsed value is read outside its declaration
   file, and a test exercises the behavior. `--help`/`-h` output IS the interface
   contract; a documented-but-unwired flag is a
   violation, not clutter.

**Abort if:** you cannot write the test or state the observable — you do not know
what you are building. Return to Gate 3.

## Gate 5 — Implement inside the negations

- **Validate before you patch.** Prologue bytes compared and matched before any
  write; the first byte written happens only after validation passes.
- **The dedicated server must be non-interactive** — no modal dialog, no prompt,
  no blocking UI on a live code path; fatal errors log structured and exit
  nonzero (N4, N102, N120 in AGENTS.md "The N-rules index"). Note
  `FatalError()` does not reliably terminate in server mode — see the engineer
  primer's fatal-error section before relying on it.
- **Success derives from the artifact, never a proxy.** Read `just build`'s own
  exit code (a plain `cmake --build`: nonzero when the compiler or linker fails)
  and check the produced binary (AGENTS.md "Commit and verification conventions").
- Modern C++ per the addendum: `static_cast`/`reinterpret_cast`/`std::bit_cast`,
  never C-style casts; named catch types, never `catch(...)`; no throw across a
  DLL boundary (convert to `HRESULT` at exports); no I/O from `DllMain`;
  structured `Log(...)`, never printf; vcpkg-manifest deps only.
- Build and test after each logical step, not only at the end.

## Gate 6 — Verify it landed (close the loop)

**Do:** run the quick check (`{PR_GATE_CMD}`) on the PR's tip, in the build slice
(`VCPKG_MAX_CONCURRENCY=6 CMAKE_BUILD_PARALLEL_LEVEL=6 systemd-run --user --scope
--slice=nevr-builds.slice -p MemoryMax=8G <cmd>`; wait while 2 or more `ninja` run; maintainers only), and
report each item with its result.
1. **The build:** `just build`, whose exit code you read (nonzero when the build
   fails). Quote the tail and exit code, read from a
   file under `~/src/nevr-runtime-work/<agent>/` (maintainers only), never `$?` after a pipe.
2. **The covering test recipes** (`just --list`; `test-auth-unit`, `test-quest-shared`,
   `test-quest-*`, `test-android` for a change under `src/quest`, ...). Every PR, whatever it
   touches: `just test-tools` and `python3 tools/verify_doc_paths.py`. The GTest floor
   (`python3 tools/verify_gtest_floor.py`) is part of the quick check as AGENTS.md "Builds" lists it.
3. **A runtime or auth change's PC login runs on main after the release cut, never on a PR** (AGENTS.md
   "Verification before merge"; "Client login test mechanics"):
   `NEVR_GAME_ROOT=<copy> ./launch-client.sh --dll <absolute path to the build's
   BugSplat64.dll> --exit-after-login` against a throwaway copy of the install (never the
   owner's `echovr/`), one client at a time, on the nested display (`env -u
   WAYLAND_DISPLAY DISPLAY=:101`, headless `Xvfb :101`, never `:0`, no `gamescope`; maintainers only). The
   run's log must show all four keys (`nevr_socket_uri, nevr_http_uri, nevr_http_key,
   nevr_server_key`) on the `built-in defaults embedded in this build:` line, then
   `LOGIN SUCCESS` and `to logged in`; read the run's JSONL log and confirm the DLL was
   restored. A `ninja -t inputs` / `-t deps` proof that the shipped DLL is unchanged is
   separate information; it never stands in for the login.
4. For a Go-suite touch: `go vet ./...` clean in the affected module. (The Go suites are
   separate recipes, not part of `just verify`.)
5. For a server change: verify the OUTCOME in the running process — read the log file
   (never pipe to `grep`; it block-buffers and looks like a hang), judge no run before
   ~45s, and state whether `DISPLAY` was set (a `DISPLAY`-set run is not equivalent to
   headless).
6. **Enablement is part of done.** A built-but-unlinked/unloaded artifact is NOT
   done — record enablement or the reason it is absent, in the same change.
7. The PR body states each command run on the exact tip and its result, what was not
   run, the tracking issue, and ends with `Read AGENTS.md at <merge-base sha>`
   (`git merge-base HEAD origin/main`, after a fetch).

`just verify` is not part of this gate: it runs only on main, after the release cut,
once per cut. Its log starts `verify: HEAD <sha>` / `verify: tree clean|DIRTY` and a
log for the tip's sha with `tree clean` ending `verify: OK` is the proof; a reviewer
reads it and does not run it again (AGENTS.md "Builds").

**Abort if:** an item is red for a reason you did not introduce — report it as a
finding (a GitHub issue) with its measurement and a Decision-line; do NOT fake green.

## Gate 7 — Record and commit

- **Findings:** a defect you do not fix in the same turn gets a GitHub issue with
  evidence (AGENTS.md "Documentation, plans and findings", "Fix it or file it, in the
  same turn"), shaped as a falsifiable claim: Evidence at `<sha>` (`file:line`, quoted),
  Not shown, Falsifier, Work (AGENTS.md "What a PR carries"). The issue number and unit
  id go in the commit. There is no `BUGS.md` and no N-prefix next-ID rule; the `N<number>`
  tags belong to the code and are indexed in AGENTS.md "The N-rules index".
- **Commit identity:** governed by AGENTS.md "Commit and verification conventions" —
  **that is the authority; read it rather than this summary if they disagree.** Fleet protocol item 9 states the
  same rule. Author `Spritz Metis Sprock <spritz@sprock.io>` from the repo's local git config, `--no-gpg-sign`,
  the house `Co-Authored-By: <agent name> <agents@sprock.io>` trailer and the `Co-Authored-By: Andrew Bates <a@sprock.io>`
  trailer; conventional prefix (`feat:`/`fix:`/`docs:`/`test:`/`build:`); one logical change per commit.
  **Verify after each commit:** `git log --format='%h %G? %an %ae %(trailers:key=Co-Authored-By,valueonly)' -1`.
  A person whose work the commit draws on gets a credit trailer (AGENTS.md "Commits, versions and requirement words").
- **Scratch** lives under `~/src/nevr-runtime-work/<agent>/` (maintainers only), never `/tmp`, never the
  repo (AGENTS.md "Working practices for every agent"). Delete the scripts, copies and
  build output you created, outright; keep run logs and evidence.
- **Branch and worktree:** cut with `git worktree add --no-track -b <seat>/<issue>-<slug>
  .claude/worktrees/<branch> origin/main`, lock with a reason, push only by explicit
  refspec, and delete the worktree and the local and origin branch when the PR merges or
  closes (AGENTS.md "Branch lifecycle"). Record each artifact you create in
  `~/.local/share/repo-hygiene/nevr-runtime-ledger.md` (maintainers only).

## Report — the standing sibling lines

Before declaring done, produce the unit report: deliverable +
every "verified/green" claim with its quoted post-hoc measurement, plus, when
applicable:

- **CANDIDATE line** — any integrity check you invented/applied ad-hoc: the
  predicate it tests + yes/no on graduation into a persistent checker.
- **USAGE line** — any `--help`/`-h`/man-page/recipe-doc gap you touched.
- **PRIMER-DRIFT line** — any stale line in the engineer primer or a brief, in
  the mandated drift format.

**Same failure twice → STOP and hand back a Decision-line.** Do not grind; do
not fake a green.

## Common mistakes

- Reading `$?` after a pipe (`just build | tail`) as the build's exit code — it is
  the last stage's. Write the output to a file and read the recipe's own exit code.
- Treating `just verify` as the PR gate, asking a reviewer to run it, or substituting a
  `ninja -t inputs` proof for the PC login. The PC login runs on main after the release cut, never on a PR.
- Reading a server hang as a deadlock when it is `FatalError` blocked on an
  invisible MessageBox — check for module-init-failure + suppressed-ExitProcess
  lines first.
- Assuming `just dist` output is complete (it has no `LibOVRPlatform64_1.dll`; the
  modules are static link inputs, not separate files) or that plugins load because
  they built or sit in `plugins/` (the loader reads the `plugins:` list in
  `config.yaml`; a DLL the config does not name is ignored).
- Completing the task by relaxing a constraint "just here." The task is the
  variable; the constraints are not.
