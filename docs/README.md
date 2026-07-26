# Documentation

Start at [`../AGENTS.md`](../AGENTS.md) — it names the reading order and which
documents bind.

| Directory | Holds | Lifetime |
| --------- | ----- | -------- |
| [`standards/`](standards/) | Binding standards. Violations are rejected in review. | Durable |
| [`process/`](process/) | Governance and fleet protocol. | Durable |
| [`guides/`](guides/) | Runbooks and how-tos — procedures you follow. | Durable |
| [`reference/`](reference/) | Format specs, address maps, lookup tables. | Durable |
| [`design/`](design/) | Plans and analyses. Records what was intended and why. | Point-in-time |
| [`primers/`](primers/) | Handoffs for work spanning multiple sessions. | Until the work lands |
| [`audits/`](audits/) | Audit outputs and captured evidence. | Point-in-time |

## Standards

- [`standards/verification.md`](standards/verification.md) — what "verified"
  means. Evidence ranks, the four shapes of silent failure, and the requirement
  to falsify every check before trusting it.
- [`standards/logging.md`](standards/logging.md) — what a component must say.
  Subsystem tags, levels, ten rules, Hard Stops.

## Conventions

- Filenames under `docs/` are `lowercase-kebab-case.md`.
- Point-in-time documents carry an ISO date prefix: `2026-07-26-thing.md`.
  Durable ones do not — a dated standard implies it expires.
- A primer is deleted or moved to `design/` once its work lands. A stale primer
  is worse than none: it hands the next reader a confident, outdated account.
- The work ledger is [`../BUGS.md`](../BUGS.md), not a document here. Findings
  go there; documents explain, the ledger records.
