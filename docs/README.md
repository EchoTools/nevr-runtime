# NEVR Runtime Documentation

## Design

Design documents, architecture decisions, and porting analysis.

| File | Description | Audience |
| ---- | ----------- | -------- |
| `2026-06-09-levr-porting-analysis.md` | leVR D3D11-to-OpenXR/Vulkan porting architecture via DXVK interop | Engine porting developers |
| `2026-06-09-streaming-integration.md` | WebSocket telemetry protocol contract between nevr-runtime and nevr-stream | Runtime and nevr-stream developers |
| `2026-06-29-serverdb-token-auth.md` | ADR: game-server ServerDB auth migration from URL-param credentials to JWT tokens | Runtime and ops developers |
| `2026-07-13-quest-crash-reporter-injection.md` | Quest arm64 crash-reporter injection design via libovrplatformloader.so hijack | Quest porting developers |

## Reference

Format specifications, symbol maps, and procedural runbooks.

| File | Description | Audience |
| ---- | ----------- | -------- |
| `cosmetics-cdn-format.md` | Normative `.evrp` binary format and CDN manifest schema for cosmetic assets | CDN tooling and game-hook developers |
| `quest-hijack-point-map.md` | Symbol-level inventory mapping every Windows hook/patch point to its Quest arm64 equivalent | Quest porting developers |
| `quest-sideload-runbook.md` | Step-by-step APK repack and sideload procedure for the Quest crash reporter | Quest testers and Andrew |
