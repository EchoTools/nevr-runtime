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
2. `just reap-merged --apply`. It removes each worktree through `tools/remove_worktree.py` (the same as
   `just remove-worktree <path>`: it unlocks, leaves the shared submodule config alone, never runs
   `git submodule deinit` or `--force`), deletes the local branch, deletes the origin branch when its PR is merged and origin still holds the tip, and
   appends a removal row to the ledger.
3. Build output is deleted outright; the Trash is the same disk. `git worktree remove` does that for the
   ignored `build/`, `vcpkg_installed/` and `target/` trees; never move them to the Trash.
4. `df -h /`. Build trees outside `.claude/worktrees` (scratch under `~/src/nevr-runtime-work/`) are
   not touched; delete the ones you made.

`--owner <seat>` limits a run to one seat's worktrees. Each decision is a JSON line in
`~/.local/state/nevr-runtime/reap-merged.jsonl`. A worktree whose submodule holds uncommitted work or a
commit on no remote, or whose plain copy under `extern/` differs from the main checkout's, is kept with
the reason. To remove one worktree by hand: `just remove-worktree <absolute path>` (`--dry-run` first).
