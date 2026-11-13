---
name: landed-commit-evidence
description: Use when a branch has passed its quick check and the final commit message is being written. Builds the message that carries the search statement, siblings, symbols, mutants, evidence, parity and issue, then runs the check on it.
---

# Landed-commit evidence

The procedure for "What a landed commit carries" and "The commit message is the evidence" in `AGENTS.md`. Those
sections say what each item is; this skill is the order of writing it and the check that refuses a missing one.

## Steps

1. Run the quick check on the branch tip. Record `git rev-parse HEAD^{tree}`.
2. Write the message with the Write tool, to a file in your scratch directory, never a heredoc. Subject: Conventional Commit.
3. Add the sections, in this order, each non-empty: `## Search statement` (command, scope, what it found),
   `## Siblings` (a fix only: the shape, the `git grep`, a verdict per hit), `## Symbols` (edited; read, not edited),
   `## Commands and results` (each command run on the tree from step 1, with its `exit N`; what was not run),
   `## Mutants` (see `mutant-proof`), `## Platform parity`.
4. Name the issue (`Closes #N` or `Refs #N`) and add the trailers the repo's commit-identity rule lists, with a
   `Co-Authored-By` for every person whose work is integrated.
5. Run `go -C tools run ./cmd/commit-evidence -file <message file>`. Exit 1 prints each missing item: write it, run again.
6. Commit with `git commit -F <message file>`; after landing is chosen, `go -C tools run ./cmd/commit-evidence -rev HEAD`.

## Artifact and consumer

The commit message. Consumers: `tools/cmd/commit-evidence` (run by hand at step 5) and `tools/cmd/land`, which runs the
same check (`tools/internal/evidence`) on the tip commit and exits 10 before the tests and the push when anything is
missing. Both read the form only; the review still checks that each claim is true.

## Done when

`commit-evidence` exits 0 on the committed message, and the review has reproduced the commands named in it.

## Proof of the check

`go -C tools test ./cmd/commit-evidence`: each section removed in turn is refused, a fix without Siblings is refused,
and a real landed commit passes.
