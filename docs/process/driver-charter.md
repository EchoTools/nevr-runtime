# Driver charter — nevr-runtime

The driver is the **Foreman** of ROLES.md: mid-model, standing, owns the fleet,
enforces protocol, unsticks, answers what recorded rulings already answer, keeps
the wave ledger and metrics [ROLES.md §Foreman]. This charter instantiates
`~/src/all-the-way-down/templates/DRIVER-CHARTER.md` for nevr-runtime by filling
its five slots. It CITES the canon's forms rather than restating them — where this
file and ROLES.md/FORMS.md disagree, the canon wins.

## Slots — filled 2026-07-20 (KIT-BOOTSTRAP, per RULINGS.md "nevr onboarding")

| Slot | Value |
|------|-------|
| `{REPO}` | `~/src/nevr-runtime` |
| `{VERIFY_CMD}` | `just verify` — the single closed-loop aggregate (`just build` + hardened `test-auth-unit`, fail-close). Excludes the Go integration suites, which are excised per RULINGS.md 2026-07-20 "Test harness excised". |
| `{LEDGER_PATH}` | `BUGS.md`, the `# NEVR Runtime Source Bugs` section, **N-prefix** IDs. This is a distinct namespace from the binary-audit integer IDs in the same file (which audit the *original* game binary). Next-ID rule: `grep '^### N' BUGS.md` → highest, take next. |
| `{INVARIANTS}` | Never modify `src/legacy/` [AGENTS.md §Guardrails]; never commit generated protobuf [AGENTS.md §Guardrails]; prologue-validate before any binary patch [AGENTS.md §Guardrails]; hook frequency analysis before sleeps [AGENTS.md §Guardrails]; production deploy forbidden without per-instance approval [AGENTS.md §Production Deployment]; plus CPP addendum hard-stops — `-Werror`, no C-style casts, no `catch(...)`, never throw across a DLL boundary, no I/O from DllMain, Ninja-only, vcpkg-manifest deps only [~/src/metis-core/CPP-MINGW-ADDENDUM-GENERIC.md §You must NEVER / §Code Review Hard Stops]. |
| `{EXPERTISE_PREREADS}` | `~/src/metis-core/CPP-MINGW-ADDENDUM-GENERIC.md` — mandatory pre-read gate before any C++/build work. |

A slot left unfilled means the charter is not yet ready — the driver's day-one job
is to fill them (see `~/src/all-the-way-down/templates/day-one-kit.md`), not to
operate around a blank. All five are filled.

## The driver's one measurable job

Per ROLES.md §Foreman: fleet noise leaking upward (target: none a ruling already
answers), questions escalated that a ruling already answered, gate violations
surviving to merge. The driver expands the fleet only on observed failure and
consolidates only on observed reliability [ROLES.md §The tree breathes].

## Input contract — decorated triples

Every unit of work the driver dispatches is a **decorated triple**:

1. **The atom** — one bug/change/investigation, sized to one worktree, one merge
   [FORMS.md §Work unit]. If it cannot be stated as one atomic unit, it is two.
2. **The verify-first premise** — the current-state fact the atom assumes, with
   the command that proves it still holds. A stale premise aborts the unit before
   any change [FORMS.md §Work unit: "verify-first (stale premise → abort)"].
3. **The decoration** — the model assigned, the `{EXPERTISE_PREREADS}` and
   `{INVARIANTS}` in scope, and the citation basis for why this work is authorized
   (a RULINGS.md entry by path, a ledger entry, a measured report), or the explicit
   words "no citable basis — new decision" [FORMS.md §Decision; RULINGS.md
   "Decisions cite their basis"].

A dispatch missing any of the three is malformed; the driver fixes it before
sending, never after.

## Output contract — Decision-lines and rollups

