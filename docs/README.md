# NEVR Runtime Documentation

## Design

Design documents, architecture decisions, and porting analysis.

| File | Description | Audience |
| ---- | ----------- | -------- |
| `2026-06-09-levr-porting-analysis.md` | leVR D3D11-to-OpenXR/Vulkan porting architecture via DXVK interop | Engine porting developers |
| `2026-06-09-streaming-integration.md` | WebSocket telemetry protocol contract between nevr-runtime and nevr-stream | Runtime and nevr-stream developers |
| `2026-06-29-serverdb-token-auth.md` | ADR: game-server ServerDB auth migration from URL-param credentials to JWT tokens | Runtime and ops developers |
| `2026-07-13-quest-crash-reporter-injection.md` | Quest arm64 crash-reporter injection design via libovrplatformloader.so hijack | Quest porting developers |
| `binary-bug-fix-plan.md` | Binary bug fix wave plan — remaining unpatched game bugs and their priority | Runtime developers |

## Reference

Format specifications, symbol maps, and procedural runbooks.

| File | Description | Audience |
| ---- | ----------- | -------- |
| `cosmetics-cdn-format.md` | Normative `.evrp` binary format and CDN manifest schema for cosmetic assets | CDN tooling and game-hook developers |
| `example-config.yaml` | Fully documented example config.yaml — BAC for every config key the runtime supports | Operators and plugin authors |
| `provider-prefix-slots.md` | Platform provider prefix code points and their slot assignments | Runtime developers |

## Process

Operational procedures and testing protocols.

| File | Description | Audience |
| ---- | ----------- | -------- |
| `smoke-tests.md` | Per-feature smoke test suite with 77 signal rows, scorer, and run table | Operators and agents |

## Standards

Coding and verification standards that bind all work in this repo.

| File | Description | Audience |
| ---- | ----------- | -------- |
| `logging.md` | Structured logging format, noise suppression rules, and identity-on-login requirements | All developers |
| `verification.md` | Evidence ladder (rank 1–5), falsification discipline, and gate contract | All developers and agents |

## Audits

Dated, immutable records of specific investigations. Do not modify — these are historical evidence.

| File | Description | Audience |
| ---- | ----------- | -------- |
| `fable-consistency-hunt-2026-07-23.md` | Fable model consistency audit across the codebase | Historical record |
| `recon-owner-bug-batch-RESULTS.md` | Owner bug batch reconstruction results | Historical record |
| `server-run-wave-a-verify.log` | Wave A verification server run log | Historical record |
