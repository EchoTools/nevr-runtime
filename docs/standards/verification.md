# NEVR Runtime Verification Standards

_Authored by @agents._

**Required reading** for ANY agent claiming a fix works, closing a `BUGS.md`
entry, or adding a check to `just verify`. Read this BEFORE writing "verified",
BEFORE closing an N-entry, and BEFORE trusting a green gate.

`docs/standards/logging.md` governs *what a component says*. This document governs *how you
know it did anything*. They are the same problem seen from two sides: a
component that says nothing and a component that does nothing are
indistinguishable from outside, and both have shipped here.

---

## Installed Is Not Running

> _A hook that installed, a type that compiled, a call site that exists — none
> of these is a thing that happened._

Every serious defect found in this codebase in the 2026-07-26 session had the
same shape: **something reported success while doing nothing.** Not a crash. Not
an exception. Silence, and a gate that accepted it.

### This IS

- A binding standard for the word "verified" in `BUGS.md` and in commit messages.
- A definition of what counts as evidence, ranked, so a claim can be graded
  rather than argued about.
- A required procedure for adding any check to `just verify`: you must break it
  and watch it fail.
- A review gate: the Hard Stops table at the end is enforced.

### This is NOT

- A testing tutorial. It does not tell you how to write a GTest.
- Optional for "obvious" fixes. Every entry in the table below was obvious to
  the agent that wrote it.
- A substitute for `CLAUDE.md` §Methodology or the CPP addendum. On conflict,
  those win.

### You MUST

- State the verification method with every "fixed"/"closed" claim, using the
  ladder below, and quote the measurement.
- Falsify every check you add. Break the thing the check watches, observe the
  check fail, restore, observe it pass. Record both in the commit.
- Prefer a discriminating measurement over a fix when the failure mode is
  ambiguous. One bit that splits two hypotheses is worth more than a day of
  plausible reasoning.
- Read `git log --follow` on any file before deleting it (see §Deleting).
- Say which parts of a fix are unverified, in the same breath as the parts
  that are.

### You must NEVER

- Write "verified" for a check you have not falsified.
- Close an entry on a gate that inspects source shape when the claim is about
  runtime behaviour.
- Let a green `just verify` stand in for "the code runs". It cannot see that.
- Treat absence of an error line as evidence of success (see §Silence).
- Delete dead code without reading its history first.

---

## The Evidence Ladder

Rank every claim. Write the rank in the ledger entry.

| Rank | Method | What it actually proves |
| ---- | ------ | ----------------------- |
| **1** | **Observed behaviour change** on a live server via `./launch-server.sh` — a before/after difference in the log or exit code | The defect was real and is now gone. The only rank that proves both. |
| **2** | **Production-linked test** — a test that links and drives the code that ships | The logic is correct. Does NOT prove that code runs in production. |
| **3** | **Falsified sensor** — a `just verify` check, broken and observed failing | The check works and the source has the shape you claim. Nothing about runtime. |
| **4** | **Type/compile enforcement** | True only of translation units the linker consumes. Verify that first. |
| **5** | **Static measurement** — disassembly, `grep`, call-graph | The hazard exists or does not. Says nothing about whether it fires. |
| **✗** | **Reasoning from a name, a comment, or a prior entry** | Nothing. This is how N83 happened. |

Rank 2 is the trap. "Production-linked" means linked to **the code that runs**,
not to a copy that compiles. Check which one ships before using the phrase.

---

## Silence: the four shapes

A component can report success while doing nothing in exactly four ways. Check
all four before closing anything.

| Shape | Question that exposes it | Precedent |
| ----- | ------------------------ | --------- |
| **Not built** | Is this translation unit in a target the linker consumes? | **N67** — `std::atomic` fix landed in a plugin CMake does not build; closed as VERIFIED-BY-TYPE |
| **Not called** | Does this function have a call site outside its own file? | **N92** — `InstallWebSocketBridge` had one call site, in the *other* copy; the gamepatches copy compiled and never ran |
| **Not reached** | Does this hook ever fire at runtime? | **N86** — tick wired into `CPrecisionSleep::Wait`, which is never called in server mode. Zero entries for a whole run |
| **Not received** | Is the thing arriving at all? | **N89** — log filter emitted happily while capturing **zero** game lines, because another module had taken its hook |

And the reporting variant, which hides all four:

| | | |
| --- | --- | --- |
| **Success logged below the filter** | Is the success line at a level production keeps? | **pnsrad** — patches logged success at `Debug`, failure at `Warning`. Silence meant *either* "all applied" or "never ran" |
| **Return value discarded** | Does the caller check what it called? | **platform_compat** — three installers returned `bool`, all discarded; "initialized" printed unconditionally |

**The generalisation:** a gate that asks *"does this exist?"* can never answer
*"did this do anything?"*. Every defect above passed its gate.

---

## Falsify Every Check

Seven checks written in a single session were silently broken. Each looked
green. Each was found only by deliberately breaking its subject.

| Failure | What happened |
| ------- | ------------- |
| `(^\|[^a-zA-Z_])X\(` matched nothing | GNU grep's ERE mishandles `^` inside an alternation group. `[^a-zA-Z_]X\(` works |
| `\bLog\(` matched its own comment | The function's comment said "no `Log()` here"; the rule flagged itself. **Strip comment lines** |
| Counted lines, not entries | Table packed two entries per line; read 20 of 30 |
| Undercounted enum members | Regex required `kName,` and missed `kName = 0,`; made the comparison unsatisfiable — the check could never fire |
| Tautological `static_assert` | `kEntries[kCount]` then asserting `sizeof/sizeof == kCount` compares a value to itself. Size the array by its **initialiser** |
| Monitor driven by its subject | Health check called only from inside the hook it monitored; when the hook died the check died with it |
| Cumulative where a rate was needed | `count == 0` never fired because 13 lines arrived before the failure began |

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

Three instances in one session, including one inside the tool written to detect
this class.

- The log-filter health check ran only from inside the log hook. When another
  module took the hook, the check stopped — precisely when its warning mattered.
- Per-frame reporting was driven from a hook that does not run in server mode.
- `HookLiveness` reported from the same dead site until it was moved.

**Rule:** drive every monitor from a site whose liveness is independently
proven, and prove it — do not assume it. `HookLiveness::Report` exists to make
that provable.

---

## Discriminate Before Fixing

When a failure has two or more plausible causes that need **different** fixes,
find the single measurement that splits them. Do this before writing any fix.

`N90` is the worked example. Two hypotheses: the log filter was *dropping* the
oversized lines, or it never *saw* them. A probe placed before every
suppress/truncate decision — one bit — settled it in one run. It never saw them,
so no amount of filter tuning would have worked. Two earlier guesses had already
been wrong.

Record the discriminator in the ledger entry **before** attempting the fix, so
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
| "Production-linked" without checking what ships | N92 closed this way | Verify the linked copy is the installed one |
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
- **`CLAUDE.md`** §Methodology, §Continuity — plan-before-code, measure before
  concluding, confirmation bias.
- **`~/src/metis-core/CPP-MINGW-ADDENDUM-GENERIC.md`** — build and C++ hard stops.
- **`BUGS.md`** — the N-ledger. Every entry carries the invariant it protects.
- Worked precedents: **N67**, **N83**, **N86**, **N88**, **N89**, **N90**, **N92**.
