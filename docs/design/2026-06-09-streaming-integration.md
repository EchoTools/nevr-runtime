# nevr-runtime → nevr-stream Integration

> **STATUS 2026-07-29: the capability already existed when this was written.**
> The doc proposes a `stream_client.cpp` that was never built — but direct
> WebSocket telemetry streaming that bypasses nakama, using exactly the
> `telemetry.v2.Envelope` / 4-byte-LE-prefix / CaptureHeader-Frame-Footer protocol
> described here, had already shipped in `src/runtime/server/telemetry_streamer.cpp`
> as of commit `488461b` (2026-03-20) — **eleven weeks before this document**. It is
> configured with a plain `telemetry_uri` key in `config.yaml`.
>
> **The one gap this doc identifies that is still real**: there is no local capture

# nevr-runtime → nevr-stream Integration

> **STATUS 2026-07-29: the capability already existed when this was written.**
> The doc proposes a `stream_client.cpp` that was never built — but direct
> WebSocket telemetry streaming that bypasses nakama, using exactly the
> `telemetry.v2.Envelope` / 4-byte-LE-prefix / CaptureHeader-Frame-Footer protocol
> described here, had already shipped in `src/runtime/server/telemetry_streamer.cpp`
> as of commit `488461b` (2026-03-20) — **eleven weeks before this document**. It is
> configured with a plain `telemetry_uri` key.
>
> **The one gap this doc identifies that is still real**: there is no local capture
> or queue fallback when the stream disconnects. Frames are dropped
> (`telemetry_streamer.cpp:687-699`).
>
> Kept for that gap and for the protocol rationale. Ignore the "not implemented"
> framing.

## The Goal

nevr-runtime (the in-process game hook DLL) streams telemetry frames directly to nevr-stream (the standalone ingestion service) via WebSocket, bypassing nakama entirely.

## Current State

- **nevr-stream** is deployed on fortytwo at `fortytwo.echovrce.com:8080`, receiving binary tape envelopes (4-byte LE length-prefixed protobuf `telemetry.v2.Envelope`) via WebSocket at `/ws`. Auth is currently disabled (`NEVR_STREAM_JWT_SECRET` empty). v1.0.0 is live.
- **nevr-runtime** on `main` at `~/src/nevr-runtime` sends telemetry through nakama's now-mostly-removed old pipeline. PR #486 (dead telemetry removal) has been merged. The streaming path needs to be rewired to nevr-stream.
- Nakama PR #486 preserved System A (`EventMatchSummary` → `nevr.match_summaries`) which nevr-stream reads. But the goal is for nevr-runtime to send frames _directly_ to nevr-stream rather than routing through nakama.

## Integration Architecture

```
nevr-runtime (game process)
  │
  ├── WebSocket ──► nevr-stream:8080/ws
  │     Binary tape envelopes (header → frames → footer)
  │     No JWT auth (for now)
  │
  └── Capture files (.tape) stored in nevr-stream's volume
        → /data/captures/ on fortytwo (Docker volume: nevr-stream_captures)
```

## The Protocol (telemetry.v2.Envelope)

Each WebSocket binary message is a 4-byte LE length prefix + protobuf payload:

1. **CaptureHeader** — session metadata (capture_id, created_at, format_version=2)
   - MUST include `EchoArena` with `session_id` (UUID)
   - Optional: MapName, MatchType, ClientName, private/tournament flags, roster

2. **Frame** (0 to N) — game state snapshots
   - `frame_index` (uint32, 0-based)
   - `timestamp_offset_ms` (uint32, ms since header created_at)
   - `payload` can include `EchoArena` for game-specific data (optional for minimal frames)

3. **CaptureFooter** — session summary
   - `frame_count` (uint32)
   - `duration_ms` (uint32)

Server responds with `{"success": true}` or `{"success": false, "error": "..."}` after each envelope.

## Client Implementation

A Go test client exists at `/tmp/test-stream-bin` with source at `/tmp/test-stream/main.go`. It sends a minimal valid stream (header + 5 frames + footer) and verifies the .tape file is created.

## What Needs to Happen

1. **In nevr-runtime**: Add a WebSocket client module that connects to nevr-stream and sends tape envelopes. The existing `gameserver` module already has WebSocket infrastructure via `ixwebsocket` — this can be reused.
   - File to create: a proposed stream_client.cpp under src/runtime (never implemented) (or similar)
   - Configuration: nevr-stream URL (default `ws://fortytwo.echovrce.com:8080/ws`)

2. **Capture flow**: When a match starts, nevr-runtime connects to nevr-stream, sends header, streams frames during gameplay, sends footer on match end.

3. **Fallback**: If nevr-stream is unreachable, fall back to local capture or queuing — don't lose data.

4. **Config**: URL should come from nevr-runtime config (env or config file), defaulting to the production fortytwo endpoint.

## References

- **nevr-stream repo**: `~/src/nevr-stream` (remote: `thesprockee/nevr-stream`)
- **nevr-runtime repo**: `~/src/nevr-runtime` (remote: `EchoTools/nevr-runtime`)
- **tape format**: `~/src/tape/pkg/codec/` — Writer/Reader for .tape files
- **Test client**: `/tmp/test-stream-bin` — working example of the protocol
- **Test source**: `/tmp/test-stream/main.go` — Go source for the test client
