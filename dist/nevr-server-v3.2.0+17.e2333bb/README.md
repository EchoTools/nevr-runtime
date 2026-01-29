# NEVR Server

## Overview

This project provides a multiplayer game server and modding framework for Echo VR. It consists of multiple DLL components for runtime patching, server management, and telemetry collection.

## Components

### Core DLLs

- **gamepatches** - Runtime game modifications (CLI flags, headless/server modes, Detours-based hooks)
- **gameserver** - Multiplayer game server (session management, events, external service communication)
- **telemetryagent** - Game state monitoring and telemetry streaming

### Legacy Components

- **gamepatcheslegacy** - Frozen v1 implementation (self-contained with local common/)
- **gameserverlegacy** - Frozen v1 implementation (self-contained with local common/)

### Development Tools

- **dbghooks** - Debugging hooks and function tracing for reverse engineering
- **supervisor** - PowerShell scripts for server orchestration (firewall, ports, instance management)

### Shared Code

- **common** - Shared utilities (logging, globals, base64, symbol resolution)
- **protobufnevr** - Protocol buffer code generation from extern/nevr-common submodule

## Directory Structure

```sh
nevr-server/
├── gamepatches/         # Game runtime patches DLL
├── gamepatcheslegacy/   # Legacy patches with local common/
├── gameserver/          # Multiplayer server DLL
├── gameserverlegacy/    # Legacy server with local common/
├── telemetryagent/      # Telemetry collection DLL
├── dbghooks/            # Debug/dev hooks
├── protobufnevr/        # Protocol buffer definitions
├── supervisor/          # PowerShell server management
├── common/              # Shared C++ utilities
├── extern/              # External dependencies (minhook, nevr-common)
├── cmake/               # Build configuration helpers
├── scripts/             # Build scripts (Wine cross-compilation)
├── docs/                # Documentation
├── CMakeLists.txt       # Top-level CMake configuration
├── CMakePresets.json    # CMake build presets
├── vcpkg.json           # Dependency manifest
└── Makefile             # Build convenience wrapper
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
make configure  # Configure with MinGW toolchain
make build      # Build all components
```

### MSVC via Wine (Linux)

```sh
./scripts/setup-msvc-wine.sh   # One-time setup
./scripts/build-with-wine.sh   # Full build using MSVC via Wine
```

### Visual Studio (Windows)

Open the command palette (CTRL+P) and CMake build, when asked to select a kit, select `Visual Studio Community 2022 Release - x86_amd64`.

Or from command line:

```sh
mkdir build
cd build
cmake ..
cmake --build .
```

## Usage

After building, DLL artifacts are in `build/mingw-release/bin/` (MinGW) or `build/release/bin/` (MSVC):

- `gamepatches.dll` → Deploy as `dbgcore.dll` to game directory
- `gameserver.dll` → Deploy as `pnsradgameserver.dll` to game directory
- `dbghooks.dll` → For debugging/development only
- `telemetryagent.dll` → For telemetry collection

See component-specific README files for detailed usage.

## Architecture & Data Flow

- **GameServer** and **GamePatches** are loaded into the game process (DLLs)
- **GameServer** communicates with the game via in-process hooks and with external services (ServerDB, WebSocket, HTTP)
- **TelemetryAgent** polls game state via HTTP API or direct memory access
- All protocol types are defined in `extern/nevr-common` submodule

## Development

### Build Scripts

- `scripts/build-with-wine.sh` - End-to-end MSVC build via Wine
- `scripts/cl-wine.sh`, `lib-wine.sh`, `link-wine.sh` - MSVC toolchain wrappers
- `scripts/protoc-wine.sh` - Protobuf compiler wrapper
- `scripts/setup-msvc-wine.sh` - Wine environment setup

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
- **nevr-common** - Shared protocol definitions and game structures

## Project Conventions

- **Logging**: Use `Log(level, format, ...)` from `common/logging.h`
- **Game flags**: CLI/mode flags (isHeadless, isServer) are in `gamepatches/patches.cpp`
- **Protocol messages**: Symbol IDs defined in `gameserver/messages.h`
- **Protobuf**: All types generated from `extern/nevr-common` - never edit generated files

## Component Comparison: Current vs Legacy

| Feature | Current (gamepatches/gameserver) | Legacy (*legacy) |
|---------|----------------------------------|------------------|
| Common code | Links to root `common/` | Self-contained `common/` subdirectory |
| Dependencies | Uses root dependencies | Minimal dependencies |
| Status | Active development | Frozen/maintenance |
| Use case | Production deployment | Compatibility/fallback |

## Distribution

Create distribution packages:

```sh
make dist       # Full package with debug symbols
make dist-lite  # Stripped binaries without debug symbols
```

Outputs:
- `dist/nevr-server-v{VERSION}.tar.zst`
- `dist/nevr-server-v{VERSION}.zip`
- `dist/nevr-server-v{VERSION}-lite.tar.zst`
- `dist/nevr-server-v{VERSION}-lite.zip`

## Contributing

This is an active development project. Key areas:

1. **Game patching** - Runtime modifications in `gamepatches/`
2. **Server logic** - Multiplayer functionality in `gameserver/`
3. **Telemetry** - Game state collection in `telemetryagent/`
4. **Protocol definitions** - Shared types in `extern/nevr-common`

## License

See LICENSE file for details.
