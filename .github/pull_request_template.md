## What and why

<!-- What this changes, and why it is needed. -->

## Linked issue

Closes #

<!-- If there is no issue, delete the line above and say here why there is none. -->

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

- [ ] The PR is linked to its issue (`Closes #N`), or says above why there is none
- [ ] The PR has the same milestone as the issue it resolves
- [ ] One logical change, with a conventional-prefix commit message (`feat`, `fix`, `docs`, `ci`, ...)
- [ ] `src/legacy/` is untouched, and no generated protobuf (`gen/cpp/*.pb.*`) was edited by hand

<!-- The last non-empty line of the PR body must be this one, with <sha> replaced by the merge-base of
your branch and the PR's target: `git fetch origin main && git merge-base HEAD origin/main`
(7 to 40 hex characters; see AGENTS.md, "Review, verification and merge"). -->

Read AGENTS.md at <sha>
