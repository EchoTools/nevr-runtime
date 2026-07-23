# Audit Artifacts — durable findings from scratch/

These files were recovered from `/var/tmp/work-nevr-runtime/` (tmpfs) during
the 2026-07-23 durable-audits sweep (C9). They represent load-bearing audit
findings paid for in real debugging time. Moving them into repo history ensures
they survive tmpfs sweeps and are discoverable by future agents.

## Artifacts

| File | Date | Description |
|------|------|-------------|
| `ctrlc-shutdown-audit.md` | 2026-07-23 | Full CTRL+C to port-zombie causal chain (N13, N37-N39 resolution) |
| `recon-owner-bug-batch-RESULTS.md` | 2026-07-22 | 19-item validation report from the owner bug batch |
| `bridge-port-audit.md` | 2026-07-23 | Port reference map — every site that references ws_bridge listen port |
| `server-run-wave-a-verify.log` | 2026-07-23 | Trimmed registration witness from Wave A server verification run |

## Generation context

All four artifacts were produced during the Waves A-C workstream (2026-07-20
through 2026-07-23), which resolved the headless DXGI rejection blocker and
established the server registration/CTRL+C teardown audit trails. They were
originally written to the scratch directory per the onboarding conventions
(RULINGS.md 2026-07-20 "Scratch dir (nevr)").

## Exclusions

The following were intentionally NOT copied — they are ephemeral queries/briefs
whose value was consumed during the work:

- `campaign-brief-*` — transient tasking briefs
- `handoff-to-spritz-*` — session handoff notes
- `verify-*` — per-run verification output, not findings
- `worktree-check.md` — temporary check artifact
- `q-*` files — one-shot query fragments
- Build/run logs (except `server-run-wave-a-verify.log`) — ephemeral output
- `*.patch` files — already applied or discarded
- Configuration extracts (`cfg*.txt`, `*.txt` extracts)
