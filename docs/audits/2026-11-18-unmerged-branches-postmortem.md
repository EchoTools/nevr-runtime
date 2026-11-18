# Postmortem: why so many branches and worktrees were never merged

Measured 2026-11-18, at the start of the cleanup and re-checked against `origin/main` (`d22e36303`) before this record
was committed. Evidence is from `git` on the clone, the ledger and the issue tracker. This record is dated by design
(`docs/audits/README.md`): it cites the tree as it was when measured.

## What was found

- Dozens of local branches and worktrees, a stack of remote branches, three stashes and detached review worktrees.
  (Counts are the owner's measurement at the start of the cleanup; the objects are gone, so they are not re-measured.)
- Most "unmerged" branches were already on `main` in another form: a rebase changes the diff's context, so `git cherry`
  showed them as unmerged although `main` carried the same change. Those were deleted with the carrying sha cited.
- A small part was real unlanded work. It was finished and landed in the same cleanup or ported. Examples that are on
  `origin/main`: the game's `_local` and `_temp` moved into the nEVR root (`68b31c13e`), DirectInput controllers only in
  desktop mode (`149c185ae`), `-noaudio` keeps the mic capture off (`d0efbc021`), cargo-pack and cargo-merge
  (`66ff53536`), the custom-map fetch worker stopped at game exit (`a37c29422`), and SDK tests (`80fd68041`). The maps
  owner probes were ported (rows below).
- A telemetry stack of eight branch tips existed as rebased twins of each other (2099, 2100, 2269, 2306 and their
  `-rebased` copies). It was deleted as rebased duplicates; the tips are recoverable by sha.

## Causes

1. **No working ledger.** AGENTS.md says every worktree, branch, scratch file and stash gets a row in the file
   `REAP_LEDGER` names. `REAP_LEDGER` lived in `.env`, which is git-ignored (`.gitignore:118`) and was never in a clone,
   so it was unset. `just reap-merged` refused without it, so the cleanup backstop never ran. Each agent kept its own
   private list (fragmented ledgers), which no other seat could read.
2. **Creation had a plan, cleanup had none that ran.** The rule "whoever creates a branch deletes it" is prose. Nothing
   made worktree creation write the ledger row, and nothing failed when a landed branch was left behind.
3. **Rebase twins.** `pre-rebase-*` and `*-rebased` branches were made as safety copies before a rebase. Landing
   rebases anyway, so each twin doubled the branch count and made the originals look unmerged.
4. **Stranded work in places nobody scans.** Stashes, detached review worktrees and one remote branch carried
   unlanded commits with no row anywhere.
5. **A test-map hole.** Scanners that read every source file were not mapped to the files they cover, so a landing
   went red on `main` (`test_spectatorstream_order`, fixed by `95335ce62`). A red main made agents hold branches back
   instead of landing.
6. **Stale instructions.** AGENTS.md said `just test-tools <file>` was a file-scoped check; it also runs the whole Go
   half. Named-person lines were stale after the team changed.
7. **Fan-out without an owner for the landing.** Several agents finished a tranche and stopped at a green check,
   a pushed branch or a report, leaving the landing to someone else.

## Mitigations (landed or filed)

Issue state read from the tracker on 2026-11-18.

| Mitigation | Status |
| --- | --- |
| `just reap-merged` defaults its ledger to `<work root>/LEDGER.md` and creates it | landed, `527293f89`, #2765 (closed) |
| `just new-worktree` creates the worktree and writes its ledger row in one command | landed, `c503508e7`, #2766 (closed) |
| Test map selects a test whose own text walks the tree; `just test-map -check` fails on an undeclared walker | landed, `a757d5d82`, #2767 (closed) |
| `just test-tools <file>` documented: the file narrows the Python half only; the Go half always runs whole | landed, `ce269c7b8` (a `docs` commit, no issue) |
| Named-person lines removed from AGENTS.md, the engineer brief and the nevr-work skill | landed, `c9b173dc7` (a `docs` commit, no issue) |
| Scanner test no longer fails on a shipped file reading the game flags field | landed, `95335ce62` |
| Reaper credits `seat/12-x` to a row for `seat/12-x.y` | filed, #2768 (open) |
| A Python walk the generator cannot resolve makes no scanner | filed, #2769 (open) |
| A pytest that reads the tree through an imported tools module is not selected | filed, #2770 (open) |
| `test-changed` does not compile the map probe game-half sources | filed, #2772 (open) |

## What landed since the first draft

| Change | Status |
| --- | --- |
| The maps owner probes ported to Go, with their packaging and test-map entry (`e9917671a`, `07bfd2e8c`, `3897f2272`) | landed, Refs #2733; #2733 stays open for the PCVR system test the probes serve. The game-half probe sources are compile-only and no probe has run in `echovr.exe` |
| The Quest map-edge tests: the shared synthetic map-set builder (`2b03b0e34`), `QuestMounter` and `ApplyMount` (`c4f1b7ec5`), the map service's mount wait, later mount and start (`e7e9e389c`), the store build's log record shape (`d22e36303`), and the logging standard's listing of the Quest `maps_start` and `maps_mount` records (`b72156824`) | landed |
| Player headlines for v6.19.0 (`73088f7ee`) | landed |
| New-worktree and ledger default (`c503508e7`, `527293f89`) | landed, listed in the mitigations table above |

## What the cleanup cost

The cleanup was run by fanning out agents, one per group of branches. Three of them were stopped mid-run, and their
branches had to be finished by hand:

- A planted mutant was left uncommitted in one worktree when its agent was stopped, so the worktree held a deliberately
  broken source file that nobody had read as such until the diff was inspected.
- One review's polling shell kept its working directory inside a worktree, which blocked `just remove-worktree` (it
  refuses while a process has its cwd inside) until that shell was ended.
- The third was stopped with its tranche unfinished and picked up from the commits in its worktree.

Mitigation, as rules for agents and reviewers: a mutant is restored from a saved copy of the file before the agent
stops, never left in the tree; a stop is not a state to leave behind, so a tranche is committed before work that can be
stopped; and no polling shell is left with its working directory in a worktree (poll from the main checkout with
absolute paths).

## Rules to keep

- Create a worktree with `just new-worktree` (AGENTS.md "Branch lifecycle"); its ledger row is the plan for deleting it.
- No `pre-rebase` or `-rebased` copies. Landing rebases the unpushed branch; the commits are in the worktree.
- A tranche ends on `origin/main` in the same turn, then `just remove-worktree` and the branch delete.
- Run `just reap-merged` (dry run) after every landing (skill `merge-cleanup`).
- Stashes carry a seat and a reason (`git stash push -u -m "<seat>: <why>"`) and a ledger row.
- A red `main` is bisected and the culprit reverted or fixed by a landed commit; landing does not stop while `main` is red
  (AGENTS.md "Verification before landing").
- Restore a planted mutant from a saved copy before stopping, and leave no shell with its cwd in a worktree.

## Not verified

The branch, stash and worktree counts, the stranded stash, review-worktree and remote-branch commits, the eight telemetry tips and the `git cherry` observation come from the cleanup
session's own measurements; the objects no longer exist. The set of landed examples in "What was found" is a sample, not
a list of every branch finished.
