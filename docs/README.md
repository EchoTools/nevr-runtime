# nEVR Runtime Documentation

## Design

Design documents, architecture decisions, and porting analysis.

| File | Description | Audience |
| ---- | ----------- | -------- |
| `2026-07-13-quest-crash-reporter-injection.md` | Quest arm64 crash-reporter injection design via libovrplatformloader.so hijack | Quest porting developers |
| `2026-09-21-mic-provider-voip-fix.md` | Microphone provider selection and VOIP initialization analysis | Runtime and Quest developers |
| `2026-10-01-social-scenario-harness.md` | One-client scenario tests for friends and parties: why one client is enough, the owner's constraints, where a scenario may enter the game, and what to build once | Runtime developers and agents testing social features |
| `2026-10-01-social-nakama-proposal.md` | Nakama-side protocol proposal for social feature support | Runtime and game-service developers |
| `2026-10-01-social-features-test-plan.md` | Every social feature the game exposes (from the facade slots, session events, SNS messages and UserProviderID callers), prioritized, with status and the scenario that tests each | Runtime developers and agents testing social features |

## Architecture decisions

| File | Description | Audience |
| ---- | ----------- | -------- |
| `adr/0001-serverdb-token-auth.md` | Game-server ServerDB registration authenticates with a JWT on its own `/nevr` route | Runtime and ops developers |
| `adr/0002-crash-report-ingest.md` | Crash reports reach the game service through a spool and a deferred uploader (not yet implemented) | Runtime and Quest developers |
| `adr/0003-quest-networking-port.md` | Quest networking shares the PCVR protocol core and differs only in adapters (tracked in #158) | Quest porting developers |
| `adr/0004-quest-verification-regime.md` | Quest networking is verified against the exact binaries, offline | Quest porting developers |
| `adr/0005-cosmetics-cdn-format.md` | Cosmetics arrive as `.evrp` packages listed in a JSON manifest on a CDN (normative format) | CDN tooling and game-hook developers |

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

## References

Current behavior and reference material lives in `reference/`; each page covers
one subsystem, protocol surface, or measured game path.

| Directory | Contents | Audience |
| --------- | -------- | -------- |
| `reference/` | Runtime behavior, configuration, protocol surfaces, and test harnesses | Runtime developers and operators |

## Audits

No audit records are kept in the tree. Findings that were still open when the
records were retired are tracked as GitHub issues; `audits/README.md` lists each
retired record with the `git show <sha>:<path>` command that returns it.
