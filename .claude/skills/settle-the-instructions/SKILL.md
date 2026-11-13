---
name: settle-the-instructions
description: Use when a repo's instruction files, docs and ADRs may disagree with each other, with the code, or with a decision recorded in an issue or ADR, or when one procedure (landing, releasing, filing, running on a copy) is written in more than one file. Produces a settlement record and a Go check that fails on a second copy.
---

# Settle the instructions

Every tool this skill adds is Go, in `tools/cmd/`, built by the repo's toolchain. No Python. Its consumer here is
`tools/cmd/one-home` (registry `docs/standards/procedure-homes.txt`), run by `just test-tools`.

## Steps

1. Work in a fresh clone of `origin/main`, on a local branch. Push nothing.
2. Read every instruction file, doc and ADR in scope, whole. Say in the record which files were not read whole.
3. List every contradiction (two files, or a file and the code, say different things) and every procedure written in
   more than one place. Record each with `file:line` for every side.
4. Settle each from a cited source, in this order: the code (`git grep`, the tool's own header), a commit, a decision
   recorded in an issue or ADR. A guess is not a source. Anything no source settles goes to the owner as a question in the record;
   change nothing for it.
5. Fold each duplicate to one home. Keep the steps in the home; every other file names the recipe or section and links.
6. Rewrite the home as imperative steps where the repo's method is the only path described: no "you are", no menu of
   alternatives. A rewrite changes no rule; a sentence that stated a rule survives.
7. Where the code still does what the settled text now forbids, say so in the text (the repo's KNOWN-WRONG form) and
   in the record. Do not change code in this skill.
8. Add the check (below), run it red on the original files and green on the result, and name it in the repo's quick
   check text.
9. Write the record: contradiction, deciding source, change. Then the questions. Then what the check cannot see.

## Artifacts and consumer

- **Settlement record**, dated, kept where the repo keeps dated records.
- **The check**: a Go command that reads a registry of procedures (name, home file, a marker regex per line of the
  steps) and fails with `file:line` when a marker matches a Markdown file other than the home. A marker that matches
  nothing in its home fails too, so a dead marker cannot pass. The consumer is the repo's quick check: the check runs as
  a Go test there, so a branch that writes a second copy cannot land. See `prose-to-guard` for how it is built.

## Done when

The check exits 1 on the original files with the lines named, 0 on the result; every contradiction in the record has a
cited source or is a question; no question was answered by guess.

## Proof of the check

`go -C tools test ./cmd/one-home` (a planted second copy is refused, the real tree passes, a dead marker fails).

## First use

`nevr-runtime`, 2026-11-12: six landing copies folded to `AGENTS.md` "Landing" from `tools/cmd/land`; the draft-release
conflict settled by an issue's recorded rule; `tools/cmd/one-home` added; red 8 lines on `origin/main`, green on the branch;
three questions left for the owner.
