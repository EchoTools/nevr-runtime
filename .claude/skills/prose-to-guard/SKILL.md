---
name: prose-to-guard
description: Use when a rule exists only as prose (an AGENTS.md line, an ADR "must", a review-only check, an admission that no check enforces it). Turns it into a Go guard that refuses at the step where the violation would happen, proven red and green, then replaces the prose with one line naming the guard.
---

# Prose to guard

The guard is Go, in `tools/cmd/`, built by the repo's toolchain. No Python. Register it in
`docs/standards/guards.txt`; its consumer is `tools/cmd/guard-proof`, run by `just test-tools`, which fails when a
registered guard has no planted-violation test, no green test, no bad-input test, or its step is not named in
`AGENTS.md`.

## Steps

1. Quote the rule and its place (`file:line`). If the text says "no check enforces this" or "the review checks", that
   is the rule to take.
2. State it as a predicate over something a machine can read: a file, a diff, a commit message, a process list, a
   transcript. If you cannot, stop: the rule stays prose, and the record says why.
3. Name the step where the violation would happen (commit, landing, build, release, the start of a run). The guard
   goes there as a check, hook or gate, so it refuses before the damage, not after.
4. Write a planted violation: the smallest input that breaks the predicate. It must be refused.
5. Write the guard in Go, in the repo's tools directory, with the repo's own test helpers. Standard library first.
   Exit codes distinguish "violated" from "the guard itself is broken".
6. Prove it red on the planted violation and green on the real tree. Add a case where the guard's own input is wrong
   (a marker that matches nothing, an empty list) and show it fails rather than passing.
7. Wire it into the step from 3 and, if that step is the quick check, into the recipe the quick check already runs.
8. Replace the prose with one line that names the guard and the step. Delete the rest.

## Artifact and consumer

The guard plus its red/green proof (commit message or record). Its consumer is the step it guards: the step refuses
when the guard exits non-zero. A guard nothing runs is not done.

## Done when

The planted violation is refused by the wired step, the real tree passes, the prose is one line, and the commit says
what the guard cannot see.

## First use

`tools/cmd/one-home` in `nevr-runtime` (the check from `settle-the-instructions`): predicate "no marker line of a named
procedure appears outside its home file"; step `just test-tools`; planted violation a second landing copy; red 8 lines
on `origin/main`, green on the branch.
