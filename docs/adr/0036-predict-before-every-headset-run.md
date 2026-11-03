# ADR 0036: every headset run tests a prediction written before the run

Status: proposed. Applies to every run on a headset or a game client that is meant to show a change works or to show a
defect is there: a PR's headset check, an issue's falsifier, a probe build's measurement.

## Context

A headset run read after the fact ("it didn't look right before, it looks right now") cannot tell a fix from a
coincidence, and cannot catch a regression nobody was looking for. `docs/standards/verification.md` requires every check
to be falsified ("You SHALL falsify every check you add"), but it is about sensors and tests; nothing requires a
headset or client run to state its expected result before it starts. ADR 0004 (the Quest verification regime) has no
prediction rule either.

The release redesign (#1997) writes each suggestion with a hypothesis, a prediction, what refutes it and the test. Some
headset sheets already list what the player should see per step (`docs/testing/quest-texture-quality-headset-sheet.md`,
its "What you should see" and "Line to find" columns), but not what refutes the change or what follows if it is
refuted.

Prior art: preregistration fixes the hypotheses and the analysis before the data are collected, because seeing the data
nudges the analysis toward the result one hoped for; exploratory work stays allowed when it is labelled as such
([Center for Open Science: preregistration](https://www.cos.io/blog/tag/preregistration)).

## Decision

1. **A run has a row, written before it.** Each check of a headset or client run has one row, in the PR body (for a PR's
   check) or the issue (for a defect's falsifier), committed or posted before the run. The row holds:
   - **H**: the hypothesis, one sentence.
   - **Predict**: the exact log lines (event and fields) and values expected, per headset or platform where they differ
     (a Quest 2 and a Quest 3 are named separately when a value depends on the headset).
   - **Refuted if**: the observation that proves H wrong.
   - **Then**: what is done if it is refuted (back to the code, a revert, a new issue), decided before the run.
   - **Test**: the steps, the build (its version and variant, ADR 0040) and what to capture.
2. **The result is read against the row.** The report quotes the lines the run produced and says, per row, matched or
   refuted. It never re-describes the prediction after the run, and a prediction is never edited once the run started:
   a changed prediction is a new row and a new run.
3. **A refuted row follows its Then.** No re-run "to see if it happens again" stands in for it.
4. **A run with no row is exploratory** and is reported as such; it can suggest a row, never confirm a change.
5. **Merge gate for Quest behaviour.** A PR that changes what the Quest build does is merged after its rows matched on
   the headsets they name, or with the open rows listed as `Refs` (docs/standards/planning.md).

## Finding a violation

Scope: PR bodies and issues of `main`'s history from this ADR on, and the headset sheets under `docs/testing/`.

- **P1.** A PR that changes Quest or client behaviour and carries no row with H, Predict, Refuted if, Then and Test.
- **P2.** A run report that states a result without quoting the run's lines, or without matched/refuted per row.
- **P3.** A row edited after the run it predicts (the PR's or issue's edit history shows it).
- **P4.** A headset sheet whose steps say what to see but not what refutes the change.

## Consequences

- A tester's run takes minutes to judge: compare the quoted lines with the row.
- A refuted row is a finding with a decided next step, not a debate.

## Not claimed

That a row catches every regression: it catches the ones it predicts. Behaviour nobody wrote a row for is still found by
testers and filed as a defect.
