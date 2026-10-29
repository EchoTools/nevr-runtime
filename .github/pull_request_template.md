## What and why

<!-- What this changes, and why it is needed. -->

## Linked issue

Closes #<issue>

<!--
A PR is opened only for an existing issue: the issue states the problem and its bar, and the PR names it. Replace the
line above with one `Closes #N` line for each issue this PR resolves. A PR that leaves part of an issue open (a live run
still owed) uses `Refs #N` and says here what is still open. A fix with no issue gets an issue first. The check fails a
body that names none (docs/standards/planning.md, "PRs").
-->

## How it was verified

<!--
The quick check is the gate for a PR (see AGENTS.md, "Builds"): the build, the test recipe(s) that cover the changed code,
and the PC login test when the change touches runtime or auth. `just verify` runs only on main, when prepping a release.
Paste the relevant output, and say what it does not cover: it does not catch wire-format bugs, login failures or
rendering regressions. For runtime changes, quote the program's own log from the login test.
See docs/standards/verification.md.
-->

- [ ] The quick check passes on this tip (output pasted above)

## Checklist

- [ ] The PR names the issue it resolves (`Closes #N`) or advances (`Refs #N`, with what is still open)
- [ ] The PR has the same milestone as the issue it resolves
- [ ] One logical change, with a conventional-prefix commit message (`feat`, `fix`, `docs`, `ci`, ...)
- [ ] Siblings: a fix names the shape it fixed, the `git grep` that finds every instance, and each one's outcome (fixed here, filed as `#N`, or not the same shape and why)
- [ ] Platform parity: a PR that adds or changes player-facing behavior says here what the other platform's status is (done in this PR, already present at `file:line`, not applicable and why, or a linked twin issue)
- [ ] No generated protobuf (`gen/cpp/*.pb.*`) was edited by hand

<!-- The last non-empty line of the PR body must be this one, with <sha> replaced by the merge-base of
your branch and the PR's target: `git fetch origin main && git merge-base HEAD origin/main`
(7 to 40 hex characters; see AGENTS.md, "Review, verification and merge"). -->

Read AGENTS.md at <sha>
