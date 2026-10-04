## What and why

<!-- What this changes, and why it is needed. -->

## Linked issue

Closes #

<!-- If there is no issue, delete the line above and say here why there is none. -->

## How it was verified

<!--
`just verify` is the gate (see AGENTS.md). Paste the relevant output, and say what it does not cover:
it does not catch wire-format bugs, login failures or rendering regressions.
For runtime changes, say how it was run against a live server and quote the program's own log.
See docs/standards/verification.md.
-->

- [ ] `just verify` passes (output pasted above)

## Checklist

- [ ] The PR is linked to its issue (`Closes #N`), or says above why there is none
- [ ] The PR has the same milestone as the issue it resolves
- [ ] One logical change, with a conventional-prefix commit message (`feat`, `fix`, `docs`, `ci`, ...)
- [ ] `src/legacy/` is untouched, and no generated protobuf (`gen/cpp/*.pb.*`) was edited by hand
