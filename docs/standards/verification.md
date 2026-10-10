# nEVR Runtime Verification Standards

**Required reading** for anyone claiming a fix works or adding a check to
`just verify`. Read it before writing "verified" or relying on a green gate.

`docs/standards/logging.md` governs *what a component says*. This document
governs *how you know it did anything*.

---

## Installed Is Not Running

> _A hook that installed, a type that compiled, a call site that exists — none
> of these is a thing that happened._

Successful installation, compilation, or registration does not prove that the
resulting behavior ran. Verification must measure the behavior relevant to the
claim.

### This IS

- A binding standard for the word "verified" in commit messages.
- A definition of what counts as evidence, ranked, so a claim can be graded
  rather than argued about.
- A required procedure for adding any check to `just verify`: you must break it
  and watch it fail.
- A review gate: the Hard Stops table at the end is enforced.

### This is NOT

- A testing tutorial. It does not tell you how to write a GTest.
- Optional for "obvious" fixes. Every entry in the table below was obvious to
  the agent that wrote it.
- A substitute for `AGENTS.md` §Methodology or the CPP addendum. On conflict,
  those win.

### You SHALL

- You **shall** state the verification method with every "fixed"/"closed" claim, using the
  ladder below, and quote the measurement.
- You **shall** falsify every check you add. Break the thing the check watches, observe the
  check fail, restore, observe it pass. Record both in the commit.
- You **shall** prefer a discriminating measurement over a fix when the failure mode is
  ambiguous. One bit that splits two hypotheses is worth more than a day of
  plausible reasoning.
- You **shall** read `git log --follow` on any file before deleting it (see §Deleting).
- You **shall** say which parts of a fix are unverified, in the same breath as the parts
  that are.

### You SHALL NOT

- You **shall not** write "verified" for a check you have not falsified.
- You **shall not** close an entry on a gate that inspects source shape when the claim is about
  runtime behaviour.
- You **shall not** let a green `just verify` stand in for "the code runs". It cannot see that.
- You **shall not** treat absence of an error line as evidence of success (see §Silence).
- You **shall not** delete dead code without reading its history first.

---

## The Evidence Ladder

Rank every claim. Record the rank and its measurement in the tracking issue or commit message.

| Rank | Method | What it actually proves |
| ---- | ------ | ----------------------- |
| **1** | **Observed behaviour change** on a live server via `./launch-server.sh` or `./verify-server.sh` — a before/after difference in the log or exit code | The defect was real and is now gone. The only rank that proves both. |
| **2** | **Production-linked test** — a test that links and drives the code that ships | The logic is correct. Does NOT prove that code runs in production. |
| **3** | **Falsified sensor** — a `just verify` check, broken and observed failing | The check works and the source has the shape you claim. Nothing about runtime. |
| **4** | **Type/compile enforcement** | True only of translation units the linker consumes. Verify that first. |
| **5** | **Static measurement** — disassembly, `grep`, call-graph | The hazard exists or does not. Says nothing about whether it fires. |
| **✗** | **Reasoning from a name, a comment, or an earlier claim** | Nothing. |

Rank 2 is the trap. "Production-linked" means linked to **the code that runs**,
not to a copy that compiles. Check which one ships before using the phrase.

---

## Silence: the four shapes

A component can report success while doing nothing in exactly four ways. Check
all four before closing anything.

| Shape | Question that exposes it |
| ----- | ------------------------ |
| **Not built** | Is this translation unit in a target the linker consumes? |
| **Not called** | Does this function have a call site outside its own file? |
| **Not reached** | Does this hook ever fire at runtime? |
| **Not received** | Is the thing arriving at all? |

And the reporting variant, which hides all four:

| | | |
| --- | --- | --- |
| **Success logged below the filter** | Is the success line at a level production keeps? |
| **Return value discarded** | Does the caller check what it called? |

**The generalisation:** a gate that asks *"does this exist?"* cannot answer
*"did this do anything?"*.

---

## Falsify Every Check

Common failure modes for static checks include:

| Failure mode | Required response |
| ------- | ------------- |
| Regex misses intended syntax | Test matching and non-matching examples; check the tool's regex dialect |
| Comments satisfy a code check | Strip comments before matching code |
| Count uses display lines instead of records | Count parsed entries, not lines |
| Enum parser misses explicit values | Cover both implicit and explicitly assigned members |
| Assertion compares a value to itself | Derive expected values independently from the initializer |
| Monitor is driven by its subject | Drive it from an independently-live site |
| Cumulative count stands in for a rate | Measure over a bounded interval |

**Procedure — not optional:**

1. Make the check pass on the current tree. Note the exit code.
2. Break the thing it watches — delete the call, revert the type, remove the flag.
3. Run the gate. **It must fail, with a message that names the file.**
4. Restore. Run again. It must pass.
5. Put both observations in the commit message.

If step 3 does not fail, you have not written a check. If a compile error masks
it, break it a way that still compiles — a clean deletion, not a corruption.

---

## A Monitor Must Not Depend On What It Monitors

**Rule:** drive every monitor from a site whose liveness is independently
proven, and prove it — do not assume it. `HookLiveness::Report` exists to make
that provable.

---

## Discriminate Before Fixing

When a failure has two or more plausible causes that need **different** fixes,
find the single measurement that splits them. Do this before writing any fix.

Record the discriminator in the tracking issue **before** attempting the fix, so
the next agent inherits the question rather than the guess.

---

## Deleting

Read `git log --follow --oneline -- <path>` before removing any file, dead code,
or partial implementation.

"Not compiled + zero call sites" proves a file is inert **today**. It says
nothing about whether a fix was applied to it that never reached its successor.
Extraction and refactor commits are exactly where a fix gets stranded in the
abandoned copy, and deleting that copy destroys the only record it existed.

For each commit in the file's history: what did it change, and does that change
exist in whatever superseded it? Diff old against new symbol by symbol. Delete
only when the successor is a demonstrated superset.

---

## Hard Stops

Enforced in review. A change that fails any of these is rejected until fixed.

| Problem | Why it's a stop | Fix |
| ------- | --------------- | --- |
| "Verified" with no method named | The word means nothing alone | State the ladder rank + quote the measurement |
| Sensor added without falsification | It may match nothing; seven did | Break it, watch it fail, record both |
| Runtime claim closed on a source-shape gate | The gate cannot see runtime | Add a rank-1 observation or downgrade the claim |
| "Production-linked" without checking what ships | The claim may refer to a copy that never runs | Verify the linked copy is the installed one |
| Success logged at DEBUG | Invisible in production; silence becomes ambiguous | Log outcomes at INFO, failures at WARNING |
| Install/patch return value discarded | Failure becomes indistinguishable from success | Capture it, report per-item + aggregate |
| Monitor driven by its own subject | Dies exactly when needed | Drive from an independently-proven site |
| Deleting without reading `git log` | A stranded fix disappears with it | Read history, diff against successor |
| Fixing an ambiguous failure without a discriminator | You will fix the wrong thing | Find the one measurement that splits the hypotheses |
| `git log -S` scoped to a file created by a later refactor | Structurally cannot find the origin | Search all paths and all history |

---

## References

- **`docs/standards/logging.md`** — what a component must say. Rule 3 ("Silence is not
  success") is the same principle applied to output.
- **`AGENTS.md`** §Methodology, §Continuity — plan-before-code, measure before
  concluding, confirmation bias.
- **`AGENTS.md`** — C++ Mingw Addendum, build and code hard stops.
