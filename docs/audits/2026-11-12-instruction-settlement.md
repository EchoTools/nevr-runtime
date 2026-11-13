# Instruction settlement, 2026-11-12

First run of `settle-the-instructions` on this repository, from a fresh clone of `origin/main` at `a2aa2d09`, on the local
branch `settle-the-instructions`. Nothing is pushed. This record is dated by design (`docs/audits/README.md`).

Scope of this run: the six copies of "Landing", the release-draft contradiction, the release-headlines README, the lists
`docs/standards/naming.md` names, the submodule count, and every other procedure the marker sweep in
`docs/standards/procedure-homes.txt` found written twice. `AGENTS.md` (1430 lines) was not read whole in this run, and
only the sections that carry these items were rewritten as imperative steps (Landing, Release, Headline). The rest of the
file is unchanged.

## Settlements

| # | Contradiction or duplicate | Deciding source | Change |
|---|---|---|---|
| 1 | Landing written six times: `AGENTS.md` "Landing" (five manual steps), `.claude/skills/nevr-work/SKILL.md` step 7, `CONTRIBUTING.md` step 4, `RELEASING.md` "Landing", the agent primer's maintainer line, `justfile` `land` | the code: `tools/cmd/land/main.go` header (lock, fetch, rebase, rerun each `-test`, push `HEAD:refs/heads/main` never forced, record `main` before and after, fast-forward the main checkout; exit codes 2-9) | Home is `AGENTS.md` "Landing", rewritten as `just land -test '<command>'` plus the exit-code actions. `nevr-work`, `CONTRIBUTING.md`, `RELEASING.md` and the primer now name `just land` and link to the home. |
| 2 | Sentences merged from two versions of the text, in `AGENTS.md` (the "Landing" row of the contributor table, "Evidence on the exact tip", the quick-check bullet, "Verification before landing"), `CONTRIBUTING.md` step 4, `RELEASING.md` "Landing", the primer | the surviving clause of each pair (the one that matches the code and the neighbouring text) | one sentence each; no sentence lost that stated a rule |
| 3 | `AGENTS.md` (draft, then CI build on the tag, then `--draft=false`) against `RELEASING.md` step 4 (`gh release create --prerelease`, never a draft) | #1865: a release appears on GitHub only once all its binaries exist, so no draft is ever created | The five-step draft recipe is removed from `AGENTS.md`; it points at the `RELEASING.md` flow table as the only statement of the steps. The paragraph that describes `build.yml`'s draft handling stays, marked `KNOWN-WRONG #1865`, because that code still exists (see question 1). |
| 4 | `AGENTS.md` placed `release-preflight` at "step 1" and "step 3" of the removed recipe | `RELEASING.md` flow table, step 0 | reworded to "step 0 of the flow in `RELEASING.md`" |
| 5 | The headline procedure written in `AGENTS.md`, `RELEASING.md` "Headline files" and `release-headlines/README.md`; the README also said "A PR" while `AGENTS.md` rule 2 says there are no pull requests | `tools/package_release.py` (`HEADLINE_DIR`, the name and length rules), `tools/tests/test_release_headlines.py`, `AGENTS.md` rule 2 | Home is `RELEASING.md` "Headline files". The README and the `AGENTS.md` bullet are one pointer each; "A PR" is gone. |
| 6 | The agent primer re-wrote the copy recipe (`cp -a --reflink=auto`) and the issue section list | `AGENTS.md` "The game install" and "Write an issue as a falsifiable claim" | the primer names the section and no longer lists the steps |
| 7 | `docs/standards/naming.md` named `KNOWN_COLLISIONS`, `KNOWN_ROLE_BREAKS`, `COMBAT_NAMES`, `KNOWN_COMBAT_BREAKS`, `RULES_HOME_SHARED_HOOK`, `KNOWN_DIRECTORY_BREAKS`; `git grep` finds none of them outside the standard; `tools/naming_inventory.py` is the only automated list | the tree (`git grep` over all tracked files) | the six names and the "fails the test" sentences are removed; the one real exception (`lobby_failure_message`) is kept as prose; a closing line says breaks found in review are filed as issues and no list exists |
| 8 | Submodule count: `AGENTS.md` "Dependencies", `README.md` (two places) and the primer said three (`minhook`, `breakpad`, `lss`) | `.gitmodules` and `git ls-tree HEAD extern/`: four, with `luau` | all four places say four |

## The check

`tools/cmd/one-home` (Go, standard library only) reads `docs/standards/procedure-homes.txt`: one line per marker, with a
procedure name, its home file and a regular expression. It fails (exit 1, with `file:line`) when a marker matches a line
of any other tracked Markdown file, except `docs/audits/`. A marker that matches nothing in its home is exit 2, so a
marker that guards nothing cannot pass. It runs as a Go test of `tools/`, so `just test-tools`, which the quick check
already runs, is its consumer; `AGENTS.md`'s quick-check bullet names it.

Proof (this branch):
- Red, on `origin/main`'s files plus the registry: 8 lines, among them `nevr-work/SKILL.md:119-120` and
  `CONTRIBUTING.md:55` (landing), `AGENTS.md:480` and `:507` (releasing), `release-headlines/README.md:5`, the primer's
  `:116` (filing) and `:328` (copy).
- Green, on this branch's files: exit 0.
- `go test ./cmd/one-home`: a planted second copy is refused and named, one home passes, a marker absent from its home is
  exit 2, `docs/audits/` is exempt, a bad registry line is exit 2, and the real tree passes.

Limits, stated so nobody reads the check as more than it is: it finds a procedure only by the markers registered for it.
`RELEASING.md` "Landing" held a fifth landing copy that no marker matches (it was reworded by hand, item 2). A paraphrase
that shares no marker passes. The registry has five procedures; "quick check" (`AGENTS.md` quick-check bullet and the
table in `nevr-work/SKILL.md` step 6) is written twice and is not yet registered or folded.

## Questions for the owner

1. `build.yml` (`drafts` job), `tools/release_draft.sh` and the signer all work on a draft release today
   (`RELEASING.md` "KNOWN-WRONG #1865"). The docs now say a release is never a draft. Is #1865 the unit that changes the
   code, and may the `AGENTS.md` paragraph marked `KNOWN-WRONG` be deleted when it lands? Until then the docs and the
   code disagree and this run changed no code.
2. `AGENTS.md` said releases are built by CI from a tag; `RELEASING.md` step 1 stages the binaries before any tag exists
   ("one build action makes every binary for one commit"), and the `justfile` still says "a release is a vX.Y.Z tag built
   by CI". Which is the flow? This run removed `AGENTS.md`'s version of the sequence and kept `RELEASING.md`'s, but did
   not touch the `justfile` comments.
3. The naming lists never existed. Should the naming rules get a real list and a guard, or stay review-only as the page
   says? This run only removed the references.
