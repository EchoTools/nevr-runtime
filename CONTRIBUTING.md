# Contributing to NEVR Runtime

NEVR Runtime is a set of Windows DLL patches for Echo VR (`echovr.exe`), written in C++17 and cross-compiled with
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

`just verify` is the single gate. It builds, runs the C++ tests under Wine, then runs the source-invariant sensors. It
fails closed: run it before you open a PR and paste its output into the PR.

It is necessary but not sufficient. It does not catch wire-format bugs, login failures or rendering regressions; for a
change to runtime code, also run the game against a server (`./launch-client.sh`) and check the program's own log.
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

1. Branch from `main`. Existing branches are named `<kind>/<short-description>`, for example `feat/...`, `fix/...`,
   `docs/...` and `ci/...`.
2. One logical change per commit, with a conventional prefix (`feat:`, `fix:`, `docs:`, `ci:`; an optional scope is
   fine, as in `fix(bridge): ...`), and the issue number in the subject when there is one.
3. Push the branch and open a pull request against `main`. The PR template asks for what and why, the linked issue,
   and how it was verified.
4. Link the issue it resolves with `Closes #N` (or say in the body why there is none), and give the PR the same
   milestone as that issue.

## Planning: milestones, points and sprints

Issues, PRs, milestones, story points and sprints are governed by
[`docs/standards/planning.md`](docs/standards/planning.md). In short: releases are planned by outcome (one milestone
per release, no due dates), stories are sized on a Fibonacci scale, and a sprint is a batch of work, not a stretch of
time. An open issue has a release milestone or the `backlog` label, never neither.

## Security

Do not report a security vulnerability in a public issue or PR. Follow [`SECURITY.md`](SECURITY.md).
