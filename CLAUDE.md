# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

NEVR Runtime — Windows DLL patches for Echo VR (echovr.exe) enabling connection to echovrce community game services. Targets both game clients and dedicated game servers. Written in C++17.

## Build Commands

```sh
just                    # Show available recipes
just configure          # Configure CMake only
just build              # Build all components
just dist               # Build + create distribution packages
just dist-lite          # Stripped binaries without debug symbols
just verbose-build      # Build with full compiler output
just clean              # Remove build/ and dist/
just preset=mingw-debug build  # Use a specific preset
```

Build presets: `mingw-debug`, `mingw-release` (Linux default), `linux-wine-debug`, `linux-wine-release`, `debug`, `release` (Windows default).

Build output lands in `build/<preset>/bin/`.

## Testing

Go-based system tests in `tests/system/`:

```sh
just test-system              # All tests
just test-system-short        # Quick tests only
just test-system-dll          # DLL loading tests only
just test-system-verbose      # No cache, verbose
cd tests/system && go test -v -run TestName ./...  # Single test
```

Tests require: Echo VR game binary, evr-test-harness, Go toolchain. See `tests/system/README.md` for prerequisites and environment variables (`NEVR_BUILD_DIR`, `EVR_GAME_DIR`).

## Architecture

### DLL Components

| Component | Output DLL | Deploy As | Purpose |
|-----------|-----------|-----------|---------|
| `src/gamepatches/` | `gamepatches.dll` | `dbgcore.dll` | Runtime hooks, CLI flags, game modifications |
| `src/gameserver/` | `gameserver.dll` | `pnsradgameserver.dll` | Multiplayer networking, session management |
| `src/telemetryagent/` | `telemetryagent.dll` | — | Game state monitoring, telemetry streaming |

All DLLs are loaded into the game process. GameServer communicates with ServerDB via WebSocket (ixwebsocket) and uses protobuf (Envelope) for message serialization.

### Shared Libraries (static)

- **`src/common/`** → `libcommon.a` — Logging, globals, base64, symbol helpers
- **`src/protobufnevr/`** → `libproto_nevr.a` — Pre-generated protobuf from `extern/nevr-proto/gen/cpp/`

### Key Source Files

- `src/gamepatches/patches.cpp` — Main patch implementation, CLI flag processing
- `src/gameserver/gameserver.cpp` — IServerLib vtable implementation
- `src/gameserver/messages.h` — Protocol message symbol IDs (uint64)
- `src/common/globals.h` — Cross-DLL globals (`isServer`, `isHeadless`, `exitOnError`, etc.)
- `src/common/logging.h` — `Log(level, format, ...)` and `FatalError()`

### Legacy & Quest

- `src/legacy/` — Frozen v1 implementations (self-contained, do not modify)
- `src/quest/` — Android/ARM64 standalone runtime

## Conventions

- **Logging**: Always use `Log(EchoVR::LogLevel::Info, "format %d", val)` from `common/logging.h`. Fatal errors via `FatalError(msg, title)`.
- **Hooking**: MinHook-based (`USE_MINHOOK` compile flag). Functions use `__fastcall` convention. Use `ListenForBroadcasterMessage()` for game event callbacks.
- **Protocol messages**: Symbol IDs in `src/gameserver/messages.h`. Serialize via protobuf `rtapi::v1::Envelope`.
- **Protobuf**: Generated from `extern/nevr-proto/proto/`. Never edit `.pb.cc`/`.pb.h` files directly.
- **Global state**: CLI flags as globals in `src/common/globals.h`, set in `src/gamepatches/patches.cpp`.
- **Local overrides**: `cmake/local.cmake` (auto-included if present).

## Dependencies

- **vcpkg** (`~/.vcpkg/`) — curl, ixwebsocket, jsoncpp, minhook, opus, protobuf
- **Submodules** (`extern/`) — nevr-proto (protocol defs), minhook, protobuf
- **Toolchain** — CMake 3.20+, Ninja, MinGW (Linux) or MSVC (Windows)
