# Planning: releases by outcome, sprints by work

The key words "MUST", "MUST NOT", "REQUIRED", "SHALL", "SHALL NOT", "SHOULD", "SHOULD NOT", "RECOMMENDED",
"NOT RECOMMENDED", "MAY", and "OPTIONAL" in this document are to be interpreted as described in BCP 14
([RFC 2119](https://www.rfc-editor.org/rfc/rfc2119), [RFC 8174](https://www.rfc-editor.org/rfc/rfc8174)) when, and only
when, they appear in all capitals, as shown here.

How work in this repo is planned and tracked on GitHub. It binds issues, PRs, branches and milestones. Planning
drifts; the hygiene rules at the end are how it gets corrected, on a schedule, the same way every time.

## Release milestones

Milestones are ordered by when they ship, not by version number. There are three:

| milestone | holds |
|---|---|
| `Next release` | what ships in the next tagged release |
| `Release after` | what ships in the release after that |
| `Future` | wanted, not scheduled |

- The description opens with `Outcome:`: the state of the world when the release is done, then its definition of done.
- **No due dates.** Releases are scoped by work, not by the calendar.
- When a release is tagged, `Next release` is renamed to the version it shipped as (`v5.6.0`) and closed. Anything
  still open in it moves to `Release after` first. Unfinished work never holds a release. `Release after` is then
  renamed `Next release`, and a new `Release after` is opened.
- Work nobody has decided to schedule has no milestone and carries the `backlog` label.

### Release versions

A release's version is not chosen ahead of time. It is computed when the release is tagged, from the
[Conventional Commits](https://www.conventionalcommits.org/en/v1.0.0/) on `main` since the last release tag:

- a breaking change of type `feat`, `fix` or `perf` (`!` before the colon, or a `BREAKING CHANGE:` or `BREAKING-CHANGE:` footer) bumps the major: `v5.5.1` to `v6.0.0`. On any other type a breaking marker bumps nothing, the same as that type without it (semver 2.0.0 section 1: the version describes the public API a release ships);
- otherwise any `feat` bumps the minor: `v5.5.1` to `v5.6.0`;
- otherwise any `fix` or `perf` bumps the patch: `v5.5.1` to `v5.5.2`;
- any other type bumps nothing, and with nothing bumped there is nothing to release.

`python3 tools/next_release_version.py` computes it and prints the tag; on stderr it prints one JSON line per commit
with the bump that commit gives. It counts every commit reachable from the release commit and not from the base, the
commits of a merged branch included. The base is the highest `vX.Y.Z` tag reachable from the release commit: a
pre-release tag is not a release tag, and a scratch dry-run tag (`v0.0.<issue>`) is never the highest. It refuses a
version whose tag already exists.

Only a commit's first line gives its type and its `!`. In a squash commit GitHub's first line is the PR title when the
PR has several commits and the one commit's subject when it has one, and the body lists the sub-commits' messages:
their subjects count for nothing, their `BREAKING CHANGE:` footers count. `tools/check_pr_title.py` checks the PR
title only, so a one-commit PR's subject MUST follow Conventional Commits as well. A commit whose type understates its
change (a `fix` that breaks an interface) gives the release the wrong version, and the review is where that is caught.


## Points

Every scheduled story gets a size on the Fibonacci scale, which measures relative effort and uncertainty together,
never hours:

| points | meaning |
|---|---|
| 0 | trivial: a typo, a one-line config change |
| 1 | small and well understood, one file |
| 2 | small but touches a few places or needs a test |
| 3 | a real change with tests, in one area |
| 5 | cross-cutting, or needs investigation first |
| 8 | big or uncertain |

Anything bigger than 8 is split before it is scheduled. An open question with no known cause is split into a spike
("find the cause", pointed) and a fix story (pointed once the cause is known).

## Sprints

A sprint is **a batch of work, not a stretch of time.** It commits roughly one velocity's worth of points and ends when
every story in it is Accepted, or explicitly moved out. There are no sprint dates.

**Velocity** is the points Accepted in a sprint; the average of the last three sprints sets the next sprint's commitment.
Remaining points in a milestone divided by velocity is the number of sprints left. It is not converted to a date.

## Story states

| state | meaning | gate to enter |
|---|---|---|
| Icebox | not scheduled | none |
| Backlog | in the active milestone, not started | pointed |
| Started | being worked | an owner is named |
| Finished | PR open | the quick check (AGENTS.md, "Builds") green on the PR head, output in the PR body |
| Delivered | merged | merged to `main`; `just verify` runs on `main` after the release cut, once per cut, output recorded |
| Accepted | proven in real use | the owner or a tester confirmed it working, or the end-to-end check passed |
| Rejected | failed acceptance | back to Started, with the failure quoted |

No workflow runs on a pull request or a push, so the Finished gate is the local capped quick check and the
`just verify` on `main` after the release cut is a local run, not a CI status. CI runs by hand: `gh workflow run <file> --ref <branch>`.

Only Accepted points count toward velocity. Delivered is not done: merged code that nobody has seen work is not yet
value.

## Hygiene rules

These are checked on a schedule and after every release. A violation is either corrected on the spot (when the fix is
mechanical and reversible) or listed for a decision. Branches and issues are never deleted or closed silently.

**Issues**
- An open issue has a release milestone or the `backlog` label, never neither.
- An issue in a release milestone has a kind label: `bug`, `enhancement`, or `documentation`.
- An issue that a merged PR fixed is closed, with the PR linked.

**PRs**
- A PR is opened only for an existing issue, and its body names it. The issue states the problem and its done-bar. The
  body links the issue it resolves with a closing keyword (`Closes #N`, `Fixes #N` or `Resolves #N`), one line for each
  issue. There is no "no issue" form: a fix that has no issue gets an issue first.
  `.github/workflows/pr-issue-link-check.yml` (`tools/check_pr_issue_link.py`) fails a body that names none.
  GitHub copies the body into the squash commit, so the issue closes when the PR merges.
- `Refs #N` (or `Part of #N`, `Completes #N`) is only for a PR that leaves a named live check owed (a headset run, a
  server run), and the body says what is still open. That check is a task line on the issue, and whoever does it closes
  the issue with the evidence. A merged PR never leaves its issue open silently.
- The review confirms that each linked issue is resolved by the diff; the body check only sees that a reference
  exists.
- A PR carries the same milestone as the issue it resolves.
- A PR with no activity across a sprint is either picked up, rebased, or closed with the reason. A PR that is far behind
  `main` or conflicting is replaced by a focused PR rather than rebased wholesale.

**Branches**
- A branch whose PR merged is deleted (GitHub keeps it restorable from the PR).
- A branch with no PR is opened as one or deleted within 24 hours, by the seat that created it (`AGENTS.md`,
  "Branch lifecycle"); nothing is left to age across sprints.

**Milestones**
- An open milestone's title is `Next release`, `Release after` or `Future`. A milestone closed when its release is tagged is
  titled with the version it shipped as, `vX.Y.Z`; the closed `v4.x` milestones keep their titles.
- The description starts with `Outcome:`.
- No due date.
- A closed milestone has no open items.
