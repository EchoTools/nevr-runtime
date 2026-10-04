# Planning: releases by outcome, sprints by work

How work in this repo is planned and tracked on GitHub. It binds issues, PRs, branches and milestones. Planning
drifts; the hygiene rules at the end are how it gets corrected, on a schedule, the same way every time.

## Release milestones

One milestone per planned release, titled by version, then the theme:

    v4.0.0 — beta testers

- The description opens with `Outcome:`: the state of the world when the release is done, then its definition of done.
- **No due dates.** Releases are scoped by work, not by the calendar.
- Keep a few releases ahead, plus `Future` for work that is wanted but not scheduled.
- When a release is ready, anything still open in its milestone **moves to the next milestone**. Unfinished work never
  holds a release.
- A patch release (`v4.0.1`) gets a milestone only when there is a fix to ship.
- Work nobody has decided to schedule has no milestone and carries the `backlog` label.

| milestone | outcome |
|---|---|
| v4.0.0 — beta testers | testers can install, log in and play, and the release is provably the one we built |
| v4.1.0 — community game servers | a community game server comes up, registers, matchmakes and shuts down correctly |
| v4.2.0 — clean exits and crash handling | closing or crashing never leaves a process behind or loses a crash report |
| v4.3.0 — honest diagnostics | every log line and status message tells the truth, and dead code is gone |
| Future | wanted, not scheduled |

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
| Finished | PR open | CI green on the PR head |
| Delivered | merged | `just verify` passes on `main` right after the merge, output recorded |
| Accepted | proven in real use | the owner or a tester confirmed it working, or the end-to-end check passed |
| Rejected | failed acceptance | back to Started, with the failure quoted |

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
- A PR links the issue it resolves (`Closes #N`), or says in its body why there is none.
- A PR carries the same milestone as the issue it resolves.
- A PR with no activity for 14 days is either picked up, rebased, or closed with the reason. A PR that is far behind
  `main` or conflicting is replaced by a focused PR rather than rebased wholesale.

**Branches**
- A branch whose PR merged is deleted (GitHub keeps it restorable from the PR).
- A branch with no PR and no commit for 30 days is listed for its author to keep or drop.

**Milestones**
- The title is `vX.Y.Z — theme`, or `Future`.
- The description starts with `Outcome:`.
- No due date.
- A closed milestone has no open items. A release milestone with no open items is ready to ship.
