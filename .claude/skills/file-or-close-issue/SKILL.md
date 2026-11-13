---
name: file-or-close-issue
description: Use when a defect is found and not fixed in the same turn, on a second sighting, or when closing an issue. Writes the issue as a falsifiable claim, or the close comment with the commit and the exit code, and checks the text for local paths before it is posted.
---

# File or close an issue

The procedure for "Fix it or file it, in the same turn" and "Write an issue as a falsifiable claim" in `AGENTS.md`, and
`docs/standards/planning.md`. A security-sensitive finding is not filed: it goes to the owner.

## Steps

1. Search the open and closed issues for the symptom first (`gh issue list --search <term> --state all`); a hit is
   commented on, not duplicated.
2. Write the text with the Write tool to a file in your scratch directory (never a heredoc): sections `Evidence at <sha>`
   (what the tree says, with `file:line`), `Not shown`, `Falsifier`, `Work`. Repo-relative paths only.
3. Run `go -C tools run ./cmd/issue-prose -kind body -file <file>`. Exit 1 prints each problem: fix it, run again.
4. File with `gh issue create --body-file <file>` and the milestone or `backlog` label `planning.md` says. Print the issue number.
5. To close: write the comment (the commit sha that landed it and the exit code of the check that shows it), run
   `go -C tools run ./cmd/issue-prose -kind close -file <file>`, then post it with `gh issue comment --body-file <file>` and close the issue.
6. Public prose is drafted for the owner's seat to post where `AGENTS.md` says a comment is sent first; this skill
   posts nothing it was not asked to.

## Artifact and consumer

The issue body or close comment. Consumer: `tools/cmd/issue-prose` (sections, sha, exit code, no local path). It cannot
see a hosting URL, a quoted chat line or whether the claim is true; the review does.

## Done when

`issue-prose` exits 0 on the text and the posted issue number is in the record.

## Proof of the check

`go -C tools test ./cmd/issue-prose`: a missing section, a missing sha, a local path and a close with no exit code are refused.