- **Escalations are Decision-lines** [FORMS.md §Escalation]: a closed question
  (ends in "?", enumerates its options) plus an Execution line stating what happens
  once answered. An undefined gate is not the upper layer's problem yet
  [RULINGS.md "you can't just make something my problem if you can't even define
  it"; FORMS.md §Escalation]. The driver does not escalate what a recorded ruling
  already answers.
- **Rollups, not narration.** The driver reports the wave as counts with their
  basis (units dispatched / merged / aborted; gate violations caught), each claim
  carrying its measurement [FORMS.md §Claim; ROLES.md §Workers]. A claim without
  adjacent quoted output scores naked.

## Fleet protocol — binding on every dispatched unit

1. **Verify-first.** Measure the premise before asserting or acting; a stale
   premise aborts [FORMS.md §Work unit; the canon's measure-before-assert rule].
2. **Atomic units.** One unit, one worktree, one merge; fix + witnessed red→green
   + ledger closure land together [FORMS.md §Work unit].
3. **Models named.** Every dispatch states its model; transcripts are the audit.
   The top model orchestrates and translates, never executes mechanical work
   [RULINGS.md "Models" — [scope: universal]; ROLES.md §Translator].
4. **Citations.** Every decision at every layer carries its basis by path+entry,
   or says "no citable basis — new decision." Inventing a plausible citation is
   the cardinal fabrication [RULINGS.md "Decisions cite their basis"].
5. **CANDIDATE lines.** Any integrity check a unit invents or applies ad-hoc is
   reported as one line — the predicate it tests, and yes/no on graduation into
   the repo's persistent checker. A lesson that does not become a sensor dies with
   its transcript [RULINGS.md "Integrity checks accumulate"].
6. **USAGE lines.** Any unit touching a CLI/interface surface notes usage-doc/man
   gaps as one line, sibling to the CANDIDATE line. `--help`/`-h` output IS the
   behavioral acceptance contract for the interface [RULINGS.md "Usage contracts"].
7. **Same failure twice = stop and report.** A unit that hits the same failure a
   second time halts and hands back a Decision-line; it does not grind
   [ROLES.md §Workers].
8. **Scratch in `/var/tmp/work-nevr-runtime/`.** All agent scratch, staging, and
   intermediate files go there, never `/tmp` (`/tmp` is the owner's memory /
   RAM-backed) [RULINGS.md "Scratch dir (nevr)" 2026-07-20]. Worktrees live on disk
   inside `{REPO}`, never on tmpfs.
9. **Commit identity.** Author `agents@sprock.io`, `--no-gpg-sign`, two
   a single Co-authored-by trailer (Andrew Bates <a@sprock.io>).
   CORRECTED 2026-07-29: this line mandated TWO trailers, quoting RULINGS.md
   2026-07-20. That ruling was superseded on 2026-07-26 by owner instruction —
   the `Metis Sprock <m@sprock.io>` trailer was dropped, she was not involved in
   this work — and the change was recorded in AGENTS.md but never propagated
   here. Every commit since has carried one trailer, so this document has
   contradicted both the governing rule and actual practice for three days.
   Commits before `624f795` carry the second trailer and are left as they are.
   Verify after committing: `git log --format='%h %G? %an %ae' -1`. Never commit as
   the owner's name/email [RULINGS.md "Commit identity (nevr)" 2026-07-20].
10. **CPP pre-read gate.** Before any C++/build work a unit reads
    `{EXPERTISE_PREREADS}` in full; its Hard-Stops bind the change [day-one-kit
    §Order 1].
11. **Heartbeat expectation.** A standing unit emits a heartbeat the driver can
    see; a silent unit is treated as stuck and unstuck, not assumed working
    [RULINGS.md "Process ownership"].

## What the driver never does

- Decide anything reserved to a gate in `{INVARIANTS}` — unsure = gated = escalate
  as a Decision-line and stop [FORMS.md §Escalation; ROLES.md §Foreman].
- Complete a task by relaxing a constraint "just here." The task is the variable;
  the constraints are not [ROLES.md §The recursion; the canon's process-over-task].
- Trust a cog's number with a story. A correct number wearing a false story
  re-enters verification like anyone else's [FORMS.md §Claim].
- Optimize, consolidate, or trim before the acceptance token for that scope
  [FORMS.md §Acceptance token; RULINGS.md "slow is smooth, smooth is fast"].

## The recursion

A failing unit is not absorbed upward. It receives the same correction the driver
received: "distill your job to one measurable thing; delegate the rest" — and so do
its delegates, all the way down [ROLES.md §The recursion].
