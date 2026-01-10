# GitHub Copilot Instructions for NEVR Server

## ⚠️ CRITICAL: Terminal Commands

**ALWAYS use `isBackground: true` for blocking commands** (make, sleep, compilation, etc.)
- `isBackground: false` truncates output (SIGINT/exit 130)
- Use `nohup` for long processes + monitor via `read_file` on logs

##  Project Overview

This repository implements the NEVR multiplayer game server and patching system. It consists of:
- **GameServer**: Multiplayer game server DLL, manages sessions, events, and communication with the game and external services.
- **GamePatches**: DLL for patching/modifying game behavior at runtime (Detours-based hooks, CLI flags, headless/server modes).
- **TelemetryAgent**: DLL for polling game state (via HTTP or memory), processing frames, and streaming telemetry to external APIs.
- **ProtobufNEVR**: Protobuf codegen for all protocol types, built from extern/nevr-common submodule.
- **common/**: Shared C++ code (logging, globals, protocol helpers).

## Architecture & Data Flow

- **GameServer** and **GamePatches** are loaded into the game process (DLLs).
- **GameServer** communicates with the game via in-process hooks and with external services (ServerDB, WebSocket, HTTP) using custom binary and protobuf messages.
- **TelemetryAgent** can poll game state via HTTP API or direct memory access, processes frames, and sends telemetry to a remote API (see `TelemetryAgent/`).
- All protocol types are defined in the extern/nevr-common submodule (see `ProtobufNEVR/CMakeLists.txt`).

## Build & Developer Workflow

- **Cross-platform build**: Uses CMake + Ninja. Windows builds use MSVC via Wine (see `scripts/build-with-wine.sh`).
- **Key scripts**:
  - `scripts/build-with-wine.sh`: End-to-end build using MSVC from Linux via Wine.
  - `scripts/cl-wine.sh`, `lib-wine.sh`, `link-wine.sh`: Wrappers for MSVC toolchain.
  - `scripts/protoc-wine.sh`: Runs protoc.exe via Wine for protobuf codegen.
  - `scripts/setup-msvc-wine.sh`: Sets up Wine prefix and Windows toolchain mapping.
- **Protobuf codegen**: Controlled by `ProtobufNEVR/CMakeLists.txt`. For cross-compiling, uses vcpkg's protoc.exe via Wine wrapper.
- **Local config**: Place overrides in `cmake/local.cmake` (auto-included by top-level CMake).
- **Build output**: All binaries/DLLs land in `build/linux-wine-linux-wine-release/bin/`.

## Project-Specific Patterns & Conventions

- **Game patching**: All CLI/game mode flags (e.g., `isHeadless`, `isServer`, `noConsole`) are set in `GamePatches/patches.cpp` and exported as globals.
- **Logging**: Use `Log(level, format, ...)` from `common/logging.h` for all logging. Fatal errors use `FatalError()`.
- **Protocol messages**: Symbol IDs for all game/broadcaster/server messages are defined in `GameServer/messages.h`.
- **Telemetry**: Frame processing and event detection logic is in `TelemetryAgent/frame_processor.cpp`.
- **Protobuf**: All protocol types are generated from extern/nevr-common. Never edit generated files directly.

## Integration & External Dependencies

- **vcpkg**: All C++ dependencies (Detours, protobuf, googleapis) are managed via `vcpkg.json`.
- **extern/nevr-common**: Submodule for protocol definitions. Update and regenerate as needed.
- **Windows toolchain**: Requires access to a Windows partition with MSVC and Windows SDK for Wine-based builds.

## Example: Building on Linux with Wine

```sh
./scripts/setup-msvc-wine.sh   # One-time setup
./scripts/build-with-wine.sh   # Full build (MSVC via Wine)
```

## Example: Adding a New Protocol Type

1. Update proto in `extern/nevr-common/proto/`
2. Rebuild (triggers codegen via Wine/protoc)
3. Use new types in C++ code (see `ProtobufNEVR/`)

## References
- Top-level: `README.md`, `CMakeLists.txt`
- Build: `scripts/`, `cmake/`
- Protocols: `extern/nevr-common/`, `ProtobufNEVR/`
- Game logic: `GameServer/`, `GamePatches/`, `TelemetryAgent/`, `common/`

---

If any section is unclear or missing key project-specific details, please provide feedback for further refinement.
