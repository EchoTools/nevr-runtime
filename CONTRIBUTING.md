# Contributing to nEVR Runtime

nEVR Runtime is a set of Windows DLL patches for Echo VR (`echovr.exe`), written in C++17 and cross-compiled with
MinGW from Linux. [`AGENTS.md`](AGENTS.md) is the full statement of conventions and guardrails; this page is the short
path from a checkout to a landed commit. There are no pull requests.

## Build

You need CMake 4.0+, Ninja and MinGW (`x86_64-w64-mingw32-g++`) to cross-compile from Linux. Dependencies come from
the vcpkg manifest and the submodules in `extern/` (`git submodule update --init`).

```sh
just                # list recipes
just build          # build everything
just dist           # build + distribution packages
```

Presets are `mingw-debug`, `mingw-release` (Linux default), `debug` and `release` (Windows default). Output lands in
`build/<preset>/bin/`. Use another preset with `just preset=mingw-debug build`.

## Verify

A branch runs the quick check before it lands: the build (`just build`), the test recipe(s) that cover the changed code
(`just --list` names them), `just test-tools` and `python3 tools/verify_doc_paths.py`. Put each command and its exit
code in the commit message. The PC login test runs on main after the release cut, not on a branch.

`just verify` is the full gate. It builds, runs the C++ tests under Wine, then runs the source-invariant sensors. It
fails closed. It runs only on main, after the release cut, once per cut; a branch does not run it.

Neither is sufficient. They do not catch wire-format bugs, login failures or rendering regressions; for a
change to runtime code, also run the game against a server (`NEVR_GAME_ROOT=<copy of the install> ./launch-client.sh`, see "The game install" in `AGENTS.md`) and check the program's own log.
[`docs/standards/verification.md`](docs/standards/verification.md) says what counts as evidence for a claim like
"fixed" or "verified", and [`docs/standards/logging.md`](docs/standards/logging.md) covers what a component must log.

The Go system suites are run with `just test-system` and are not part of `just verify`; they need the Echo VR game
binary. See "Testing" in `AGENTS.md`.

## Before you change code

- Read `AGENTS.md`, in particular the Guardrails: do not edit generated protobuf in `gen/` (regenerate with
  `just proto`), and validate the expected bytes before any binary patch.
- A non-trivial change starts with a written plan that says how it will be tested.
- Build and test after each logical step, not only at the end.
- Put new defects and ideas in GitHub issues. Beta testers have a bug-report form under "New issue".

## Branch, commit, review, land

1. Create and name the branch as described under “Branch lifecycle” in [`AGENTS.md`](AGENTS.md).
2. One logical change per commit, with a conventional prefix (`feat:`, `fix:`, `docs:`, `ci:`; an optional scope is
   fine, as in `fix(bridge): ...`), and the issue number in the subject when there is one.
3. Open the issue first, and name it in the commit message: `Closes #N` for each issue the commit resolves, or
   `Refs #N` for one it only advances (the message says what is still open). The message also carries what and why,
   each command run on the tip with its exit code, and what was not run (`AGENTS.md`, "What a landed commit carries").
4. Review the branch in a fresh context and land it on `main` as "Review, verification and landing" in
   [`AGENTS.md`](AGENTS.md) says: a MERGE verdict, then `just land` from the branch's own worktree. Without push access, stop
   after the review's MERGE and the rebase, push the branch to your fork and post its name and tip SHA on the issue. No workflow runs on a push to `main` except `vcpkg-cache.yml`, when a
   vcpkg input changes; run CI by hand on `main` with `gh workflow run <file> --ref main` (`build.yml`, `android.yml`,
   `vcpkg-cache.yml`, `defender-scan.yml`).

## Planning: milestones, points and sprints

Issues, milestones, story points and sprints are governed by
[`docs/standards/planning.md`](docs/standards/planning.md). In short: releases are planned by outcome (one milestone
per release, no due dates), stories are sized on a Fibonacci scale, and a sprint is a batch of work, not a stretch of
time. An open issue has a release milestone or the `backlog` label, never neither.

## Security

Do not report a security vulnerability in a public issue or commit. Follow [`SECURITY.md`](SECURITY.md).
