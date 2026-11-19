---
name: repo-hygiene
description: Use at the start and end of any work in a repo, and whenever asked to clean up a repo — worktrees, local or remote branches, dirty files, a detached checkout, a full disk from build trees and scratch, issues with no milestone, label or link, shipped milestones left open. Leaves the repo cleaner than it was found.
---

# Repo hygiene: leave the repo cleaner than you found it

`AGENTS.md` governs this skill. Where this file says less, follow `AGENTS.md` ("Branch lifecycle", "Ownership and
cleanup", "Landing", "Git"); where the two disagree, `AGENTS.md` wins and this file is the defect.

Every agent that works in a repo creates things: worktrees, branches, build trees, scratch. When nobody resolves them,
the disk fills, the checkout drifts, and the next agent cannot tell finished work from abandoned work. Run this skill when
you start work in a repo and again before you report done.

**Resolve, never preserve.** Every leftover is either worth having, so it lands on `origin/main`, or it is not, so its
creator removes it. Nothing is kept "just in case": no wip branches to park it, no archives, no Trash.

**Remove what is yours and proven; report the rest.** You remove what your ledger row names and a proof shows is
redundant ("Ownership and cleanup"). Anything you cannot prove is yours is left alone and reported with its evidence. You
do not stop to ask for a go on your own work (rule 6 of "How an agent works here").

## Every file gets a lifecycle when it is made

Decide what a thing is for, whose it is and when it goes when you create it, not when someone finds it:

- **Anything that matters goes to its real home at once:** a finding onto its issue ("Fix it or file it, in the same
  turn"), code onto a branch in its own worktree that lands through "Landing" (no pull request), a game or protocol fact
  into a tracked file ("Write down what you learn"), a reverse-engineering result into the reconstruction. Scratch never
  holds the only copy of anything.
- **A sign-in file or key is never copied into scratch or the repo.** A run links the account's stored sign-in
  (`launch-client.sh --creds`); a copy that exists is removed by exact path as soon as nothing reads it.
- **Anything private about a person** (family words, messages, photos) never goes on an issue, in a commit or in scratch.
  Leave it where its owner keeps it.
- **Worktrees** are made by `just new-worktree <seat>/<slug>` under `.claude/worktrees/`, locked, with a ledger row.
  **Scratch** is under `<work root>/<seat>/` (the work root is `$NEVR_WORK_ROOT`, else `~/src/nevr-runtime-work`), never
  `/tmp`, never the repo, never another seat's directory. A directory directly under the work root carries a `README.md`
  saying what it is for, whose it is and when it is removed.
- **Every other thing** (a build tree, a game copy, a run's output, a stash, an issue) gets a row in the ledger
  (`$REAP_LEDGER`, default `<work root>/LEDGER.md`): artifact, owner (your seat name), purpose, remove-when. A
  "remove-when" that nobody reads is a label, so at the end of your work you read your own rows and remove what is past
  its end.

## Whose leftovers these are

The ledger names an owner for each worktree, branch, scratch path and stash. Another seat's items are theirs: do not touch
them, and do not infer ownership from a name or a location ("Ownership is proven, never inferred"). An item with no ledger
row and no lock reason is reported to the owner with its evidence; you do not resolve it on your own say-so.

- **"Not running" is judged by effect:** a live process with its cwd there (`just remove-worktree` refuses it), or a
  session that answers. Never by a window name or a stale pid.
- **Uncommitted changes you did not make are someone's work** (rule 3): read them (`git diff`), leave them, report them.

## Scope

The repo containing your working directory, and only that repo. Another repo has to be named explicitly.

## 1. Look

Print the raw output of each.

```
git status -sb                          # the checkout: branch, dirty, ahead/behind
git fetch origin main                   # so "merged" means merged into what origin has now
git worktree list --porcelain
git branch -vv
git config user.name; git config user.email
git stash list | wc -l                  # stashes live in the common git dir; removing a worktree loses where they came from
```

A detached HEAD, a dirty main checkout, or a checkout behind its upstream is your **first finding**, before anything
measured against it. Every ahead/behind and every "merged" is measured against `origin/main` by name, never against HEAD.

Then the inventories. The worktree inventory is the ledger (`$REAP_LEDGER`) read against `git worktree list` and
`just reap-merged` (the dry run, skill `merge-cleanup`), which prints a REAP or KEEP block with its proofs for each
worktree. The planning check is `docs/standards/planning.md` ("Hygiene rules") applied by hand with `gh issue list`,
`gh api repos/<owner>/<repo>/milestones` and `git branch -r`. A check that could not run is not a pass: quote the error and
say it did not run.

For each local branch not checked out anywhere, measure:

| question | command |
|---|---|
| merged by ancestry | `git merge-base --is-ancestor <b> origin/main` (exit 0 = merged) |
| merged by rebase | `git cherry origin/main <b>`: no `+` lines means every patch is already upstream |
| unpushed commits | `git rev-list --count @{u}..<b>` where it has an upstream (a branch stays local, so most have none) |
| open pull request (a contributor's) | `gh pr list --head <b> --state open` |

## 2. Analyze everything that is not merged or clean

Git can tell you a branch is not merged. It cannot tell you whether that matters. So before anything is resolved, read
each unmerged branch, dirty worktree and untracked file, and write a verdict for it.

For each one:

1. **What is in it:** its commits (`git log --format='%h %an %ad %s' origin/main..<b>`), the unmerged count
   (`git cherry origin/main <b> | grep -c '^+'`), the diffstat against main, and for a dirty worktree the diff itself.
   Read the diff, not only the subjects: a commit called "fix" can be a guard line deleted by a mutation test.
2. **What it is for:** the issue it serves (branch name, commit messages), the ledger, its worktree lock. Whose it is.
3. **Does main already have it?** `git cherry` catches only exact patches. Also look for the change itself on main: the
   files it adds, a distinctive line from its diff (`git grep -n '<line>' origin/main`). Main having the outcome under
   different code counts as merged.
4. **Is it still wanted?** The issue is open, or real work main does not have. An experiment whose result is recorded
   (on its issue, with the numbers) is done; one whose result is not recorded is not done yet.
5. **The verdict, in one paragraph.** **Merged:** main has the outcome, so the branch goes, naming the commit on main
   that has it. **Unfinished:** finish it, run its quick check, review it and land it ("Finished work lands"). Work that
   main had and then reverted counts as merged: main's outcome is the revert (`git log origin/main --grep=Revert`).
   Never "keep for now", "park it" or "a draft for later".

The verdicts go in the report, one per item. If you stop short of reading everything, say how far you got ("analyzed 12
of 40, newest first; 28 not read"). You may hand a branch's reading to a subagent, but the verdict is yours, and you
check it against the diff yourself.

## 3. Resolve what you can, yourself

None of this destroys anything, so do it without asking:

- **Work worth keeping** (a dirty worktree or unmerged branch of yours whose change is still wanted): commit it with named
  paths (never `git add -A` or `git add .`), then run the quick check, the review and `just land` ("Landing"). Untracked
  and ignored files are never committed by this skill; list them by name in the report. Unplanned work with no issue and
  no plan is reported with its diffstat and how you searched for its issue, never removed.
- **The main checkout is left alone** ("Landing"), and work happens in worktrees. If it is detached, off `main` or dirty,
  that is your first finding: report it with `git status -sb` and `git diff`, and do not switch it (rule 3).
- **GitHub planning findings** (milestones, labels, issues with no milestone) are reported to the owner as one block with a
  recommendation each, per `docs/standards/planning.md`.

## 4. Remove what is yours, by the sanctioned commands

Remove an item only when its ledger row names your seat and a proof shows it is redundant: merged by ancestry or
`git cherry`, regenerable build output, or abandoned with its tip SHA printed and its worktree clean.

- **Worktree:** `just remove-worktree <absolute path>` (`--dry-run` first), from the main checkout or another worktree.
  It refuses a live cwd, uncommitted or ignored work beyond build output, and a submodule with unpushed commits. Never
  `git worktree remove` by hand, never `--force`, never `git submodule deinit`.
- **Branch:** `just remove-worktree <abs path> && git branch -D <branch>`, after checking the tip still matches the SHA you
  reported (a moved tip means someone is working on it: skip it and say so). Delete an origin branch only if one was ever
  pushed there.
- **Scratch and build directories:** move each finished item, by its exact path, into a new holding directory under your
  own scratch root (`<work root>/<seat>/to-delete-<n>/`), list it, then `rm -r -- <holding dir>`; or write one `rm` per
  exact absolute path into a script (`set -eu`, no globs), read it back, check each path is yours, run it. No other
  recursive removal, no Trash (it frees nothing on this disk), no variable, glob or `..` in the path.
- **Join the steps with `&&` or run them under `set -e`, never `;`:** a failed check stops every delete after it.
- Update the ledger row, then run step 1 again and quote it. That output is the result, not your account of it.

Deleting a document needs the owner's confirmation ("Docs state the current tree").

## Evidence

Every finding and every action comes with the raw command output that shows it. A count comes with the list it counts. If
you limited a check (the last 100 pull requests, say) or one could not run, write that next to the item. Otherwise "found
nothing" and "did not look" read the same.

## Traps

- **A rebase-merge is invisible to `--merged`.** Use `git cherry` too, or merged work looks unmerged forever.
- **Someone may be in the room.** Before removing any directory, check `/proc/*/cwd`, open fds (`/proc/*/fd`) for a
  process inside it. Leave out your own session, and say you did.
- **Detached HEAD in a worktree:** its commits are on no branch. Name a branch before you remove anything.
- **Ignored files are invisible to `git status`,** and `--ignored=matching` collapses ignored directories. Ask the
  filesystem what is in a directory before calling it empty.
- **Removing a directory under live sessions** leaves them running off deleted inodes: alive, unrestartable, no source
  behind them. That is why the room check comes before every delete.
- **A check that fires and then gets explained away:** if you find a reason a finding does not count, the reason is the
  finding. Report it; do not drop it.

## Report

```
look:      <git status -sb line>; <worktree count>, <branch count>, <stash count>; planning: <summary line>
resolved:  <each step-3 action, with its output>
removed:   <each step-4 removal, with its proof>
left:      <items not provably yours, each with its evidence>
DECIDE:    <planning findings, grouped, one recommendation each>
not checked: <anything limited or failed>
```
