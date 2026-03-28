# NEVR Runtime

## Overview

Runtime patches for Echo VR (echovr.exe) that enable it to connect to [echovrce](https://github.com/echotools) community game services. Both the game client and dedicated game server use these DLLs to communicate with the Nakama-based backend.

Part of the **nEVR** project — keeping Echo VR alive.

## Components

### Core DLLs

- **gamepatches** → `dbgcore.dll` — Runtime game modifications (CLI flags, headless/server modes, Detours-based hooks). Used by both client and dedicated server.
- **gameserver** → `pnsradgameserver.dll` — Game server networking (session management, events, external service communication). Used by the dedicated server only.
- **telemetryagent** — Game state monitoring and telemetry streaming (in `extras/`, needs protobuf-lite refactor)

### Plugins

Optional DLLs loaded at runtime from a `plugins/` subdirectory next to the game binary:

- **log-filter** → `log_filter.dll` — Structured log filtering, suppression, and file rotation
- **server-timing** → `server_timing.dll` — Wine CPU optimization for headless servers
- **broadcaster-bridge** → `broadcaster_bridge.dll` — Network message mirroring/injection over UDP
- **audio-intercom** → `audio_intercom.dll` — VoIP audio streaming via UDP
- **game-rules-override** → `game_rules_override.dll` — Balance config overrides (health, stun, physics)
- **session-unlocker** → `session_unlocker.dll` — Unlock /session HTTP API in all game modes

### Legacy Components

- **gamepatcheslegacy** — Frozen v1 implementation (self-contained with local common/)
- **gameserverlegacy** — Frozen v1 implementation (self-contained with local common/)

### Shared Code

- **common** — Shared utilities (logging, globals, base64, symbol resolution)
- **protobufnevr** — Protocol buffer code generation from extern/nevr-proto submodule

## Directory Structure

```sh
nevr-runtime/
├── src/
│   ├── gamepatches/         # Game runtime patches DLL (PC)
│   ├── gameserver/          # Game server networking DLL (PC)
│   ├── server/              # Standalone server executable
│   ├── legacy/              # Frozen v1 implementations
│   ├── common/              # Shared C++ utilities
│   └── protobufnevr/        # Protocol buffer definitions
├── plugins/
│   ├── common/              # Shared plugin headers (address registry, config utils)
│   ├── log-filter/          # Log filtering and rotation
│   ├── server-timing/       # Wine CPU optimization
│   ├── broadcaster-bridge/  # Network message mirroring
│   ├── audio-intercom/      # VoIP audio streaming
│   ├── game-rules-override/ # Balance config overrides
│   └── session-unlocker/    # /session API unlock
├── extern/                  # External dependencies (minhook, nevr-proto)
├── cmake/                   # Build configuration helpers
├── tests/                   # Go-based system tests
├── docs/                    # Documentation
├── CMakeLists.txt           # Top-level CMake configuration
├── CMakePresets.json        # CMake build presets
├── justfile                 # Build recipes
└── vcpkg.json               # Dependency manifest
```

## Building the Project

### Prerequisites

- CMake 3.20 or higher [Download](https://cmake.org/download/)
- One of:
  - **MinGW** (cross-compile from Linux) - Recommended
  - **MSVC via Wine** (Linux with Windows SDK mounted)
  - **Visual Studio 2022** (native Windows)

### MinGW Build (Linux - Recommended)

```sh
just configure  # Configure with MinGW toolchain
just build      # Build all components (core DLLs + plugins)
just dist       # Build + create distribution packages
just dist-lite  # Stripped binaries without debug symbols
```

### Visual Studio (Windows)

```sh
just preset=release configure
just preset=release build
```

## Usage

After building, DLL artifacts are in `build/mingw-release/bin/` (MinGW) or `build/release/bin/` (MSVC):

- `gamepatches.dll` → Deploy as `dbgcore.dll` to game directory
- `gameserver.dll` → Deploy as `pnsradgameserver.dll` to game directory
- Plugin DLLs → Deploy to `plugins/` subdirectory next to game binary

## Architecture & Data Flow

- **GamePatches** and **GameServer** are loaded into the game process (DLLs)
- **GameServer** communicates with the game via in-process hooks and with external services (ServerDB, WebSocket, HTTP)
- **Plugins** are discovered and loaded by GamePatches at runtime via the `NvrPluginInterface` lifecycle
- All protocol types are defined in `extern/nevr-proto` submodule

## Related Projects

| Project | Description |
|---------|-------------|
| **nevr-runtime** (this repo) | Runtime patches for echovr.exe (PC + Quest) |
| **nevr-server** | Custom game server (Rust) |
| **nakama** | echovrce game service backend |

## Development

### Local Configuration

Add local customizations to `cmake/local.cmake` (auto-included if exists). This file can override settings or add custom install rules.

Example:
```cmake
# Deploy DLLs to game directory after build
set(GAME_DIR "C:/Program Files/Echo VR")
add_custom_command(TARGET gamepatches POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:gamepatches> "${GAME_DIR}/dbgcore.dll"
)
```

## Dependencies

Dependencies are managed via vcpkg (see `vcpkg.json`):
- **curl** - HTTP client for external API communication
- **jsoncpp** - JSON parsing
- **minhook** - Function hooking (Windows API)
- **opus** - Audio codec
- **protobuf** - Protocol buffer serialization

External submodules (in `extern/`):
- **nevr-proto** - Shared protocol definitions and game structures

## Project Conventions

- **Logging**: Use `Log(level, format, ...)` from `common/logging.h`
- **Game flags**: CLI/mode flags (isHeadless, isServer) are in `gamepatches/patches.cpp`
- **Protocol messages**: Symbol IDs defined in `gameserver/messages.h`
- **Protobuf**: All types generated from `extern/nevr-proto` - never edit generated files

## Distribution

Create distribution packages:

```sh
just dist       # Full package with debug symbols
just dist-lite  # Stripped binaries without debug symbols
```

Outputs:
- `dist/nevr-runtime-v{VERSION}.tar.zst`
- `dist/nevr-runtime-v{VERSION}.zip`
- `dist/nevr-runtime-v{VERSION}-lite.tar.zst`
- `dist/nevr-runtime-v{VERSION}-lite.zip`

## License

See LICENSE file for details.
