# nevr-runtime to nevr-stream Telemetry Protocol

This document defines the WebSocket protocol used by nevr-runtime to stream
telemetry frames to nevr-stream. It is the contract between the in-process game
hook (nevr-runtime) and the standalone ingestion service (nevr-stream).

## Architecture

```
nevr-runtime (game process)
  |
  +-- WebSocket --> nevr-stream:8080/ws
       Binary tape envelopes (header -> frames -> footer)
```

## Protocol: telemetry.v2.Envelope

Each WebSocket binary message is a 4-byte little-endian length prefix followed
by a protobuf payload.

### Message types

1. **CaptureHeader** -- session metadata
   - `capture_id` (string), `created_at` (timestamp), `format_version` = 2
   - MUST include `EchoArena` with `session_id` (UUID)
   - Optional: `MapName`, `MatchType`, `ClientName`, private/tournament flags, roster

2. **Frame** (0 to N) -- game state snapshots
   - `frame_index` (uint32, 0-based)
   - `timestamp_offset_ms` (uint32, ms since header created_at)
   - `payload` can include `EchoArena` for game-specific data

3. **CaptureFooter** -- session summary
   - `frame_count` (uint32)
   - `duration_ms` (uint32)

### Server response

After each envelope the server responds with:

```json
{"success": true}
```

or

```json
{"success": false, "error": "..."}
```

## References

- nevr-stream: `~/src/nevr-stream` (remote: `thesprockee/nevr-stream`)
- Tape format library: `~/src/tape/pkg/codec/`
