# src/telemetryagent/

**Generated:** 2026-02-09  
**Commit:** abc5734  
**Branch:** build/mingw-minhook

## OVERVIEW

Frame data streaming DLL (`telemetryagent.dll`). Polls game state via HTTP or memory, processes frames for event detection, streams telemetry to external API. Exposes C API for external integration.

## STRUCTURE

```
frames.cpp              # HTTP/memory polling, frame acquisition
frame_processor.cpp     # Event detection, state tracking
telemetry_api.cpp       # C API interface for external callers
dllmain.cpp             # DLL entry point
common/                 # Self-contained copy (v1 frozen, NOT shared)
```

## WHERE TO LOOK

| Task | Location | Notes |
|------|----------|-------|
| Frame polling | `frames.cpp` line 42 | HTTP GET to game API, 60Hz default |
| Memory polling | `frames.cpp` line 156 | Direct memory access (faster, requires offsets) |
| Event detection | `frame_processor.cpp` line 89 | GoalScored, ThrowSuccess, Save events |
| State tracking | `frame_processor.cpp` line 234 | Player positions, disc velocity, match state |
| C API | `telemetry_api.cpp` line 12 | Start/Stop/GetFrame exports |
| Telemetry streaming | `frames.cpp` line 378 | TCP send to external API |
| Protobuf serialization | `frames.cpp` line 412 | Frame → telemetry::Frame proto |

## CONVENTIONS

- **Self-contained common/** (v1 frozen, does NOT link parent src/common/)
- **C API exports** (telemetry_start, telemetry_stop, telemetry_get_frame)
- **60Hz polling default** (configurable via API)
- **Protobuf telemetry schema** (defined in extern/nevr-common)
- **Static runtime** (`/MT` for MSVC, `-static-libstdc++` for MinGW)

## ANTI-PATTERNS (THIS PROJECT)

| Forbidden | Why |
|-----------|-----|
| Linking parent src/common/ | v1 is FROZEN; uses self-contained common/ copy |
| Hardcoded HTTP endpoint | Read from config or pass via C API |
| Blocking in polling thread | Async I/O only, 60Hz is tight |
| Memory offsets without version check | Game updates break offsets; validate or fallback to HTTP |
| Modifying this module | Maintenance-only; new features go in v2 (future) |
| Dynamic CRT | Must use static `/MT` for injection (if loaded by game) |

## BUILD

- **CMake target**: `telemetryagent`
- **Output**: `telemetryagent.dll`
- **Dependencies**: curl, protobufnevr, opus (for audio encoding, optional)
- **No auto-injection** (loaded manually or via PowerShell, not renamed)

## DEPLOYMENT

1. Build produces `telemetryagent.dll`
2. Load manually via LoadLibrary or PowerShell
3. Call C API: `telemetry_start(endpoint, poll_rate_hz)`
4. Call `telemetry_stop()` to clean up

## POLLING MODES

- **HTTP**: 60Hz, high stability, requires game HTTP API enabled (`-httpapi`)
- **Memory**: 240Hz, medium stability, requires game version-specific offsets

**Selection**: HTTP default (`frames.cpp` line 42), memory fallback if offsets available (line 156)

## EVENT DETECTION (frame_processor.cpp)

- **GoalScored** (line 89): Disc enters goal volume (position + velocity)
- **ThrowSuccess** (line 134): Disc released with >15m/s velocity
- **Save** (line 178): Goalie blocks disc in goal zone
- **Assist** (line 212): Pass within 3s before goal

## DEBUGGING

- **No frames received**: Check HTTP API enabled (`-httpapi` flag), verify endpoint
- **Memory polling crash**: Offsets invalid for game version, fallback to HTTP
- **Event false positives**: Tune thresholds in frame_processor.cpp
- **Protobuf errors**: Regenerate proto with `scripts/build-protobuf.sh`
