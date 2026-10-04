# NEVR Runtime Documentation

## Design

Design documents, architecture decisions, and porting analysis.

| File | Description | Audience |
| ---- | ----------- | -------- |
| `2026-06-29-serverdb-token-auth.md` | ADR: game-server ServerDB auth migration from URL-param credentials to JWT tokens | Runtime and ops developers |
| `2026-07-13-quest-crash-reporter-injection.md` | Quest arm64 crash-reporter injection design via libovrplatformloader.so hijack | Quest porting developers |
| `2026-10-01-social-scenario-harness.md` | One-client scenario tests for friends and parties: why one client is enough, the owner's constraints, where a scenario may enter the game, and what to build once | Runtime developers and agents testing social features |
| `2026-10-01-social-features-test-plan.md` | Every social feature the game exposes (from the facade slots, session events, SNS messages and UserProviderID callers), prioritized, with status and the scenario that tests each | Runtime developers and agents testing social features |

## Reference

Format specifications, symbol maps, and procedural runbooks.

| File | Description | Audience |
| ---- | ----------- | -------- |
| `cosmetics-cdn-format.md` | Normative `.evrp` binary format and CDN manifest schema for cosmetic assets | CDN tooling and game-hook developers |
| `provider-prefix-slots.md` | Platform provider prefix code points and their slot assignments | Runtime developers |

## Process

Operational procedures and testing protocols.

| File | Description | Audience |
| ---- | ----------- | -------- |
| *(see `just --list` for automated test and verification recipes)* | | |

## Standards

Coding and verification standards that bind all work in this repo.

| File | Description | Audience |
| ---- | ----------- | -------- |
| `logging.md` | Structured logging format, noise suppression rules, and identity-on-login requirements | All developers |
| `verification.md` | Evidence ladder (rank 1–5), falsification discipline, and gate contract | All developers and agents |
| `planning.md` | Release milestones by outcome, Fibonacci points, sprints by work, story states, and the hygiene rules for issues, PRs, branches and milestones | All developers and agents |

## Audits

No audit records are kept in the tree. Findings that were still open when the
records were retired are tracked as GitHub issues; `audits/README.md` lists each
retired record with the `git show <sha>:<path>` command that returns it.
