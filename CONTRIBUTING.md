# Contributing to nEVR Runtime

nEVR Runtime is a set of Windows DLL patches for Echo VR (`echovr.exe`), written in C++17 and cross-compiled with
MinGW from Linux. [`AGENTS.md`](AGENTS.md) is the full statement of conventions and guardrails; this page is the short
path from a checkout to a pull request.

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

A PR runs the quick check: the build (`just build`), the test recipe(s) that cover the changed code (`just --list`
names them), and, for a change to runtime or auth code, the PC login test. Paste its output into the PR.

`just verify` is the full gate. It builds, runs the C++ tests under Wine, then runs the source-invariant sensors. It
fails closed. It runs only on main, when prepping a release, once per tip; a PR does not run it.

Neither is sufficient. They do not catch wire-format bugs, login failures or rendering regressions; for a
change to runtime code, also run the game against a server (`NEVR_GAME_ROOT=<copy of the install> ./launch-client.sh`, see "The game install" in `AGENTS.md`) and check the program's own log.
[`docs/standards/verification.md`](docs/standards/verification.md) says what counts as evidence for a claim like
"fixed" or "verified", and [`docs/standards/logging.md`](docs/standards/logging.md) covers what a component must log.

The Go system suites are run with `just test-system` and are not part of `just verify`; they need the Echo VR game
binary. See "Testing" in `AGENTS.md`.

## Before you change code

- Read `AGENTS.md`, in particular the Guardrails: do not edit generated protobuf in `gen/` (regenerate with
  `just proto`), do not modify `src/legacy/`, and validate the expected bytes before any binary patch.
- A non-trivial change starts with a written plan that says how it will be tested.
- Build and test after each logical step, not only at the end.
- Put new defects and ideas in GitHub issues. Beta testers have a bug-report form under "New issue".

## Branch, commit, open a PR

1. Create and name the branch as described under “Branch lifecycle” in [`AGENTS.md`](AGENTS.md).
2. One logical change per commit, with a conventional prefix (`feat:`, `fix:`, `docs:`, `ci:`; an optional scope is
   fine, as in `fix(bridge): ...`), and the issue number in the subject when there is one.
3. Push the branch and open a pull request against `main`. The PR template asks for what and why, the linked issue,
   and how it was verified. The merge gate is the local capped quick check (`just verify` runs only on main, when prepping a release); no workflow runs on a pull request or
   a push. Run CI by hand with `gh workflow run <file> --ref <branch>` (`build.yml`, `android.yml`,
   `vcpkg-cache.yml`, `defender-scan.yml`).
4. Open the issue first, and name it in the PR body: `Closes #N` for each issue the PR resolves, or `Refs #N` for one it
   only advances (the body says what is still open). `.github/workflows/pr-issue-link-check.yml` fails a body that names
   none. Give the PR the same milestone as that issue.

## Planning: milestones, points and sprints

Issues, PRs, milestones, story points and sprints are governed by
[`docs/standards/planning.md`](docs/standards/planning.md). In short: releases are planned by outcome (one milestone
per release, no due dates), stories are sized on a Fibonacci scale, and a sprint is a batch of work, not a stretch of
time. An open issue has a release milestone or the `backlog` label, never neither.

## Security

Do not report a security vulnerability in a public issue or PR. Follow [`SECURITY.md`](SECURITY.md).
