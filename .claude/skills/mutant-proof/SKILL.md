---
name: mutant-proof
description: Use when a test, gate or fix is written and must be shown to fail without the change, or when a mutation run reports "N of N killed". Mutates one line at a time from a committed tree and records red, then the restored tree green.
---

# Mutant proof

The procedure for "A test must fail without the change" and "A mutant for every changed line of behaviour" in `AGENTS.md`.

## Steps

1. Commit the change first. Run `go -C tools run ./cmd/mutant-proof -clean`: exit 1 means uncommitted work is in the tree;
   commit or stash it (`git stash push -u -m "<seat>: <why>"`). Never mutate over uncommitted work.
2. Save a copy of the file to mutate under your scratch directory. For each changed line of behaviour: alter that line
   alone, check `git diff --stat` shows the mutation landed (an empty diff is "never landed", not "killed"), run the
   covering test, and copy the failing output.
3. A mutant no test fails is a survivor. Report it before adding a test; then add the test and mutate again.
4. Restore from the saved copy or `git stash`, never `git checkout` of a file with other uncommitted work. Run the tests
   on the restored tree.
5. Write the block in the commit message: one `- M<n> <what> -> FAIL <test>: <output>` line per mutant, then
   `- restored tree: <n> tests OK`. A commit that changes no behaviour writes `None: <reason>`.
6. Run `go -C tools run ./cmd/mutant-proof -file <message file>`.

## Artifact and consumer

The `## Mutants` block. Consumer: `tools/cmd/mutant-proof` (the block's form, and `-clean` for the tree). It does not
re-run the mutants; the review does.

## Done when

`mutant-proof` exits 0 on the message, with every mutant `FAIL` and a restored-tree line.

## Proof of the check

`go -C tools test ./cmd/mutant-proof`: a survivor, a never-landed mutant, a missing restore line, an empty reason and a
dirty tree are each refused; a real landed commit passes.
