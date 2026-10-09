---
name: merge-cleanup
description: Use right after you merge a PR, or when df shows the disk tight. Removes the worktrees, local branches and origin branches whose work has landed, each one only after a proof.
---

# merge-cleanup

1. `just reap-merged` (dry run). Read all of it.
   - `REAP` blocks pass every proof: the tip is in `origin/main` or is the exact head of a MERGED PR,
     the tree is clean, only build output is ignored, no process has its cwd inside, the ledger names
     an owner. The ignored-file list under each is what goes with the worktree.
   - `KEEP` blocks failed a proof and say which. Leave them. Fix the cause (move a run log out, stop
     the process, add the ledger row) and run the dry run again.
2. `just reap-merged --apply`. It unlocks, runs `git worktree remove` (never `--force`), deletes the
   local branch, deletes the origin branch when its PR is merged and origin still holds the tip, and
   appends a removal row to the ledger.
3. `df -h /`. Build trees outside `.claude/worktrees` (scratch under `/var/tmp/work-nevr-runtime/`)
   are not touched; delete the ones you made.

`--owner <seat>` limits a run to one seat's worktrees. Each decision is a JSON line in
`~/.local/state/nevr-runtime/reap-merged.jsonl`. A worktree with initialised submodules is always
kept: deinit it by hand as AGENTS.md describes.
