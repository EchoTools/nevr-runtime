# ADR 0036: a headset or client run is checked against a row written before it

Status: proposed. Applies to every run on a headset or a game client that is meant to show a change works or a defect
is there: a PR's headset check, an issue's falsifier run, a probe build's measurement.

## Context

A headset run read after the fact ("it didn't look right before, it looks right now") cannot tell a fix from a
coincidence, and cannot catch a regression nobody was looking for.

The rule that answers this already exists in general form. AGENTS.md "Do what was asked": "Every check carries a decision
rule. Before any test, build or check, write down the result that would change what you do and the action it would
trigger." AGENTS.md "Review, verification and merge": "Name the result before the check." An issue states its Falsifier
(AGENTS.md "What a PR carries"), and `docs/standards/verification.md` asks that the discriminator be recorded in the
tracking issue before the fix is attempted. The ADRs put it into practice for their first runs: ADRs 0007, 0009, 0014,
0015, 0018 and 0020 to 0026 carry a "Prediction" section, and ADR 0019's section 11 is a table of predictions with a
falsifier and the place each is tested (`docs/adr/0019-quest-plugin-loader.md`, "Verification and predictions").

What the rule does not say is what a headset or client run's written result must contain so that a tester can read the
run against it in minutes. Recent PRs of the release redesign (#1998, #1999, #2022) use a "Hypothesis ... Refuted if"
form; headset sheets such as `docs/testing/quest-texture-quality-headset-sheet.md` list per step what to see, the line
to find and a "Pass and fail" section.

Prior art: preregistration fixes the hypotheses and the analysis before the data are seen, because the same data cannot
both generate a hypothesis and test it; exploratory tests stay allowed when they are kept apart from the confirmatory
ones ([Center for Open Science: preregistration](https://www.cos.io/initiatives/prereg)).

## Decision

This ADR gives AGENTS.md's "decision rule" a fixed form for headset and client runs.

1. **A run has a row for each check, written before it**, in the ADR, the PR body (a PR's check) or the issue (a
   defect's falsifier), committed or posted before the run. The row holds:
   - **H**: the hypothesis, one sentence.
   - **Predict**: the exact log lines (event and fields) and values expected, per headset or platform where they differ
     (a Quest 2 and a Quest 3 are named separately when a value depends on the headset).
   - **Refuted if**: the observation that proves H wrong.
   - **Then**: what is done if it is refuted (back to the code, a revert, a new issue), decided before the run.
   - **Test**: the steps, the build (its version and variant, ADR 0040) and what to capture.

   An issue's Falsifier (AGENTS.md "What a PR carries") serves as its row's "Refuted if". A run whose expected result
   AGENTS.md already states has that statement as its row: the PC login test after a release cut, whose log shows the
   four keys, then `LOGIN SUCCESS` and `to logged in` (AGENTS.md "Verification before merge").
2. **The result is read against the row.** The report quotes the lines the run produced and says, per row, matched or
   refuted. It never re-describes the prediction after the run, and a prediction is never edited once the run started:
   a changed prediction is a new row and a new run.
3. **A refuted row follows its Then** (AGENTS.md: "'Run it N times' is neither proof nor a done condition").
4. **A run with no row is exploratory** and is reported as such; it can suggest a row, never confirm a change.
5. **An owed run is a task line on the issue** (`docs/standards/planning.md`): a PR whose rows still wait for their run
   links the issue with `Refs`, and the issue is closed with the run's quoted result.

## Finding a violation

Scope: PRs and issues from this ADR on, and the headset sheets under `docs/testing/`.

- **P1.** A PR or issue that names a headset or client run to be made and has no row with H, Predict, Refuted if, Then
  and Test for it. A PR that makes no run is not a hit.
- **P2.** A run report that states a result without quoting the run's lines, or without matched or refuted per row.
- **P3.** A row edited after the run it predicts (the PR's or issue's edit history shows it).
- **P4.** A headset sheet that does not say, for each check it makes, what refutes it and what follows, either per step
  or in one "Pass and fail" section that names each check.

## Consequences

- A tester's run takes minutes to judge: compare the quoted lines with the row.
- A refuted row is a finding with a decided next step.

## Not claimed

That a row catches every regression: it catches the ones it predicts. Behaviour nobody wrote a row for is still found by
testers and filed as a defect.
